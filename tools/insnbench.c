/*
 * S5LBox — interpreter throughput benchmark.
 *
 * WHY THIS EXISTS. docs/dynarec.md §1.1-§1.2 records how fast the ARMv6
 * interpreter runs, and every one of those numbers was taken on one desktop
 * x86-64 box with an untracked scratch harness. The document says so itself:
 * "All throughput numbers are desktop x86 numbers." Every statement about the
 * target A9 is therefore an extrapolation from a host with a different ISA, a
 * different memory system and a different compiler.
 *
 * This tool is the reproducible half of that measurement. It is deliberately
 * NOT a ctest: it produces a number, not a verdict, and a number measured on a
 * shared, virtualised CI runner is not something a build should be gated on.
 * It is wired into .github/workflows/core-tests.yml's existing three-platform
 * matrix so that the SAME binary from the SAME commit runs on x86-64 Linux,
 * x86-64 Windows and arm64 macOS on the same day — which is the only way the
 * arm64/x86 ratio means anything.
 *
 * WHAT IT MEASURES, AND WHAT IT DOES NOT.
 *
 * It measures the interpreter's throughput on synthetic loops, reproducing the
 * six configurations §1.1-§1.2 used. It executes real guest instructions
 * through arm_step() — the same entry point tools/bootkernel.c and the iOS app
 * use — on a real s5l8900_t machine, so every access goes through the same
 * arm_mmu_translate/bus_read dispatch chain a boot uses. Nothing here bypasses
 * decode, and nothing hand-rolls a dispatcher.
 *
 * It does NOT measure real guest code. Real code is branchier, is 68.95% Thumb
 * (§1.3), and has a different memory-instruction mix; a synthetic loop is an
 * upper bound on a real workload and the document treats it as one. The
 * rows that say "MMU on" DO include the full ARMv6 page-table walk on every
 * instruction fetch and on every data access, because there is no TLB (§6.1) —
 * that is precisely what the MMU-off/MMU-on pair is here to price.
 *
 * HOW THE RATE IS COMPUTED. The divisor is arm_cpu_t::cycles, the core's own
 * retired-instruction counter, sampled before and after the timed region — not
 * a count this file keeps. The step loop's own iteration count is compared
 * against it afterwards and a mismatch is a hard failure, so a run that retired
 * fewer instructions than it stepped (an exception, a fault, a halt) cannot be
 * silently divided into a large-looking rate.
 *
 * AND WHY THE WORKLOAD CANNOT BE OPTIMISED AWAY. arm_step() lives in libemucore
 * in a different translation unit and the build uses no LTO, so the compiler
 * cannot see through it, let alone delete it. Independently of that, every run
 * is checked against an architectural end state that could only hold if every
 * instruction really executed — the load/store loop leaves its iteration count
 * in *guest memory*, read back through the machine bus — and the checked values
 * are folded into a sink that is printed. A run that executed nothing fails
 * loudly instead of reporting an enormous rate.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"          /* pulls in arm.h */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ clock ---
 *
 * One monotonic wall clock, chosen at compile time from what <time.h> offers,
 * with no platform headers and no third dependency:
 *
 *   clock_gettime(CLOCK_MONOTONIC)  POSIX 2008. Linux, macOS 10.12+, and
 *                                   MinGW-w64 (winpthreads) all have it, and it
 *                                   is the only one of the three that cannot be
 *                                   stepped by NTP mid-measurement.
 *   timespec_get(TIME_UTC)          ISO C11 §7.27.2.5. This is the branch MSVC
 *                                   takes on the windows-latest runner, which
 *                                   has no CLOCK_MONOTONIC.
 *   clock()                         ISO C89, last resort. On Windows this is
 *                                   wall time at CLOCKS_PER_SEC (1 kHz), which
 *                                   is still four orders of magnitude finer
 *                                   than the seconds-long runs measured here.
 *
 * Which one was compiled in is printed in the header line, because a result
 * that does not say how it was timed is not a measurement.
 */
