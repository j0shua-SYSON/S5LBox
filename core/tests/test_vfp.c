/*
 * S5LBox — VFPv2 (VFP11) unit tests.
 *
 * Same shape as test_arm.c: a flat 1 MiB RAM behind arm_bus_t, hand-assembled
 * ARM encodings, single-stepped, with assertions. Every encoding used here was
 * cross-checked against the ARM ARM's bit layout by hand and matches what an
 * assembler emits for the mnemonic in the comment.
 *
 * The things worth testing about a VFP are not the happy-path adds. They are:
 * the s/d aliasing (the classic source of silent corruption), the load/store
 * multiple forms the kernel's own context switch depends on, the FPEXC.EN
 * asymmetry that IS the lazy-enable mechanism, and — most of all — that the
 * encodings this unit does not implement still stop the machine instead of
 * inventing a number.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include "vfp.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- flat memory */
#define RAM_SIZE (1u << 20)
static uint8_t g_ram[RAM_SIZE];

static uint32_t m_r32(void *ctx, uint32_t a){ (void)ctx; uint32_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],4); return v; }
static uint16_t m_r16(void *ctx, uint32_t a){ (void)ctx; uint16_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],2); return v; }
static uint8_t  m_r8 (void *ctx, uint32_t a){ (void)ctx; return g_ram[a&(RAM_SIZE-1)]; }
static void m_w32(void *ctx, uint32_t a, uint32_t v){ (void)ctx; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,4); }
static void m_w16(void *ctx, uint32_t a, uint16_t v){ (void)ctx; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,2); }
static void m_w8 (void *ctx, uint32_t a, uint8_t  v){ (void)ctx; g_ram[a&(RAM_SIZE-1)]=v; }

/* Designated, so a new optional hook on arm_bus_t cannot break this file.
 * Every omitted member is a NULL optional hook. */
static const arm_bus_t g_bus = {
    .ctx     = NULL,
    .read32  = m_r32, .read16  = m_r16, .read8  = m_r8,
    .write32 = m_w32, .write16 = m_w16, .write8 = m_w8,
};

/* ------------------------------------------------------------- test runner */
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Reset into SYS mode with VFP fully available: CPACR grants CP10/CP11 and
 * FPEXC.EN is set, which is the state XNU leaves a thread in after
 * _vfp_switch. Tests that care about the disabled case clear it themselves. */
static void vfp_reset(arm_cpu_t *c) {
    memset(g_ram, 0, sizeof g_ram);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS;
    c->cp15.cpacr  = 0xfu << 20;
    c->vfp_fpexc   = ARM_FPEXC_EN;
}

static void load(const uint32_t *prog, size_t words) {
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i * 4), prog[i]);
}

/* Run `steps` instructions from PC=0, returning the status of the last one. */
static arm_status_t run(arm_cpu_t *c, const uint32_t *prog, size_t words, int steps) {
    arm_status_t st = ARM_OK;
    load(prog, words);
    for (int i = 0; i < steps; i++) { st = arm_step(c); if (st != ARM_OK) break; }
    return st;
}

/* ------------------------------------------------------------- encodings ---
 *
 * Extension register load/store:  cond 110 P U D W L Rn Vd 101 sz imm8
 */
#define VFP_LS(P,U,D,W,L,rn,vd,sz,imm8)                                       \
    (0xec000000u | ((uint32_t)(P)<<24) | ((uint32_t)(U)<<23)                  \
     | ((uint32_t)(D)<<22) | ((uint32_t)(W)<<21) | ((uint32_t)(L)<<20)        \
     | ((uint32_t)(rn)<<16) | ((uint32_t)(vd)<<12) | 0xa00u                   \
     | ((uint32_t)(sz)<<8) | (uint32_t)(imm8))

/*
 * VFP data processing: cond 1110 p D q r Vn Vd 101 sz N s M 0 Vm.
 * p/q/r are bits 23/21/20 (the opcode, with D wedged between them) and s is
 * bit 6, opc3<0>.
 */
#define VFP_DP(p,q,r,D,vn,vd,sz,N,s,M,vm)                                     \
    (0xee000a00u | ((uint32_t)(p)<<23) | ((uint32_t)(D)<<22)                  \
     | ((uint32_t)(q)<<21) | ((uint32_t)(r)<<20) | ((uint32_t)(vn)<<16)       \
     | ((uint32_t)(vd)<<12) | ((uint32_t)(sz)<<8) | ((uint32_t)(N)<<7)        \
     | ((uint32_t)(s)<<6) | ((uint32_t)(M)<<5) | (uint32_t)(vm))

/* Single-precision register N split into its Vx field and its extra bit: the
 * low bit of a single register number lives in D/N/M, the other four in the
 * Vd/Vn/Vm field. Getting this backwards is the classic VFP decode bug, so the
 * tests spell real register numbers and let these do the splitting. */
#define SV(n) ((n) >> 1)
#define SB(n) ((n) & 1u)

/* Three-operand arithmetic, by register number. `s` is opc3<0>. */
#define DP_S(p,q,r,s, sd,sn,sm)                                               \
    VFP_DP((p),(q),(r), SB(sd), SV(sn), SV(sd), 0, SB(sn), (s), SB(sm), SV(sm))
#define DP_D(p,q,r,s, dd,dn,dm)                                               \
    VFP_DP((p),(q),(r), 0, (dn), (dd), 1, 0, (s), 0, (dm))

/* The "other" group: p=q=r=1 and opc3<0>=1, keyed by opc2 in the Vn field,
 * with opc3<1> (bit 7) as `top`. */
#define UN_S(opc2,top, sd,sm)                                                 \
    VFP_DP(1,1,1, SB(sd), (opc2), SV(sd), 0, (top), 1, SB(sm), SV(sm))
#define UN_D(opc2,top, dd,dm)                                                 \
    VFP_DP(1,1,1, 0, (opc2), (dd), 1, (top), 1, 0, (dm))
/* The two conversions whose source and destination differ in width. */
#define UN_D_FROM_S(opc2,top, dd,sm)                                          \
    VFP_DP(1,1,1, 0, (opc2), (dd), 0, (top), 1, SB(sm), SV(sm))
#define UN_S_FROM_D(opc2,top, sd,dm)                                          \
    VFP_DP(1,1,1, SB(sd), (opc2), SV(sd), 1, (top), 1, 0, (dm))

#define VMRS(rt,crn) (0xeef00a10u | ((uint32_t)(crn)<<16) | ((uint32_t)(rt)<<12))
#define VMSR(crn,rt) (0xeee00a10u | ((uint32_t)(crn)<<16) | ((uint32_t)(rt)<<12))
#define VMOV_S_R(sn,rt) (0xee000a10u | ((uint32_t)SV(sn)<<16) | ((uint32_t)(rt)<<12) | ((uint32_t)SB(sn)<<7))
#define VMOV_R_S(rt,sn) (0xee100a10u | ((uint32_t)SV(sn)<<16) | ((uint32_t)(rt)<<12) | ((uint32_t)SB(sn)<<7))
#define VMOV_DWORD_R(dn,hi,rt) (0xee000b10u | ((uint32_t)(hi)<<21) | ((uint32_t)(dn)<<16) | ((uint32_t)(rt)<<12))
#define VMOV_R_DWORD(rt,dn,hi) (0xee100b10u | ((uint32_t)(hi)<<21) | ((uint32_t)(dn)<<16) | ((uint32_t)(rt)<<12))

/* --------------------------------------------------------------- bit casts */
static uint32_t f2u(float f)  { uint32_t u; memcpy(&u,&f,4); return u; }
static float    u2f(uint32_t u){ float f;   memcpy(&f,&u,4); return f; }
static uint64_t d2u(double d) { uint64_t u; memcpy(&u,&d,8); return u; }
static double   u2d(uint64_t u){ double d;  memcpy(&d,&u,8); return d; }

/* ================================================== the register file ===== */

/* Cortex-A8 VMOV core transfers. Actual register numbers, independently
 * encoded from DDI0406C.b A8.8.341-345; kind 0=S, 1=D word, 2=S pair, 3=D. */
static uint32_t a8_core_move(unsigned kind, unsigned load, unsigned fp,
                              unsigned rt, unsigned rt2, unsigned hi) {
    if (kind < 2u) return 0xee000a10u | (load << 20) | (rt << 12) |
        (kind ? 0x100u | ((fp & 15u) << 16) | ((fp >> 4) << 7) | (hi << 21) :
                ((fp >> 1) << 16) | ((fp & 1u) << 7));
    return 0xec400a10u | (load << 20) | (rt << 12) | (rt2 << 16) |
        (kind == 3u ? 0x100u | (fp & 15u) | ((fp >> 4) << 5) :
                      (fp >> 1) | ((fp & 1u) << 5));
}

static arm_status_t a8_move_step(arm_cpu_t *c, unsigned thumb, uint32_t insn) {
    uint32_t pc = c->r[15];
    if (thumb) { m_w16(NULL, pc, (uint16_t)(insn >> 16)); m_w16(NULL, pc + 2u, (uint16_t)insn); }
    else m_w32(NULL, pc, insn);
    return arm_step(c);
}

static void a8_move_reset(arm_cpu_t *c, unsigned thumb) {
    CHECK(arm_reset_profile(c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset A8");
    c->cpsr = ARM_MODE_SYS | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q |
              (5u << 16) | (thumb ? ARM_CPSR_T : 0u);
    c->cp15.cpacr = 0x00f00000u; c->vfp_fpexc = ARM_FPEXC_EN;
    /* Nondefault controls cannot change a bitwise transfer. */
    c->vfp_fpscr = ARM_FPSCR_QC | ARM_FPSCR_RMODE | ARM_FPSCR_DN | ARM_FPSCR_FZ | ARM_FPSCR_IDC;
    c->r[15] = 0x100u;
}

static void test_a8_vfp_full_bank_core_moves(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
        /* Fill every D register through two core registers, then read each
         * half through a different encoding. D16 must not overwrite D0. */
        for (unsigned d = 0; d < 32u; d++) {
            c.r[2] = 0x7f800001u + d; c.r[9] = 0xff800020u - d;
            CHECK(a8_move_step(&c, thumb, a8_core_move(3u, 0u, d, 2u, 9u, 0u)) == ARM_OK,
                  "A8 VMOV D%u write T=%u", d, thumb);
        }
        for (unsigned d = 0; d < 32u; d++) {
            for (unsigned hi = 0; hi < 2u; hi++) {
                CHECK(a8_move_step(&c, thumb, a8_core_move(1u, 1u, d, 4u, 0u, hi)) == ARM_OK &&
                      c.r[4] == (hi ? 0xff800020u - d : 0x7f800001u + d),
                      "A8 independent D%u half%u T=%u", d, hi, thumb);
            }
            CHECK(vfp_get_d(&c, d) == ((uint64_t)(0xff800020u - d) << 32 | (0x7f800001u + d)),
                  "A8 public D getter aliases a different register d=%u", d);
        }
        /* S0..S31 still alias only the low sixteen D registers. */
        for (unsigned s = 0; s < 32u; s++) {
            CHECK(a8_move_step(&c, thumb, a8_core_move(0u, 1u, s, 4u, 0u, 0u)) == ARM_OK &&
                  c.r[4] == (s & 1u ? 0xff800020u - s / 2u : 0x7f800001u + s / 2u),
                  "A8 S/D low-bank alias s=%u T=%u", s, thumb);
            c.r[4] = 0x11223300u + s;
            CHECK(a8_move_step(&c, thumb, a8_core_move(0u, 0u, s, 4u, 0u, 0u)) == ARM_OK,
                  "A8 S write s=%u T=%u", s, thumb);
        }
        for (unsigned d = 0; d < 32u; d++) {
            CHECK(a8_move_step(&c, thumb, a8_core_move(3u, 1u, d, 4u, 6u, 0u)) == ARM_OK &&
                  c.r[4] == (d < 16u ? 0x11223300u + d * 2u : 0x7f800001u + d) &&
                  c.r[6] == (d < 16u ? 0x11223301u + d * 2u : 0xff800020u - d),
                  "A8 S write corrupted upper D bank d=%u T=%u", d, thumb);
            c.r[4] = 0xdead0000u + d;
            CHECK(a8_move_step(&c, thumb, a8_core_move(1u, 0u, d, 4u, 0u, d & 1u)) == ARM_OK,
                  "A8 D half write d=%u T=%u", d, thumb);
            CHECK(a8_move_step(&c, thumb, a8_core_move(3u, 1u, d, 2u, 9u, 0u)) == ARM_OK &&
                  c.r[d & 1u ? 9u : 2u] == 0xdead0000u + d &&
                  c.r[d & 1u ? 2u : 9u] == (d < 16u ? 0x11223300u + d * 2u + !(d & 1u) :
                                                     d & 1u ? 0x7f800001u + d : 0xff800020u - d),
                  "A8 D half write changed the other half d=%u T=%u", d, thumb);
        }
        CHECK(c.cpsr == flags && c.vfp_fpscr == fpscr && c.vfp_fpexc == ARM_FPEXC_EN,
              "A8 core VMOV altered floating-point controls or ARM flags");
        /* A reset clears both banks, including after a profile transition. */
        arm_reset(&c, &g_bus);
        a8_move_reset(&c, thumb);
        for (unsigned d = 0; d < 32u; d++) {
            CHECK(a8_move_step(&c, thumb, a8_core_move(3u, 1u, d, 2u, 9u, 0u)) == ARM_OK &&
                  c.r[2] == 0u && c.r[9] == 0u, "A8 reset retained D%u T=%u", d, thumb);
        }
        /* A pair starting at odd S crosses a D boundary, never into D16. */
        for (unsigned s = 0; s < 31u; s++) {
            c.r[2] = 0xaaa00000u + s; c.r[9] = 0xbbb00000u + s;
            CHECK(a8_move_step(&c, thumb, a8_core_move(2u, 0u, s, 2u, 9u, 0u)) == ARM_OK &&
                  vfp_get_s(&c, s) == c.r[2] && vfp_get_s(&c, s + 1u) == c.r[9],
                  "A8 consecutive S pair write s=%u T=%u", s, thumb);
            CHECK(a8_move_step(&c, thumb, a8_core_move(2u, 1u, s, 4u, 6u, 0u)) == ARM_OK &&
                  c.r[4] == c.r[2] && c.r[6] == c.r[9], "A8 S pair read s=%u T=%u", s, thumb);
        }
    }
}

static void test_a8_vfp_core_move_permissions_and_registers(void) {
    static const unsigned permissions[] = {0,1,3};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned kind = 0; kind < 4u; kind++) {
      for (unsigned load = 0; load < 2u; load++) {
       for (unsigned user = 0; user < 2u; user++) {
        for (unsigned enabled = 0; enabled < 2u; enabled++) {
         for (unsigned acc = 0; acc < 3u; acc++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            c.cp15.cpacr = permissions[acc] * 0x00500000u;
            uint32_t flags = c.cpsr, generation = c.tlb_gen;
            c.r[2] = 0x11223344u; c.r[9] = 0x55667788u;
            bool allowed = enabled && (permissions[acc] == 3u || (permissions[acc] == 1u && !user));
            CHECK(a8_move_step(&c, thumb, a8_core_move(kind, load, 30u, 2u, 9u, 1u)) == ARM_OK,
                  "A8 core VMOV disposition T=%u kind=%u U=%u EN=%u acc=%u", thumb, kind, user, enabled, permissions[acc]);
            if (allowed) {
                CHECK(c.r[15] == 0x104u && c.cpsr == flags &&
                      c.r[2] == (load ? 0u : 0x11223344u) &&
                      c.r[9] == (load && kind >= 2u ? 0u : 0x55667788u),
                      "A8 allowed core VMOV effects T=%u kind=%u L=%u", thumb, kind, load);
            } else {
                CHECK(c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == (thumb ? 0x102u : 0x104u) &&
                      c.spsr[ARM_BANK_UND] == flags && (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND &&
                      c.r[2] == 0x11223344u && c.r[9] == 0x55667788u && vfp_get_s(&c, 30u) == 0u &&
                      vfp_get_d(&c, 30u) == 0u, "A8 denied core VMOV changed operands or exception state");
            }
            CHECK(c.tlb_gen == generation && c.cycles == 1u, "core VMOV changed translation/retirement count");
         }
        }
       }
       for (unsigned rt = 0; rt < 16u; rt++) {
        for (unsigned rt2 = 0; rt2 < (kind >= 2u ? 16u : 1u); rt2++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            for (unsigned n = 0; n < 15u; n++) c.r[n] = 0x12340000u + n;
            uint32_t regs[16]; memcpy(regs, c.r, sizeof regs);
            bool allowed = rt != 15u && !(thumb && rt == 13u) &&
                (kind < 2u || (rt2 != 15u && !(thumb && rt2 == 13u) && (!load || rt != rt2)));
            CHECK(a8_move_step(&c, thumb, a8_core_move(kind, load, 30u, rt, rt2, 1u)) ==
                  (allowed ? ARM_OK : ARM_UNDEFINED),
                  "A8 core VMOV register legality T=%u kind=%u L=%u Rt=%u Rt2=%u", thumb, kind, load, rt, rt2);
            if (allowed) { if (load) { regs[rt] = 0u; if (kind >= 2u) regs[rt2] = 0u; } regs[15] += 4u; }
            CHECK(memcmp(regs, c.r, sizeof regs) == 0,
                  "A8 core VMOV changed other core registers T=%u kind=%u L=%u Rt=%u Rt2=%u", thumb, kind, load, rt, rt2);
            if (allowed && !load) {
                uint32_t lo = c.r[rt], hi = kind >= 2u ? c.r[rt2] : 0u;
                CHECK(kind == 0u ? vfp_get_s(&c, 30u) == lo : kind == 1u ?
                      vfp_get_d(&c, 30u) == (uint64_t)lo << 32 : kind == 2u ?
                      vfp_get_s(&c, 30u) == lo && vfp_get_s(&c, 31u) == hi :
                      vfp_get_d(&c, 30u) == ((uint64_t)hi << 32 | lo), "A8 core VMOV stored wrong data");
            }
        }
       }
      }
     }
    }
}

static void test_a8_vfp_core_move_refusals_and_it(void) {
    const uint32_t invalid[] = {
        a8_core_move(0u,0u,30u,2u,9u,0u) | 1u,
        a8_core_move(0u,1u,30u,2u,9u,0u) | 0x20u,
        a8_core_move(0u,1u,30u,2u,9u,0u) | 0x40u,
        a8_core_move(1u,0u,30u,2u,9u,0u) | 0x40u,
        a8_core_move(1u,1u,30u,2u,9u,0u) | 0x400000u, /* SIMD byte */
        a8_core_move(1u,0u,30u,2u,9u,0u) | 0x20u, /* SIMD halfword */
        a8_core_move(2u,0u,31u,2u,9u,0u),
        a8_core_move(2u,1u,31u,2u,9u,0u),
        a8_core_move(3u,0u,31u,2u,9u,0u) | 0x40u,
        a8_core_move(3u,1u,31u,2u,9u,0u) | 0x80u,
        a8_core_move(3u,0u,31u,2u,9u,0u) & ~0x10u,
        a8_core_move(3u,1u,31u,2u,9u,0u) & ~0x10u
    };
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned enabled = 0; enabled < 2u; enabled++) {
      for (unsigned skip = 0; skip < 2u; skip++) {
       for (unsigned n = 0; n < sizeof invalid / sizeof invalid[0]; n++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d, UINT64_C(0x7ff01234dead0000) + d);
        c.r[2] = 0xabcdef01u; c.r[9] = 0x12345678u;
        c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
        if (skip) c.cp15.cpacr = 0u;
        if (thumb) {
            m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); /* IT EQ/NE */
            CHECK(arm_step(&c) == ARM_OK, "invalid VMOV IT setup");
        }
        uint32_t flags = c.cpsr, pc = c.r[15], insn = invalid[n];
        if (!thumb && skip) insn &= 0x0fffffffu; /* EQ false */
        CHECK(a8_move_step(&c, thumb, insn) == (skip ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (skip ? pc + 4u : pc) && c.r[2] == 0xabcdef01u && c.r[9] == 0x12345678u &&
              c.cpsr == (thumb && skip ? flags & ~0x0600fc00u : flags),
              "A8 reserved/conditional VMOV T=%u EN=%u skip=%u n=%u", thumb, enabled, skip, n);
        for (unsigned d = 0; d < 32u; d++)
            CHECK(vfp_get_d(&c, d) == UINT64_C(0x7ff01234dead0000) + d,
                  "refused/skipped VMOV modified D%u", d);
       }
      }
     }
     for (unsigned kind = 0; kind < 4u; kind++) {
      for (unsigned load = 0; load < 2u; load++) {
       for (unsigned skip = 0; skip < 2u; skip++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.r[2] = 0xdeadbeefu; c.r[9] = 0x11223344u;
        if (skip) { c.cp15.cpacr = 0u; c.vfp_fpexc = 0u; }
        if (thumb) {
            m_w16(NULL, 0x100u, skip ? 0xbf0cu : 0xbf1cu); /* first EQ/NE slot */
            CHECK(arm_step(&c) == ARM_OK, "VMOV IT setup");
        }
        uint32_t flags = c.cpsr, pc = c.r[15];
        uint32_t insn = a8_core_move(kind, load, 30u, 2u, 9u, 1u);
        if (!thumb && skip) insn &= 0x0fffffffu;
        CHECK(a8_move_step(&c, thumb, insn) == ARM_OK && c.r[15] == pc + 4u &&
              c.r[2] == (!skip && load ? 0u : 0xdeadbeefu) &&
              c.r[9] == (!skip && load && kind >= 2u ? 0u : 0x11223344u) &&
              c.cpsr == (thumb ? (flags & ~0x0600fc00u) | 0x1800u : flags),
              "A8 core VMOV lost conditional/IT retirement T=%u kind=%u L=%u skip=%u", thumb, kind, load, skip);
       }
      }
     }
    }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++) {
     for (unsigned kind = 1u; kind < 4u; kind += 2u) {
      for (unsigned load = 0; load < 2u; load++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "reset legacy");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        c.r[2] = 0x12345678u; c.r[9] = 0xabcdef01u;
        vfp_set_d(&c, 0u, UINT64_C(0x1122334455667788));
        CHECK(a8_move_step(&c, 0u, a8_core_move(kind, load, 16u, 2u, 9u, 1u)) == ARM_UNDEFINED &&
              c.r[2] == 0x12345678u && c.r[9] == 0xabcdef01u &&
              vfp_get_d(&c, 0u) == UINT64_C(0x1122334455667788), "A8 upper bank leaked into legacy VMOV");
      }
     }
    }
    /* Upper-bank arithmetic is still separate work. */
    arm_cpu_t c;
    a8_move_reset(&c, 0u);
    vfp_set_d(&c, 16u, UINT64_C(0x1122334455667788));
    CHECK(a8_move_step(&c, 0u, VFP_DP(0,1,1,1,0,0,1,0,0,0,0)) == ARM_UNDEFINED &&
          vfp_get_d(&c, 16u) == UINT64_C(0x1122334455667788), "core VMOV enabled upper-bank arithmetic");
}

/* VLDR/VSTR use D:Vd for doublewords and Vd:D for singlewords. */
static uint32_t a8_single_memory(unsigned load, unsigned dbl, unsigned fp,
                                  unsigned rn, unsigned add, unsigned imm8) {
    return VFP_LS(1,add,dbl ? fp >> 4 : fp & 1u,0,load,rn,
                  dbl ? fp & 15u : fp >> 1,dbl,imm8);
}

static void test_a8_vfp_single_memory_bank_and_offsets(void) {
    const unsigned offsets[] = {0u,1u,255u};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned dbl = 0; dbl < 2u; dbl++) {
      for (unsigned fp = 0; fp < 32u; fp++) {
       for (unsigned add = 0; add < 2u; add++) {
        for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.r[4] = 0x4004u; /* Four-byte alignment suffices for D registers. */
            uint32_t address = add ? c.r[4] + offsets[i] * 4u : c.r[4] - offsets[i] * 4u;
            uint32_t lo = 0x7f800001u + fp, hi = 0xff800001u + fp;
            if (dbl) vfp_set_d(&c, fp, (uint64_t)hi << 32 | lo);
            else vfp_set_s(&c, fp, lo);
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            m_w32(NULL, address - 4u, 0x11223344u);
            m_w32(NULL, address, 0u); m_w32(NULL, address + 4u, 0xaabbccddu);
            m_w32(NULL, address + 8u, 0x55667788u);
            CHECK(a8_move_step(&c, thumb, a8_single_memory(0u,dbl,fp,4u,add,offsets[i])) == ARM_OK &&
                  m_r32(NULL, address) == lo && m_r32(NULL, address + 4u) == (dbl ? hi : 0xaabbccddu) &&
                  m_r32(NULL, address - 4u) == 0x11223344u && m_r32(NULL, address + 8u) == 0x55667788u,
                  "A8 VSTR bank/size/address T=%u D=%u reg=%u add=%u imm8=%u", thumb, dbl, fp, add, offsets[i]);
            m_w32(NULL, address, lo ^ 0x12345678u); m_w32(NULL, address + 4u, hi ^ 0x87654321u);
            CHECK(a8_move_step(&c, thumb, a8_single_memory(1u,dbl,fp,4u,add,offsets[i])) == ARM_OK &&
                  (dbl ? vfp_get_d(&c, fp) == ((uint64_t)(hi ^ 0x87654321u) << 32 | (lo ^ 0x12345678u)) :
                         vfp_get_s(&c, fp) == (lo ^ 0x12345678u)),
                  "A8 VLDR bank/size/address T=%u D=%u reg=%u add=%u imm8=%u", thumb, dbl, fp, add, offsets[i]);
            CHECK(c.r[4] == 0x4004u && c.r[15] == 0x108u && c.cycles == 2u && c.cpsr == flags &&
                  c.vfp_fpscr == fpscr && c.vfp_fpexc == ARM_FPEXC_EN,
                  "single FP memory transfer changed base, flags, controls or retirement");
        }
       }
      }
     }
    }
}

static void test_a8_vfp_single_memory_bases_and_conditions(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned half = 0; half < (thumb ? 2u : 1u); half++) {
      for (unsigned dbl = 0; dbl < 2u; dbl++) {
       for (unsigned load = 0; load < 2u; load++) {
        for (unsigned rn = 0; rn < 16u; rn++) {
         for (unsigned add = 0; add < 2u; add++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            for (unsigned n = 0; n < 15u; n++) c.r[n] = 0x2004u;
            c.r[15] += half * 2u;
            uint32_t pc = c.r[15], regs[16], flags = c.cpsr;
            memcpy(regs, c.r, sizeof regs);
            uint32_t base = rn == 15u ? ((pc + (thumb ? 4u : 8u)) & ~3u) : 0x2004u;
            uint32_t address = add ? base + 64u : base - 64u;
            m_w32(NULL, address, 0x01234567u); m_w32(NULL, address + 4u, 0x89abcdefu);
            if (dbl) vfp_set_d(&c, 31u, UINT64_C(0xff8000017f800001));
            else vfp_set_s(&c, 31u, 0x7f800001u);
            bool allowed = !(thumb && !load && rn == 15u);
            CHECK(a8_move_step(&c, thumb, a8_single_memory(load,dbl,31u,rn,add,16u)) ==
                  (allowed ? ARM_OK : ARM_UNDEFINED), "A8 VLDR/VSTR base T=%u half=%u D=%u L=%u Rn=%u",
                  thumb, half, dbl, load, rn);
            if (allowed) regs[15] += 4u;
            CHECK(memcmp(regs, c.r, sizeof regs) == 0 && c.cpsr == flags,
                  "A8 VLDR/VSTR changed a core register or flags");
            CHECK((dbl ? vfp_get_d(&c, 31u) == (allowed && load ? UINT64_C(0x89abcdef01234567) :
                                                                               UINT64_C(0xff8000017f800001)) :
                         vfp_get_s(&c, 31u) == (allowed && load ? 0x01234567u : 0x7f800001u)) &&
                  m_r32(NULL, address) == (allowed && !load ? 0x7f800001u : 0x01234567u) &&
                  m_r32(NULL, address + 4u) == (allowed && !load && dbl ? 0xff800001u : 0x89abcdefu),
                  "A8 VLDR/VSTR PC/offset/data semantics T=%u half=%u D=%u L=%u Rn=%u",
                  thumb, half, dbl, load, rn);
         }
        }
       }
      }
     }
     for (unsigned load = 0; load < 2u; load++) {
      for (unsigned dbl = 0; dbl < 2u; dbl++) {
       for (unsigned skip = 0; skip < 2u; skip++) {
        for (unsigned big = 0; big < 2u; big++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.r[4] = 0x2004u;
            if (big) c.cpsr |= ARM_CPSR_E; /* Shared memory accessors are LE-only. */
            if (skip) { c.cp15.cpacr = 0u; c.vfp_fpexc = 0u; }
            if (thumb) {
                m_w16(NULL, c.r[15], skip ? 0xbf0cu : 0xbf1cu); /* First ITE EQ/NE slot. */
                CHECK(arm_step(&c) == ARM_OK, "single memory IT setup");
            }
            uint32_t pc = c.r[15], flags = c.cpsr, insn = a8_single_memory(load,dbl,31u,4u,1u,0u);
            if (!thumb && skip) insn &= 0x0fffffffu;
            m_w32(NULL, 0x2004u, 0x12345678u); m_w32(NULL, 0x2008u, 0x9abcdef0u);
            bool completed = skip || !big;
            CHECK(a8_move_step(&c, thumb, insn) == (completed ? ARM_OK : ARM_UNDEFINED) &&
                  c.r[15] == (completed ? pc + 4u : pc) && c.r[4] == 0x2004u &&
                  c.cpsr == (completed && thumb ? (flags & ~0x0600fc00u) | 0x1800u : flags),
                  "A8 single FP memory skip/BE/IT T=%u L=%u D=%u skip=%u E=%u", thumb, load, dbl, skip, big);
            CHECK((dbl ? vfp_get_d(&c, 31u) == (!skip && !big && load ? UINT64_C(0x9abcdef012345678) : 0u) :
                         vfp_get_s(&c, 31u) == (!skip && !big && load ? 0x12345678u : 0u)) &&
                  m_r32(NULL, 0x2004u) == (!skip && !big && !load ? 0u : 0x12345678u) &&
                  m_r32(NULL, 0x2008u) == (!skip && !big && !load && dbl ? 0u : 0x9abcdef0u),
                  "skipped/refused single FP memory transfer touched data");
        }
       }
      }
     }
    }
}

static void test_a8_vfp_single_memory_permissions(void) {
    const unsigned permissions[] = {0u,1u,3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned load = 0; load < 2u; load++) {
      for (unsigned dbl = 0; dbl < 2u; dbl++) {
       for (unsigned user = 0; user < 2u; user++) {
        for (unsigned enabled = 0; enabled < 2u; enabled++) {
         for (unsigned a = 0; a < 3u; a++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[a] * 0x00500000u;
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            c.r[4] = 0x2005u; /* Valid access reaches an alignment abort; denied FP must fault first. */
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            bool allowed = enabled && (permissions[a] == 3u || (permissions[a] == 1u && !user));
            CHECK(a8_move_step(&c, thumb, a8_single_memory(load,dbl,31u,4u,1u,0u)) == ARM_OK &&
                  c.r[15] == (allowed ? ARM_VEC_DATA_ABORT : ARM_VEC_UNDEFINED) &&
                  c.r[14] == (allowed ? 0x108u : thumb ? 0x102u : 0x104u) &&
                  c.spsr[allowed ? ARM_BANK_ABT : ARM_BANK_UND] == flags &&
                  c.r[4] == 0x2005u && vfp_get_d(&c, 31u) == 0u && vfp_get_s(&c, 31u) == 0u &&
                  c.vfp_fpscr == fpscr && c.vfp_fpexc == (enabled ? ARM_FPEXC_EN : 0u),
                  "A8 single memory access priority T=%u L=%u D=%u U=%u EN=%u acc=%u",
                  thumb, load, dbl, user, enabled, permissions[a]);
         }
        }
       }
      }
     }
    }
}

static void test_a8_vfp_single_memory_decode_boundaries(void) {
    const uint32_t neighbors[] = {
        0xfdc4fb00u, 0xfdd4fb00u, /* LDC2/STC2 are not Thumb VFP forms. */
        0xedc4f900u, 0xedd4fc00u, /* Other coprocessors. */
        0xedf4fb02u, 0xede4fb02u, /* P=U=W=1 is undefined. */
        0xecf4fb04u, 0xed64fb04u  /* Multiple lists extending past D31. */
    };
    for (unsigned n = 0; n < sizeof neighbors / sizeof neighbors[0]; n++) {
        arm_cpu_t c;
        a8_move_reset(&c, 1u);
        c.r[4] = 0x2004u;
        uint32_t flags = c.cpsr;
        m_w32(NULL, 0x2004u, 0x12345678u);
        CHECK(a8_move_step(&c, 1u, neighbors[n]) == ARM_UNDEFINED && c.r[15] == 0x100u &&
              c.r[4] == 0x2004u && c.cpsr == flags && vfp_get_d(&c, 31u) == 0u &&
              m_r32(NULL, 0x2004u) == 0x12345678u, "single FP memory route aliased neighbor %u", n);
    }
    const arm_arch_t profiles[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned p = 0; p < 2u; p++) {
     for (unsigned load = 0; load < 2u; load++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset legacy");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        c.r[4] = 0x2004u;
        CHECK(a8_move_step(&c, 0u, a8_single_memory(load,1u,31u,4u,1u,0u)) == ARM_UNDEFINED &&
              c.r[15] == 0u && c.r[4] == 0x2004u, "upper single FP transfer enabled on legacy profile");
     }
    }
}