#if defined(CLOCK_MONOTONIC)
#  define INSNBENCH_TIMER "clock_gettime(CLOCK_MONOTONIC)"
static double now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#elif defined(TIME_UTC)
#  define INSNBENCH_TIMER "timespec_get(TIME_UTC)"
static double now_seconds(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#else
#  define INSNBENCH_TIMER "clock()"
static double now_seconds(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}
#endif

/* ------------------------------------------------------- host description ---
 * A rate without a host is not a result. These are all compile-time facts, so
 * the binary describes itself and a pasted log line stays interpretable.
 */
#if defined(__aarch64__) || defined(_M_ARM64)
#  define INSNBENCH_ARCH "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#  define INSNBENCH_ARCH "x86_64"
#elif defined(__arm__) || defined(_M_ARM)
#  define INSNBENCH_ARCH "arm32"
#elif defined(__i386__) || defined(_M_IX86)
#  define INSNBENCH_ARCH "x86"
#else
#  define INSNBENCH_ARCH "unknown-arch"
#endif

#if defined(_WIN32)
#  define INSNBENCH_OS "windows"
#elif defined(__APPLE__)
#  define INSNBENCH_OS "macos"
#elif defined(__linux__)
#  define INSNBENCH_OS "linux"
#else
#  define INSNBENCH_OS "unknown-os"
#endif

#define INSNBENCH_STR2(x) #x
#define INSNBENCH_STR(x)  INSNBENCH_STR2(x)

#if defined(__clang__)
#  define INSNBENCH_CC "clang " __clang_version__
#elif defined(__GNUC__)
#  define INSNBENCH_CC "gcc " __VERSION__
#elif defined(_MSC_VER)
#  define INSNBENCH_CC "msvc " INSNBENCH_STR(_MSC_FULL_VER)
#else
#  define INSNBENCH_CC "unknown-compiler"
#endif

/*
 * Neither GCC, Clang nor MSVC exposes the -O level as a macro, so the honest
 * report is "optimiser on/off/size" plus the CMake configuration the binary was
 * built in. The workflow step prints CMAKE_C_COMPILER and the resolved
 * CMAKE_C_FLAGS_<CONFIG> from the cache immediately before running this, which
 * is where the literal `-O3 -DNDEBUG` comes from.
 */
#if defined(__OPTIMIZE_SIZE__)
#  define INSNBENCH_OPT "optimise-for-size"
#elif defined(__OPTIMIZE__)
#  define INSNBENCH_OPT "optimised"
#elif defined(_MSC_VER)
#  define INSNBENCH_OPT "msvc-unknown"
#else
#  define INSNBENCH_OPT "unoptimised"
#endif

#ifndef INSNBENCH_BUILD_TYPE
#  define INSNBENCH_BUILD_TYPE "unspecified"
#endif

#if defined(NDEBUG)
#  define INSNBENCH_NDEBUG "NDEBUG"
#else
#  define INSNBENCH_NDEBUG "no-NDEBUG"
#endif

/* ------------------------------------------------------------ the machine ---
 *
 * 16 MB of DRAM at the S5L8900's real base. Small enough that a hosted runner
 * does not notice the allocation, large enough that the identity map below is
 * built the same way a real one would be (16 first-level entries, and with 4 KB
 * pages 16 second-level tables).
 *
 * The layout is fixed rather than computed so every address in this file can be
 * checked by eye against the ARMv6 alignment rules:
 *
 *   +0x000000  first-level table, 16 KB, 16 KB-aligned as TTBR0 requires
 *   +0x004000  sixteen second-level tables, 1 KB each and 1 KB-aligned
 *   +0x010000  the loop body
 *   +0x020000  the loop's data word — a DIFFERENT 4 KB page from the code, so
 *              the 4 KB rows really do walk two distinct second-level entries
 *              per iteration rather than reusing one
 */
#define BENCH_RAM_BASE  S5L8900_SDRAM_BASE
#define BENCH_RAM_SIZE  (16u * 1024u * 1024u)
#define BENCH_L1_PA     (BENCH_RAM_BASE + 0x00000000u)
#define BENCH_L2_PA     (BENCH_RAM_BASE + 0x00004000u)
#define BENCH_CODE_VA   (BENCH_RAM_BASE + 0x00010000u)
#define BENCH_DATA_VA   (BENCH_RAM_BASE + 0x00020000u)
#define BENCH_SECTIONS  (BENCH_RAM_SIZE >> 20)          /* 16 */

/* The loop counter's start value. Both loops decrement it once per iteration
 * and neither may reach zero inside a run: a loop that fell out of itself would
 * quietly start executing whatever follows it. 4 G iterations is 21 G
 * instructions, three orders of magnitude past any run this tool performs, and
 * the end-state check pins the exact value it must have decayed to. */
#define BENCH_COUNTER_INIT 0xffffffffu

/*
 * THE TWO LOOPS. Five ARM instructions each, one taken conditional branch each,
 * one flag-setting SUBS each. The only difference between them is that two of
 * the ALU operations become one load and one store — which is what makes the
 * two rows a controlled comparison rather than two unrelated programs.
 *
 * Five, not four, so the pair can be identical in shape. §1.1's historical loop
 * is described only as "a four-instruction loop"; its exact body is not in the
 * tree (the harness was never tracked), so this file states its own rather than
 * guessing at one. See the report note in the header comment.
 */

/*  ALU/branch:
 *      loop:  ADD  r0, r0, #1
 *             ADD  r1, r1, #7
 *             ADD  r3, r3, #1
 *             SUBS r2, r2, #1
 *             BNE  loop
 */
static const uint32_t g_prog_alu[] = {
    0xe2800001u,   /* ADD  r0, r0, #1  */
    0xe2811007u,   /* ADD  r1, r1, #7  */
    0xe2833001u,   /* ADD  r3, r3, #1  */
    0xe2522001u,   /* SUBS r2, r2, #1  */
    0x1afffffau    /* BNE  loop        */
};

/*  load/store:
 *      loop:  LDR  r3, [r4]
 *             ADD  r3, r3, #1
 *             STR  r3, [r4]
 *             SUBS r2, r2, #1
 *             BNE  loop
 *
 *  The read-modify-write is deliberate. It leaves the iteration count in GUEST
 *  MEMORY, which is the strongest end-state assertion available here: the word
 *  at BENCH_DATA_VA can only hold N if all N loads, all N adds and all N stores
 *  really executed, in order, through the real bus.
 */
static const uint32_t g_prog_ldst[] = {
    0xe5943000u,   /* LDR  r3, [r4]    */
    0xe2833001u,   /* ADD  r3, r3, #1  */
    0xe5843000u,   /* STR  r3, [r4]    */
    0xe2522001u,   /* SUBS r2, r2, #1  */
    0x1afffffau    /* BNE  loop        */
};

/*  THUMB alu/branch -- the same five-instruction loop, in the other encoding.
 *
 *      loop:  ADDS r0, #1      3001
 *             ADDS r1, #7      3107
 *             ADDS r3, #1      3301
 *             SUBS r2, #1      3A01
 *             BNE  loop        D1FA
 *
 * WHY THIS ROW EXISTS, AND WHY IT SHOULD HAVE FROM THE START. Every throughput
 * number this file has ever produced was measured on ARM, and §1.3 says real
 * guest code is 68.95% THUMB. So the whole basis for "the interpreter runs at
 * N M insn/s" was an encoding the guest mostly does not use, and the 3x gap
 * between these synthetic rows and a real boot (11.27 vs 3.82 M insn/s) had no
 * way to distinguish "real code is branchier" from "the Thumb decoder is
 * slower". Same loop, same registers, same iteration count, one encoding
 * different: whatever this row differs by IS the encoding.
 *
 * Format-3 ADD/SUB immediate always sets the flags in Thumb, unlike the ARM
 * row's plain ADDs. It changes nothing here because the BNE tests the Z from
 * the SUBS that immediately precedes it either way.
 *
 * Packed two halfwords per word so the existing poke32 loader is unchanged;
 * the sixth halfword is padding and is never reached.
 */
static const uint32_t g_prog_thumb[] = {
    0x31073001u,   /* ADDS r0,#1 ; ADDS r1,#7 */
    0x3A013301u,   /* ADDS r3,#1 ; SUBS r2,#1 */
    0x0000D1FAu    /* BNE  loop  ; (padding)  */
};

/*  MIXED -- ten instructions that land in TEN DIFFERENT PLACES in the ARM
 *  decoder, instead of the same one over and over.
 *
 * WHY THIS ROW EXISTS. Every other row in this file is a homogeneous loop: the
 * same handful of encodings, forever. The ARM decoder in arm_interp.c is not a
 * jump table -- it is a long chain of `if ((insn & mask) == value)` tests -- so
 * a homogeneous loop settles into ONE position in that chain and the host's
 * branch predictor learns it perfectly. Real guest code does not. That makes
 * every synthetic number in this file a best case for the decoder specifically,
 * which is exactly the component under suspicion.
 *
 * The 7.3x gap between the synthetic rows and a real boot (30.92 vs 4.21 M
 * insn/s) has two candidate explanations that the existing rows cannot tell
 * apart: real code is BRANCHIER AND COLDER (i-cache, TLB, indirect targets), or
 * real code is DECODE-SCATTERED. This row changes only the second one -- same
 * loop shape, same tight working set, same host cache behaviour, ten decode
 * positions instead of one. Whatever it costs relative to the load/store row IS
 * the price of walking the chain.
 *
 *      loop:  LDR   r3, [r4]        single data transfer, immediate
 *             ADD   r3, r3, #1      data processing, immediate
 *             STR   r3, [r4]        single data transfer, store
 *             MOV   r5, r3, LSL #1  data processing, immediate-shifted register
 *             MUL   r6, r3, r3      multiply (bits[7:4] == 1001)
 *             EOR   r5, r5, r6      data processing, plain register
 *             LDRB  r7, [r4]        byte load
 *             STRH  r7, [r4, #4]    misc load/store (bits[7:4] == 1011)
 *             SUBS  r2, r2, #1
 *             BNE   loop
 *
 * MUL's Rd differs from its Rm, so it is not the UNPREDICTABLE Rd==Rm form.
 * STRH writes to DATA+4, clear of the word the read-modify-write owns, so the
 * two end-state assertions stay independent.
 */
static const uint32_t g_prog_mixed[] = {
    0xe5943000u,   /* LDR   r3, [r4]       */
    0xe2833001u,   /* ADD   r3, r3, #1     */
    0xe5843000u,   /* STR   r3, [r4]       */
    0xe1a05083u,   /* MOV   r5, r3, LSL #1 */
    0xe0060393u,   /* MUL   r6, r3, r3     */
    0xe0255006u,   /* EOR   r5, r5, r6     */
    0xe5d47000u,   /* LDRB  r7, [r4]       */
    0xe1c470b4u,   /* STRH  r7, [r4, #4]   */
    0xe2522001u,   /* SUBS  r2, r2, #1     */
    0x1afffff5u    /* BNE   loop           */
};

/*
 * VFP, because a composited frame is full of it.
 *
 * The QuartzCore software rasteriser this project is trying to get past does
 * its texture-coordinate stepping in single-precision floating point --
 * CA::OGL::sw_sample_nearest_BGRA8 opens with vldr/vmul.f32/vcvt.s32.f32 and
 * has a vdiv.f32 in its span setup. Every one of those is interpreted here,
 * and until this row existed the bench measured integer work exclusively, so
 * the cost of the instructions the frame is actually made of was unmeasured.
 *
 * The values are chosen to be a fixed point of the loop: s0 = 2.0 and
 * s1 = 1.0 give s2 = 2.0, s3 = 3.0 and s4 = 3 on every iteration. Nothing
 * grows, so nothing reaches an infinity or a denormal and the row cannot
 * accidentally become a measurement of the slow paths in vfp.c instead of the
 * ordinary ones. Every word below was verified by disassembling it with
 * Capstone rather than trusted from a manual.
 */
static const uint32_t g_prog_vfp[] = {
    0xee201a20u,   /* VMUL.F32     s2, s0, s1 */
    0xee711a20u,   /* VADD.F32     s3, s2, s1 */
    0xeebd2ae1u,   /* VCVT.S32.F32 s4, s3     */
    0xe2522001u,   /* SUBS         r2, r2, #1 */
    0x1afffffau    /* BNE          loop       */
};

/* Every configuration's loop length divides this, so one rounded instruction
 * budget ends every configuration exactly at the top of its loop. Rows carry
 * their own length in bench_cfg_t::loop_insns; this is only the budget's
 * granularity. */
#define BENCH_LOOP_LCM   10u

/* Every checked end-state value is folded in here and the result is printed, so
 * no part of the workload or its verification is dead code. */
static uint64_t g_sink;

typedef struct {
    const char *loop;        /* "alu/branch" or "load/store"            */
    const char *mmu;         /* "off" | "sections-1M" | "pages-4K"      */
    const char *tick;        /* "no" | "yes"                            */
    const uint32_t *prog;
    unsigned    prog_words;
    unsigned    loop_insns;  /* instructions per iteration; divides the LCM  */
    bool        vfp;         /* enable CP10/CP11 and FPEXC.EN, seed s0/s1    */
    bool        mmu_on;
    bool        small_pages;
    bool        do_tick;
    bool        thumb;       /* enter with CPSR.T set                   */
} bench_cfg_t;

/* Designated initialisers deliberately: this table gained a field once and
 * every positional row would have shifted silently. core-tests spent twelve
 * commits red for exactly that mistake in the arm_bus_t initialisers. */
static const bench_cfg_t g_configs[] = {
    { .loop = "alu/branch",  .mmu = "off",         .tick = "no",
      .prog = g_prog_alu,   .prog_words = (unsigned)(sizeof g_prog_alu   / 4),
      .loop_insns = 5u },
    { .loop = "load/store",  .mmu = "off",         .tick = "no",
      .prog = g_prog_ldst,  .prog_words = (unsigned)(sizeof g_prog_ldst  / 4),
      .loop_insns = 5u },
    { .loop = "load/store",  .mmu = "off",         .tick = "yes",
      .prog = g_prog_ldst,  .prog_words = (unsigned)(sizeof g_prog_ldst  / 4),
      .loop_insns = 5u, .do_tick = true },
    { .loop = "load/store",  .mmu = "sections-1M", .tick = "no",
      .prog = g_prog_ldst,  .prog_words = (unsigned)(sizeof g_prog_ldst  / 4),
      .loop_insns = 5u, .mmu_on = true },
    { .loop = "load/store",  .mmu = "pages-4K",    .tick = "no",
      .prog = g_prog_ldst,  .prog_words = (unsigned)(sizeof g_prog_ldst  / 4),
      .loop_insns = 5u, .mmu_on = true, .small_pages = true },
    { .loop = "load/store",  .mmu = "pages-4K",    .tick = "yes",
      .prog = g_prog_ldst,  .prog_words = (unsigned)(sizeof g_prog_ldst  / 4),
      .loop_insns = 5u, .mmu_on = true, .small_pages = true, .do_tick = true },
    /* The encoding real code actually uses. Paired with the ARM row above it
     * and with the MMU row, so the comparison is one variable at a time. */
    { .loop = "alu/branch T",.mmu = "off",         .tick = "no",
      .prog = g_prog_thumb, .prog_words = (unsigned)(sizeof g_prog_thumb / 4),
      .loop_insns = 5u, .thumb = true },
    { .loop = "alu/branch T",.mmu = "pages-4K",    .tick = "yes",
      .prog = g_prog_thumb, .prog_words = (unsigned)(sizeof g_prog_thumb / 4),
      .loop_insns = 5u, .mmu_on = true, .small_pages = true, .do_tick = true,
      .thumb = true },
    /* Decode scatter. Paired with the "load/store off/no" row: same working
     * set, same host cache behaviour, ten decoder positions instead of one. */
    { .loop = "mixed x10",   .mmu = "off",         .tick = "no",
      .prog = g_prog_mixed, .prog_words = (unsigned)(sizeof g_prog_mixed / 4),
      .loop_insns = 10u },
    { .loop = "mixed x10",   .mmu = "pages-4K",    .tick = "yes",
      .prog = g_prog_mixed, .prog_words = (unsigned)(sizeof g_prog_mixed / 4),
      .loop_insns = 10u, .mmu_on = true, .small_pages = true, .do_tick = true },

    /* VFP. The frame's rasteriser is float-heavy, so the integer rows above
     * do not price the instructions it is actually made of. */
    { .loop = "vfp mul/add/cvt", .mmu = "off",      .tick = "no",
      .prog = g_prog_vfp,   .prog_words = (unsigned)(sizeof g_prog_vfp / 4),
      .loop_insns = 5u, .vfp = true },
    { .loop = "vfp mul/add/cvt", .mmu = "pages-4K", .tick = "yes",
      .prog = g_prog_vfp,   .prog_words = (unsigned)(sizeof g_prog_vfp / 4),
      .loop_insns = 5u, .mmu_on = true, .small_pages = true, .do_tick = true,
      .vfp = true }
};
#define BENCH_CONFIG_COUNT (sizeof g_configs / sizeof g_configs[0])

/* ------------------------------------------------------------ page tables ---
 *
 * Written through the machine's own bus, at physical addresses, exactly as a
 * boot loader would — so the descriptors the walker reads back came through the
 * same path the guest's would.
 */
static void poke32(s5l8900_t *m, uint32_t pa, uint32_t val) {
    m->bus.write32(m->bus.ctx, pa, val);
}

static uint32_t peek32(s5l8900_t *m, uint32_t pa) {
    return m->bus.read32(m->bus.ctx, pa);
}

/*
 * An identity map of the whole 16 MB DRAM aperture, in the ARMv6 extended (XP)
 * descriptor format the ARM1176 uses with SCTLR.XP set — which is the format
 * XNU programs (see the SCTLR note below).
 *
 *   section     va | AP(3) << 10 | type 2       = va | 0xc02
 *   coarse L1   l2_pa | domain(0) << 5 | type 1 = l2_pa | 0x001
 *   small page  pa | AP(3) << 4 | type 2        = pa | 0x032
 *
 * AP == 3 is read/write for all, APX/XN clear, domain 0. DACR gives domain 0
 * the CLIENT attribute rather than MANAGER on purpose: a manager domain skips
 * the AP check entirely, so it would price a cheaper walk than the guest's.
 */
static void build_tables(s5l8900_t *m, bool small_pages) {
    for (unsigned i = 0; i < BENCH_SECTIONS; i++) {
        uint32_t va = BENCH_RAM_BASE + (i << 20);
        uint32_t l1_pa = BENCH_L1_PA + ((va >> 20) << 2);

        if (!small_pages) {
            poke32(m, l1_pa, (va & 0xfff00000u) | 0xc02u);
            continue;
        }

        uint32_t l2_pa = BENCH_L2_PA + i * 1024u;
        poke32(m, l1_pa, (l2_pa & 0xfffffc00u) | 0x001u);
        for (unsigned p = 0; p < 256u; p++) {
            uint32_t page = va + (p << 12);
            poke32(m, l2_pa + (p << 2), (page & 0xfffff000u) | 0x032u);
        }
    }
}

/*
 * Prove the map is the map this row claims to be measuring, BEFORE any timing.
 *
 * This exists because the difference between the sections row and the 4 KB row
 * is entirely a difference in how many guest memory reads the walker performs —
 * one for a section, two for a small page — and a typo that quietly left both
 * rows walking a section would produce two plausible, wrong numbers and no
 * symptom at all. Checking the first-level descriptor's type settles it: with
 * type 1 the section path in arm_mmu_translate is not reachable, so a
 * successful translation is proof the second level really was read.
 */
static bool check_map(s5l8900_t *m, bool small_pages) {
    uint32_t l1 = peek32(m, BENCH_L1_PA + ((BENCH_CODE_VA >> 20) << 2));
    unsigned want = small_pages ? 1u : 2u;
    if ((l1 & 3u) != want) {
        printf("  ERROR: first-level descriptor 0x%08x has type %u, expected %u\n",
               l1, l1 & 3u, want);
        return false;
    }

    uint32_t pa = 0;
    if (arm_mmu_translate(&m->cpu, BENCH_CODE_VA, ARM_ACCESS_FETCH, true, &pa) != 0
        || pa != BENCH_CODE_VA) {
        printf("  ERROR: the code page does not translate to itself\n");
        return false;
    }
    if (arm_mmu_translate(&m->cpu, BENCH_DATA_VA, ARM_ACCESS_WRITE, true, &pa) != 0
        || pa != BENCH_DATA_VA) {
        printf("  ERROR: the data page is not writable through the map\n");
        return false;
    }
    return true;
}

/*
 * Bring one configuration up. Called once per repetition, OUTSIDE the timed
 * region, so every repetition times an identical amount of identical work from
 * an identical starting state.
 */
static bool setup(s5l8900_t *m, const bench_cfg_t *cfg) {
    if (!s5l8900_init(m, BENCH_RAM_BASE, BENCH_RAM_SIZE)) return false;

    for (unsigned i = 0; i < cfg->prog_words; i++)
        poke32(m, BENCH_CODE_VA + i * 4u, cfg->prog[i]);
    poke32(m, BENCH_DATA_VA, 0u);

    if (cfg->mmu_on) build_tables(m, cfg->small_pages);

    arm_cpu_t *cpu = &m->cpu;

    /*
     * SCTLR.XP and SCTLR.U are set in EVERY configuration, and only SCTLR.M
     * differs between the MMU rows. That is what makes this a measurement of
     * the MMU rather than of the MMU plus an alignment-model change: XNU sets
     * XP|U at __start+0x16c, so leaving them set with the MMU off keeps the
     * load/store path byte-for-byte the same in both rows.
     */
    cpu->cp15.sctlr = ARM_SCTLR_XP | ARM_SCTLR_U;
    if (cfg->mmu_on) {
        cpu->cp15.sctlr |= ARM_SCTLR_M;
        cpu->cp15.ttbr0  = BENCH_L1_PA;
        cpu->cp15.ttbcr  = 0;          /* TTBR0 for the whole address space */
        cpu->cp15.dacr   = 0x1u;       /* domain 0 = client; AP is checked  */
    }

    /*
     * arm_reset leaves the core in SVC with CPSR.I and CPSR.F set, and nothing
     * here clears them. That is load-bearing for the two `tick=yes` rows: the
     * device tick can raise a VIC line, and an interrupt actually taken would
     * add exception entry to what is supposed to be the price of s5l8900_tick()
     * alone. The end-state check requires the core to still be in SVC with its
     * PC at the top of the loop, which no taken exception could satisfy.
     */
    cpu->r[0] = 0;
    cpu->r[1] = 0;
    cpu->r[2] = BENCH_COUNTER_INIT;
    cpu->r[3] = 0;
    cpu->r[4] = BENCH_DATA_VA;
    cpu->r[15] = BENCH_CODE_VA;
    /* Thumb state is a CPSR bit, not an address bit: r15 stays even and the
     * interpreter's fetch width follows T. Set AFTER the mode/CPSR setup above
     * so nothing there clears it. */
    if (cfg->thumb) cpu->cpsr |= ARM_CPSR_T;

    /*
     * VFP is off out of reset and a disabled unit makes every one of these
     * instructions UNDEFINED, which would turn this row into a measurement of
     * the exception path. XNU's _init_vfp grants CP10 and CP11 full access and
     * then gates per thread with FPEXC.EN; both halves are needed here for the
     * same reason they are needed there.
     */
    if (cfg->vfp) {
        cpu->cp15.cpacr |= 0xfu << ARM_CPACR_CP10_SHIFT;
        cpu->vfp_fpexc  |= ARM_FPEXC_EN;
        cpu->vfp_s[0] = 0x40000000u;   /* 2.0f */
        cpu->vfp_s[1] = 0x3f800000u;   /* 1.0f */
    }

    if (cfg->mmu_on && !check_map(m, cfg->small_pages)) return false;
    return true;
}

/* ---------------------------------------------------------- the timed run ---
 *
 * Two loops rather than one with a branch in it, so the `tick=no` rows do not
 * pay for a per-instruction test of a flag that never changes.
 */
static bool run_burst(s5l8900_t *m, bool do_tick, uint64_t insns,
                      uint64_t *retired, double *seconds) {
    arm_cpu_t *cpu = &m->cpu;
    uint64_t before = cpu->cycles;
    bool ok = true;

    double t0 = now_seconds();
    if (do_tick) {
        for (uint64_t i = 0; i < insns; i++) {
            if (arm_step(cpu) != ARM_OK) { ok = false; break; }
            s5l8900_tick(m, 1u);
        }
    } else {
        for (uint64_t i = 0; i < insns; i++) {
            if (arm_step(cpu) != ARM_OK) { ok = false; break; }
        }
    }
    double t1 = now_seconds();

    /* The divisor is the core's own retired-instruction counter, not the loop
     * trip count above. They are compared by the caller. */
    *retired = cpu->cycles - before;
    *seconds = t1 - t0;
    return ok;
}

/*
 * The end-state check. Everything here is a value the loop could not hold
 * unless it really ran to completion, and the load/store row's strongest
 * witness — the iteration count sitting in guest RAM — is read back through the
 * machine bus rather than out of the host array.
 */
static bool verify(s5l8900_t *m, const bench_cfg_t *cfg, uint64_t retired) {
    const arm_cpu_t *cpu = &m->cpu;
    uint64_t iters = retired / cfg->loop_insns;
    uint32_t it32 = (uint32_t)iters;
    bool ok = true;

    if (retired % cfg->loop_insns != 0) {
        printf("  ERROR: retired %" PRIu64 " is not a whole number of %u-instruction iterations\n",
               retired, cfg->loop_insns);
        ok = false;
    }
    if (cpu->r[15] != BENCH_CODE_VA) {
        printf("  ERROR: pc=0x%08x, expected the loop head 0x%08x\n",
               cpu->r[15], (unsigned)BENCH_CODE_VA);
        ok = false;
    }
    if ((cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_SVC) {
        printf("  ERROR: cpsr mode 0x%02x, expected SVC -- an exception was taken\n",
               cpu->cpsr & ARM_CPSR_MODE_MASK);
        ok = false;
    }
    if (cpu->abort_pending) {
        printf("  ERROR: an abort is pending\n");
        ok = false;
    }
    if (cpu->r[2] != BENCH_COUNTER_INIT - it32) {
        printf("  ERROR: r2=0x%08x, expected 0x%08x\n",
               cpu->r[2], BENCH_COUNTER_INIT - it32);
        ok = false;
    }

    if (cfg->prog == g_prog_vfp) {
        /*
         * The loop is a fixed point, so these are the same after one iteration
         * as after twenty million -- which is exactly what makes them a
         * witness that the VFP unit was ENABLED and executing. A trapped,
         * undefined VFP instruction leaves s2 at zero and would be caught
         * here rather than silently timing the exception path.
         */
        if (cpu->vfp_s[2] != 0x40000000u) {
            printf("  ERROR: s2=0x%08x, expected 0x40000000 (2.0f)\n",
                   cpu->vfp_s[2]);
            ok = false;
        }
        if (cpu->vfp_s[3] != 0x40400000u) {
            printf("  ERROR: s3=0x%08x, expected 0x40400000 (3.0f)\n",
                   cpu->vfp_s[3]);
            ok = false;
        }
        if (cpu->vfp_s[4] != 3u) {
            printf("  ERROR: s4=%u, expected 3\n", cpu->vfp_s[4]);
            ok = false;
        }
        g_sink += (uint64_t)cpu->vfp_s[2] + cpu->vfp_s[3] + cpu->vfp_s[4];
    } else if (cfg->prog == g_prog_alu || cfg->prog == g_prog_thumb) {
        if (cpu->r[0] != it32) {
            printf("  ERROR: r0=%u, expected %u\n", cpu->r[0], it32);
            ok = false;
        }
        if (cpu->r[1] != it32 * 7u) {
            printf("  ERROR: r1=%u, expected %u\n", cpu->r[1], it32 * 7u);
            ok = false;
        }
        if (cpu->r[3] != it32) {
            printf("  ERROR: r3=%u, expected %u\n", cpu->r[3], it32);
            ok = false;
        }
        g_sink += (uint64_t)cpu->r[0] + cpu->r[1] + cpu->r[3];
    } else {
        uint32_t mem = peek32(m, BENCH_DATA_VA);
        if (cpu->r[3] != it32) {
            printf("  ERROR: r3=%u, expected %u\n", cpu->r[3], it32);
            ok = false;
        }
        if (mem != it32) {
            printf("  ERROR: guest memory at 0x%08x holds %u, expected %u -- "
                   "the loop did not execute\n",
                   (unsigned)BENCH_DATA_VA, mem, it32);
            ok = false;
        }
        g_sink += (uint64_t)cpu->r[3] + mem;

        /* The six extra encodings the mixed row adds. Checking them is what
         * makes "ten decoder positions" a claim about work that provably
         * happened rather than about bytes that were merely present. */
        if (cfg->prog == g_prog_mixed) {
            uint32_t want_r6 = it32 * it32;              /* MUL  r6, r3, r3   */
            uint32_t want_r5 = (it32 << 1) ^ want_r6;    /* MOV/LSL then EOR  */
            uint32_t want_r7 = it32 & 0xffu;             /* LDRB of the word  */
            uint32_t half    = peek32(m, BENCH_DATA_VA + 4u) & 0xffffu;
            if (cpu->r[6] != want_r6) {
                printf("  ERROR: r6=%u, expected %u -- MUL\n", cpu->r[6], want_r6);
                ok = false;
            }
            if (cpu->r[5] != want_r5) {
                printf("  ERROR: r5=%u, expected %u -- MOV/LSL then EOR\n",
                       cpu->r[5], want_r5);
                ok = false;
            }
            if (cpu->r[7] != want_r7) {
                printf("  ERROR: r7=%u, expected %u -- LDRB\n", cpu->r[7], want_r7);
                ok = false;
            }
            if (half != want_r7) {
                printf("  ERROR: halfword at 0x%08x holds %u, expected %u -- STRH\n",
                       (unsigned)(BENCH_DATA_VA + 4u), half, want_r7);
                ok = false;
            }
            g_sink += (uint64_t)cpu->r[5] + cpu->r[6] + cpu->r[7] + half;
        }
    }
    g_sink += cpu->r[2];
    return ok;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void usage(const char *argv0) {
    printf("usage: %s [--insns N] [--reps N]\n"
           "\n"
           "  --insns N   guest instructions per repetition (default 20000000,\n"
           "              the sample size docs/dynarec.md section 1.1 used; rounded up\n"
           "              to a multiple of %u so every row ends at the top of its loop)\n"
           "  --reps  N   repetitions per configuration (default 5)\n"
           "\n"
           "Prints M instructions/sec per configuration. This is a measurement,\n"
           "not a test: it has no pass threshold and is not registered with\n"
           "ctest. It exits non-zero only if a run failed its end-state check.\n",
           argv0, BENCH_LOOP_LCM);
}

int main(int argc, char **argv) {
    uint64_t insns = 20000000u;
    unsigned reps = 5u;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--insns") == 0 && i + 1 < argc) {
            insns = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            reps = (unsigned)strtoul(argv[++i], NULL, 0);
        } else {
            fprintf(stderr, "insnbench: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (insns < BENCH_LOOP_LCM) insns = BENCH_LOOP_LCM;
    /* Round up so a run always ends exactly at the top of the loop, which is
     * what lets the end state be an exact equality rather than a range. The
     * LCM rather than one row's length: every row runs the same budget, and it
     * has to divide evenly for all of them. */
    insns = ((insns + BENCH_LOOP_LCM - 1u) / BENCH_LOOP_LCM) * BENCH_LOOP_LCM;
    if (reps < 1u) reps = 1u;

    double *rates = calloc((size_t)BENCH_CONFIG_COUNT * reps, sizeof *rates);
    double *sorted = calloc(reps, sizeof *sorted);
    bool cfg_ok[BENCH_CONFIG_COUNT];
    uint64_t retired_first[BENCH_CONFIG_COUNT];
    if (!rates || !sorted) {
        fprintf(stderr, "insnbench: out of memory\n");
        free(rates); free(sorted);
        return 1;
    }
    for (size_t c = 0; c < BENCH_CONFIG_COUNT; c++) {
        cfg_ok[c] = true;
        retired_first[c] = 0;
    }

    printf("INSNBENCH-HOST arch=%s os=%s compiler=\"%s\" opt=%s %s cmake_build_type=%s\n",
           INSNBENCH_ARCH, INSNBENCH_OS, INSNBENCH_CC,
           INSNBENCH_OPT, INSNBENCH_NDEBUG, INSNBENCH_BUILD_TYPE);
    printf("INSNBENCH-METHOD timer=%s insns_per_rep=%" PRIu64 " reps=%u headline=median "
           "order=interleaved ram=%uMB loop_insns=%u\n",
           INSNBENCH_TIMER, insns, reps,
           (unsigned)(BENCH_RAM_SIZE >> 20), BENCH_LOOP_LCM);
    /*
     * WHY THE MEDIAN IS THE HEADLINE, AND NOT BEST-OF-N.
     *
     * Best-of-N is the usual choice, and the usual argument for it is sound as
     * far as it goes: a neighbour competing for the machine can only ever ADD
     * time to a fixed amount of work, so the fastest of N samples is the one
     * least contaminated. That argument silently assumes the host executes at a
     * fixed rate, and no host this runs on does. A laptop and a cloud runner
     * both scale frequency, and a repetition that happens to land in a boost
     * window finishes genuinely faster without the interpreter having done
     * anything differently. Best-of-N then reports peak boost rather than
     * sustained throughput — and, worse, reports it unevenly across rows.
     *
     * That is not hypothetical: measured on the project's dev box, best-of-5
     * inverted two rows relative to their medians, ranking a configuration with
     * a two-level page-table walk on every access AHEAD of the same loop with
     * the MMU disabled entirely. The median cannot do that, because it is the
     * only one of the three statistics robust in BOTH directions — against a
     * neighbour that inflates elapsed time and against a boost window that
     * deflates it. Best and worst are printed beside it so the spread stays
     * visible and the choice stays falsifiable.
     *
     * WHY THE REPETITIONS ARE INTERLEAVED. Running all N repetitions of one
     * configuration before starting the next makes the table an ordering
     * artefact on any host whose clock rate depends on how long it has been
     * busy. The two tick=yes rows here are several times longer than the
     * others, so a configuration measured immediately after one of them was
     * being priced against a hotter, slower CPU than the one measured first.
     * Sweeping all six configurations once per repetition spreads each row's
     * samples across the whole session instead, so a drift that is monotonic in
     * time falls on every row roughly equally.
     */
    printf("INSNBENCH-LEGEND median/best/worst are M guest instructions per second over the\n");
    printf("INSNBENCH-LEGEND repetitions. The MEDIAN is the headline: it is the only one of\n");
    printf("INSNBENCH-LEGEND the three robust both to a busy neighbour (which adds time) and\n");
    printf("INSNBENCH-LEGEND to a frequency-boost window (which removes it). Repetitions\n");
    printf("INSNBENCH-LEGEND sweep all configurations in turn so that host clock drift cannot\n");
    printf("INSNBENCH-LEGEND be mistaken for a difference between rows.\n");
    printf("INSNBENCH-LEGEND mmu=off is an identity translation with no table walk;\n");
    printf("INSNBENCH-LEGEND mmu=sections-1M and mmu=pages-4K INCLUDE the full ARMv6 walk on\n");
    printf("INSNBENCH-LEGEND every fetch and every data access, because there is no TLB.\n");
    fflush(stdout);

    int failures = 0;

    for (unsigned r = 0; r < reps; r++) {
        for (size_t c = 0; c < BENCH_CONFIG_COUNT; c++) {
            const bench_cfg_t *cfg = &g_configs[c];
            if (!cfg_ok[c]) continue;

            s5l8900_t m;
            if (!setup(&m, cfg)) {
                printf("  ERROR: could not bring up loop=%s mmu=%s\n",
                       cfg->loop, cfg->mmu);
                cfg_ok[c] = false;
                s5l8900_free(&m);
                continue;
            }

            uint64_t retired = 0;
            double seconds = 0.0;
            bool stepped = run_burst(&m, cfg->do_tick, insns, &retired, &seconds);

            if (!stepped) {
                printf("  ERROR: arm_step returned a non-OK status after %" PRIu64
                       " instructions\n", retired);
                cfg_ok[c] = false;
            }
            if (retired != insns) {
                printf("  ERROR: the core retired %" PRIu64 " instructions but %" PRIu64
                       " steps were taken\n", retired, insns);
                cfg_ok[c] = false;
            }
            if (seconds <= 0.0) {
                printf("  ERROR: the clock did not advance (%.9f s) -- the timer is unusable\n",
                       seconds);
                cfg_ok[c] = false;
            }
            if (!verify(&m, cfg, retired)) cfg_ok[c] = false;

            rates[c * reps + r] = (seconds > 0.0)
                                    ? ((double)retired / seconds) / 1e6
                                    : 0.0;
            if (r == 0) retired_first[c] = retired;

            s5l8900_free(&m);
        }
    }

    for (size_t c = 0; c < BENCH_CONFIG_COUNT; c++) {
        const bench_cfg_t *cfg = &g_configs[c];
        if (!cfg_ok[c]) {
            printf("INSNBENCH loop=%-10s mmu=%-11s tick=%-3s FAILED\n",
                   cfg->loop, cfg->mmu, cfg->tick);
            failures++;
            continue;
        }

        memcpy(sorted, &rates[c * reps], (size_t)reps * sizeof *sorted);
        qsort(sorted, reps, sizeof *sorted, cmp_double);
        double worst = sorted[0];
        double best  = sorted[reps - 1];
        double median = (reps % 2u == 1u)
                          ? sorted[reps / 2u]
                          : 0.5 * (sorted[reps / 2u - 1u] + sorted[reps / 2u]);

        printf("INSNBENCH loop=%-10s mmu=%-11s tick=%-3s "
               "median=%7.2f best=%7.2f worst=%7.2f Minsn/s retired=%" PRIu64 " reps=%u\n",
               cfg->loop, cfg->mmu, cfg->tick, median, best, worst,
               retired_first[c], reps);
    }
    fflush(stdout);

    free(rates);
    free(sorted);
    printf("INSNBENCH-SINK 0x%016" PRIx64 " (checked end-state values; a zero here would "
           "mean nothing executed)\n", g_sink);
    printf("INSNBENCH-DONE failures=%d\n", failures);
    return failures ? 1 : 0;
}