/* PUW: 010 = IA, 011 = IA!, 101 = DB!. The latter two also encode VPOP/VPUSH. */
static uint32_t a8_multiple_memory(unsigned form, unsigned load, unsigned dbl,
                                    unsigned first, unsigned rn, unsigned words) {
    return VFP_LS(form >> 2,(form >> 1) & 1u,dbl ? first >> 4 : first & 1u,form & 1u,
                  load,rn,dbl ? first & 15u : first >> 1,dbl,words);
}

static void test_a8_vfp_multiple_memory_register_lists(void) {
    const unsigned forms[] = {2u,3u,5u}, counts[] = {1u,2u,4u,16u,32u};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned dbl = 0; dbl < 2u; dbl++) {
      for (unsigned f = 0; f < 3u; f++) {
       for (unsigned first = 0; first < 32u; first++) {
        for (unsigned k = 0; k < sizeof counts / sizeof counts[0]; k++) {
            unsigned count = counts[k], words = count * (dbl ? 2u : 1u), rn = f ? 13u : 4u;
            if (first + count > 32u || (dbl && count > 16u)) continue;
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.r[rn] = 0x4004u;
            uint32_t address = forms[f] == 5u ? 0x4004u - words * 4u : 0x4004u;
            uint32_t final_base = forms[f] == 2u ? 0x4004u : forms[f] == 3u ? 0x4004u + words * 4u : address;
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            for (unsigned r = 0; r < 32u; r++) {
                if (dbl) vfp_set_d(&c, r, ((uint64_t)(0xff800000u + r) << 32) | (0x7f800001u + r));
                else vfp_set_s(&c, r, 0x7f800001u + r);
            }
            m_w32(NULL, address - 4u, 0xdeadbeefu); m_w32(NULL, address + words * 4u, 0xaabbccddu);
            CHECK(a8_move_step(&c, thumb, a8_multiple_memory(forms[f],0u,dbl,first,rn,words)) == ARM_OK &&
                  c.r[rn] == final_base, "A8 VSTM list T=%u D=%u form=%u first=%u count=%u",
                  thumb, dbl, forms[f], first, count);
            for (unsigned r = 0; r < count; r++) {
                uint32_t at = address + r * (dbl ? 8u : 4u);
                CHECK(m_r32(NULL, at) == 0x7f800001u + first + r &&
                      (!dbl || m_r32(NULL, at + 4u) == 0xff800000u + first + r), "VSTM stored wrong register order");
                m_w32(NULL, at, 0x12340000u + first + r);
                if (dbl) m_w32(NULL, at + 4u, 0x56780000u + first + r);
            }
            c.r[rn] = 0x4004u;
            CHECK(a8_move_step(&c, thumb, a8_multiple_memory(forms[f],1u,dbl,first,rn,words)) == ARM_OK &&
                  c.r[rn] == final_base && c.r[15] == 0x108u && c.cycles == 2u && c.cpsr == flags &&
                  c.vfp_fpscr == fpscr && c.vfp_fpexc == ARM_FPEXC_EN, "VLDM lost base/controls/retirement");
            for (unsigned r = 0; r < 32u; r++) {
                bool changed = r >= first && r < first + count;
                uint32_t lo = (changed ? 0x12340000u : 0x7f800001u) + r;
                uint32_t hi = (changed ? 0x56780000u : 0xff800000u) + r;
                CHECK(dbl ? vfp_get_d(&c, r) == ((uint64_t)hi << 32 | lo) : vfp_get_s(&c, r) == lo,
                      "VLDM changed wrong register T=%u D=%u first=%u count=%u r=%u", thumb, dbl, first, count, r);
            }
            CHECK(m_r32(NULL, address - 4u) == 0xdeadbeefu && m_r32(NULL, address + words * 4u) == 0xaabbccddu,
                  "multiple transfer extended outside its memory list");
        }
       }
      }
     }
    }
}

static void test_a8_vfp_multiple_memory_rejections_and_pc(void) {
    const unsigned forms[] = {0u,1u,2u,3u,5u,7u};
    static const struct { unsigned dbl, first, words; } lists[] = {
        {0u,0u,0u}, {0u,31u,2u}, {0u,0u,33u}, {1u,0u,0u}, {1u,0u,1u},
        {1u,0u,34u}, {1u,31u,4u}, {1u,16u,3u}, {1u,15u,5u}, /* Invalid lists. */
        {0u,31u,1u}, {1u,31u,2u}, {1u,0u,33u}, {1u,15u,3u} /* Valid boundary lists. */
    };
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned load = 0; load < 2u; load++) {
      for (unsigned f = 0; f < sizeof forms / sizeof forms[0]; f++) {
       for (unsigned n = 0; n < sizeof lists / sizeof lists[0]; n++) {
        for (unsigned use_pc = 0; use_pc < 2u; use_pc++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.r[4] = 0x2004u;
            uint32_t flags = c.cpsr;
            bool valid = n >= 9u && (forms[f] == 2u || forms[f] == 3u || forms[f] == 5u) &&
                         (!use_pc || (!thumb && forms[f] == 2u));
            uint32_t insn = a8_multiple_memory(forms[f],load,lists[n].dbl,lists[n].first,use_pc ? 15u : 4u,lists[n].words);
            /* PUW=000 with D=1 is the core-pair space; test its existing
             * checked decoder separately instead of assigning it a list. */
            if (forms[f] == 0u && (insn & (1u << 22))) continue;
            CHECK(a8_move_step(&c, thumb, insn) == (valid ? ARM_OK : ARM_UNDEFINED) &&
                  c.r[15] == (valid ? 0x104u : 0x100u) && c.cpsr == flags,
                  "A8 multiple legality T=%u L=%u form=%u list=%u PC=%u", thumb, load, forms[f], n, use_pc);
            if (!valid) CHECK(c.r[4] == 0x2004u && vfp_get_d(&c, 31u) == 0u && vfp_get_s(&c, 31u) == 0u,
                              "invalid multiple transfer changed registers");
        }
       }
      }
     }
    }
}

static void test_a8_vfp_multiple_access_and_skip(void) {
    /* Allowed FP reaches a misalignment fault. CPACR/EN denial must take
     * Undefined first, while a false condition suppresses either fault. */
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned load = 0; load < 2u; load++) {
      for (unsigned dbl = 0; dbl < 2u; dbl++) {
       for (unsigned test = 0; test < 6u; test++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        if (test == 1u || test == 5u) c.cp15.cpacr = 0u;
        if (test == 2u || test == 5u) c.vfp_fpexc = 0u;
        if (test == 3u) { c.cp15.cpacr = 0x00500000u; c.cpsr = (c.cpsr & ~31u) | ARM_MODE_USR; }
        if (test >= 4u) c.cpsr |= ARM_CPSR_E;
        c.r[4] = 0x2005u;
        if (thumb) {
            m_w16(NULL, 0x100u, test == 5u ? 0xbf0cu : 0xbf1cu);
            CHECK(arm_step(&c) == ARM_OK, "multiple FP IT setup");
        }
        uint32_t pc = c.r[15], flags = c.cpsr;
        uint32_t insn = a8_multiple_memory(3u,load,dbl,31u,4u,dbl ? 2u : 1u);
        if (!thumb && test == 5u) insn &= 0x0fffffffu;
        CHECK(a8_move_step(&c, thumb, insn) == (test == 4u ? ARM_UNDEFINED : ARM_OK) &&
              c.r[4] == 0x2005u && vfp_get_d(&c, 31u) == 0u && vfp_get_s(&c, 31u) == 0u,
              "multiple FP access/refusal disposition T=%u L=%u D=%u case=%u", thumb, load, dbl, test);
        if (test < 4u) CHECK(c.r[15] == (test ? ARM_VEC_UNDEFINED : ARM_VEC_DATA_ABORT) &&
                            c.r[14] == pc + (test ? thumb ? 2u : 4u : 8u) &&
                            c.spsr[test ? ARM_BANK_UND : ARM_BANK_ABT] == flags,
                            "multiple FP access priority/link/IT");
        else CHECK(c.r[15] == pc + (test == 5u ? 4u : 0u) &&
                   c.cpsr == (test == 5u && thumb ? (flags & ~0x0600fc00u) | 0x1800u : flags),
                   "multiple FP skip/BE refusal lost PC or IT");
       }
      }
     }
     arm_cpu_t c;
     a8_move_reset(&c, thumb);
     c.vfp_fpexc = 0u; c.cp15.cpacr = 0u;
     CHECK(a8_move_step(&c, thumb, a8_multiple_memory(3u,1u,1u,31u,4u,4u)) == ARM_UNDEFINED &&
           c.r[15] == 0x100u, "invalid multiple list was reclassified as an enable fault");
    }
}

/* The odd-word legacy format reserves address space but A8's VLDM/VSTM
 * pseudocode transfers only complete D registers. The gap is not memory IO. */
static void test_a8_vfp_multiple_odd_word_gap(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned load = 0; load < 2u; load++) {
      for (unsigned db = 0; db < 2u; db++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        m_w32(NULL, 0x4000u, 0x6001u);
        m_w32(NULL, 0x6000u, 0x803eu); m_w32(NULL, 0x6004u, 0xa03eu); m_w32(NULL, 0x6008u, 0u);
        c.r[4] = db ? 0x2004u : 0x1ff8u;
        uint32_t insn = a8_multiple_memory(db ? 5u : 3u,load,1u,15u,4u,3u);
        if (thumb) { m_w16(NULL, 0x8100u, (uint16_t)(insn >> 16)); m_w16(NULL, 0x8102u, (uint16_t)insn); }
        else m_w32(NULL, 0x8100u, insn);
        m_w32(NULL, 0xaff8u, 0x12345678u); m_w32(NULL, 0xaffcu, 0x9abcdef0u);
        vfp_set_d(&c, 15u, UINT64_C(0xaabbccdd11223344));
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x104u && c.r[4] == (db ? 0x1ff8u : 0x2004u) &&
              c.cpsr == flags && vfp_get_d(&c, 15u) == (load ? UINT64_C(0x9abcdef012345678) :
                                                                             UINT64_C(0xaabbccdd11223344)) &&
              m_r32(NULL, 0xaff8u) == (load ? 0x12345678u : 0x11223344u) &&
              m_r32(NULL, 0xaffcu) == (load ? 0x9abcdef0u : 0xaabbccddu),
              "A8 odd-imm8 transfer touched unmapped trailing word or lost writeback T=%u L=%u DB=%u", thumb, load, db);
      }
     }
    }
}

/*
 * d0-d15 are not a second bank, they are a second NAME for s0-s31. Writing the
 * two halves of d3 through s6 and s7 and reading it back as a double is the
 * cheapest test that says the aliasing and the word order are both right; get
 * the word order backwards and every double the guest ever loads is garbage
 * that still looks like a plausible float.
 */
static void test_s_d_aliasing(void) {
    arm_cpu_t c; vfp_reset(&c);

    vfp_set_s(&c, 6, 0x12345678u);       /* low  word of d3 */
    vfp_set_s(&c, 7, 0x9abcdef0u);       /* high word of d3 */
    CHECK(vfp_get_d(&c, 3) == 0x9abcdef012345678ull,
          "d3 = 0x%016llx", (unsigned long long)vfp_get_d(&c, 3));

    vfp_set_d(&c, 0, d2u(1.0));
    CHECK(vfp_get_s(&c, 0) == 0x00000000u, "s0 = 0x%08x", vfp_get_s(&c, 0));
    CHECK(vfp_get_s(&c, 1) == 0x3ff00000u, "s1 = 0x%08x", vfp_get_s(&c, 1));

    /* The aliasing must be complete: d15 is s30/s31 and there is nothing
     * beyond it, so vfp_set_d(16) must wrap onto d0 rather than run off. */
    vfp_set_d(&c, 15, 0xaaaaaaaabbbbbbbbull);
    CHECK(vfp_get_s(&c, 30) == 0xbbbbbbbbu, "s30 = 0x%08x", vfp_get_s(&c, 30));
    CHECK(vfp_get_s(&c, 31) == 0xaaaaaaaau, "s31 = 0x%08x", vfp_get_s(&c, 31));
}

/* ==================================================== load and store ====== */

/*
 * The instruction the whole milestone turns on. _vfp_switch does
 * "VLDMIA r1!, {s0-s31}" (0xecb10a20) to restore a thread's register file, so
 * this exact word must move 128 bytes and advance r1 by 128.
 */
static void test_vldmia_writeback_the_vfp_switch_form(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { VFP_LS(0,1,0,1,1, 1, 0, 0, 32) };
    CHECK(prog[0] == 0xecb10a20u, "encoding 0x%08x", prog[0]);

    for (unsigned i = 0; i < 32; i++) m_w32(NULL, 0x1000u + i * 4u, 0xc0de0000u + i);
    c.r[1] = 0x1000;
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "VLDMIA trapped");
    for (unsigned i = 0; i < 32; i++)
        CHECK(vfp_get_s(&c, i) == 0xc0de0000u + i, "s%u = 0x%08x", i, vfp_get_s(&c, i));
    CHECK(c.r[1] == 0x1080u, "r1 = 0x%08x (want 0x1080)", c.r[1]);
}

/* And the other half of a context switch: VSTMIA writes the same 32 words
 * back, so a store-then-load round trip must be the identity. */
static void test_vstmia_vldmia_round_trip(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VFP_LS(0,1,0,1,0, 1, 0, 0, 32),      /* VSTMIA r1!, {s0-s31} */
        VFP_LS(0,1,0,1,1, 2, 0, 0, 32),      /* VLDMIA r2!, {s0-s31} */
    };
    for (unsigned i = 0; i < 32; i++) vfp_set_s(&c, i, 0xfeed0000u + i * 7u);
    c.r[1] = 0x2000; c.r[2] = 0x2000;
    load(prog, 2);
    CHECK(arm_step(&c) == ARM_OK, "VSTMIA trapped");
    for (unsigned i = 0; i < 32; i++) vfp_set_s(&c, i, 0);
    CHECK(arm_step(&c) == ARM_OK, "VLDMIA trapped");
    for (unsigned i = 0; i < 32; i++)
        CHECK(vfp_get_s(&c, i) == 0xfeed0000u + i * 7u, "s%u = 0x%08x", i, vfp_get_s(&c, i));
    CHECK(c.r[1] == 0x2080u && c.r[2] == 0x2080u, "writeback r1=%08x r2=%08x", c.r[1], c.r[2]);
}

/*
 * A doubleword list must lay out identically to the single list that aliases
 * it: VSTM {d0-d1} and VSTM {s0-s3} write the same four words in the same
 * order. If the word order inside a double were wrong this is where it shows.
 */
static void test_double_list_matches_single_list(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VFP_LS(0,1,0,0,0, 1, 0, 1, 4),       /* VSTMIA r1, {d0-d1}  */
        VFP_LS(0,1,0,0,0, 2, 0, 0, 4),       /* VSTMIA r2, {s0-s3}  */
    };
    for (unsigned i = 0; i < 4; i++) vfp_set_s(&c, i, 0x11110000u * (i + 1u));
    c.r[1] = 0x3000; c.r[2] = 0x3100;
    CHECK(run(&c, prog, 2, 2) == ARM_OK, "VSTM trapped");
    for (unsigned i = 0; i < 4; i++)
        CHECK(m_r32(NULL, 0x3000u + i*4u) == m_r32(NULL, 0x3100u + i*4u),
              "word %u: d-form 0x%08x vs s-form 0x%08x",
              i, m_r32(NULL, 0x3000u + i*4u), m_r32(NULL, 0x3100u + i*4u));
    CHECK(c.r[1] == 0x3000u, "no-writeback form modified r1");
}

/* VPUSH/VPOP are the DB-writeback and IA-writeback forms with Rn == sp. */
static void test_vpush_vpop(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VFP_LS(1,0,0,1,0, 13, 0, 1, 4),      /* VPUSH {d0-d1} */
        VFP_LS(0,1,0,1,1, 13, 2, 1, 4),      /* VPOP  {d2-d3} */
    };
    CHECK(prog[0] == 0xed2d0b04u, "VPUSH encoding 0x%08x", prog[0]);
    CHECK(prog[1] == 0xecbd2b04u, "VPOP encoding 0x%08x", prog[1]);
    vfp_set_d(&c, 0, 0x0123456789abcdefull);
    vfp_set_d(&c, 1, 0xfedcba9876543210ull);
    c.r[13] = 0x4000;
    load(prog, 2);
    CHECK(arm_step(&c) == ARM_OK, "VPUSH trapped");
    CHECK(c.r[13] == 0x3ff0u, "sp after VPUSH = 0x%08x", c.r[13]);
    CHECK(arm_step(&c) == ARM_OK, "VPOP trapped");
    CHECK(c.r[13] == 0x4000u, "sp after VPOP = 0x%08x", c.r[13]);
    CHECK(vfp_get_d(&c, 2) == 0x0123456789abcdefull, "d2 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 2));
    CHECK(vfp_get_d(&c, 3) == 0xfedcba9876543210ull, "d3 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 3));
}

static void test_vldr_vstr(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VFP_LS(1,1,0,0,1, 1, 0, 0, 2),       /* VLDR s0, [r1,#8]  */
        VFP_LS(1,0,0,0,1, 1, 0, 1, 2),       /* VLDR d0, [r1,#-8] */
        VFP_LS(1,1,0,0,0, 2, 0, 1, 0),       /* VSTR d0, [r2]     */
    };
    CHECK(prog[0] == 0xed910a02u, "VLDR encoding 0x%08x", prog[0]);
    m_w32(NULL, 0x5008, 0xcafebabeu);
    m_w32(NULL, 0x4ff8, 0x11112222u);
    m_w32(NULL, 0x4ffc, 0x33334444u);
    c.r[1] = 0x5000; c.r[2] = 0x6000;
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "VLDR/VSTR trapped");
    /* s0 is the low half of d0, so the second load overwrote it. */
    CHECK(vfp_get_d(&c, 0) == 0x3333444411112222ull, "d0 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 0));
    CHECK(m_r32(NULL, 0x6000) == 0x11112222u, "stored low  0x%08x", m_r32(NULL, 0x6000));
    CHECK(m_r32(NULL, 0x6004) == 0x33334444u, "stored high 0x%08x", m_r32(NULL, 0x6004));
    (void)prog;
}

/* A list running past s31 or d15 names registers this part does not have. */
static void test_overlong_lists_trap(void) {
    arm_cpu_t c;
    uint32_t over_s[] = { VFP_LS(0,1,1,0,1, 1, 0, 0, 32) };  /* s1..s32  */
    uint32_t over_d[] = { VFP_LS(0,1,0,0,1, 1, 15, 1, 4) };  /* d15..d16 */
    uint32_t empty[]  = { VFP_LS(0,1,0,0,1, 1, 0, 0, 0) };   /* empty list */
    vfp_reset(&c); CHECK(run(&c, over_s, 1, 1) == ARM_UNDEFINED, "s-list overrun ran");
    vfp_reset(&c); CHECK(run(&c, over_d, 1, 1) == ARM_UNDEFINED, "d-list overrun ran");
    vfp_reset(&c); CHECK(run(&c, empty,  1, 1) == ARM_UNDEFINED, "empty list ran");
}

/* ================================================ system registers ======== */

static void test_vmrs_vmsr(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VMRS(0, 0),                          /* VMRS r0, FPSID */
        VMSR(1, 2),                          /* VMSR FPSCR, r2 */
        VMRS(3, 1),                          /* VMRS r3, FPSCR */
        VMRS(4, 8),                          /* VMRS r4, FPEXC */
    };
    c.r[2] = 0xffffffffu;
    CHECK(run(&c, prog, 4, 4) == ARM_OK, "VMRS/VMSR trapped");
    CHECK(c.r[0] == ARM1176_FPSID, "FPSID = 0x%08x", c.r[0]);
    /* Reserved FPSCR bits read as zero on the ARM1176, so the write is masked
     * rather than stored verbatim. */
    CHECK(c.r[3] == ARM_FPSCR_WMASK, "FPSCR readback = 0x%08x", c.r[3]);
    CHECK(c.r[4] == ARM_FPEXC_EN, "FPEXC = 0x%08x", c.r[4]);
}

static void test_fpsid_is_read_only(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { VMSR(0, 0) };        /* VMSR FPSID, r0 */
    CHECK(run(&c, prog, 1, 1) == ARM_UNDEFINED, "VMSR FPSID was accepted");
}

static void test_system_register_privilege(void) {
    arm_cpu_t c;
    uint32_t read_fpsid[]  = { VMRS(0, 0) };
    uint32_t write_fpscr[] = { VMSR(1, 1), VMRS(2, 1) };
    uint32_t read_fpexc[]  = { VMRS(0, 8) };
    uint32_t write_fpexc[] = { VMSR(8, 0) };

    /* With VFP enabled, user mode may access FPSID and FPSCR. */
    vfp_reset(&c);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
    CHECK(run(&c, read_fpsid, 1, 1) == ARM_OK,
          "user VMRS FPSID trapped while VFP enabled");
    CHECK(c.r[0] == ARM1176_FPSID, "user FPSID = 0x%08x", c.r[0]);

    vfp_reset(&c);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
    c.r[1] = 0xffffffffu;
    CHECK(run(&c, write_fpscr, 2, 2) == ARM_OK,
          "user FPSCR read/write trapped while VFP enabled");
    CHECK(c.r[2] == ARM_FPSCR_WMASK, "user FPSCR = 0x%08x", c.r[2]);

    /* FPEXC is privileged whether VFP is enabled or disabled. */
    vfp_reset(&c);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
    c.r[0] = 0x11223344u;
    CHECK(run(&c, read_fpexc, 1, 1) == ARM_OK &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND &&
          c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == 4u,
          "enabled user VMRS FPEXC did not enter guest Undefined");
    CHECK(c.r[0] == 0x11223344u && c.vfp_fpexc == ARM_FPEXC_EN,
          "denied FPEXC read changed state: r0=%08x FPEXC=%08x",
          c.r[0], c.vfp_fpexc);

    vfp_reset(&c);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
    c.r[0] = 0u;
    CHECK(run(&c, write_fpexc, 1, 1) == ARM_OK &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND &&
          c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == 4u,
          "enabled user VMSR FPEXC did not enter guest Undefined");
    CHECK(c.vfp_fpexc == ARM_FPEXC_EN,
          "denied FPEXC write changed FPEXC to %08x", c.vfp_fpexc);

    /* When disabled, FPSID also becomes privileged. These denied operations
     * take the guest's Undefined vector and must not expose or mutate state. */
    vfp_reset(&c);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
    c.vfp_fpexc = 0u;
    c.r[0] = 0x55667788u;
    CHECK(run(&c, read_fpsid, 1, 1) == ARM_OK &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND,
          "disabled user FPSID did not take the Undefined vector");
    CHECK(c.r[0] == 0x55667788u,
          "disabled user FPSID exposed 0x%08x", c.r[0]);

    vfp_reset(&c);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
    c.vfp_fpexc = 0u;
    c.r[0] = ARM_FPEXC_EN;
    CHECK(run(&c, write_fpexc, 1, 1) == ARM_OK &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND,
          "disabled user FPEXC write did not take the Undefined vector");
    CHECK(c.vfp_fpexc == 0u,
          "disabled user write enabled FPEXC: %08x", c.vfp_fpexc);
}

/*
 * The lazy-enable asymmetry, which is not a quirk but the mechanism XNU relies
 * on: with FPEXC.EN clear, privileged code can still read FPEXC and FPSID
 * (that is how _get_vfp_enabled asks whether VFP is on) and everything else
 * is UNDEFINED.
 * Undefined here means the guest is VECTORED, not halted — arm_step routes it
 * through undefined_instruction — so a passing test sees ARM_OK with PC at the
 * Undefined vector.
 */
static void test_fpexc_en_gates_the_other_registers(void) {
    arm_cpu_t c;
    uint32_t read_fpexc[] = { VMRS(0, 8) };
    uint32_t read_fpsid[] = { VMRS(0, 0) };
    uint32_t read_fpscr[] = { VMRS(0, 1) };
    uint32_t an_add[]     = { DP_S(0,1,1,0, 0,0,0) };
    uint32_t dword_move[] = { 0xee274b10u };

    vfp_reset(&c); c.vfp_fpexc = 0;
    CHECK(run(&c, read_fpexc, 1, 1) == ARM_OK, "VMRS FPEXC refused while disabled");
    CHECK(c.r[0] == 0u, "FPEXC read back 0x%08x", c.r[0]);
    /* PC after a retired instruction at 0 is 4, which is also the Undefined
     * vector address, so the MODE is what distinguishes "ran" from
     * "vectored". */
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS,
          "VMRS FPEXC vectored: mode = 0x%02x", c.cpsr & 0x1fu);

    vfp_reset(&c); c.vfp_fpexc = 0;
    CHECK(run(&c, read_fpsid, 1, 1) == ARM_OK, "VMRS FPSID refused while disabled");
    CHECK(c.r[0] == ARM1176_FPSID, "FPSID read back 0x%08x", c.r[0]);

    vfp_reset(&c); c.vfp_fpexc = 0;
    CHECK(run(&c, read_fpscr, 1, 1) == ARM_OK, "VMRS FPSCR should vector, not halt");
    CHECK(c.r[15] == ARM_VEC_UNDEFINED, "PC = 0x%08x, want the Undefined vector", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND, "mode = 0x%02x", c.cpsr & 0x1fu);

    vfp_reset(&c); c.vfp_fpexc = 0;
    CHECK(run(&c, an_add, 1, 1) == ARM_OK, "VADD should vector, not halt");
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND, "mode = 0x%02x", c.cpsr & 0x1fu);

    vfp_reset(&c); c.vfp_fpexc = 0; c.r[4] = 0x11223344u;
    vfp_set_s(&c, 15, 0x55667788u);
    CHECK(run(&c, dword_move, 1, 1) == ARM_OK,
          "FMDHR should vector, not halt, while VFP is disabled");
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND,
          "FMDHR disabled mode = 0x%02x", c.cpsr & 0x1fu);
    CHECK(vfp_get_s(&c, 15) == 0x55667788u,
          "disabled FMDHR changed s15 to 0x%08x", vfp_get_s(&c, 15));

    /* CPACR withholding CP10/CP11 does the same, even with FPEXC.EN set. */
    vfp_reset(&c); c.cp15.cpacr = 0;
    CHECK(run(&c, read_fpsid, 1, 1) == ARM_OK, "CPACR denial should vector");
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND, "mode = 0x%02x", c.cpsr & 0x1fu);
}

/* ================================================== core transfers ======== */

static void test_vmov_core_registers(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VMOV_S_R(5, 0),                      /* VMOV s5, r0      */
        VMOV_R_S(1, 5),                      /* VMOV r1, s5      */
        0xec410b10u,                         /* VMOV d0, r0, r1  */
        0xec532b10u,                         /* VMOV r2, r3, d0  */
        0xec454a11u,                         /* VMOV s2, s3, r4, r5 */
        0xec576a11u,                         /* VMOV r6, r7, s2, s3 */
    };
    CHECK(VMOV_S_R(0,0) == 0xee000a10u, "VMOV s0,r0 = 0x%08x", VMOV_S_R(0,0));
    CHECK(VMOV_R_S(0,0) == 0xee100a10u, "VMOV r0,s0 = 0x%08x", VMOV_R_S(0,0));
    c.r[0] = 0xdeadbeefu;
    c.r[4] = 0x0badf00du; c.r[5] = 0xfeedface;
    CHECK(run(&c, prog, 6, 6) == ARM_OK, "a VMOV trapped");
    CHECK(vfp_get_s(&c, 5) == 0xdeadbeefu, "s5 = 0x%08x", vfp_get_s(&c, 5));
    CHECK(c.r[1] == 0xdeadbeefu, "r1 = 0x%08x", c.r[1]);
    CHECK(c.r[2] == 0xdeadbeefu && c.r[3] == 0xdeadbeefu,
          "64-bit move back r2=0x%08x r3=0x%08x", c.r[2], c.r[3]);
    CHECK(c.r[6] == 0x0badf00du && c.r[7] == 0xfeedfaceu,
          "s-pair move back r6=0x%08x r7=0x%08x", c.r[6], c.r[7]);
    /* The pair really landed in s2 and s3, i.e. in d1. */
    CHECK(vfp_get_d(&c, 1) == 0xfeedface0badf00dull, "d1 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 1));
}

/*
 * VFP11 names each half of d0-d15 through cp11. Modern disassemblers print
 * these as VMOV.32 lane transfers, but the legacy FMDLR/FMDHR/FMRDL/FMRDH
 * instructions are VFPv2 and are used by the original armv6 libm.
 */
static void test_vmov_double_register_words(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VMOV_DWORD_R(7, 1, 4),              /* FMDHR d7, r4 */
        VMOV_DWORD_R(7, 0, 5),              /* FMDLR d7, r5 */
        VMOV_R_DWORD(6, 7, 0),              /* FMRDL r6, d7 */
        VMOV_R_DWORD(7, 7, 1),              /* FMRDH r7, d7 */
        VMOV_DWORD_R(15, 1, 8),             /* d15 high word is s31 */
        VMOV_R_DWORD(9, 15, 1),
    };
    uint32_t cpsr, fpexc;

    CHECK(prog[0] == 0xee274b10u, "FMDHR d7,r4 = 0x%08x", prog[0]);
    CHECK(prog[1] == 0xee075b10u, "FMDLR d7,r5 = 0x%08x", prog[1]);
    CHECK(prog[2] == 0xee176b10u, "FMRDL r6,d7 = 0x%08x", prog[2]);
    CHECK(prog[3] == 0xee377b10u, "FMRDH r7,d7 = 0x%08x", prog[3]);

    vfp_set_d(&c, 7, 0xaabbccdd11223344ull);
    vfp_set_d(&c, 15, 0x8899aabb44556677ull);
    c.r[4] = 0xdeadbeefu;
    c.r[5] = 0xcafebabeu;
    c.r[8] = 0x01020304u;
    c.vfp_fpscr = ARM_FPSCR_WMASK;          /* raw moves ignore every mode */
    cpsr = c.cpsr;
    fpexc = c.vfp_fpexc;

    CHECK(run(&c, prog, sizeof prog / sizeof prog[0],
              (int)(sizeof prog / sizeof prog[0])) == ARM_OK,
          "a VFPv2 double-register word move trapped");
    CHECK(vfp_get_d(&c, 7) == 0xdeadbeefcafebabeull,
          "d7 = 0x%016llx", (unsigned long long)vfp_get_d(&c, 7));
    CHECK(vfp_get_s(&c, 14) == 0xcafebabeu &&
          vfp_get_s(&c, 15) == 0xdeadbeefu,
          "d7 aliases s14/s15 as %08x/%08x",
          vfp_get_s(&c, 14), vfp_get_s(&c, 15));
    CHECK(c.r[6] == 0xcafebabeu && c.r[7] == 0xdeadbeefu,
          "d7 readback r6/r7 = %08x/%08x", c.r[6], c.r[7]);
    CHECK(vfp_get_d(&c, 15) == 0x0102030444556677ull &&
          vfp_get_s(&c, 31) == 0x01020304u && c.r[9] == 0x01020304u,
          "d15/s31 boundary = %016llx/%08x/%08x",
          (unsigned long long)vfp_get_d(&c, 15), vfp_get_s(&c, 31), c.r[9]);
    CHECK(c.r[4] == 0xdeadbeefu && c.r[5] == 0xcafebabeu &&
          c.r[8] == 0x01020304u,
          "core-to-VFP moves modified their sources");
    CHECK(c.vfp_fpscr == ARM_FPSCR_WMASK && c.vfp_fpexc == fpexc &&
          c.cpsr == cpsr,
          "raw word moves modified FPSCR/FPEXC/CPSR: %08x/%08x/%08x",
          c.vfp_fpscr, c.vfp_fpexc, c.cpsr);

    /* Exhaust the complete VFP11 bank in both directions. This catches a
     * swapped lane bit, a d/s alias off-by-one, and an accidental d15 wrap
     * without relying on the handful of registers used by the real crash. */
    for (unsigned dn = 0; dn < 16u; dn++) {
        for (unsigned hi = 0; hi < 2u; hi++) {
            unsigned rt = (dn + hi) % 15u;          /* never select PC */
            unsigned sn = dn * 2u + hi;
            unsigned other = dn * 2u + (hi ^ 1u);
            uint32_t source = 0xa5000000u | (dn << 8) | hi;
            uint32_t other_before = 0x3c000000u | (dn << 8) | hi;
            uint32_t one[] = { VMOV_DWORD_R(dn, hi, rt) };

            vfp_reset(&c);
            c.r[rt] = source;
            vfp_set_s(&c, sn, 0x5a5a5a5au);
            vfp_set_s(&c, other, other_before);
            CHECK(run(&c, one, 1, 1) == ARM_OK,
                  "core-to-d%u[%u] trapped", dn, hi);
            CHECK(vfp_get_s(&c, sn) == source &&
                  vfp_get_s(&c, other) == other_before,
                  "d%u[%u] write = %08x, other half = %08x",
                  dn, hi, vfp_get_s(&c, sn), vfp_get_s(&c, other));

            one[0] = VMOV_R_DWORD(rt, dn, hi);
            vfp_reset(&c);
            vfp_set_s(&c, sn, source);
            vfp_set_s(&c, other, other_before);
            c.r[rt] = 0xccccccccu;
            CHECK(run(&c, one, 1, 1) == ARM_OK,
                  "d%u[%u]-to-core trapped", dn, hi);
            CHECK(c.r[rt] == source && vfp_get_s(&c, sn) == source &&
                  vfp_get_s(&c, other) == other_before,
                  "d%u[%u] read = %08x, source/other = %08x/%08x",
                  dn, hi, c.r[rt], vfp_get_s(&c, sn), vfp_get_s(&c, other));
        }
    }
}

/*
 * The exact six-instruction block reached in iPhone OS 3.1.3's armv6 libm
 * `_fmod` at shared-cache VA 0x33acca88. Run20 stopped on its first word after
 * mistaking the legacy VFP11 high-word transfer for a NEON lane move.
 *
 * Keep this as a sequence rather than six isolated decoder checks. In
 * particular, Apple's binary writes d6's high word twice before consuming the
 * complete aliased register in VADD.F64. Starting d6 at 2.0 makes a swapped
 * half decode observable: the architecturally correct sequence clears d6 to
 * +0.0 and leaves the VADD destination at 1.0, while a low-half write would
 * leave it at 2.0 and produce 3.0. The soft-float ABI result remains in r0:r1;
 * this d6 value is an internal temporary in Apple's block.
 */
static void test_ios313_libm_fmod_return_block(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        0xee274b10u,                         /* FMDHR d7, r4          */
        0xee070b10u,                         /* FMDLR d7, r0          */
        0xe0222002u,                         /* EOR   r2, r2, r2      */
        0xee262b10u,                         /* FMDHR d6, r2          */
        0xee262b10u,                         /* FMDHR d6, r2 (again)  */
        0xee366b07u,                         /* VADD.F64 d6, d6, d7   */
    };

    CHECK(prog[0] == VMOV_DWORD_R(7, 1, 4), "unexpected first _fmod word");
    CHECK(prog[1] == VMOV_DWORD_R(7, 0, 0), "unexpected second _fmod word");
    CHECK(prog[3] == VMOV_DWORD_R(6, 1, 2), "unexpected d6 high-word move");
    CHECK(prog[5] == DP_D(0,1,1,0, 6,6,7),
          "unexpected _fmod VADD encoding 0x%08x", prog[5]);

    c.r[0] = 0x00000000u;                   /* low word of 1.0 */
    c.r[2] = 0xdeadbeefu;                   /* must be cleared by EOR */
    c.r[4] = 0x3ff00000u;                   /* high word of 1.0 */
    vfp_set_d(&c, 6, d2u(2.0));             /* exposes a half-swap */

    CHECK(run(&c, prog, sizeof prog / sizeof prog[0],
              (int)(sizeof prog / sizeof prog[0])) == ARM_OK,
          "the exact iOS 3.1.3 _fmod return block trapped");
    CHECK(c.r[2] == 0u, "_fmod EOR left r2 = 0x%08x", c.r[2]);
    CHECK(vfp_get_d(&c, 7) == d2u(1.0),
          "_fmod assembled d7 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 7));
    CHECK(vfp_get_d(&c, 6) == d2u(1.0),
          "_fmod temporary d6 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 6));
    CHECK(c.r[0] == 0u && c.r[4] == 0x3ff00000u,
          "_fmod word transfers modified r0/r4: %08x/%08x", c.r[0], c.r[4]);
    CHECK(c.r[15] == sizeof prog,
          "_fmod block PC = 0x%08x, expected 0x%08x",
          c.r[15], (unsigned)sizeof prog);
    CHECK((c.vfp_fpscr & 0x9fu) == 0u,
          "exact _fmod block raised FPSCR flags 0x%08x", c.vfp_fpscr);

    /*
     * Also retain the original condition chain at 0x33acc900..0x33acc924.
     * Relocating that span to zero preserves every PC-relative branch: the
     * common block becomes offset 0x188. Exercise both routes that select it:
     * finite y with x == 0 (the run20 path), and y == infinity.
     */
    {
        uint32_t path[(0x188u / 4u) + (sizeof prog / sizeof prog[0])] = {0};
        struct {
            uint32_t r0, r2, r4, r5;
            uint64_t expected;
            int steps;
            const char *why;
        } cases[] = {
            { 0x00000000u, 0xffe00000u, 0x00000000u, 0x41efffffu,
              0x0000000000000000ull, 16, "x == 0 (run20)" },
            { 0x00000000u, 0x00000000u, 0x3ff00000u, 0x7ff00000u,
              0x3ff0000000000000ull, 13, "y == infinity" },
        };

        path[0x00u / 4u] = 0xe3550000u;      /* CMP   r5, #0          */
        path[0x04u / 4u] = 0x03520000u;      /* CMPEQ r2, #0          */
        path[0x08u / 4u] = 0x0a000066u;      /* BEQ   another return   */
        path[0x0cu / 4u] = 0xe1550006u;      /* CMP   r5, r6          */
        path[0x10u / 4u] = 0x03520000u;      /* CMPEQ r2, #0          */
        path[0x14u / 4u] = 0x8a00006du;      /* BHI   reduction path  */
        path[0x18u / 4u] = 0x0a00005au;      /* BEQ   common block    */
        path[0x1cu / 4u] = 0xe3540000u;      /* CMP   r4, #0          */
        path[0x20u / 4u] = 0x03500000u;      /* CMPEQ r0, #0          */
        path[0x24u / 4u] = 0x0a000057u;      /* BEQ   common block    */
        memcpy(&path[0x188u / 4u], prog, sizeof prog);

        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            vfp_reset(&c);
            c.r[0] = cases[i].r0;
            c.r[2] = cases[i].r2;
            c.r[4] = cases[i].r4;
            c.r[5] = cases[i].r5;
            c.r[6] = 0x7ff00000u;
            vfp_set_d(&c, 6, d2u(2.0));

            CHECK(run(&c, path, sizeof path / sizeof path[0],
                      cases[i].steps) == ARM_OK,
                  "_fmod %s condition path trapped", cases[i].why);
            CHECK(vfp_get_d(&c, 6) == cases[i].expected,
                  "_fmod %s temporary d6 = 0x%016llx",
                  cases[i].why, (unsigned long long)vfp_get_d(&c, 6));
            CHECK(c.r[15] == 0x1a0u,
                  "_fmod %s path ended at 0x%08x", cases[i].why, c.r[15]);
        }
    }
}

/* ================================================== arithmetic =========== */

static void set_f32(arm_cpu_t *c, unsigned n, float v) { vfp_set_s(c, n, f2u(v)); }
static float get_f32(const arm_cpu_t *c, unsigned n)   { return u2f(vfp_get_s(c, n)); }
static void set_f64(arm_cpu_t *c, unsigned n, double v){ vfp_set_d(c, n, d2u(v)); }
static double get_f64(const arm_cpu_t *c, unsigned n)  { return u2d(vfp_get_d(c, n)); }

static void test_single_precision_arithmetic(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        DP_S(0,1,1,0, 4,0,1),                /* VADD.F32 s4, s0, s1 */
        DP_S(0,1,1,1, 5,0,1),                /* VSUB.F32 s5, s0, s1 */
        DP_S(0,1,0,0, 6,0,1),                /* VMUL.F32 s6, s0, s1 */
        DP_S(1,0,0,0, 7,0,1),                /* VDIV.F32 s7, s0, s1 */
        UN_S(1,1, 8,0),                      /* VSQRT.F32 s8, s0    */
    };
    CHECK(prog[0] == 0xee302a20u, "VADD.F32 s4,s0,s1 = 0x%08x", prog[0]);
    CHECK(prog[4] == 0xeeb14ac0u, "VSQRT.F32 s8,s0 = 0x%08x", prog[4]);
    set_f32(&c, 0, 9.0f);
    set_f32(&c, 1, 2.0f);
    CHECK(run(&c, prog, 5, 5) == ARM_OK, "arithmetic trapped");
    CHECK(get_f32(&c, 4) == 11.0f, "VADD = %f", (double)get_f32(&c, 4));
    CHECK(get_f32(&c, 5) ==  7.0f, "VSUB = %f", (double)get_f32(&c, 5));
    CHECK(get_f32(&c, 6) == 18.0f, "VMUL = %f", (double)get_f32(&c, 6));
    CHECK(get_f32(&c, 7) ==  4.5f, "VDIV = %f", (double)get_f32(&c, 7));
    CHECK(get_f32(&c, 8) ==  3.0f, "VSQRT = %f", (double)get_f32(&c, 8));
    CHECK((c.vfp_fpscr & 0x9fu) == 0u, "clean arithmetic set FPSCR flags 0x%08x", c.vfp_fpscr);
}

static void test_double_precision_arithmetic(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        DP_D(0,1,1,0, 4,0,1),                /* VADD.F64 d4, d0, d1 */
        DP_D(1,0,0,0, 5,0,1),                /* VDIV.F64 d5, d0, d1 */
        UN_D(1,1, 6,0),                      /* VSQRT.F64 d6, d0    */
    };
    CHECK(prog[0] == 0xee304b01u, "VADD.F64 d4,d0,d1 = 0x%08x", prog[0]);
    set_f64(&c, 0, 2.0);
    set_f64(&c, 1, 4.0);
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "double arithmetic trapped");
    CHECK(get_f64(&c, 4) == 6.0,  "VADD.F64 = %f", get_f64(&c, 4));
    CHECK(get_f64(&c, 5) == 0.5,  "VDIV.F64 = %f", get_f64(&c, 5));
    CHECK(d2u(get_f64(&c, 6)) == d2u(1.4142135623730951),
          "VSQRT.F64 = %.17g", get_f64(&c, 6));
}

/*
 * VFPv2 short vectors are selected by FPSCR.LEN and advance within one of
 * four circular register banks. This is not an optimization hint: executing
 * one of these as a scalar silently leaves up to seven architectural results
 * unwritten. Voice Memos reaches the first exact instruction below when its
 * level meter opens, so keep the real word as well as the general bank tests.
 */
static void test_short_vector_arithmetic(void) {
    arm_cpu_t c;

    /* VMUL.F32 s20,s4,s0: Fn advances even though it is in bank zero, while
     * Fm in bank zero is a scalar broadcast. */
    {
        uint32_t voice[] = { 0xee22aa00u };
        CHECK(voice[0] == DP_S(0,1,0,0, 20,4,0),
              "Voice Memos VMUL encoding = 0x%08x", voice[0]);
        vfp_reset(&c);
        /* Captured RunFast mode: FZ, DN, four lanes, stride one. */
        c.vfp_fpscr = ARM_FPSCR_FZ | ARM_FPSCR_DN | (3u << 16);
        set_f32(&c, 0, 10.0f);
        for (unsigned i = 0; i < 4; i++) set_f32(&c, 4u + i, (float)(i + 1u));
        CHECK(run(&c, voice, 1, 1) == ARM_OK, "Voice Memos VMUL trapped");
        for (unsigned i = 0; i < 4; i++)
            CHECK(get_f32(&c, 20u + i) == (float)((i + 1u) * 10u),
                  "Voice lane %u = %f", i, (double)get_f32(&c, 20u + i));
    }

    /* ARM DDI 0274H example 2-1: all three vectors wrap independently inside
     * their original eight-register banks. */
    {
        static const unsigned DREG[] = { 11, 12, 13, 14, 15, 8 };
        static const unsigned NREG[] = { 22, 23, 16, 17, 18, 19 };
        static const unsigned MREG[] = { 31, 24, 25, 26, 27, 28 };
        uint32_t add[] = { DP_S(0,1,1,0, 11,22,31) };
        vfp_reset(&c);
        c.vfp_fpscr = 5u << 16;                  /* six lanes, stride one */
        for (unsigned i = 0; i < 6; i++) {
            set_f32(&c, NREG[i], (float)(i + 1u));
            set_f32(&c, MREG[i], (float)((i + 1u) * 10u));
        }
        CHECK(run(&c, add, 1, 1) == ARM_OK, "wrapping VADD trapped");
        for (unsigned i = 0; i < 6; i++)
            CHECK(get_f32(&c, DREG[i]) == (float)((i + 1u) * 11u),
                  "wrapped lane %u (s%u) = %f", i, DREG[i],
                  (double)get_f32(&c, DREG[i]));
    }

    /* STRIDE=b11 advances by two. Registers skipped by the destination vector
     * must remain untouched. */
    {
        uint32_t add[] = { DP_S(0,1,1,0, 8,16,24) };
        vfp_reset(&c);
        c.vfp_fpscr = (3u << 16) | (3u << 20);    /* four lanes, stride two */
        for (unsigned i = 0; i < 8; i++) set_f32(&c, 8u + i, -100.0f - (float)i);
        for (unsigned i = 0; i < 4; i++) {
            set_f32(&c, 16u + i * 2u, (float)(i + 1u));
            set_f32(&c, 24u + i * 2u, (float)((i + 1u) * 10u));
        }
        CHECK(run(&c, add, 1, 1) == ARM_OK, "stride-two VADD trapped");
        for (unsigned i = 0; i < 4; i++) {
            CHECK(get_f32(&c, 8u + i * 2u) == (float)((i + 1u) * 11u),
                  "stride-two lane %u = %f", i,
                  (double)get_f32(&c, 8u + i * 2u));
            CHECK(get_f32(&c, 9u + i * 2u) == -101.0f - (float)(i * 2u),
                  "stride-two clobbered s%u", 9u + i * 2u);
        }
    }

    /* Double vectors use four-register banks, not the single-precision bank
     * size inherited through the shared S/D storage. */
    {
        uint32_t add[] = { DP_D(0,1,1,0, 4,8,12) };
        vfp_reset(&c);
        c.vfp_fpscr = 3u << 16;
        for (unsigned i = 0; i < 4; i++) {
            set_f64(&c, 8u + i, (double)(i + 1u));
            set_f64(&c, 12u + i, (double)((i + 1u) * 10u));
        }
        CHECK(run(&c, add, 1, 1) == ARM_OK, "double short-vector VADD trapped");
        for (unsigned i = 0; i < 4; i++)
            CHECK(get_f64(&c, 4u + i) == (double)((i + 1u) * 11u),
                  "double lane %u = %f", i, get_f64(&c, 4u + i));
    }

    /* All lane inputs are architectural source values from before the vector
     * writes begin. Lane 1 therefore reads the old s8, not lane 0's result. */
    {
        uint32_t mul[] = { DP_S(0,1,0,0, 8,15,0) };
        vfp_reset(&c);
        c.vfp_fpscr = 1u << 16;                  /* two lanes */
        set_f32(&c, 0, 10.0f);
        set_f32(&c, 15, 3.0f);
        set_f32(&c, 8, 7.0f);
        CHECK(run(&c, mul, 1, 1) == ARM_OK, "overlapping vector VMUL trapped");
        CHECK(get_f32(&c, 8) == 30.0f, "overlap lane 0 = %f", (double)get_f32(&c, 8));
        CHECK(get_f32(&c, 9) == 70.0f, "overlap lane 1 = %f", (double)get_f32(&c, 9));
    }

    /* Exception flags accumulate across every iteration. */
    {
        uint32_t div[] = { DP_S(1,0,0,0, 8,16,24) };
        vfp_reset(&c);
        c.vfp_fpscr = 1u << 16;
        set_f32(&c, 16, 1.0f); vfp_set_s(&c, 24, 0u); /* division by zero */
        set_f32(&c, 17, 1.0f); set_f32(&c, 25, 3.0f); /* inexact */
        CHECK(run(&c, div, 1, 1) == ARM_OK, "vector VDIV trapped");
        CHECK((c.vfp_fpscr & (ARM_FPSCR_DZC | ARM_FPSCR_IXC)) ==
              (ARM_FPSCR_DZC | ARM_FPSCR_IXC),
              "vector exceptions = 0x%08x", c.vfp_fpscr);
    }

    /* A failure in a later lane must not leave earlier destination lanes
     * committed. This boundary result is deliberately refused by the existing
     * fail-closed FZ rule, so it also proves the vector write is atomic. */
    {
        uint32_t mul[] = { DP_S(0,1,0,0, 8,16,24) };
        vfp_reset(&c);
        c.vfp_fpscr = ARM_FPSCR_FZ | (1u << 16);
        set_f32(&c, 8, -11.0f); set_f32(&c, 9, -12.0f);
        set_f32(&c, 16, 2.0f); set_f32(&c, 24, 3.0f);
        vfp_set_s(&c, 17, 0x00800000u);          /* smallest normal */
        vfp_set_s(&c, 25, 0x3f7fffffu);          /* 1.0 - 2^-24 */
        CHECK(run(&c, mul, 1, 1) == ARM_UNDEFINED,
              "later-lane FZ ambiguity did not trap");
        CHECK(get_f32(&c, 8) == -11.0f, "failed vector committed lane 0");
        CHECK(get_f32(&c, 9) == -12.0f, "failed vector committed lane 1");
    }
}

static void test_short_vector_unary_and_scalar_only(void) {
    arm_cpu_t c;

    /* A two-operand source in bank zero is broadcast to the whole vector. */
    {
        uint32_t neg[] = { UN_S(1,0, 8,1) };       /* VNEG.F32 s8,s1 */
        vfp_reset(&c);
        c.vfp_fpscr = 2u << 16;                    /* three lanes */
        set_f32(&c, 1, 7.0f);
        CHECK(run(&c, neg, 1, 1) == ARM_OK, "vector VNEG trapped");
        for (unsigned i = 0; i < 3; i++)
            CHECK(get_f32(&c, 8u + i) == -7.0f, "VNEG lane %u = %f", i,
                  (double)get_f32(&c, 8u + i));
    }

    /* A destination in bank zero makes an otherwise vector-capable operation
     * scalar without clearing LEN. */
    {
        uint32_t add[] = { DP_S(0,1,1,0, 2,8,16) };
        vfp_reset(&c);
        c.vfp_fpscr = 3u << 16;
        set_f32(&c, 8, 2.0f); set_f32(&c, 16, 5.0f);
        set_f32(&c, 3, 123.0f);
        CHECK(run(&c, add, 1, 1) == ARM_OK, "bank-zero scalar VADD trapped");
        CHECK(get_f32(&c, 2) == 7.0f, "bank-zero VADD = %f", (double)get_f32(&c, 2));
        CHECK(get_f32(&c, 3) == 123.0f, "bank-zero VADD became a vector");
    }

    /* Comparisons and conversions are scalar-only by definition, even if
     * their register number is outside bank zero and LEN remains nonzero. */
    {
        uint32_t cmp[] = { UN_S(4,0, 16,17) };
        vfp_reset(&c);
        c.vfp_fpscr = 3u << 16;
        set_f32(&c, 16, 1.0f); set_f32(&c, 17, 2.0f);
        CHECK(run(&c, cmp, 1, 1) == ARM_OK, "VCMP refused with LEN set");
        CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == ARM_FPSCR_N,
              "VCMP with LEN set produced 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);
    }
    {
        uint32_t cvt[] = { UN_S(8,1, 16,0) };      /* VCVT.F32.S32 s16,s0 */
        vfp_reset(&c);
        c.vfp_fpscr = 3u << 16;
        vfp_set_s(&c, 0, 7u);
        set_f32(&c, 17, 123.0f);
        CHECK(run(&c, cvt, 1, 1) == ARM_OK, "VCVT refused with LEN set");
        CHECK(get_f32(&c, 16) == 7.0f, "VCVT with LEN set = %f", (double)get_f32(&c, 16));
        CHECK(get_f32(&c, 17) == 123.0f, "scalar-only VCVT wrote a second lane");
    }
}

static void test_invalid_short_vector_shapes_halt_without_writes(void) {
    arm_cpu_t c;
    uint32_t smul[] = { DP_S(0,1,0,0, 8,16,24) };
    uint32_t dmul[] = { DP_D(0,1,0,0, 4,8,12) };

    /* With vector length one, STRIDE=b11 is architecturally Unpredictable. */
    vfp_reset(&c); c.vfp_fpscr = 3u << 20; set_f32(&c, 8, 91.0f);
    CHECK(run(&c, smul, 1, 1) == ARM_UNDEFINED, "LEN=0/STRIDE=2 ran");
    CHECK(get_f32(&c, 8) == 91.0f, "invalid LEN=0 shape wrote s8");

    /* STRIDE encodings b01 and b10 are reserved. */
    vfp_reset(&c); c.vfp_fpscr = (1u << 16) | (1u << 20); set_f32(&c, 8, 92.0f);
    CHECK(run(&c, smul, 1, 1) == ARM_UNDEFINED, "reserved STRIDE ran");
    CHECK(get_f32(&c, 8) == 92.0f, "reserved STRIDE wrote s8");

    /* A five-lane stride-two single vector and a three-lane stride-two double
     * vector would each visit a register twice inside their circular bank. */
    vfp_reset(&c); c.vfp_fpscr = (4u << 16) | (3u << 20); set_f32(&c, 8, 93.0f);
    CHECK(run(&c, smul, 1, 1) == ARM_UNDEFINED, "repeating single vector ran");
    CHECK(get_f32(&c, 8) == 93.0f, "repeating single vector wrote s8");

    vfp_reset(&c); c.vfp_fpscr = (2u << 16) | (3u << 20); set_f64(&c, 4, 94.0);
    CHECK(run(&c, dmul, 1, 1) == ARM_UNDEFINED, "repeating double vector ran");
    CHECK(get_f64(&c, 4) == 94.0, "repeating double vector wrote d4");
}

/*
 * VMLA is a multiply, ROUNDED, then an add, ROUNDED. VFPv2 has no fused
 * multiply-add, so an implementation that let the compiler contract the two
 * would differ from hardware in the last bit. These operands are chosen so
 * that fused and unfused disagree: 1 + 2^-24*(1+2^-24) is exactly the
 * halfway-plus-epsilon case where the product's lost bits change the sum.
 */
static void test_vmla_is_not_fused(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { DP_S(0,0,0,0, 2,0,1) };   /* VMLA.F32 s2,s0,s1 */
    float a = 1.0f + 1.0f/16777216.0f;       /* 1 + 2^-24 is not representable; */
    volatile float product, sum;             /* the host rounds it to 1.0f      */
    CHECK(prog[0] == 0xee001a20u, "VMLA.F32 s2,s0,s1 = 0x%08x", prog[0]);

    set_f32(&c, 0, 0.0009765625f);           /* 2^-10 */
    set_f32(&c, 1, 0.0009765625f);           /* 2^-10 */
    set_f32(&c, 2, 1.0f);
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "VMLA trapped");
    product = 0.0009765625f * 0.0009765625f;
    sum     = 1.0f + product;
    CHECK(f2u(get_f32(&c, 2)) == f2u(sum), "VMLA = 0x%08x want 0x%08x",
          f2u(get_f32(&c, 2)), f2u(sum));
    (void)a;

    /* VMLS/VNMLA/VNMLS are the same product with sign flips applied. */
    vfp_reset(&c);
    {
        uint32_t p2[] = {
            DP_S(0,0,0,1, 2,0,1),                /* VMLS.F32  s2,s0,s1 */
            DP_S(0,0,1,0, 3,0,1),                /* VNMLS.F32 s3,s0,s1 */
            DP_S(0,0,1,1, 4,0,1),                /* VNMLA.F32 s4,s0,s1 */
        };
        set_f32(&c, 0, 3.0f); set_f32(&c, 1, 5.0f);
        set_f32(&c, 2, 100.0f); set_f32(&c, 3, 100.0f); set_f32(&c, 4, 100.0f);
        CHECK(run(&c, p2, 3, 3) == ARM_OK, "VMLS family trapped");
        CHECK(get_f32(&c, 2) ==   85.0f, "VMLS  = %f", (double)get_f32(&c, 2));
        CHECK(get_f32(&c, 3) ==  -85.0f, "VNMLS = %f", (double)get_f32(&c, 3));
        CHECK(get_f32(&c, 4) == -115.0f, "VNMLA = %f", (double)get_f32(&c, 4));
    }
}

static void test_vabs_vneg_vmov_are_sign_bit_operations(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        UN_S(0,0, 1,0),                      /* VMOV.F32 s1, s0 */
        UN_S(0,1, 2,0),                      /* VABS.F32 s2, s0 */
        UN_S(1,0, 3,0),                      /* VNEG.F32 s3, s0 */
    };
    CHECK(prog[0] == 0xeef00a40u, "VMOV.F32 s1,s0 = 0x%08x", prog[0]);
    CHECK(prog[1] == 0xeeb01ac0u, "VABS.F32 s2,s0 = 0x%08x", prog[1]);
    CHECK(prog[2] == 0xeef11a40u, "VNEG.F32 s3,s0 = 0x%08x", prog[2]);

    /* A signalling NaN. VABS and VNEG touch only the sign bit: the payload
     * survives and IOC is NOT raised, which is what distinguishes them from
     * "multiply by -1". */
    vfp_set_s(&c, 0, 0xff800001u);
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "sign-bit ops trapped");
    CHECK(vfp_get_s(&c, 1) == 0xff800001u, "VMOV = 0x%08x", vfp_get_s(&c, 1));
    CHECK(vfp_get_s(&c, 2) == 0x7f800001u, "VABS = 0x%08x", vfp_get_s(&c, 2));
    CHECK(vfp_get_s(&c, 3) == 0x7f800001u, "VNEG = 0x%08x", vfp_get_s(&c, 3));
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) == 0, "sign-bit op raised IOC");

    /* And -0.0 must stay -0.0 through VMOV and become +0.0 through VABS. */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x80000000u);
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "sign-bit ops trapped on -0");
    CHECK(vfp_get_s(&c, 1) == 0x80000000u, "VMOV -0 = 0x%08x", vfp_get_s(&c, 1));
    CHECK(vfp_get_s(&c, 2) == 0x00000000u, "VABS -0 = 0x%08x", vfp_get_s(&c, 2));
    CHECK(vfp_get_s(&c, 3) == 0x00000000u, "VNEG -0 = 0x%08x", vfp_get_s(&c, 3));
}

/* ============================================ NaN, infinity, flags ======== */

static void test_infinity_and_nan(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t div[] = { DP_S(1,0,0,0, 2,0,1) };    /* VDIV.F32 s2,s0,s1 */

    /* 1.0 / 0.0 -> +inf, DZC. */
    set_f32(&c, 0, 1.0f); vfp_set_s(&c, 1, 0u);
    CHECK(run(&c, div, 1, 1) == ARM_OK, "VDIV trapped");
    CHECK(vfp_get_s(&c, 2) == 0x7f800000u, "1/0 = 0x%08x", vfp_get_s(&c, 2));
    CHECK((c.vfp_fpscr & ARM_FPSCR_DZC) != 0, "DZC not set, FPSCR = 0x%08x", c.vfp_fpscr);
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) == 0, "IOC set for 1/0");

    /* -1.0 / 0.0 -> -inf. */
    vfp_reset(&c);
    set_f32(&c, 0, -1.0f); vfp_set_s(&c, 1, 0u);
    run(&c, div, 1, 1);
    CHECK(vfp_get_s(&c, 2) == 0xff800000u, "-1/0 = 0x%08x", vfp_get_s(&c, 2));

    /* 0.0 / 0.0 -> a quiet NaN, IOC (not DZC). */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0u); vfp_set_s(&c, 1, 0u);
    run(&c, div, 1, 1);
    CHECK((vfp_get_s(&c, 2) & 0x7fc00000u) == 0x7fc00000u &&
          (vfp_get_s(&c, 2) & 0x007fffffu) != 0,
          "0/0 = 0x%08x, want a quiet NaN", vfp_get_s(&c, 2));
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "IOC not set for 0/0");
    CHECK((c.vfp_fpscr & ARM_FPSCR_DZC) == 0, "DZC set for 0/0");

    /* inf * 0 -> NaN, IOC. */
    vfp_reset(&c);
    {
        uint32_t mul[] = { DP_S(0,1,0,0, 2,0,1) };  /* VMUL.F32 s2,s0,s1 */
        vfp_set_s(&c, 0, 0x7f800000u); vfp_set_s(&c, 1, 0u);
        run(&c, mul, 1, 1);
        CHECK((vfp_get_s(&c, 2) & 0x7f800000u) == 0x7f800000u &&
              (vfp_get_s(&c, 2) & 0x007fffffu) != 0,
              "inf*0 = 0x%08x", vfp_get_s(&c, 2));
        CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "IOC not set for inf*0");
    }

    /* Overflow of finite operands must reach infinity and set OFC + IXC. */
    vfp_reset(&c);
    {
        uint32_t mul[] = { DP_S(0,1,0,0, 2,0,1) };
        vfp_set_s(&c, 0, 0x7f7fffffu);       /* FLT_MAX */
        set_f32(&c, 1, 2.0f);
        run(&c, mul, 1, 1);
        CHECK(vfp_get_s(&c, 2) == 0x7f800000u, "overflow = 0x%08x", vfp_get_s(&c, 2));
        CHECK((c.vfp_fpscr & ARM_FPSCR_OFC) != 0, "OFC not set, FPSCR = 0x%08x", c.vfp_fpscr);
        CHECK((c.vfp_fpscr & ARM_FPSCR_IXC) != 0, "IXC not set, FPSCR = 0x%08x", c.vfp_fpscr);
    }

    /* And a plain inexact result sets IXC but nothing else. */
    vfp_reset(&c);
    {
        uint32_t d[] = { DP_S(1,0,0,0, 2,0,1) };
        set_f32(&c, 0, 1.0f); set_f32(&c, 1, 3.0f);
        run(&c, d, 1, 1);
        CHECK((c.vfp_fpscr & 0x9fu) == ARM_FPSCR_IXC,
              "1/3 set FPSCR flags 0x%08x, want only IXC", c.vfp_fpscr & 0x9fu);
    }
}

/* ================================================== comparisons ========== */

/* VCMP writes FPSCR.NZCV, never CPSR. VMRS APSR_nzcv is the only thing that
 * moves them across, and testing both together is the only way to catch an
 * implementation that wrote the wrong register. */
static void test_vcmp_writes_fpscr_not_cpsr(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        UN_S(4,0, 0,1),                      /* VCMP.F32 s0, s1     */
        0xeef1fa10u,                         /* VMRS APSR_nzcv, FPSCR */
    };
    CHECK(prog[0] == 0xeeb40a60u, "VCMP.F32 s0,s1 = 0x%08x", prog[0]);

    /* 1.0 < 2.0 -> N set, everything else clear. */
    set_f32(&c, 0, 1.0f); set_f32(&c, 1, 2.0f);
    load(prog, 2);
    CHECK(arm_step(&c) == ARM_OK, "VCMP trapped");
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == ARM_FPSCR_N,
          "less-than FPSCR.NZCV = 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);
    CHECK((c.cpsr & 0xf0000000u) == 0, "VCMP wrote CPSR: 0x%08x", c.cpsr);
    CHECK(arm_step(&c) == ARM_OK, "VMRS APSR_nzcv trapped");
    CHECK((c.cpsr & 0xf0000000u) == ARM_CPSR_N, "CPSR after VMRS = 0x%08x", c.cpsr);

    /* Equal -> Z and C. -0.0 compares equal to +0.0. */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x80000000u); vfp_set_s(&c, 1, 0u);
    run(&c, prog, 1, 1);
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == (ARM_FPSCR_Z | ARM_FPSCR_C),
          "-0 == +0 gave 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);

    /* Greater-than -> C alone. */
    vfp_reset(&c);
    set_f32(&c, 0, 5.0f); set_f32(&c, 1, 2.0f);
    run(&c, prog, 1, 1);
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == ARM_FPSCR_C,
          "greater-than gave 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);

    /* Unordered -> C and V. A QUIET NaN does not raise IOC for plain VCMP... */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x7fc00001u); set_f32(&c, 1, 2.0f);
    run(&c, prog, 1, 1);
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == (ARM_FPSCR_C | ARM_FPSCR_V),
          "unordered gave 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) == 0, "VCMP raised IOC on a quiet NaN");

    /* ...but VCMPE does, and so does VCMP on a SIGNALLING NaN. */
    vfp_reset(&c);
    {
        uint32_t e[] = { UN_S(4,1, 0,1) };             /* VCMPE.F32 s0, s1 */
        CHECK(e[0] == 0xeeb40ae0u, "VCMPE.F32 s0,s1 = 0x%08x", e[0]);
        vfp_set_s(&c, 0, 0x7fc00001u); set_f32(&c, 1, 2.0f);
        run(&c, e, 1, 1);
        CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "VCMPE did not raise IOC");
    }
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x7f800001u);           /* signalling NaN */
    set_f32(&c, 1, 2.0f);
    run(&c, prog, 1, 1);
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "VCMP did not raise IOC on an SNaN");
}

static void test_vcmp_with_zero(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { UN_S(5,0, 0,0) };                /* VCMP.F32 s0, #0.0 */
    CHECK(prog[0] == 0xeeb50a40u, "VCMP.F32 s0,#0 = 0x%08x", prog[0]);
    set_f32(&c, 0, -3.0f);
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "VCMP #0 trapped");
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == ARM_FPSCR_N,
          "-3 vs 0 gave 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);

    /* A non-zero Vm field in the compare-with-zero form is UNPREDICTABLE. */
    vfp_reset(&c);
    { uint32_t bad[] = { UN_S(5,0, 0,2) };
      CHECK(run(&c, bad, 1, 1) == ARM_UNDEFINED, "VCMP #0 with Vm != 0 ran"); }
}

/* ================================================== conversions ========== */

static void test_conversions(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        UN_S(8,1, 2,0),                      /* VCVT.F32.S32 s2, s0 */
        UN_S(8,0, 4,0),                      /* VCVT.F32.U32 s4, s0 */
        UN_S(13,1, 6,6),                     /* VCVT.S32.F32 s6, s6 */
    };
    CHECK(prog[0] == 0xeeb81ac0u, "VCVT.F32.S32 s2,s0 = 0x%08x", prog[0]);
    CHECK(prog[1] == 0xeeb82a40u, "VCVT.F32.U32 s4,s0 = 0x%08x", prog[1]);

    vfp_set_s(&c, 0, 0xffffffffu);           /* -1 signed, 4294967295 unsigned */
    set_f32(&c, 6, -2.5f);
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "conversions trapped");
    CHECK(get_f32(&c, 2) == -1.0f, "S32->F32 = %f", (double)get_f32(&c, 2));
    CHECK(get_f32(&c, 4) == 4294967296.0f, "U32->F32 = %f", (double)get_f32(&c, 4));
    /* VCVT (not VCVTR) truncates toward zero: -2.5 -> -2. */
    CHECK((int32_t)vfp_get_s(&c, 6) == -2, "F32->S32 = %d", (int32_t)vfp_get_s(&c, 6));

    /* VCVTR uses FPSCR rounding, which is round-to-nearest-EVEN: -2.5 -> -2
     * and 2.5 -> 2, but 3.5 -> 4. */
    vfp_reset(&c);
    {
        uint32_t r[] = { UN_S(13,0, 0,0) };              /* VCVTR.S32.F32 s0,s0 */
        CHECK(r[0] == 0xeebd0a40u, "VCVTR.S32.F32 s0,s0 = 0x%08x", r[0]);
        set_f32(&c, 0, 2.5f);  run(&c, r, 1, 1);
        CHECK((int32_t)vfp_get_s(&c, 0) == 2, "VCVTR 2.5 -> %d", (int32_t)vfp_get_s(&c, 0));
        vfp_reset(&c);
        set_f32(&c, 0, 3.5f);  run(&c, r, 1, 1);
        CHECK((int32_t)vfp_get_s(&c, 0) == 4, "VCVTR 3.5 -> %d", (int32_t)vfp_get_s(&c, 0));
    }

    /*
     * VCVTR under each FPSCR.RMode. The conversion rounds in software, so a
     * directed mode is implemented exactly and must not be refused -- UIKit
     * converts this way on the path to SpringBoard's first frame, and the exact
     * encoding that stopped run30 is 0xeefd7a67, VCVTR.S32.F32 s15, s15.
     *
     * VCVT (round-toward-zero) must ignore RMode entirely, and the arithmetic
     * paths must still refuse a directed mode because they delegate rounding to
     * the host FPU.
     */
    {
        static const struct {
            uint32_t rmode; float in; int32_t expect; const char *name;
        } ROUND[] = {
            { 0u << 22,  2.5f,  2, "RN 2.5 ties-to-even" },
            { 0u << 22,  3.5f,  4, "RN 3.5 ties-to-even" },
            { 1u << 22,  2.25f, 3, "RP toward +inf"      },
            { 1u << 22, -2.75f,-2, "RP toward +inf neg"  },
            { 2u << 22,  2.75f, 2, "RM toward -inf"      },
            { 2u << 22, -2.25f,-3, "RM toward -inf neg"  },
            { 3u << 22,  2.75f, 2, "RZ toward zero"      },
            { 3u << 22, -2.75f,-2, "RZ toward zero neg"  },
        };
        uint32_t r[] = { UN_S(13,0, 0,0) };              /* VCVTR.S32.F32 s0,s0 */
        for (unsigned i = 0; i < sizeof ROUND / sizeof ROUND[0]; i++) {
            vfp_reset(&c);
            c.vfp_fpscr = (c.vfp_fpscr & ~ARM_FPSCR_RMODE) | ROUND[i].rmode;
            set_f32(&c, 0, ROUND[i].in);
            CHECK(run(&c, r, 1, 1) == ARM_OK,
                  "VCVTR refused in %s", ROUND[i].name);
            CHECK((int32_t)vfp_get_s(&c, 0) == ROUND[i].expect,
                  "%s: got %d expected %d", ROUND[i].name,
                  (int32_t)vfp_get_s(&c, 0), ROUND[i].expect);
        }

        /* The exact instruction run30 stopped on. */
        vfp_reset(&c);
        {
            uint32_t exact[] = { 0xeefd7a67u };      /* VCVTR.S32.F32 s15,s15 */
            c.vfp_fpscr = (c.vfp_fpscr & ~ARM_FPSCR_RMODE) | (1u << 22);
            set_f32(&c, 15, 2.25f);
            CHECK(run(&c, exact, 1, 1) == ARM_OK,
                  "the exact run30 encoding 0xeefd7a67 was refused");
            CHECK((int32_t)vfp_get_s(&c, 15) == 3,
                  "0xeefd7a67 under RP: got %d expected 3",
                  (int32_t)vfp_get_s(&c, 15));
        }

        /* VCVT truncates regardless of RMode. */
        vfp_reset(&c);
        {
            uint32_t t[] = { UN_S(13,1, 0,0) };          /* VCVT.S32.F32 s0,s0 */
            c.vfp_fpscr = (c.vfp_fpscr & ~ARM_FPSCR_RMODE) | (1u << 22);
            set_f32(&c, 0, 2.75f);
            CHECK(run(&c, t, 1, 1) == ARM_OK, "VCVT refused under RP");
            CHECK((int32_t)vfp_get_s(&c, 0) == 2,
                  "VCVT must truncate regardless of RMode: got %d",
                  (int32_t)vfp_get_s(&c, 0));
        }

    }

    /* Out of range and NaN saturate with IOC, per ARM's FPToFixed. */
    vfp_reset(&c);
    {
        uint32_t t[] = { UN_S(13,1, 0,0) };              /* VCVT.S32.F32 s0,s0 */
        vfp_set_s(&c, 0, 0x7f800000u);                   /* +inf */
        run(&c, t, 1, 1);
        CHECK(vfp_get_s(&c, 0) == 0x7fffffffu, "+inf -> 0x%08x", vfp_get_s(&c, 0));
        CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "no IOC on saturation");

        vfp_reset(&c);
        vfp_set_s(&c, 0, 0xff800000u);                   /* -inf */
        run(&c, t, 1, 1);
        CHECK(vfp_get_s(&c, 0) == 0x80000000u, "-inf -> 0x%08x", vfp_get_s(&c, 0));

        vfp_reset(&c);
        vfp_set_s(&c, 0, 0x7fc00000u);                   /* NaN  */
        run(&c, t, 1, 1);
        CHECK(vfp_get_s(&c, 0) == 0u, "NaN -> 0x%08x, want 0", vfp_get_s(&c, 0));
        CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "no IOC on a NaN conversion");
    }

    /* Single <-> double. Widening is exact; narrowing rounds. */
    vfp_reset(&c);
    {
        uint32_t w[] = {
            UN_D_FROM_S(7,1, 0,1),           /* VCVT.F64.F32 d0, s1 */
            UN_S_FROM_D(7,1, 4,0),           /* VCVT.F32.F64 s4, d0 */
        };
        CHECK(w[0] == 0xeeb70ae0u, "VCVT.F64.F32 d0,s1 = 0x%08x", w[0]);
        CHECK(w[1] == 0xeeb72bc0u, "VCVT.F32.F64 s4,d0 = 0x%08x", w[1]);
        set_f32(&c, 1, 0.5f);
        CHECK(run(&c, w, 2, 2) == ARM_OK, "VCVT f32/f64 trapped");
        CHECK(get_f64(&c, 0) == 0.5, "F32->F64 = %f", get_f64(&c, 0));
        CHECK(get_f32(&c, 4) == 0.5f, "F64->F32 = %f", (double)get_f32(&c, 4));
    }

    /* Widening has no rounding step. Pin its complete bit behavior so the
     * signed engine can implement it without borrowing host floating state. */
    {
        static const struct {
            uint32_t input;
            uint32_t mode;
            uint64_t output;
            uint32_t raised;
            const char *name;
        } WIDEN[] = {
            {UINT32_C(0x00000000), 0u, UINT64_C(0x0000000000000000), 0u,
             "+zero"},
            {UINT32_C(0x80000000), 0u, UINT64_C(0x8000000000000000), 0u,
             "-zero"},
            {UINT32_C(0x3f800000), ARM_FPSCR_RMODE,
             UINT64_C(0x3ff0000000000000), 0u, "one under RZ"},
            {UINT32_C(0x7f7fffff), 0u,
             UINT64_C(0x47efffffe0000000), 0u, "largest finite"},
            {UINT32_C(0x00800000), 0u,
             UINT64_C(0x3810000000000000), 0u, "smallest normal"},
            {UINT32_C(0x00000001), 0u,
             UINT64_C(0x36a0000000000000), 0u, "smallest subnormal"},
            {UINT32_C(0x007fffff), 0u,
             UINT64_C(0x380fffffc0000000), 0u, "largest subnormal"},
            {UINT32_C(0x80000001), ARM_FPSCR_FZ,
             UINT64_C(0x8000000000000000), ARM_FPSCR_IDC,
             "FZ negative subnormal"},
            {UINT32_C(0x7f800000), 0u,
             UINT64_C(0x7ff0000000000000), 0u, "+infinity"},
            {UINT32_C(0xff800000), 0u,
             UINT64_C(0xfff0000000000000), 0u, "-infinity"},
            {UINT32_C(0x7fc12345), 0u,
             UINT64_C(0x7ff82468a0000000), 0u, "quiet NaN payload"},
            {UINT32_C(0xff812345), 0u,
             UINT64_C(0xfff82468a0000000), ARM_FPSCR_IOC,
             "signalling NaN"},
            {UINT32_C(0xffc12345), ARM_FPSCR_DN,
             UINT64_C(0x7ff8000000000000), 0u, "default NaN"},
        };
        uint32_t widen[] = {
            UN_D_FROM_S(7,1, 2,1),       /* VCVT.F64.F32 d2,s1 */
        };
        for (unsigned i = 0u; i < sizeof WIDEN / sizeof WIDEN[0]; i++) {
            uint32_t initial = WIDEN[i].mode | ARM_FPSCR_DZC;
            vfp_reset(&c);
            c.vfp_fpscr = initial;
            vfp_set_s(&c, 1, WIDEN[i].input);
            CHECK(run(&c, widen, 1, 1) == ARM_OK,
                  "widening refused for %s", WIDEN[i].name);
            CHECK(vfp_get_d(&c, 2) == WIDEN[i].output,
                  "%s widened to 0x%016llx, expected 0x%016llx",
                  WIDEN[i].name,
                  (unsigned long long)vfp_get_d(&c, 2),
                  (unsigned long long)WIDEN[i].output);
            CHECK(c.vfp_fpscr == (initial | WIDEN[i].raised),
                  "%s FPSCR = 0x%08x, expected 0x%08x",
                  WIDEN[i].name, c.vfp_fpscr,
                  initial | WIDEN[i].raised);
        }
    }
}

/*
 * The exact compiler-runtime helpers called by CoreFoundation immediately
 * after the run20 `_fmod` site. These are not synthetic mnemonics: their words
 * come from the original iPhone OS 3.1.3 shared cache. Test them as complete
 * call/return units, including BX back to the Thumb caller, because a correct
 * VCVT surrounded by a wrong transfer or interworking edge is still a boot
 * failure.
 */
static void test_ios313_corefoundation_vfp_helpers(void) {
    uint32_t fix_u32[] = {
        0xec410b17u,                         /* VMOV d7, r0, r1       */
        0xeefc7bc7u,                         /* VCVT.U32.F64 s15, d7  */
        0xee170a90u,                         /* VMOV r0, s15          */
        0xe12fff1eu,                         /* BX   lr               */
    };
    uint32_t float_u32[] = {
        0xee070a90u,                         /* VMOV s15, r0          */
        0xeeb87b67u,                         /* VCVT.F64.U32 d7, s15  */
        0xec510b17u,                         /* VMOV r0, r1, d7       */
        0xe12fff1eu,                         /* BX   lr               */
    };
    struct {
        uint32_t input;
        const char *why;
    } unsigned_cases[] = {
        { 0x00000000u, "zero" },
        { 0xffffffffu, "UINT32_MAX" },
    };
    arm_cpu_t c;

    CHECK(fix_u32[1] == UN_S_FROM_D(12,1, 15,7),
          "shared-cache VCVT.U32.F64 encoding = 0x%08x", fix_u32[1]);
    CHECK(fix_u32[2] == VMOV_R_S(0,15),
          "shared-cache VMOV r0,s15 encoding = 0x%08x", fix_u32[2]);
    CHECK(float_u32[0] == VMOV_S_R(15,0),
          "shared-cache VMOV s15,r0 encoding = 0x%08x", float_u32[0]);
    CHECK(float_u32[1] == VFP_DP(1,1,1, 0,8,7, 1,0,1, 1,7),
          "shared-cache VCVT.F64.U32 encoding = 0x%08x", float_u32[1]);

    for (unsigned i = 0;
         i < sizeof unsigned_cases / sizeof unsigned_cases[0]; i++) {
        uint64_t source = d2u((double)unsigned_cases[i].input);

        vfp_reset(&c);
        c.vfp_fpscr = ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_RMODE;
        c.r[0] = (uint32_t)source;
        c.r[1] = (uint32_t)(source >> 32);
        c.r[14] = 0x101u;                    /* real caller is Thumb */
        CHECK(run(&c, fix_u32, sizeof fix_u32 / sizeof fix_u32[0], 4) == ARM_OK,
              "___fixunsdfsivfp trapped for %s", unsigned_cases[i].why);
        CHECK(c.r[0] == unsigned_cases[i].input,
              "___fixunsdfsivfp(%s) = 0x%08x",
              unsigned_cases[i].why, c.r[0]);
        CHECK(c.r[15] == 0x100u && (c.cpsr & ARM_CPSR_T) != 0,
              "___fixunsdfsivfp(%s) returned to %08x CPSR=%08x",
              unsigned_cases[i].why, c.r[15], c.cpsr);

        vfp_reset(&c);
        c.vfp_fpscr = ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_RMODE;
        c.r[0] = unsigned_cases[i].input;
        c.r[14] = 0x101u;
        CHECK(run(&c, float_u32, sizeof float_u32 / sizeof float_u32[0], 4) == ARM_OK,
              "___floatunssidfvfp trapped for %s", unsigned_cases[i].why);
        CHECK(((uint64_t)c.r[1] << 32 | c.r[0]) ==
              d2u((double)unsigned_cases[i].input),
              "___floatunssidfvfp(%s) = 0x%08x%08x",
              unsigned_cases[i].why, c.r[1], c.r[0]);
        CHECK(c.r[15] == 0x100u && (c.cpsr & ARM_CPSR_T) != 0,
              "___floatunssidfvfp(%s) returned to %08x CPSR=%08x",
              unsigned_cases[i].why, c.r[15], c.cpsr);
    }

    /* The helper's VCVT form is explicitly round-toward-zero and therefore
     * ignores FPSCR.RMode. A fractional operand distinguishes it from VCVTR. */
    {
        uint64_t source = d2u(42.75);
        vfp_reset(&c);
        c.vfp_fpscr = ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_RMODE;
        c.r[0] = (uint32_t)source;
        c.r[1] = (uint32_t)(source >> 32);
        c.r[14] = 0x101u;
        CHECK(run(&c, fix_u32, sizeof fix_u32 / sizeof fix_u32[0], 4) == ARM_OK,
              "___fixunsdfsivfp trapped on a fractional input");
        CHECK(c.r[0] == 42u, "___fixunsdfsivfp(42.75) = %u", c.r[0]);
        CHECK((c.vfp_fpscr & ARM_FPSCR_IXC) != 0,
              "fractional ___fixunsdfsivfp did not set IXC");
    }
}

/* ============================================ things that must trap ====== */

/*
 * The contract: an encoding this unit does not implement stops the machine
 * with a named reason. It must NEVER be a no-op, and it must never be quietly
 * folded onto a register that happens to exist. These are all cases where a
 * plausible-looking wrong answer was easy to produce.
 */
static void test_unimplemented_encodings_still_halt(void) {
    arm_cpu_t c;
    struct { uint32_t insn; const char *what; } cases[] = {
        /* d16-d31: VFPv2 has 16 doubles, and the D bit that would name d16 is
         * SBZ. Folding it back onto d0 would corrupt an unrelated register. */
        { VFP_DP(0,1,1, 1,0,0, 1, 0,0, 0,0), "VADD.F64 d16, d0, d0" },
        { VFP_DP(0,1,1, 0,0,0, 1, 1,0, 0,0), "VADD.F64 d0, d16, d0" },
        { VFP_DP(0,1,1, 0,0,0, 1, 0,0, 1,0), "VADD.F64 d0, d0, d16" },
        { VFP_LS(1,1,1,0,1, 1, 0, 1, 0),     "VLDR d16, [r1]"       },
        /* VFPv3 and VFPv4 encodings that the VFP11 does not have. */
        { 0xeeb00b00u,                       "VMOV.F64 d0, #imm (VFPv3)" },
        { UN_S(2,1, 0,0),                    "VCVTB half-precision (VFPv3)" },
        { UN_S(10,1, 0,0),                   "VCVT fixed-point (VFPv3)" },
        { DP_S(1,1,0,0, 0,0,0),              "VFMA (VFPv4)" },
        { DP_S(1,0,1,0, 0,0,0),              "VFNMA (VFPv4)" },
        /* Advanced SIMD scalar transfers: only VFPv2's 32-bit d0-d15 word
         * transfers exist on this part. 8/16-bit lanes need NEON, while d16+
         * is simply outside VFP11's register bank. */
        { 0xee400b10u,                       "VMOV.8 d0[0], r0 (NEON)" },
        { 0xee000b30u,                       "VMOV.16 d0[0], r0 (NEON)" },
        { 0xee000b90u,                       "VMOV.32 d16[0], r0 (absent on VFP11)" },
        { 0xeee84b10u,                       "VDUP.8 q4, r4 (must not alias VMSR FPEXC)" },
        { 0xeef14b10u,                       "NEON lane read (must not alias VMRS FPSCR)" },
        /* Malformed/reserved VFP transfer encodings. */
        { 0xee200a10u,                       "reserved cp10 opc1=1 transfer" },
        { 0xee000b11u,                       "FMDLR with reserved CRm bits" },
        { 0xee27fb10u,                       "FMDHR with PC as core register" },
        { 0xee37fb10u,                       "FMRDH with PC as core register" },
        { 0xeef10a30u,                       "VMRS with reserved opc2 bits" },
        { VMRS(15, 0),                       "VMRS PC, FPSID" },
        { VMRS(15, 8),                       "VMRS PC, FPEXC" },
        /* VFP system registers we do not implement. */
        { VMRS(0, 7),                        "VMRS r0, MVFR0" },
        { VMRS(0, 9),                        "VMRS r0, FPINST" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t p[1]; p[0] = cases[i].insn;
        vfp_reset(&c);
        CHECK(run(&c, p, 1, 1) == ARM_UNDEFINED, "%s did not halt", cases[i].what);
        CHECK(vfp_trap_reason() != NULL, "%s halted without a reason", cases[i].what);
    }
}

/*
 * FPSCR modes we cannot reproduce bit-exactly must stop the machine rather
 * than compute a plausible wrong number. The split matters: VMOV and VABS do
 * not consult the rounding mode, so they must keep working in RZ, while VADD
 * must not.
 */
static void test_fpscr_mode_handling(void) {
    arm_cpu_t c;
    uint32_t add[] = { DP_S(0,1,1,0, 2,0,1) };   /* VADD.F32 s2,s0,s1 */
    uint32_t mov[] = { UN_S(0,0, 1,0) };         /* VMOV.F32 s1,s0    */

    struct { uint32_t fpscr; const char *what; } modes[] = {
        { ARM_FPSCR_IOE, "IOE trap enable" },
    };
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        vfp_reset(&c); c.vfp_fpscr = modes[i].fpscr;
        CHECK(run(&c, add, 1, 1) == ARM_UNDEFINED, "VADD ran with %s", modes[i].what);
    }

    /*
     * RMode is no longer in that list: a directed rounding mode is implemented
     * rather than refused, because vfp_execute() adopts it on the host FPU for
     * the duration of the instruction. Check it actually rounds, not merely
     * that it runs -- 1.0f + 2^-25 is inexact in single precision, so it must
     * round down to 1.0 toward zero and up to the next representable float
     * toward +infinity.
     */
    {
        static const struct {
            uint32_t rmode; float expect; const char *what;
        } DIRECTED[] = {
            { 3u << 22, 1.0f,                  "RZ rounds toward zero"     },
            { 2u << 22, 1.0f,                  "RM rounds toward -inf"     },
            { 1u << 22, 1.0f + 1.1920929e-7f,  "RP rounds toward +inf"     },
        };
        for (size_t i = 0; i < sizeof DIRECTED / sizeof DIRECTED[0]; i++) {
            vfp_reset(&c);
            c.vfp_fpscr = (c.vfp_fpscr & ~ARM_FPSCR_RMODE) | DIRECTED[i].rmode;
            set_f32(&c, 0, 1.0f);
            set_f32(&c, 1, 2.9802322e-8f);          /* 2^-25, below the ulp */
            CHECK(run(&c, add, 1, 1) == ARM_OK,
                  "VADD refused with %s", DIRECTED[i].what);
            CHECK(get_f32(&c, 2) == DIRECTED[i].expect,
                  "%s: got %.9g expected %.9g", DIRECTED[i].what,
                  (double)get_f32(&c, 2), (double)DIRECTED[i].expect);
        }
    }

    /* VMOV survives directed rounding and trap enables because it cannot
     * raise an exception. With a bank-zero destination it also remains scalar
     * while LEN is nonzero. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_RMODE;
    CHECK(run(&c, mov, 1, 1) == ARM_OK, "VMOV refused in RZ mode");
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_IOE;
    CHECK(run(&c, mov, 1, 1) == ARM_OK, "VMOV refused with a trap enable set");
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_LEN; vfp_set_s(&c, 0, 0x12345678u);
    CHECK(run(&c, mov, 1, 1) == ARM_OK, "bank-zero VMOV refused with LEN set");
    CHECK(vfp_get_s(&c, 1) == 0x12345678u, "bank-zero VMOV with LEN set was wrong");

    /*
     * An integer-source conversion has nothing for FZ or DN to act on and, for
     * a double destination, cannot even be inexact — so it must run in any of
     * those modes. This is not a hypothetical: dyld runs in ARM's RunFast
     * configuration (FZ set) and its first floating-point instruction is
     * VCVT.F64.U32 on the mach_timebase_info numerator. Refusing it here
     * stopped the boot dead at pc 0x2120.
     */
    {
        uint32_t cvt[] = { VFP_DP(1,1,1, 0, 8, 6, 1, 0, 1, 1, 5) };  /* VCVT.F64.U32 d6,s11 */
        CHECK(cvt[0] == 0xeeb86b65u, "VCVT.F64.U32 d6,s11 = 0x%08x", cvt[0]);
        vfp_reset(&c);
        c.vfp_fpscr = ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_RMODE;
        vfp_set_s(&c, 11, 1000000000u);
        CHECK(run(&c, cvt, 1, 1) == ARM_OK, "VCVT.F64.U32 refused in RunFast mode");
        CHECK(get_f64(&c, 6) == 1000000000.0, "d6 = %f", get_f64(&c, 6));
    }
}

/*
 * Flush-to-zero, FPSCR.FZ, which dyld turns on. Denormal operands become zero
 * of the same sign with IDC set, and a result below the smallest normal
 * becomes zero of the result's sign with UFC set.
 */
static void test_flush_to_zero(void) {
    arm_cpu_t c;
    uint32_t mul[] = { DP_S(0,1,0,0, 2,0,1) };   /* VMUL.F32 s2, s0, s1 */
    uint32_t add[] = { DP_S(0,1,1,0, 2,0,1) };   /* VADD.F32 s2, s0, s1 */

    /* A denormal input is flushed, so denormal + 0 is exactly zero, not the
     * denormal. IDC records that it happened. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_FZ;
    vfp_set_s(&c, 0, 0x00000001u);               /* the smallest denormal */
    vfp_set_s(&c, 1, 0u);
    CHECK(run(&c, add, 1, 1) == ARM_OK, "VADD refused in FZ mode");
    CHECK(vfp_get_s(&c, 2) == 0u, "denormal + 0 = 0x%08x", vfp_get_s(&c, 2));
    CHECK((c.vfp_fpscr & ARM_FPSCR_IDC) != 0, "IDC not set, FPSCR = 0x%08x", c.vfp_fpscr);

    /* The flushed zero keeps its sign, which is what makes a multiply come out
     * negative rather than positive. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_FZ;
    vfp_set_s(&c, 0, 0x80000001u);               /* a NEGATIVE denormal */
    set_f32(&c, 1, 2.0f);
    CHECK(run(&c, mul, 1, 1) == ARM_OK, "VMUL refused in FZ mode");
    CHECK(vfp_get_s(&c, 2) == 0x80000000u, "-denormal * 2 = 0x%08x, want -0",
          vfp_get_s(&c, 2));

    /* A result that underflows into the denormal range is flushed on the way
     * out, with UFC set. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_FZ;
    vfp_set_s(&c, 0, 0x00800000u);               /* the smallest normal */
    set_f32(&c, 1, 0.25f);
    CHECK(run(&c, mul, 1, 1) == ARM_OK, "VMUL refused in FZ mode");
    CHECK(vfp_get_s(&c, 2) == 0u, "underflow = 0x%08x, want +0", vfp_get_s(&c, 2));
    CHECK((c.vfp_fpscr & ARM_FPSCR_UFC) != 0, "UFC not set, FPSCR = 0x%08x", c.vfp_fpscr);

    /* With FZ clear the same operation keeps the denormal: the mode really is
     * doing something, rather than the host doing it for us. */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x00800000u);
    set_f32(&c, 1, 0.25f);
    run(&c, mul, 1, 1);
    CHECK(vfp_get_s(&c, 2) == 0x00200000u, "no-FZ underflow = 0x%08x",
          vfp_get_s(&c, 2));

    /* And FZ leaves normal arithmetic completely alone. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_FZ;
    set_f32(&c, 0, 1.5f); set_f32(&c, 1, 2.5f);
    run(&c, add, 1, 1);
    CHECK(get_f32(&c, 2) == 4.0f, "FZ perturbed a normal add: %f",
          (double)get_f32(&c, 2));
    CHECK((c.vfp_fpscr & 0x9fu) == 0u, "FZ set flags on an exact add: 0x%08x",
          c.vfp_fpscr & 0x9fu);
}

/* Default-NaN mode replaces every NaN result with the one default quiet NaN,
 * which also makes the NaN-payload deviation documented in vfp.c unobservable. */
static void test_default_nan(void) {
    arm_cpu_t c;
    uint32_t add[] = { DP_S(0,1,1,0, 2,0,1) };   /* VADD.F32 s2, s0, s1 */

    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_DN;
    vfp_set_s(&c, 0, 0x7fc12345u);               /* a quiet NaN with a payload */
    set_f32(&c, 1, 1.0f);
    CHECK(run(&c, add, 1, 1) == ARM_OK, "VADD refused in DN mode");
    CHECK(vfp_get_s(&c, 2) == 0x7fc00000u, "DN result = 0x%08x, want the default NaN",
          vfp_get_s(&c, 2));

    /* Without DN the payload propagates instead. */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x7fc12345u);
    set_f32(&c, 1, 1.0f);
    run(&c, add, 1, 1);
    CHECK(vfp_get_s(&c, 2) == 0x7fc12345u, "non-DN result = 0x%08x, want the payload",
          vfp_get_s(&c, 2));

    /* Double precision has its own default NaN. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_DN;
    {
        uint32_t addd[] = { DP_D(0,1,1,0, 2,0,1) };   /* VADD.F64 d2, d0, d1 */
        vfp_set_d(&c, 0, 0x7ff8123456789abcull);
        set_f64(&c, 1, 1.0);
        CHECK(run(&c, addd, 1, 1) == ARM_OK, "VADD.F64 refused in DN mode");
        CHECK(vfp_get_d(&c, 2) == 0x7ff8000000000000ull, "DN.F64 = 0x%016llx",
              (unsigned long long)vfp_get_d(&c, 2));
    }
}

/* A VLDM whose list crosses into unmapped memory must abort like an LDM. The
 * flat test bus never faults, so this checks the plumbing instead: a load that
 * runs is a load that reached vfp_bus, and the abort latch stays clear. */
static void test_ldm_uses_the_translating_bus(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { VFP_LS(0,1,0,0,1, 1, 0, 1, 8) };  /* VLDMIA r1, {d0-d3} */
    for (unsigned i = 0; i < 8; i++) m_w32(NULL, 0x7000u + i*4u, 0x5a5a0000u + i);
    c.r[1] = 0x7000;
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "VLDMIA {d0-d3} trapped");
    CHECK(!c.abort_pending, "abort latched on a clean load");
    CHECK(vfp_get_d(&c, 0) == 0x5a5a00015a5a0000ull, "d0 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 0));
    CHECK(vfp_get_d(&c, 3) == 0x5a5a00075a5a0006ull, "d3 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 3));
}

static unsigned g_fault_reads32, g_fault_writes32;
static uint32_t fault_first_r32(arm_cpu_t *c, uint32_t va) {
    g_fault_reads32++;
    c->abort_pending = true;
    c->abort_fsr = ARM_FSR_PAGE_TRANSLATION;
    c->abort_far = va;
    return 0x11223344u;
}
static void fault_first_w32(arm_cpu_t *c, uint32_t va, uint32_t v) {
    (void)v;
    g_fault_writes32++;
    c->abort_pending = true;
    c->abort_fsr = ARM_FSR_PAGE_TRANSLATION | (1u << 11);
    c->abort_far = va;
}
static const vfp_bus_t g_fault_first_bus = { fault_first_r32, fault_first_w32 };

static void test_double_transfers_stop_after_the_first_abort(void) {
    arm_cpu_t c;
    static const uint32_t insns[] = {
        VFP_LS(1,1,0,0,1, 1,0,1,0),            /* VLDR   d0,[r1]   */
        VFP_LS(0,1,0,0,1, 1,0,1,2),            /* VLDMIA r1,{d0}  */
        VFP_LS(1,1,0,0,0, 1,0,1,0),            /* VSTR   d0,[r1]   */
        VFP_LS(0,1,0,0,0, 1,0,1,2),            /* VSTMIA r1,{d0}  */
    };

    for (unsigned i = 0; i < 4u; i++) {
        vfp_reset(&c);
        c.r[1] = 0x800u;
        vfp_set_d(&c, 0, 0x8877665544332211ull);
        c.abort_pending = false;
        g_fault_reads32 = g_fault_writes32 = 0u;
        CHECK(vfp_execute(&c, 0u, insns[i], &g_fault_first_bus) == ARM_OK,
              "faulting double transfer %u returned a decode error", i);
        CHECK(c.abort_pending, "double transfer %u did not preserve its abort", i);
        if (i < 2u)
            CHECK(g_fault_reads32 == 1u && g_fault_writes32 == 0u,
                  "double load %u issued %u reads/%u writes after first abort",
                  i, g_fault_reads32, g_fault_writes32);
        else
            CHECK(g_fault_writes32 == 1u && g_fault_reads32 == 0u,
                  "double store %u issued %u writes/%u reads after first abort",
                  i, g_fault_writes32, g_fault_reads32);
    }
}

/* Conditional execution applies to VFP like everything else. */
static void test_condition_codes_apply(void) {
    arm_cpu_t c; vfp_reset(&c);
    /* ADDEQ-style: make the VADD NE, then set Z so it is skipped. */
    uint32_t prog[] = { (DP_S(0,1,1,0, 2,0,1) & 0x0fffffffu) | 0x10000000u };
    set_f32(&c, 0, 1.0f); set_f32(&c, 1, 2.0f); set_f32(&c, 2, 99.0f);
    c.cpsr |= ARM_CPSR_Z;
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "conditional VADD trapped");
    CHECK(get_f32(&c, 2) == 99.0f, "VADDNE executed with Z set: %f",
          (double)get_f32(&c, 2));
    CHECK(c.r[15] == 4u, "PC = 0x%08x", c.r[15]);

    vfp_reset(&c);
    {
        uint32_t move_ne[] = { 0x1e274b10u };       /* FMDHRNE d7,r4 */
        c.r[4] = 0x11223344u;
        vfp_set_s(&c, 15, 0x55667788u);
        c.cpsr |= ARM_CPSR_Z;
        CHECK(run(&c, move_ne, 1, 1) == ARM_OK, "conditional FMDHR trapped");
        CHECK(vfp_get_s(&c, 15) == 0x55667788u,
              "FMDHRNE executed with Z set: s15 = 0x%08x", vfp_get_s(&c, 15));
        CHECK(c.r[15] == 4u, "conditional FMDHR PC = 0x%08x", c.r[15]);
    }
}

/* --------------------------------------------------------------- main ---- */
int main(void) {
    test_a8_vfp_multiple_memory_register_lists();
    test_a8_vfp_multiple_memory_rejections_and_pc();
    test_a8_vfp_multiple_odd_word_gap();
    test_a8_vfp_multiple_access_and_skip();
    test_a8_vfp_single_memory_bank_and_offsets();
    test_a8_vfp_single_memory_bases_and_conditions();
    test_a8_vfp_single_memory_permissions();
    test_a8_vfp_single_memory_decode_boundaries();
    test_a8_vfp_core_move_refusals_and_it();
    test_a8_vfp_full_bank_core_moves();
    test_a8_vfp_core_move_permissions_and_registers();
    printf("VFPv2 (VFP11) tests\n");
    test_s_d_aliasing();
    test_vldmia_writeback_the_vfp_switch_form();
    test_vstmia_vldmia_round_trip();
    test_double_list_matches_single_list();
    test_vpush_vpop();
    test_vldr_vstr();
    test_overlong_lists_trap();
    test_vmrs_vmsr();
    test_fpsid_is_read_only();
    test_system_register_privilege();
    test_fpexc_en_gates_the_other_registers();
    test_vmov_core_registers();
    test_vmov_double_register_words();
    test_ios313_libm_fmod_return_block();
    test_single_precision_arithmetic();
    test_double_precision_arithmetic();
    test_short_vector_arithmetic();
    test_short_vector_unary_and_scalar_only();
    test_invalid_short_vector_shapes_halt_without_writes();
    test_vmla_is_not_fused();
    test_vabs_vneg_vmov_are_sign_bit_operations();
    test_infinity_and_nan();
    test_vcmp_writes_fpscr_not_cpsr();
    test_vcmp_with_zero();
    test_conversions();
    test_ios313_corefoundation_vfp_helpers();
    test_unimplemented_encodings_still_halt();
    test_fpscr_mode_handling();
    test_flush_to_zero();
    test_default_nan();
    test_ldm_uses_the_translating_bus();
    test_double_transfers_stop_after_the_first_abort();
    test_condition_codes_apply();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
