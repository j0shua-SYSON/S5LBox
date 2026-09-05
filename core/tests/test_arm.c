/*
 * S5LBox — ARMv6 interpreter unit tests.
 *
 * A tiny dependency-free test harness: a flat 1 MiB RAM behind the arm_bus_t,
 * hand-assembled ARM encodings, single-stepped, with register/flag assertions.
 * Runs identically on Windows (MinGW), Linux, and macOS CI.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- flat memory */
#define RAM_SIZE (1u << 20)
static uint8_t g_ram[RAM_SIZE];

/* Optional one-address bus watch used to prove that privilege/fault rejection
 * happens before a device-like read or write. Disabled for ordinary tests. */
static uint32_t g_watch_addr = 0xffffffffu;
static unsigned g_watch_reads32, g_watch_writes32;
static unsigned g_watch_reads16, g_watch_writes16;
static unsigned g_watch_reads8, g_watch_writes8;

static uint32_t m_r32(void *ctx, uint32_t a){ (void)ctx; uint32_t v; if(a==g_watch_addr)g_watch_reads32++; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],4); return v; }
static uint16_t m_r16(void *ctx, uint32_t a){ (void)ctx; uint16_t v; if(a==g_watch_addr)g_watch_reads16++; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],2); return v; }
static uint8_t  m_r8 (void *ctx, uint32_t a){ (void)ctx; if(a==g_watch_addr)g_watch_reads8++; return g_ram[a&(RAM_SIZE-1)]; }
static void m_w32(void *ctx, uint32_t a, uint32_t v){ (void)ctx; if(a==g_watch_addr)g_watch_writes32++; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,4); }
static void m_w16(void *ctx, uint32_t a, uint16_t v){ (void)ctx; if(a==g_watch_addr)g_watch_writes16++; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,2); }
static void m_w8 (void *ctx, uint32_t a, uint8_t  v){ (void)ctx; if(a==g_watch_addr)g_watch_writes8++; g_ram[a&(RAM_SIZE-1)]=v; }

static uint8_t *m_host_ram_write(void *ctx, uint32_t a, uint32_t len) {
    (void)ctx;
    if (!len || (uint64_t)a + len > RAM_SIZE) return NULL;
    return &g_ram[a];
}

static uint8_t *m_host_ram(void *ctx, uint32_t a, uint32_t len) {
    return m_host_ram_write(ctx, a, len);
}

/* Designated, so a new optional hook on arm_bus_t cannot break this file: the
 * positional form listed ten members and the struct has grown four. Every
 * omitted member is a NULL optional hook, which is what the trailing NULLs
 * meant. See test_jit.c for how this stayed red on one CI job for a dozen
 * commits. */
static const arm_bus_t g_bus = {
    .ctx     = NULL,
    .read32  = m_r32, .read16  = m_r16, .read8  = m_r8,
    .write32 = m_w32, .write16 = m_w16, .write8 = m_w8,
};

static bool count_wfi(void *ctx) {
    unsigned *calls = ctx;
    (*calls)++;
    return false;                 /* exercise the core's non-blocking fallback */
}

typedef struct svc_probe {
    arm_svc_result_t result;
    arm_cpu_t       *expected_cpu;
    unsigned         calls;
    uint32_t         seen_pc;
    uint32_t         seen_encoding;
    uint32_t         seen_cpsr;
    uint32_t         seen_r0;
    uint32_t         seen_cpu_pc;
    bool             saw_expected_cpu;
    bool             mutate;
    bool             redirect;
    bool             redirect_thumb;
    uint32_t         redirect_pc;
} svc_probe_t;

static arm_svc_result_t probe_privileged_svc(void *ctx, arm_cpu_t *cpu,
                                              uint32_t pc,
                                              uint32_t encoding) {
    svc_probe_t *probe = ctx;
    probe->calls++;
    probe->seen_pc = pc;
    probe->seen_encoding = encoding;
    probe->seen_cpsr = cpu->cpsr;
    probe->seen_r0 = cpu->r[0];
    probe->seen_cpu_pc = cpu->r[15];
    probe->saw_expected_cpu = cpu == probe->expected_cpu;

    if (probe->mutate) {
        cpu->r[0] = 0xfeed0001u;
        cpu->r[7] = 0xfeed0007u;
        cpu->r[15] = 0xfeed0015u;
        cpu->cp15.context_id = 0xfeedc013u;
        cpu->cpsr ^= ARM_CPSR_N;
        cpu->excl_valid = false;
        cpu->excl_addr = 0xfeedeeeeu;
    }
    if (probe->redirect) {
        cpu->r[15] = probe->redirect_pc;
        if (probe->redirect_thumb) cpu->cpsr |= ARM_CPSR_T;
        else                       cpu->cpsr &= ~ARM_CPSR_T;
    }
    return probe->result;
}

/* ------------------------------------------------------------- test runner */
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Load a sequence of instruction words at 0 and run n steps from PC=0. */
static void load_and_run(arm_cpu_t *c, const uint32_t *prog, size_t words, int steps) {
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i*4), prog[i]);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS; /* SYS so all regs are flat */
    for (int i = 0; i < steps; i++) if (arm_step(c) != ARM_OK) break;
}

/* Like load_and_run but returns the status of the last step (for trap tests). */
static arm_status_t run_status(arm_cpu_t *c, const uint32_t *prog, size_t words, int steps) {
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i*4), prog[i]);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS;
    arm_status_t st = ARM_OK;
    for (int i = 0; i < steps; i++) { st = arm_step(c); if (st != ARM_OK) break; }
    return st;
}

/* --------------------------------------------------------------- the tests */

static void test_mov_imm(void) {
    /* MOV r0, #42  -> e3a0002a */
    uint32_t p[] = { 0xe3a0002a };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[0] == 42, "r0=%u expect 42", c.r[0]);
    CHECK(c.r[15] == 4, "pc=%u expect 4", c.r[15]);
}

static void test_add_reg(void) {
    /* MOV r1,#40 ; MOV r2,#2 ; ADD r0,r1,r2 */
    uint32_t p[] = { 0xe3a01028, 0xe3a02002, 0xe0810002 };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 42, "r0=%u expect 42", c.r[0]);
}

static void test_sub_flags(void) {
    /* MOV r0,#5 ; SUBS r0,r0,#5  -> Z set, C set (no borrow) */
    uint32_t p[] = { 0xe3a00005, 0xe2500005 };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 0, "r0=%u expect 0", c.r[0]);
    CHECK((c.cpsr & ARM_CPSR_Z) != 0, "Z should be set");
    CHECK((c.cpsr & ARM_CPSR_C) != 0, "C should be set (no borrow)");
    CHECK((c.cpsr & ARM_CPSR_N) == 0, "N should be clear");
}

static void test_subs_negative(void) {
    /* MOV r0,#1 ; SUBS r0,r0,#2 -> result 0xffffffff, N set, C clear (borrow) */
    uint32_t p[] = { 0xe3a00001, 0xe2500002 };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 0xffffffffu, "r0=%08x expect ffffffff", c.r[0]);
    CHECK((c.cpsr & ARM_CPSR_N) != 0, "N should be set");
    CHECK((c.cpsr & ARM_CPSR_C) == 0, "C should be clear (borrow)");
}

static void test_adds_overflow(void) {
    /* r0 = 0x7fffffff ; ADDS r0,r0,#1 -> V set, N set */
    /* MOV r0,#0xff000000-ish is awkward; build 0x7fffffff via MVN r0,#0x80000000? */
    /* Simpler: MOV r0,#0x7f000000 rotated... use two: MVN r0,#0x80000000 gives 0x7fffffff */
    /* MVN r0, #0x80000000 : imm=0x80000000 not encodable directly; use ror.
       0x80000000 = 0x2 ror 2? 0x02 rotated right by 2 -> 0x80000000. rot field=1 (=2*1). */
    uint32_t p[] = { 0xe3e00102, /* MVN r0,#0x80000000 -> r0=0x7fffffff */
                     0xe2900001  /* ADDS r0,r0,#1 */ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 0x80000000u, "r0=%08x expect 80000000", c.r[0]);
    CHECK((c.cpsr & ARM_CPSR_V) != 0, "V should be set on signed overflow");
    CHECK((c.cpsr & ARM_CPSR_N) != 0, "N should be set");
}

static void test_nzcv_updates_preserve_the_rest_of_cpsr(void) {
    static const struct {
        const char *name;
        uint32_t insn, a, b;
        uint32_t addend, cin;
        bool input_carry;
    } arithmetic[] = {
        /* ADDS r0,r1,r2: positive overflow without carry. */
        { "ADDS", 0xe0910002u, 0x7fffffffu, 1u,          1u,          0u, false },
        /* ADCS r0,r1,r2: the incoming carry produces zero and carry-out. */
        { "ADCS", 0xe0b10002u, 0xffffffffu, 0u,          0u,          1u, true  },
        /* SUBS r0,r1,r2 is implemented as a + ~b + 1. */
        { "SUBS", 0xe0510002u, 0u,          1u,          0xfffffffeu, 1u, false },
        /* SBCS with C clear subtracts the extra one and crosses INT32_MIN. */
        { "SBCS", 0xe0d10002u, 0x80000000u, 0u,          0xffffffffu, 0u, false },
    };
    const uint32_t nzcv_mask = ARM_CPSR_N | ARM_CPSR_Z |
                               ARM_CPSR_C | ARM_CPSR_V;
    const uint32_t preserved = ARM_MODE_SYS | ARM_CPSR_Q | ARM_CPSR_A |
                               ARM_CPSR_E | ARM_CPSR_I | ARM_CPSR_F |
                               0x000f0000u; /* GE[3:0] */

    for (size_t i = 0; i < sizeof arithmetic / sizeof arithmetic[0]; i++) {
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0u, arithmetic[i].insn);
        arm_cpu_t c;
        arm_reset(&c, &g_bus);
        c.cpsr = preserved | ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_V |
                 (arithmetic[i].input_carry ? ARM_CPSR_C : 0u);
        c.r[1] = arithmetic[i].a;
        c.r[2] = arithmetic[i].b;

        uint64_t wide = (uint64_t)arithmetic[i].a +
                        (uint64_t)arithmetic[i].addend + arithmetic[i].cin;
        int64_t signed_wide = (int64_t)(int32_t)arithmetic[i].a +
                              (int64_t)(int32_t)arithmetic[i].addend +
                              arithmetic[i].cin;
        uint32_t result = (uint32_t)wide;
        uint32_t expected_flags = (result & ARM_CPSR_N)
                                | (result == 0u ? ARM_CPSR_Z : 0u)
                                | ((wide >> 32) != 0u ? ARM_CPSR_C : 0u)
                                | (signed_wide > 2147483647ll ||
                                   signed_wide < -2147483648ll ? ARM_CPSR_V : 0u);

        CHECK(arm_step(&c) == ARM_OK && c.r[0] == result,
              "%s result=%08x expect %08x", arithmetic[i].name, c.r[0], result);
        CHECK(c.cpsr == (preserved | expected_flags),
              "%s CPSR=%08x expect %08x (changed non-NZCV bits=%08x)",
              arithmetic[i].name, c.cpsr, preserved | expected_flags,
              (c.cpsr ^ preserved) & ~nzcv_mask);
    }

    static const struct {
        const char *name;
        uint32_t insn, r1, r2, result, nzc;
        bool input_carry;
    } logical[] = {
        /* MOVS r0,r2,LSL #1 takes C from bit 31 of r2. */
        { "MOVS zero", 0xe1b00082u, 0u, 0x80000000u, 0u,
          ARM_CPSR_Z | ARM_CPSR_C, false },
        { "MOVS negative", 0xe1b00082u, 0u, 0x40000000u, 0x80000000u,
          ARM_CPSR_N, true },
        /* ANDS with LSL #0 preserves the incoming shifter carry. */
        { "ANDS", 0xe0110002u, 0xf0u, 0x0fu, 0u,
          ARM_CPSR_Z | ARM_CPSR_C, true },
    };

    for (size_t i = 0; i < sizeof logical / sizeof logical[0]; i++) {
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0u, logical[i].insn);
        arm_cpu_t c;
        arm_reset(&c, &g_bus);
        c.cpsr = preserved | ARM_CPSR_V |
                 ((~logical[i].nzc) & (ARM_CPSR_N | ARM_CPSR_Z)) |
                 (logical[i].input_carry ? ARM_CPSR_C : 0u);
        c.r[1] = logical[i].r1;
        c.r[2] = logical[i].r2;

        CHECK(arm_step(&c) == ARM_OK && c.r[0] == logical[i].result,
              "%s result=%08x expect %08x", logical[i].name,
              c.r[0], logical[i].result);
        CHECK(c.cpsr == (preserved | ARM_CPSR_V | logical[i].nzc),
              "%s CPSR=%08x expect %08x", logical[i].name, c.cpsr,
              preserved | ARM_CPSR_V | logical[i].nzc);
    }
}

static void test_barrel_lsl(void) {
    /* MOV r1,#1 ; MOV r0, r1, LSL #4 -> 16 */
    uint32_t p[] = { 0xe3a01001, 0xe1a00201 };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 16, "r0=%u expect 16", c.r[0]);
}

static void test_register_shifted_pc_operands_are_unpredictable(void) {
    static const struct { uint32_t insn; const char *what; } cases[] = {
        { 0xe1a0011fu, "Rm=pc" }, /* MOV r0,pc,LSL r1 */
        { 0xe1a00f11u, "Rs=pc" }, /* MOV r0,r1,LSL pc */
        { 0xe08f0211u, "Rn=pc" }, /* ADD r0,pc,r1,LSL r2 */
        { 0xe080f211u, "Rd=pc" }, /* ADD pc,r0,r1,LSL r2 */
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        arm_cpu_t c;
        uint32_t before[16], cpsr;
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0u, cases[i].insn);
        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS | ARM_CPSR_C;
        c.r[0] = 0x11111111u; c.r[1] = 3u; c.r[2] = 1u; c.r[15] = 0u;
        memcpy(before, c.r, sizeof before);
        cpsr = c.cpsr;
        CHECK(arm_step(&c) == ARM_UNDEFINED,
              "register-shifted data processing with %s must trap", cases[i].what);
        CHECK(memcmp(before, c.r, sizeof before) == 0 && c.cpsr == cpsr,
              "register-shifted %s changed registers or flags before trapping",
              cases[i].what);
    }
}

static void test_str_and_stm_store_pc_plus_12(void) {
    arm_cpu_t c;

    /* STR pc,[r0] at 0x100 stores 0x10c, not the ordinary visible PC 0x108. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x100u, 0xe580f000u);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.r[0] = 0x800u;
    c.r[15] = 0x100u;
    CHECK(arm_step(&c) == ARM_OK, "STR pc should execute");
    CHECK(m_r32(NULL, 0x800u) == 0x10cu,
          "STR pc stored %08x expect 0000010c", m_r32(NULL, 0x800u));

    /* The same exceptional PC value applies when r15 appears in an STM list. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x100u, 0xe8808000u);             /* STMIA r0,{pc} */
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.r[0] = 0x800u;
    c.r[15] = 0x100u;
    CHECK(arm_step(&c) == ARM_OK, "STMIA {pc} should execute");
    CHECK(m_r32(NULL, 0x800u) == 0x10cu,
          "STM pc stored %08x expect 0000010c", m_r32(NULL, 0x800u));
}

static void test_branch(void) {
    /* B +8 (skip one), then MOV r0,#1 (skipped), then MOV r0,#7 */
    /* At pc=0: B to pc=0+8+off. We want to land at word index 2 (addr 8).
       target = pc+8+off = 0+8+off = 8 => off=0 -> encoding e a 000000 */
    uint32_t p[] = { 0xea000000, /* B #8 -> lands at 0x8 */
                     0xe3a00001, /* MOV r0,#1 (skipped) */
                     0xe3a00007  /* MOV r0,#7 */ };
    arm_cpu_t c; load_and_run(&c, p, 3, 2);
    CHECK(c.r[0] == 7, "r0=%u expect 7 (branch skipped the #1)", c.r[0]);
}

static void test_bl_sets_lr(void) {
    /* BL to somewhere; check LR = pc+4 */
    uint32_t p[] = { 0xeb000002 /* BL #... */ };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[14] == 4, "lr=%u expect 4", c.r[14]);
}

static void test_ldr_str(void) {
    /* MOV r0,#0xAB ; MOV r1,#0x400 ; STR r0,[r1] ; LDR r2,[r1] */
    uint32_t p[] = { 0xe3a000ab, 0xe3a01b01 /*MOV r1,#0x400*/,
                     0xe5810000 /*STR r0,[r1]*/, 0xe5912000 /*LDR r2,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 0xab, "r2=%08x expect ab", c.r[2]);
    CHECK(m_r32(NULL, 0x400) == 0xab, "mem[0x400]=%08x expect ab", m_r32(NULL,0x400));
}

static void test_ldrb(void) {
    /* Load the word 0x11223344 from a PC-relative literal, store it at 0x500,
     * then LDRB from 0x500 -> 0x44, proving little-endian byte extraction. */
    uint32_t p[] = { 0xe3a01c05 /*MOV  r1,#0x500        addr 0 */,
                     0xe59f0004 /*LDR  r0,[pc,#4]       addr 4 (pc+8=0xC, +4=0x10) */,
                     0xe5810000 /*STR  r0,[r1]          addr 8 */,
                     0xe5d12000 /*LDRB r2,[r1]          addr 0xC */,
                     0x11223344 /*literal               addr 0x10 */ };
    arm_cpu_t c; load_and_run(&c, p, 5, 4);
    CHECK(c.r[0] == 0x11223344u, "r0=%08x expect 11223344", c.r[0]);
    CHECK(c.r[2] == 0x44, "r2=%02x expect 44 (LE low byte)", c.r[2]);
}

static void test_cond_not_taken(void) {
    /* MOVEQ r0,#9 with Z clear -> not executed; r0 stays 0 */
    uint32_t p[] = { 0x03a00009 /*MOVEQ r0,#9*/ };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[0] == 0, "r0=%u expect 0 (cond failed)", c.r[0]);
}

static void test_mul(void) {
    /* MOV r1,#6 ; MOV r2,#7 ; MUL r0,r1,r2 -> 42  (MUL rd,rm,rs: e0000291) */
    uint32_t p[] = { 0xe3a01006, 0xe3a02007, 0xe0000291 };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 42, "r0=%u expect 42", c.r[0]);
}

/* ARMv5TE's halfword multiply encodings are easy to transpose: x (bit 5)
 * selects Rm while y (bit 6) selects Rs.  Use four different signed halves so
 * every selector combination has a distinct answer. */
static void test_dsp_smul_halfword_selectors_and_real_alias(void) {
    static const struct {
        uint32_t insn, expect;
        const char *name;
    } cases[] = {
        { 0xe1600281u, 0xfffffff1u, "SMULBB" }, /*  3 * -5 = -15 */
        { 0xe16002a1u, 0x0000000au, "SMULTB" }, /* -2 * -5 =  10 */
        { 0xe16002c1u, 0x0000000cu, "SMULBT" }, /*  3 *  4 =  12 */
        { 0xe16002e1u, 0xfffffff8u, "SMULTT" }, /* -2 *  4 =  -8 */
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        arm_cpu_t c;
        load_and_run(&c, &cases[i].insn, 1, 0);
        c.r[1] = 0xfffe0003u;
        c.r[2] = 0x0004fffbu;
        c.cpsr |= ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q;
        uint32_t flags = c.cpsr;
        arm_status_t st = arm_step(&c);
        CHECK(st == ARM_OK && c.r[0] == cases[i].expect,
              "%s status=%d result=%08x expect %08x", cases[i].name,
              (int)st, c.r[0], cases[i].expect);
        CHECK(c.cpsr == flags, "%s changed CPSR %08x -> %08x",
              cases[i].name, flags, c.cpsr);
    }

    /* Exact instruction and live register values at the current real-guest
     * stop.  Rd aliases Rs, so all sources must be read before writeback. */
    const uint32_t blocker = 0xe1630381u;       /* SMULBB r3,r1,r3 */
    arm_cpu_t c;
    load_and_run(&c, &blocker, 1, 0);
    c.r[1] = 1u;
    c.r[3] = 0x34u;
    CHECK(arm_step(&c) == ARM_OK && c.r[3] == 0x34u && c.r[15] == 4u,
          "real SMULBB alias result=%08x pc=%08x", c.r[3], c.r[15]);
}

static void test_dsp_smla_wraps_and_sets_sticky_q(void) {
    const uint32_t smlabb = 0xe1003281u;        /* SMLABB r0,r1,r2,r3 */
    arm_cpu_t c;

    load_and_run(&c, &smlabb, 1, 0);
    c.r[1] = 1u; c.r[2] = 1u; c.r[3] = 0x7fffffffu;
    c.cpsr |= ARM_CPSR_N | ARM_CPSR_C;
    uint32_t nzcv = c.cpsr & (ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x80000000u,
          "positive-overflow SMLABB result=%08x", c.r[0]);
    CHECK((c.cpsr & ARM_CPSR_Q) != 0u &&
          (c.cpsr & (ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V)) == nzcv,
          "positive-overflow SMLABB flags=%08x", c.cpsr);

    load_and_run(&c, &smlabb, 1, 0);
    c.r[1] = 0xffffu; c.r[2] = 1u; c.r[3] = 0x80000000u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x7fffffffu &&
          (c.cpsr & ARM_CPSR_Q) != 0u,
          "negative-overflow SMLABB result=%08x flags=%08x", c.r[0], c.cpsr);

    /* Q is sticky, and an accumulator may alias the destination. */
    const uint32_t alias = 0xe1033281u;         /* SMLABB r3,r1,r2,r3 */
    load_and_run(&c, &alias, 1, 0);
    c.r[1] = 2u; c.r[2] = 3u; c.r[3] = 4u;
    c.cpsr |= ARM_CPSR_Q | ARM_CPSR_Z | ARM_CPSR_V;
    uint32_t before = c.cpsr;
    CHECK(arm_step(&c) == ARM_OK && c.r[3] == 10u,
          "aliased SMLABB result=%08x", c.r[3]);
    CHECK(c.cpsr == before, "non-overflow SMLABB changed sticky flags %08x -> %08x",
          before, c.cpsr);
}

static void test_dsp_smlal_sign_carry_wrap_and_alias(void) {
    const uint32_t smlalbb = 0xe1410382u;       /* SMLALBB r0,r1,r2,r3 */
    static const struct {
        uint32_t lo, hi, rm, rs, expect_lo, expect_hi;
        const char *name;
    } cases[] = {
        { 0xffffffffu, 0u, 1u, 1u, 0u, 1u, "low-word carry" },
        { 0u, 0u, 0xffffu, 1u, 0xffffffffu, 0xffffffffu, "negative sign extension" },
        { 0xffffffffu, 0xffffffffu, 1u, 1u, 0u, 0u, "modulo-2^64 wrap" },
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        arm_cpu_t c;
        load_and_run(&c, &smlalbb, 1, 0);
        c.r[0] = cases[i].lo; c.r[1] = cases[i].hi;
        c.r[2] = cases[i].rm; c.r[3] = cases[i].rs;
        c.cpsr |= ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V | ARM_CPSR_Q;
        uint32_t flags = c.cpsr;
        arm_status_t st = arm_step(&c);
        CHECK(st == ARM_OK && c.r[0] == cases[i].expect_lo &&
              c.r[1] == cases[i].expect_hi,
              "SMLALBB %s status=%d got=%08x:%08x expect=%08x:%08x",
              cases[i].name, (int)st, c.r[1], c.r[0],
              cases[i].expect_hi, cases[i].expect_lo);
        CHECK(c.cpsr == flags, "SMLALBB %s changed flags", cases[i].name);
    }

    /* Rm aliases RdLo. The old low accumulator value is both an input to the
     * multiplication and part of the 64-bit add. */
    const uint32_t alias = 0xe1410280u;         /* SMLALBB r0,r1,r0,r2 */
    arm_cpu_t c;
    load_and_run(&c, &alias, 1, 0);
    c.r[0] = 2u; c.r[1] = 0u; c.r[2] = 3u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 8u && c.r[1] == 0u,
          "aliased SMLALBB got=%08x:%08x", c.r[1], c.r[0]);
}

static void test_dsp_word_halfword_extract_and_accumulate(void) {
    const uint32_t smulwb = 0xe12002a1u;        /* SMULWB r0,r1,r2 */
    const uint32_t smulwt = 0xe12002e1u;        /* SMULWT r0,r1,r2 */
    arm_cpu_t c;

    /* Bits[47:16] of -1 are all ones. C division by 65536 would incorrectly
     * truncate toward zero here, so this pins the required arithmetic extract. */
    load_and_run(&c, &smulwb, 1, 0);
    c.r[1] = 0xffffffffu; c.r[2] = 1u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0xffffffffu,
          "SMULWB(-1,1)=%08x expect ffffffff", c.r[0]);

    load_and_run(&c, &smulwb, 1, 0);
    c.r[1] = 0x80000000u; c.r[2] = 0x8000u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x40000000u,
          "SMULWB(INT_MIN,-32768)=%08x expect 40000000", c.r[0]);

    load_and_run(&c, &smulwt, 1, 0);
    c.r[1] = 0x00010000u; c.r[2] = 0xfffe0001u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0xfffffffeu,
          "SMULWT top-half result=%08x expect fffffffe", c.r[0]);

    const uint32_t smlawb = 0xe1203281u;        /* SMLAWB r0,r1,r2,r3 */
    load_and_run(&c, &smlawb, 1, 0);
    c.r[1] = 0x00010000u; c.r[2] = 1u; c.r[3] = 0x7fffffffu;
    c.cpsr |= ARM_CPSR_N | ARM_CPSR_C;
    uint32_t nzcv = c.cpsr & (ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x80000000u &&
          (c.cpsr & ARM_CPSR_Q) != 0u,
          "overflowing SMLAWB result=%08x flags=%08x", c.r[0], c.cpsr);
    CHECK((c.cpsr & (ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V)) == nzcv,
          "SMLAWB changed NZCV flags=%08x", c.cpsr);

    const uint32_t smlawt = 0xe12032c1u;        /* SMLAWT r0,r1,r2,r3 */
    load_and_run(&c, &smlawt, 1, 0);
    c.r[1] = 0x00010000u; c.r[2] = 0x00010000u; c.r[3] = 5u;
    c.cpsr |= ARM_CPSR_Q | ARM_CPSR_Z | ARM_CPSR_V;
    uint32_t flags = c.cpsr;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 6u && c.cpsr == flags,
          "non-overflow SMLAWT result=%08x flags=%08x", c.r[0], c.cpsr);
}

static void test_dsp_multiply_unpredictable_and_reserved_forms_trap(void) {
    /* Every named R15 operand, SMLAL's equal destination pair, and reserved
     * near-misses must stop without corrupting registers or flags. */
#define SMLA(rd,rn,rs,rm)  (0xe1000080u | ((rd)<<16) | ((rn)<<12) | ((rs)<<8) | (rm))
#define SMLAL(hi,lo,rs,rm) (0xe1400080u | ((hi)<<16) | ((lo)<<12) | ((rs)<<8) | (rm))
#define SMLAW(rd,rn,rs,rm) (0xe1200080u | ((rd)<<16) | ((rn)<<12) | ((rs)<<8) | (rm))
#define SMUL(rd,rs,rm)     (0xe1600080u | ((rd)<<16) | ((rs)<<8) | (rm))
#define SMULW(rd,rs,rm)    (0xe12000a0u | ((rd)<<16) | ((rs)<<8) | (rm))
    static const uint32_t bad[] = {
        SMLA(15u,4u,2u,1u), SMLA(3u,15u,2u,1u),
        SMLA(3u,4u,15u,1u), SMLA(3u,4u,2u,15u),
        SMLAL(15u,4u,2u,1u), SMLAL(3u,15u,2u,1u),
        SMLAL(3u,4u,15u,1u), SMLAL(3u,4u,2u,15u),
        SMLAL(3u,3u,2u,1u),
        SMLAW(15u,4u,2u,1u), SMLAW(3u,15u,2u,1u),
        SMLAW(3u,4u,15u,1u), SMLAW(3u,4u,2u,15u),
        SMUL(15u,2u,1u), SMUL(3u,15u,1u), SMUL(3u,2u,15u),
        SMULW(15u,2u,1u), SMULW(3u,15u,1u), SMULW(3u,2u,15u),
        0xe1601281u,                         /* SMULxy with SBZ[15:12] != 0 */
        0xe12012a1u,                         /* SMULWy with SBZ[15:12] != 0 */
        0xe1600291u,                         /* reserved bit 4 set */
        0xe1600201u,                         /* required bit 7 clear */
        0xf1600281u,                         /* cond=1111 is not this instruction */
    };
#undef SMLA
#undef SMLAL
#undef SMLAW
#undef SMUL
#undef SMULW

    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        arm_cpu_t c;
        load_and_run(&c, &bad[i], 1, 0);
        for (unsigned r = 0; r < 15u; r++) c.r[r] = 0x11110000u + r;
        c.cpsr |= ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q;
        uint32_t regs[15]; memcpy(regs, c.r, sizeof regs);
        uint32_t cpsr = c.cpsr;
        arm_status_t st = arm_step(&c);
        CHECK(st == ARM_UNDEFINED, "bad DSP encoding %08x status=%d", bad[i], (int)st);
        CHECK(memcmp(regs, c.r, sizeof regs) == 0 && c.r[15] == 0u && c.cpsr == cpsr,
              "bad DSP encoding %08x changed architectural state", bad[i]);
    }

    /* A legal instruction with a failed condition is a true NOP. */
    const uint32_t eq = 0x01630381u;          /* SMULBBEQ r3,r1,r3 */
    arm_cpu_t c;
    load_and_run(&c, &eq, 1, 0);
    c.r[1] = 7u; c.r[3] = 9u;                /* Z is clear after reset */
    uint32_t cpsr = c.cpsr;
    CHECK(arm_step(&c) == ARM_OK && c.r[3] == 9u && c.r[15] == 4u &&
          c.cpsr == cpsr,
          "condition-failed DSP instruction was not a NOP");
}

static void test_orr_bic_mvn(void) {
    /* MOV r0,#0xF0 ; ORR r0,r0,#0x0F -> 0xFF ; BIC r0,r0,#0x0F -> 0xF0 ; MVN r1,#0 -> 0xffffffff */
    uint32_t p[] = { 0xe3a000f0, 0xe380000f, 0xe3c0000f, 0xe3e01000 };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[0] == 0xf0, "r0=%08x expect f0", c.r[0]);
    CHECK(c.r[1] == 0xffffffffu, "r1=%08x expect ffffffff", c.r[1]);
}

static void test_bx_branches(void) {
    /* MOV r1,#0x100 ; BX r1  -> PC = 0x100 (regression: BX must actually branch,
     * not silently execute as a TEQ comparison). */
    uint32_t p[] = { 0xe3a01c01 /*MOV r1,#0x100*/, 0xe12fff11 /*BX r1*/ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[15] == 0x100, "pc=%08x expect 100 (BX branched)", c.r[15]);
}

/*
 * Returning from an exception into Thumb code must land on the halfword the
 * SPSR says, not on the word below it.
 *
 * This one cost a real boot. The kernel's decrementer FIQ interrupts Thumb
 * code roughly half the time at an address that is 2 mod 4. Masking the
 * resume address with ~3 rewinds it by two bytes, so the guest re-executes
 * the *preceding* halfword. In xnu-1357 that turned a zone free into a jump
 * onto the locked path's tail, which then unlocked a mutex whose pointer was
 * a stale scratch value of 1 -- surfacing as a data abort at
 * _lck_mtx_unlock+0x8 with DFAR 0x1, a mile from the actual bug.
 *
 * The tell was statistical: every single one of 761 exception returns landed
 * 4-byte aligned, where hardware would be about half and half.
 */
static void test_exception_return_to_thumb_keeps_halfword(void) {
    uint32_t p[] = { 0xe1b0f00e };   /* MOVS pc, lr */
    arm_cpu_t c; load_and_run(&c, p, 1, 0);   /* load only; we set up state */

    arm_set_mode(&c, ARM_MODE_FIQ);
    c.spsr[arm_bank_of_mode(ARM_MODE_FIQ)] = ARM_MODE_SVC | ARM_CPSR_T;
    c.r[14] = 0x0000101a;            /* Thumb, and 2 mod 4 */
    c.r[15] = 0;
    arm_step(&c);

    CHECK(c.r[15] == 0x0000101a,
          "pc=%08x expect 101a (a ~3 mask would rewind it to 1018)", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_T) != 0, "should have resumed in Thumb state");
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "mode=%02x expect SVC restored from SPSR",
          c.cpsr & ARM_CPSR_MODE_MASK);
}

/* The same return into ARM code must still be word-aligned. */
static void test_exception_return_to_arm_stays_word_aligned(void) {
    uint32_t p[] = { 0xe1b0f00e };   /* MOVS pc, lr */
    arm_cpu_t c; load_and_run(&c, p, 1, 0);

    arm_set_mode(&c, ARM_MODE_FIQ);
    c.spsr[arm_bank_of_mode(ARM_MODE_FIQ)] = ARM_MODE_SVC;  /* T clear */
    c.r[14] = 0x0000101a;
    c.r[15] = 0;
    arm_step(&c);

    CHECK(c.r[15] == 0x00001018, "pc=%08x expect 1018 (ARM state aligns to 4)",
          c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "should have resumed in ARM state");
}

/*
 * LDM {..., pc}^ is an exception return, and differs from a plain POP {..,pc}
 * in two ways that are easy to get wrong together: bit 0 of the loaded word is
 * part of the address rather than a Thumb selector, and the alignment must
 * follow the T bit the SPSR restores. Doing the PC write before the restore
 * gets both wrong.
 */
static void test_ldm_exception_return_takes_state_from_spsr(void) {
    /* LDMIA sp, {pc}^  ->  0xe8dd8000 */
    uint32_t p[] = { 0xe8dd8000u };
    arm_cpu_t c; load_and_run(&c, p, 1, 0);

    arm_set_mode(&c, ARM_MODE_IRQ);
    c.spsr[arm_bank_of_mode(ARM_MODE_IRQ)] = ARM_MODE_SVC | ARM_CPSR_T;
    c.r[13] = 0x800;
    c.bus->write32(c.bus->ctx, 0x800, 0x0000101au);  /* even: no Thumb bit set */
    c.r[15] = 0;
    arm_step(&c);

    CHECK(c.r[15] == 0x0000101a, "pc=%08x expect 101a (halfword, from SPSR.T)",
          c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_T) != 0,
          "T must come from the SPSR, not from bit 0 of the loaded word");
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "mode=%02x expect SVC restored from SPSR",
          c.cpsr & ARM_CPSR_MODE_MASK);

    /* In ARM state bit 1 cannot be represented. LDM(3) must reject the frame
     * after reading it but before committing PC, writeback, CPSR, or any other
     * loaded register. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe8fd8001u);               /* LDMIA sp!,{r0,pc}^ */
    m_w32(NULL, 0x800u, 0xfeedfaceu);
    m_w32(NULL, 0x804u, 0x0000101au);           /* invalid ARM halfword */
    arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_IRQ);
    c.spsr[ARM_BANK_IRQ] = ARM_MODE_SVC;         /* T clear */
    c.r[0] = 0x11111111u;
    c.r[13] = 0x800u;
    c.r[15] = 0u;
    uint32_t before_cpsr = c.cpsr;
    CHECK(arm_step(&c) == ARM_UNDEFINED,
          "LDM(3) accepted an ARM-state target with bit 1 set");
    CHECK(c.r[0] == 0x11111111u && c.r[13] == 0x800u && c.r[15] == 0u &&
          c.cpsr == before_cpsr,
          "invalid LDM(3) target committed registers/writeback/CPSR");
}

/* RFE restores PC and CPSR from memory; the same alignment rule applies. */
static void test_rfe_aligns_for_the_restored_state(void) {
    /* RFEIA r0 -> 0xf8900a00 (P=0, U=1: read from [r0], not [r0-4]) */
    uint32_t p[] = { 0xf8900a00u };
    arm_cpu_t c; load_and_run(&c, p, 1, 0);

    arm_set_mode(&c, ARM_MODE_IRQ);
    c.r[0] = 0x900;
    c.bus->write32(c.bus->ctx, 0x900, 0x00001018u);      /* aligned ARM pc */
    c.bus->write32(c.bus->ctx, 0x904, ARM_MODE_SVC);     /* new cpsr, T clear */
    c.r[15] = 0;
    arm_step(&c);

    CHECK(c.r[15] == 0x00001018,
          "pc=%08x expect 1018 (ARM state must align to a word)", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "should have resumed in ARM state");

    /* The same frame with bit 1 set is UNPREDICTABLE, not rounded down. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xf8b00a00u);                       /* RFEIA r0! */
    m_w32(NULL, 0x900u, 0x0000101au);
    m_w32(NULL, 0x904u, ARM_MODE_SVC);
    arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_IRQ);
    c.r[0] = 0x900u;
    c.r[15] = 0u;
    uint32_t before_cpsr = c.cpsr;
    CHECK(arm_step(&c) == ARM_UNDEFINED,
          "RFE accepted an ARM-state target with bit 1 set");
    CHECK(c.r[0] == 0x900u && c.r[15] == 0u && c.cpsr == before_cpsr,
          "invalid RFE target committed base/PC/CPSR");
}

static void test_srs_and_rfe_reject_unprivileged_execution(void) {
    arm_cpu_t c;

    /* A User-mode SRS must be rejected before touching the target mode's stack. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xf8cd0513u);                  /* SRSIA sp,#SVC */
    arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_USR);
    c.bank_r13[ARM_BANK_SVC] = 0x900u;
    c.r[14] = 0x11223344u;
    c.r[15] = 0;
    g_watch_addr = 0x900u; g_watch_reads32 = g_watch_writes32 = 0;
    CHECK(arm_step(&c) == ARM_UNDEFINED, "User-mode SRS must be undefined");
    CHECK(g_watch_writes32 == 0u, "User-mode SRS performed %u target-stack writes",
          g_watch_writes32);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR,
          "User-mode SRS changed mode to %02x", c.cpsr & ARM_CPSR_MODE_MASK);

    /* RFE can replace every CPSR mode bit, so privilege must be checked before
     * even reading its attacker-controlled frame (which may be MMIO). */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xf8900a00u);                  /* RFEIA r0 */
    m_w32(NULL, 0x900u, 0x1000u);
    m_w32(NULL, 0x904u, ARM_MODE_SVC);
    arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_USR);
    c.r[0] = 0x900u;
    c.r[15] = 0;
    g_watch_addr = 0x900u; g_watch_reads32 = g_watch_writes32 = 0;
    CHECK(arm_step(&c) == ARM_UNDEFINED, "User-mode RFE must be undefined");
    CHECK(g_watch_reads32 == 0u, "User-mode RFE performed %u frame reads",
          g_watch_reads32);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR,
          "User-mode RFE escalated to mode %02x", c.cpsr & ARM_CPSR_MODE_MASK);

    /* System mode is privileged but has no SPSR; fabricating one from CPSR made
     * SRS silently wrong. Refuse that architecturally unpredictable form. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xf8cd0513u);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.bank_r13[ARM_BANK_SVC] = 0x900u;
    c.r[15] = 0;
    g_watch_addr = 0x900u; g_watch_reads32 = g_watch_writes32 = 0;
    CHECK(arm_step(&c) == ARM_UNDEFINED, "System-mode SRS must be refused");
    CHECK(g_watch_writes32 == 0u, "System-mode SRS performed %u target-stack writes",
          g_watch_writes32);

    g_watch_addr = 0xffffffffu;
}

static void test_exception_returns_reject_invalid_modes_before_mutation(void) {
    arm_cpu_t c;

    /* An invalid current mode must not alias to the User bank and then execute
     * with the privilege checks accidentally passing. Reject it before fetch. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe1a00000u);                 /* NOP */
    arm_reset(&c, &g_bus);
    c.cpsr &= ~ARM_CPSR_MODE_MASK;                /* unimplemented mode 0 */
    c.r[15] = 0u;
    g_watch_addr = 0u; g_watch_reads32 = g_watch_writes32 = 0u;
    CHECK(arm_step(&c) == ARM_UNDEFINED, "invalid current CPSR mode must trap");
    CHECK(g_watch_reads32 == 0u, "invalid current mode fetched %u instructions",
          g_watch_reads32);

    /* R15 is not a legal RFE base. Refuse it before reading the PC-relative
     * address that the generic register helper would otherwise synthesize. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xf89f0a00u);                 /* RFEIA pc (UNPREDICTABLE) */
    arm_reset(&c, &g_bus);
    c.r[15] = 0u;
    g_watch_addr = 8u; g_watch_reads32 = g_watch_writes32 = 0u;
    CHECK(arm_step(&c) == ARM_UNDEFINED, "RFE with Rn=pc must be refused");
    CHECK(g_watch_reads32 == 0u, "RFE pc issued %u frame reads", g_watch_reads32);

    /* A malformed frame must not rebank through arm_bank_of_mode's historical
     * default-to-User fallback or apply writeback before it is validated. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xf8b00a00u);                 /* RFEIA r0! */
    m_w32(NULL, 0x900u, 0x1000u);
    m_w32(NULL, 0x904u, 0u);                      /* invalid restored mode */
    arm_reset(&c, &g_bus);
    c.r[0] = 0x900u;
    c.r[15] = 0u;
    uint32_t before = c.cpsr;
    CHECK(arm_step(&c) == ARM_UNDEFINED, "RFE accepted an invalid CPSR mode");
    CHECK(c.cpsr == before && c.r[15] == 0u && c.r[0] == 0x900u,
          "invalid RFE mutated cpsr/pc/base: %08x/%08x/%08x",
          c.cpsr, c.r[15], c.r[0]);

    /* SRS's target mode field is also guest-controlled. Invalid modes must be
     * rejected before selecting a bank or exposing exception state to memory. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xf8cd0500u);                 /* SRSIA sp,#invalid-0 */
    arm_reset(&c, &g_bus);
    c.bank_r13[ARM_BANK_USR] = 0x900u;
    c.r[15] = 0u;
    g_watch_addr = 0x900u; g_watch_reads32 = g_watch_writes32 = 0u;
    CHECK(arm_step(&c) == ARM_UNDEFINED, "SRS accepted an invalid target mode");
    CHECK(g_watch_writes32 == 0u, "invalid SRS issued %u state writes",
          g_watch_writes32);

    /* LDM^ and data-processing exception returns both source the mode from an
     * SPSR. They must validate it before memory or arithmetic has side effects. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe8d08000u);                 /* LDMIA r0,{pc}^ */
    arm_reset(&c, &g_bus);
    c.spsr[ARM_BANK_SVC] = 0u;
    c.r[0] = 0x900u;
    c.r[15] = 0u;
    g_watch_addr = 0x900u; g_watch_reads32 = g_watch_writes32 = 0u;
    CHECK(arm_step(&c) == ARM_UNDEFINED, "LDM^ accepted an invalid SPSR mode");
    CHECK(g_watch_reads32 == 0u, "invalid LDM^ issued %u frame reads",
          g_watch_reads32);

    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe25ef004u);                 /* SUBS pc,lr,#4 */
    arm_reset(&c, &g_bus);
    c.spsr[ARM_BANK_SVC] = 0u;
    c.r[14] = 0x1000u;
    c.r[15] = 0u;
    before = c.cpsr;
    CHECK(arm_step(&c) == ARM_UNDEFINED,
          "data-processing exception return accepted invalid SPSR mode");
    CHECK(c.cpsr == before && c.r[15] == 0u,
          "invalid SUBS pc mutated cpsr/pc: %08x/%08x", c.cpsr, c.r[15]);
    g_watch_addr = 0xffffffffu;
}

/*
 * SETEND LE is a no-op for a little-endian machine and must execute. SETEND BE
 * must keep trapping: we do not model a big-endian data path, so honouring it
 * would silently corrupt every subsequent load.
 */
static void test_setend_le_runs_be_traps(void) {
    uint32_t le[] = { 0xf1010000u, 0xe3a0002au };   /* SETEND LE ; MOV r0,#42 */
    arm_cpu_t c; load_and_run(&c, le, 2, 2);
    CHECK(c.r[0] == 42, "r0=%u expect 42 (SETEND LE must be a no-op)", c.r[0]);

    uint32_t be[] = { 0xf1010200u };                /* SETEND BE */
    arm_cpu_t d; arm_status_t st = run_status(&d, be, 1, 1);
    CHECK(st == ARM_UNDEFINED,
          "status=%d expect ARM_UNDEFINED for SETEND BE", (int)st);
}

/*
 * The 64-bit multiplies. A driver in the real 3.1.3 kernelcache stopped the
 * boot on a plain UMULL, so these are ordinary compiled code rather than an
 * exotic corner. The signed/unsigned distinction is the part worth pinning:
 * doing both in 64-bit unsigned yields the correct low word and a silently
 * wrong high word, which is the kind of error that surfaces a long way away.
 */
static void test_umull_and_smull(void) {
    /* UMULL r0,r1,r2,r3 with 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE00000001 */
    uint32_t p[] = { 0xe3e02000 /* MVN r2,#0  -> 0xffffffff */,
                     0xe3e03000 /* MVN r3,#0  -> 0xffffffff */,
                     0xe0810392 /* UMULL r0,r1,r2,r3        */ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 0x00000001u, "lo=%08x expect 00000001", c.r[0]);
    CHECK(c.r[1] == 0xfffffffeu, "hi=%08x expect fffffffe", c.r[1]);

    /* SMULL of the same bit patterns is (-1) * (-1) = 1, so the high word is
     * ZERO. Same inputs, different answer — this is the check that catches an
     * unsigned implementation of the signed form. */
    uint32_t q[] = { 0xe3e02000, 0xe3e03000,
                     0xe0c10392 /* SMULL r0,r1,r2,r3 */ };
    arm_cpu_t d; load_and_run(&d, q, 3, 3);
    CHECK(d.r[0] == 0x00000001u, "lo=%08x expect 00000001 (-1 * -1)", d.r[0]);
    CHECK(d.r[1] == 0x00000000u,
          "hi=%08x expect 00000000 — a signed multiply done unsigned gives "
          "fffffffe here", d.r[1]);
}

static void test_umlal_accumulates(void) {
    /* r0:r1 = 5, then UMLAL r0,r1,r2,r3 with 3 * 7 -> 26 */
    uint32_t p[] = { 0xe3a00005 /* MOV r0,#5 */,
                     0xe3a01000 /* MOV r1,#0 */,
                     0xe3a02003 /* MOV r2,#3 */,
                     0xe3a03007 /* MOV r3,#7 */,
                     0xe0a10392 /* UMLAL r0,r1,r2,r3 */ };
    arm_cpu_t c; load_and_run(&c, p, 5, 5);
    CHECK(c.r[0] == 26, "lo=%u expect 26 (5 + 3*7)", c.r[0]);
    CHECK(c.r[1] == 0,  "hi=%u expect 0", c.r[1]);
}

static void test_clz(void) {
    /* CLZ r0,r1 -> e16f0f11.  Compilers emit this constantly. */
    uint32_t p[] = { 0xe3a01001 /* MOV r1,#1        */, 0xe16f0f11 /* CLZ r0,r1 */,
                     0xe3a01000 /* MOV r1,#0        */, 0xe16f2f11 /* CLZ r2,r1 */,
                     0xe3e01000 /* MVN r1,#0 -> ~0  */, 0xe16f3f11 /* CLZ r3,r1 */ };
    arm_cpu_t c; load_and_run(&c, p, 6, 6);
    CHECK(c.r[0] == 31, "clz(1)=%u expect 31", c.r[0]);
    CHECK(c.r[2] == 32, "clz(0)=%u expect 32 — the case that tempts a loop bug", c.r[2]);
    CHECK(c.r[3] == 0,  "clz(0xffffffff)=%u expect 0", c.r[3]);
}

/*
 * Saturating arithmetic clamps instead of wrapping, and sets the sticky Q
 * flag. Wrapping would be silently correct everywhere except the extremes,
 * which is exactly where these instructions are used.
 */
static void test_qadd_saturates_and_sets_q(void) {
    /* r1 = 0x7fffffff, r2 = 1, QADD r0,r2,r1  (Rd, Rm, Rn) */
    uint32_t p[] = { 0xe3e01102 /* MVN r1,#0x80000000 -> 0x7fffffff */,
                     0xe3a02001 /* MOV r2,#1                        */,
                     0xe1010052 /* QADD r0,r2,r1                    */ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 0x7fffffffu,
          "r0=%08x expect 7fffffff (clamped, not wrapped to 80000000)", c.r[0]);
    CHECK((c.cpsr & ARM_CPSR_Q) != 0, "Q must be set on saturation");

    /* Without saturation Q must stay clear and the result is ordinary. */
    uint32_t q[] = { 0xe3a01005 /* MOV r1,#5 */, 0xe3a02003 /* MOV r2,#3 */,
                     0xe1010052 /* QADD r0,r2,r1 */ };
    arm_cpu_t d; load_and_run(&d, q, 3, 3);
    CHECK(d.r[0] == 8, "r0=%u expect 8", d.r[0]);
    CHECK((d.cpsr & ARM_CPSR_Q) == 0, "Q must not be set without saturation");
}

static void test_swp_exchanges(void) {
    /* SWP is an atomic read-then-write: Rd gets the old memory word and the
     * new value comes from Rm. Getting the order wrong (writing before
     * reading) silently returns the value just stored, which looks correct
     * whenever Rd and Rm happen to be the same register. */
    uint32_t p[] = { 0xe3a01b02 /* MOV r1,#0x800   */,
                     0xe3a000aa /* MOV r0,#0xAA    */,
                     0xe5810000 /* STR r0,[r1]     seed memory with 0xAA */,
                     0xe3a004bb /* MOV r0,#0xBB000000 */,
                     0xe1012090 /* SWP r2,r0,[r1]  */,
                     0xe5913000 /* LDR r3,[r1]     */ };
    arm_cpu_t c; load_and_run(&c, p, 6, 6);
    CHECK(c.r[2] == 0xaa, "r2=%08x expect aa (SWP returns the OLD word)", c.r[2]);
    CHECK(c.r[3] == 0xbb000000, "r3=%08x expect bb000000 (SWP stored Rm)", c.r[3]);
}

static void test_swp_legacy_unaligned_word_semantics(void) {
    arm_cpu_t c;

    /* U=0,A=0 defines SWP as WLoad followed by WStore: align down, rotate the
     * loaded word by the byte offset, then store the new word unrotated at the
     * aligned address. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe1012090u);              /* SWP r2,r0,[r1] */
    m_w32(NULL, 0x800u, 0x11223344u);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.cp15.sctlr &= ~(ARM_SCTLR_U | ARM_SCTLR_A);
    c.r[0] = 0xa1b2c3d4u;
    c.r[1] = 0x801u;
    g_watch_addr = 0x800u;
    g_watch_reads32 = g_watch_writes32 = 0u;
    CHECK(arm_step(&c) == ARM_OK && c.r[2] == 0x44112233u,
          "legacy unaligned SWP returned %08x", c.r[2]);
    CHECK(g_watch_reads32 == 1u && g_watch_writes32 == 1u,
          "legacy SWP issued reads=%u writes=%u at aligned address",
          g_watch_reads32, g_watch_writes32);
    g_watch_addr = 0xffffffffu;
    CHECK(m_r32(NULL, 0x800u) == 0xa1b2c3d4u,
          "legacy SWP stored %08x", m_r32(NULL, 0x800u));

    /* Either modern unaligned control makes a misaligned SWP alignment-fault
     * before its read half; the write nature is preserved in DFSR.WnR. */
    for (unsigned mode = 1u; mode <= 3u; mode++) {
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0u, 0xe1012090u);
        m_w32(NULL, 0x800u, 0x11223344u);
        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
        c.cp15.sctlr &= ~(ARM_SCTLR_U | ARM_SCTLR_A);
        if (mode & 1u) c.cp15.sctlr |= ARM_SCTLR_U;
        if (mode & 2u) c.cp15.sctlr |= ARM_SCTLR_A;
        c.r[0] = 0xa1b2c3d4u;
        c.r[1] = 0x801u;
        g_watch_addr = 0x800u;
        g_watch_reads32 = g_watch_writes32 = 0u;
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT &&
              (c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT &&
              (c.cp15.dfsr & (1u << 11)) != 0u && c.cp15.dfar == 0x801u,
              "misaligned SWP mode=%u did not alignment-fault", mode);
        CHECK(g_watch_reads32 == 0u && g_watch_writes32 == 0u,
              "faulting SWP mode=%u touched memory", mode);
    }
    g_watch_addr = 0xffffffffu;
}

/*
 * LDREXD/STREXD: the doubleword exclusive pair the kernel's 64-bit atomics
 * use. OSAddAtomic64 is the first one a real boot reaches, so this encoding
 * is load-bearing rather than obscure.
 */
static void test_ldrexd_strexd_roundtrip(void) {
    uint32_t p[] = { 0xe3a01b02 /* MOV r1,#0x800        */,
                     0xe3a00012 /* MOV r0,#0x12         */,
                     0xe5810000 /* STR r0,[r1]          low word  */,
                     0xe3a00034 /* MOV r0,#0x34         */,
                     0xe5810004 /* STR r0,[r1,#4]       high word */,
                     0xe1b14f9f /* LDREXD r4,r5,[r1]    */,
                     0xe3a06056 /* MOV r6,#0x56         */,
                     0xe3a07078 /* MOV r7,#0x78         */,
                     0xe1a13f96 /* STREXD r3,r6,r7,[r1] */,
                     0xe5918000 /* LDR r8,[r1]          */,
                     0xe5919004 /* LDR r9,[r1,#4]       */ };
    arm_cpu_t c; load_and_run(&c, p, 11, 11);
    CHECK(c.r[4] == 0x12, "r4=%08x expect 12 (LDREXD low)",  c.r[4]);
    CHECK(c.r[5] == 0x34, "r5=%08x expect 34 (LDREXD high)", c.r[5]);
    CHECK(c.r[3] == 0,    "r3=%08x expect 0 (STREXD succeeded)", c.r[3]);
    CHECK(c.r[8] == 0x56, "r8=%08x expect 56 (STREXD low)",  c.r[8]);
    CHECK(c.r[9] == 0x78, "r9=%08x expect 78 (STREXD high)", c.r[9]);
}

/*
 * CLREX must actually drop the monitor. If it silently does nothing, a STREX
 * that the architecture requires to fail will succeed, and two threads can
 * both believe they hold the same lock.
 */
static void test_clrex_makes_strex_fail(void) {
    uint32_t p[] = { 0xe3a01b02 /* MOV r1,#0x800   */,
                     0xe1910f9f /* LDREX r0,[r1]   */,
                     0xf57ff01f /* CLREX           */,
                     0xe1812f90 /* STREX r2,r0,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 1, "r2=%08x expect 1 (STREX must fail after CLREX)", c.r[2]);
}

static void test_failed_strex_still_checks_write_permission(void) {
    arm_cpu_t c;
    const uint32_t va = 0x80000800u;

    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe1812f90u);              /* STREX r2,r0,[r1] */
    m_w32(NULL, 0x4000u, (3u << 10) | 2u);     /* user-RW code section */
    m_w32(NULL, 0x6000u, (2u << 10) | 2u);     /* user-RO target section */
    m_w32(NULL, 0x800u, 0x11223344u);
    arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_USR);
    c.cp15.ttbr0 = 0x4000u;
    c.cp15.dacr = 1u;                          /* domain 0: Client */
    c.cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP;
    c.r[0] = 0xa1b2c3d4u;
    c.r[1] = va;
    c.r[2] = 0xfeedfaceu;
    c.r[15] = 0u;
    c.excl_valid = false;
    g_watch_addr = 0x800u;
    g_watch_writes32 = 0u;

    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "failed-monitor STREX did not take its write-permission abort");
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_SECTION_PERMISSION &&
          (c.cp15.dfsr & (1u << 11)) != 0u && c.cp15.dfar == va,
          "failed-monitor STREX dfsr=%x dfar=%08x", c.cp15.dfsr, c.cp15.dfar);
    CHECK(c.r[2] == 0xfeedfaceu && g_watch_writes32 == 0u,
          "faulting STREX wrote status/target before abort");
    g_watch_addr = 0xffffffffu;
}

/*
 * An exception clears the monitor too. Without this, an interrupt landing
 * between LDREX and STREX leaves the tag intact and the preempted thread's
 * STREX still succeeds -- two owners of one spinlock. Modelled here with SWI,
 * which is the exception we can raise from a test program.
 */
static void test_exception_clears_exclusive_monitor(void) {
    /* Laid out so the SWI vector at 0x08 is part of the same image: branch
     * over it, run LDREX / SWI / STREX, and let the handler return to the
     * STREX via MOV pc,lr. */
    uint32_t p[] = { 0xea000002 /* 0x00 B 0x10        skip the vector */,
                     0x00000000 /* 0x04 (unused)      */,
                     0xe1a0f00e /* 0x08 MOV pc,lr     SWI vector        */,
                     0x00000000 /* 0x0c (unused)      */,
                     0xe3a01b02 /* 0x10 MOV r1,#0x800 */,
                     0xe1910f9f /* 0x14 LDREX r0,[r1] */,
                     0xef000000 /* 0x18 SWI #0        */,
                     0xe1812f90 /* 0x1c STREX r2,r0,[r1] */ };
    arm_cpu_t c; load_and_run(&c, p, 8, 6);
    CHECK(c.r[2] == 1, "r2=%08x expect 1 (STREX must fail after an exception)",
          c.r[2]);
}

static void test_strh_ldrh(void) {
    /* MOV r0,#0x1234-ish via halfword: store 0xAB and read it back as a halfword */
    uint32_t p[] = { 0xe3a000ab /*MOV r0,#0xAB*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe1c100b0 /*STRH r0,[r1]*/,
                     0xe1d120b0 /*LDRH r2,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 0xab, "r2=%08x expect ab (LDRH)", c.r[2]);
}

static void test_ldrsb_sign_extends(void) {
    /* store byte 0xFF, load it signed -> 0xFFFFFFFF */
    uint32_t p[] = { 0xe3a000ff /*MOV r0,#0xFF*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe5c10000 /*STRB r0,[r1]*/,
                     0xe1d120d0 /*LDRSB r2,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 0xffffffffu, "r2=%08x expect ffffffff (LDRSB)", c.r[2]);
}

static void test_ldrsh_sign_extends(void) {
    /* store halfword 0x8000, load it signed -> 0xFFFF8000 */
    uint32_t p[] = { 0xe3a00902 /*MOV r0,#0x8000*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe1c100b0 /*STRH r0,[r1]*/,
                     0xe1d120f0 /*LDRSH r2,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 0xffff8000u, "r2=%08x expect ffff8000 (LDRSH)", c.r[2]);
}

static void test_stmia_ldmia(void) {
    /* STMIA r1!,{r0,r2} then LDMIA r1,{r3,r4} round-trips both words. */
    uint32_t p[] = { 0xe3a00011 /*MOV r0,#0x11*/,
                     0xe3a02022 /*MOV r2,#0x22*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe8a10005 /*STMIA r1!,{r0,r2}*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe8910018 /*LDMIA r1,{r3,r4}*/ };
    arm_cpu_t c; load_and_run(&c, p, 6, 6);
    CHECK(c.r[3] == 0x11, "r3=%08x expect 11", c.r[3]);
    CHECK(c.r[4] == 0x22, "r4=%08x expect 22", c.r[4]);
}

static void test_push_pop(void) {
    /* The classic prologue/epilogue pair: STMDB sp!,{r0,r1} / LDMIA sp!,{r2,r3}.
     * Also checks the stack pointer returns to where it started. */
    uint32_t p[] = { 0xe3a0dc09 /*MOV sp,#0x900*/,
                     0xe3a000aa /*MOV r0,#0xAA*/,
                     0xe3a010bb /*MOV r1,#0xBB*/,
                     0xe92d0003 /*STMDB sp!,{r0,r1}  (push)*/,
                     0xe8bd000c /*LDMIA sp!,{r2,r3}  (pop)*/ };
    arm_cpu_t c; load_and_run(&c, p, 5, 5);
    CHECK(c.r[2] == 0xaa, "r2=%08x expect aa", c.r[2]);
    CHECK(c.r[3] == 0xbb, "r3=%08x expect bb", c.r[3]);
    CHECK(c.r[13] == 0x900, "sp=%08x expect 900 (balanced)", c.r[13]);
}

static void test_ldm_to_pc_branches(void) {
    /* LDMIA sp!,{pc} must branch. Push 0x300 then pop it into PC. */
    uint32_t p[] = { 0xe3a0dc09 /*MOV sp,#0x900*/,
                     0xe3a00c03 /*MOV r0,#0x300*/,
                     0xe92d0001 /*STMDB sp!,{r0}*/,
                     0xe8bd8000 /*LDMIA sp!,{pc}*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[15] == 0x300, "pc=%08x expect 300 (LDM to PC branched)", c.r[15]);
}

static void test_plain_loads_to_pc_reject_arm_halfword_targets(void) {
    arm_cpu_t c;

    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe590f000u);               /* LDR pc,[r0] */
    m_w32(NULL, 0x800u, 0x102u);                /* ARM target with bit 1 set */
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.r[0] = 0x800u;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u,
          "LDR pc accepted the impossible ARM-state 0b10 target");

    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe8b08000u);               /* LDMIA r0!,{pc} */
    m_w32(NULL, 0x800u, 0x102u);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.r[0] = 0x800u;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.r[0] == 0x800u,
          "plain LDM pc accepted 0b10 or committed writeback before rejection");
}

static void test_unaligned_ldr_to_pc_never_reads_memory(void) {
    for (unsigned a = 0; a < 2u; a++) {
        for (unsigned u = 0; u < 2u; u++) {
            arm_cpu_t c;
            memset(g_ram, 0, sizeof g_ram);
            m_w32(NULL, 0u, 0xe590f000u);       /* LDR pc,[r0] */
            arm_reset(&c, &g_bus);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
            c.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
            if (a) c.cp15.sctlr |= ARM_SCTLR_A;
            if (u) c.cp15.sctlr |= ARM_SCTLR_U;
            c.r[0] = 0x801u;
            /* The legacy U=0 path would align the bus address down. */
            g_watch_addr = u ? 0x801u : 0x800u;
            g_watch_reads32 = 0u;

            arm_status_t st = arm_step(&c);
            CHECK(g_watch_reads32 == 0u,
                  "unaligned LDR pc read memory with A=%u U=%u", a, u);
            if (a) {
                CHECK(st == ARM_OK &&
                      (c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT &&
                      c.cp15.dfar == 0x801u,
                      "A=1,U=%u LDR pc status=%d dfsr=%x dfar=%08x",
                      u, (int)st, c.cp15.dfsr, c.cp15.dfar);
            } else {
                CHECK(st == ARM_UNDEFINED && c.r[15] == 0u,
                      "A=0,U=%u LDR pc must be UNPREDICTABLE", u);
            }
            g_watch_addr = 0xffffffffu;
        }
    }
}

static void test_imm_dp_not_trapped(void) {
    /* Regression for the extra-load/store mask: MOV r0,#0x90 has imm8 bits 7 and
     * 4 set, but is immediate data-processing (bit25=1) and must execute. */
    uint32_t p[] = { 0xe3a00090 /*MOV r0,#0x90*/ };
    arm_cpu_t c; arm_status_t st = run_status(&c, p, 1, 1);
    CHECK(st == ARM_OK, "status=%d expect ARM_OK for MOV imm", (int)st);
    CHECK(c.r[0] == 0x90, "r0=%08x expect 90", c.r[0]);
}

static void test_banked_sp_per_mode(void) {
    /* r13 is banked per mode: a value written in SVC must not be visible in IRQ,
     * and must come back when we switch back. */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[13] = 0xAAAA0000u;
    arm_set_mode(&c, ARM_MODE_IRQ);
    CHECK(c.r[13] != 0xAAAA0000u, "sp_irq leaked sp_svc (%08x)", c.r[13]);
    c.r[13] = 0xBBBB0000u;
    arm_set_mode(&c, ARM_MODE_SVC);
    CHECK(c.r[13] == 0xAAAA0000u, "sp_svc=%08x expect aaaa0000 after switch back", c.r[13]);
    arm_set_mode(&c, ARM_MODE_IRQ);
    CHECK(c.r[13] == 0xBBBB0000u, "sp_irq=%08x expect bbbb0000", c.r[13]);
}

static void test_fiq_banks_r8_r12(void) {
    /* FIQ additionally banks r8-r12. */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[8] = 0x1234;
    arm_set_mode(&c, ARM_MODE_FIQ);
    CHECK(c.r[8] != 0x1234, "r8_fiq leaked r8_usr (%08x)", c.r[8]);
    c.r[8] = 0x5678;
    arm_set_mode(&c, ARM_MODE_SVC);
    CHECK(c.r[8] == 0x1234, "r8=%08x expect 1234 restored", c.r[8]);
}

static void test_mrs_reads_cpsr(void) {
    /* MRS r0, CPSR */
    uint32_t p[] = { 0xe10f0000 };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[0] == c.cpsr, "r0=%08x expect cpsr=%08x", c.r[0], c.cpsr);
}

static void test_msr_switches_mode(void) {
    /* MOV r0,#0xD2 (IRQ mode, I set) ; MSR CPSR_c, r0 -> mode becomes IRQ */
    uint32_t p[] = { 0xe3a000d2 /*MOV r0,#0xD2*/,
                     0xe121f000 /*MSR CPSR_c, r0*/ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_IRQ,
          "mode=%02x expect IRQ(12)", c.cpsr & ARM_CPSR_MODE_MASK);
}

static void test_swi_enters_svc(void) {
    /* SWI #0 from SYS mode: vectors to 0x08, enters SVC, LR=return, SPSR=old. */
    uint32_t p[] = { 0xef000000 /*SWI #0*/ };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[15] == ARM_VEC_SWI, "pc=%08x expect 08 (SWI vector)", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "mode=%02x expect SVC(13)", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.r[14] == 4, "lr_svc=%08x expect 4 (return address)", c.r[14]);
    CHECK((c.cpsr & ARM_CPSR_I) != 0, "IRQs should be masked on exception entry");
    CHECK((c.spsr[ARM_BANK_SVC] & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS,
          "spsr_svc should record the interrupted SYS mode");
}

static void test_exception_return_restores_mode(void) {
    /* Full round trip: SWI from SYS lands in SVC; the handler pushes LR and
     * returns with LDMIA sp!,{pc}^ which restores CPSR from SPSR (back to SYS). */
    uint32_t p[16] = {0};
    p[0] = 0xef000000;                 /* 0x00: SWI #0 -> vectors to 0x08 */
    p[2] = 0xe3a0dc09;                 /* 0x08: MOV sp,#0x900 (sp_svc)    */
    p[3] = 0xe92d4000;                 /* 0x0c: STMDB sp!,{lr}            */
    p[4] = 0xe8fd8000;                 /* 0x10: LDMIA sp!,{pc}^  (return) */
    arm_cpu_t c; load_and_run(&c, p, 16, 4);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS,
          "mode=%02x expect back in SYS(1f)", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.r[15] == 4, "pc=%08x expect 4 (returned after the SWI)", c.r[15]);
}

static void test_cp15_reads_midr(void) {
    /* MRC p15,0,r0,c0,c0,0 -> the ARM1176JZF-S main ID. The kernel reads this
     * to identify the CPU it is running on. */
    uint32_t p[] = { 0xee100f10 };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[0] == ARM1176_MIDR, "r0=%08x expect %08x (MIDR)", c.r[0], ARM1176_MIDR);
}

static void test_cp15_cpuid_feature_bank(void) {
    /* CP15 c0 CRm=1 and CRm=2 are the ARMv6 CPUID feature identification
     * registers. MIDR[19:16] on the ARM1176JZF-S reads 0xF, which means
     * "the architecture version is described by this bank, not by MIDR" —
     * so a kernel that wants to know what it is running on has to read it. */
    uint32_t p[] = { 0xee100f11 /*MRC p15,0,r0,c0,c1,0  ID_PFR0 */,
                     0xee101f91 /*MRC p15,0,r1,c0,c1,4  ID_MMFR0*/,
                     0xee102f12 /*MRC p15,0,r2,c0,c2,0  ID_ISAR0*/,
                     0xee103f32 /*MRC p15,0,r3,c0,c2,1  ID_ISAR1*/,
                     0xee104f92 /*MRC p15,0,r4,c0,c2,4  ID_ISAR4*/,
                     0xee105f31 /*MRC p15,0,r5,c0,c1,1  ID_PFR1 */ };
    arm_cpu_t c; load_and_run(&c, p, 6, 6);
    CHECK(c.r[0] == ARM1176_ID_PFR0,  "ID_PFR0=%08x expect %08x",  c.r[0], ARM1176_ID_PFR0);
    CHECK(c.r[1] == ARM1176_ID_MMFR0, "ID_MMFR0=%08x expect %08x", c.r[1], ARM1176_ID_MMFR0);
    CHECK(c.r[2] == ARM1176_ID_ISAR0, "ID_ISAR0=%08x expect %08x", c.r[2], ARM1176_ID_ISAR0);
    CHECK(c.r[3] == ARM1176_ID_ISAR1, "ID_ISAR1=%08x expect %08x", c.r[3], ARM1176_ID_ISAR1);
    CHECK(c.r[4] == ARM1176_ID_ISAR4, "ID_ISAR4=%08x expect %08x", c.r[4], ARM1176_ID_ISAR4);
    CHECK(c.r[5] == ARM1176_ID_PFR1,  "ID_PFR1=%08x expect %08x",  c.r[5], ARM1176_ID_PFR1);
}

static void test_cp15_cpuid_scheme_grades_as_armv6(void) {
    /*
     * This is xnu-1357.5.30's do_cpuid(), lifted verbatim from the kernel at
     * 0xc006257c and run against our CP15. It reads MIDR, notices the 0xF
     * architecture field, reads ID_ISAR1, and if the Jazelle field says 2 it
     * rewrites the architecture field to 7 (ARMv6).
     *
     * cpu_init() then indexes a jump table with (arch - 2) and stores
     * CPU_SUBTYPE_ARM_V6 for arch 7; anything it does not recognise stores
     * CPU_SUBTYPE_ARM_ALL (0), and grade_binary() rejects every ARMv6 Mach-O
     * on the disk with EBADARCH. If this assertion ever fails again, /sbin/launchd
     * stops exec'ing.
     */
    uint32_t p[] = { 0xee101f10 /*MRC p15,0,r1,c0,c0,0   MIDR         */,
                     0xee103f32 /*MRC p15,0,r3,c0,c2,1   ID_ISAR1     */,
                     0xe1a03423 /*LSR   r3, r3, #8                    */,
                     0xe20330f0 /*AND   r3, r3, #0xf0                 */,
                     0xe3530020 /*CMP   r3, #0x20        Jazelle == 2 */,
                     0x03c13702 /*BICEQ r3, r1, #0x80000              */,
                     0x03833807 /*ORREQ r3, r3, #0x70000              */ };
    arm_cpu_t c; load_and_run(&c, p, 7, 7);
    CHECK(((c.r[1] >> 16) & 0xfu) == 0xfu,
          "MIDR arch nibble=%x expect f (CPUID scheme)", (c.r[1] >> 16) & 0xfu);
    CHECK(((c.r[3] >> 16) & 0xfu) == 7u,
          "fixed-up arch nibble=%x expect 7 (ARMv6) — cpu_subtype would be ARM_ALL",
          (c.r[3] >> 16) & 0xfu);
}

static void test_cp15_id_dfr0_matches_absent_debug_unit(void) {
    /* We do not model the CP14 debug unit, so DBGDIDR reads zero. ID_DFR0 must
     * agree and report "no debug architecture": XNU's do_debugid() treats a
     * non-zero ID_DFR0 as licence to publish a breakpoint count derived from
     * DBGDIDR, so the two answers have to be consistent with each other. */
    uint32_t p[] = { 0xee100f51 /*MRC p15,0,r0,c0,c1,2   ID_DFR0 */,
                     0xee101e10 /*MRC p14,0,r1,c0,c0,0   DBGDIDR */ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 0, "ID_DFR0=%08x expect 0 (no debug unit modelled)", c.r[0]);
    CHECK(c.r[1] == 0, "DBGDIDR=%08x expect 0", c.r[1]);
}

static void test_cp15_sctlr_roundtrip(void) {
    /* Deliberately write SCTLR.C (data cache) rather than SCTLR.M: setting M
     * would switch the MMU on mid-program and, with no page tables loaded, the
     * very next fetch would legitimately take a prefetch abort. */
    uint32_t p[] = { 0xe3a00004 /*MOV r0,#4 (SCTLR.C)*/,
                     0xee010f10 /*MCR p15,0,r0,c1,c0,0*/,
                     0xee111f10 /*MRC p15,0,r1,c1,c0,0*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[1] == 4, "r1=%08x expect 4 (SCTLR readback)", c.r[1]);
    CHECK((c.cp15.sctlr & ARM_SCTLR_C) != 0, "SCTLR.C should be set");
}

static void test_cp15_c1_is_gated_on_crm_zero(void) {
    /* c1 with CRm != 0 is the ARM1176JZF-S TrustZone bank — Secure
     * Configuration, Secure Debug Enable, Non-Secure Access Control — which
     * this core does not model. Keying only on opcode_2 aliased that bank onto
     * SCTLR/ACTLR/CPACR, so MCR p15,0,Rd,c1,c1,0 replaced SCTLR wholesale; a
     * cleared SCTLR.U silently swaps the machine from the ARMv6 unaligned model
     * to the legacy rotate one, corrupting data rather than faulting. */
    uint32_t p[] = { 0xe3a00004 /*MOV r0,#4               */,
                     0xee010f10 /*MCR p15,0,r0,c1,c0,0 SCTLR = 4  */,
                     0xe3a00080 /*MOV r0,#0x80            */,
                     0xee010f50 /*MCR p15,0,r0,c1,c0,2 CPACR = 0x80*/,
                     0xe3a00020 /*MOV r0,#0x20            */,
                     0xee010f11 /*MCR p15,0,r0,c1,c1,0 (SCR)      */,
                     0xee010f51 /*MCR p15,0,r0,c1,c1,2 (NSACR)    */,
                     0xee111f11 /*MRC p15,0,r1,c1,c1,0            */,
                     0xee112f10 /*MRC p15,0,r2,c1,c0,0            */ };
    arm_cpu_t c; load_and_run(&c, p, 9, 9);
    CHECK(c.cp15.sctlr == 4u,
          "sctlr=%08x expect 4 — c1,c1,0 must not write SCTLR", c.cp15.sctlr);
    CHECK(c.cp15.cpacr == 0x80u,
          "cpacr=%08x expect 80 — c1,c1,2 must not write CPACR", c.cp15.cpacr);
    CHECK(c.r[1] == 0u,
          "r1=%08x expect 0 — c1,c1,0 must not read SCTLR back", c.r[1]);
    CHECK(c.r[2] == 4u,
          "r2=%08x expect 4 — SCTLR is still readable at CRm==0", c.r[2]);
}

static void test_cp15_ttbr0_roundtrip(void) {
    /* Translation table base survives a write/read cycle (needed for the MMU). */
    uint32_t p[] = { 0xe3a00b02 /*MOV r0,#0x800*/,
                     0xee020f10 /*MCR p15,0,r0,c2,c0,0  (TTBR0)*/,
                     0xee121f10 /*MRC p15,0,r1,c2,c0,0*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.cp15.ttbr0 == 0x800, "ttbr0=%08x expect 800", c.cp15.ttbr0);
    CHECK(c.r[1] == 0x800, "r1=%08x expect 800", c.r[1]);
}

static void test_cp15_ttbcr_masks_only_reserved_bits(void) {
    /* PD1, PD0 and N are writable on ARM1176. Bit 3 and [31:6] are SBZ/UNP. */
    uint32_t p[] = { 0xe3a000ff /*MOV r0,#0xff*/,
                     0xee020f50 /*MCR p15,0,r0,c2,c0,2 (TTBCR)*/,
                     0xee121f50 /*MRC p15,0,r1,c2,c0,2*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.cp15.ttbcr == 0x37u, "ttbcr=%08x expect 00000037", c.cp15.ttbcr);
    CHECK(c.r[1] == 0x37u, "TTBCR readback=%08x expect 00000037", c.r[1]);
}

static void test_cp15_cache_op_is_accepted(void) {
    /* MCR p15,0,r0,c7,c5,0 (invalidate I-cache) must not trap. */
    uint32_t p[] = { 0xee070f15 };
    arm_cpu_t c; arm_status_t st = run_status(&c, p, 1, 1);
    CHECK(st == ARM_OK, "status=%d expect ARM_OK for cache maintenance", (int)st);
}

static void test_cp15_wfi_uses_only_the_exact_privileged_hook(void) {
    /* ARM1176's legacy WFI coordinates: MCR p15,0,<Rd>,c7,c0,4, with
     * the VALUE in Rd SBZ.  The platform callback returning false must not turn
     * a CPU-only harness into a hang; the one WFI still retires and advances
     * to the following instruction. */
    const uint32_t wfi = 0xee070f90u;
    unsigned calls = 0;
    arm_bus_t bus = g_bus;
    bus.ctx = &calls;
    bus.wait_for_interrupt = count_wfi;

    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, wfi);
    arm_cpu_t c;
    arm_reset(&c, &bus);
    c.cpsr = ARM_MODE_SYS | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_N;
    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK && calls == 1, "status=%d calls=%u expect OK/1",
          (int)st, calls);
    CHECK(c.r[15] == 4u && c.cycles == 1u,
          "pc=%08x cycles=%llu expect one retired WFI at pc 0",
          c.r[15], (unsigned long long)c.cycles);
    CHECK(c.cpsr == (ARM_MODE_SYS | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_N),
          "WFI changed CPSR to %08x", c.cpsr);

    /* This is the exact instruction in iPhone OS 3.1.3's _cpu_idle: XNU clears
     * r2, executes DSB through c7,c10,4, then WFI through c7,c0,4.  "Rd SBZ"
     * describes r2's transferred value; it does not require the r0 field. */
    calls = 0;
    m_w32(NULL, 0, 0xee072f90u);                /* MCR p15,0,r2,c7,c0,4 */
    arm_reset(&c, &bus);
    c.cpsr = ARM_MODE_SYS;
    c.r[2] = 0u;
    st = arm_step(&c);
    CHECK(st == ARM_OK && calls == 1 && c.r[15] == 4u,
          "XNU r2 WFI status=%d calls=%u pc=%08x",
          (int)st, calls, c.r[15]);

    /* No platform hook preserves the flat core's historical accepted-no-op
     * behavior.  This is intentional: a standalone CPU has no clock source it
     * could safely advance and must not block its host indefinitely. */
    m_w32(NULL, 0, wfi);
    arm_reset(&c, &g_bus);
    c.cpsr = ARM_MODE_SYS;
    st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[15] == 4u && c.cycles == 1u,
          "hookless WFI status=%d pc=%08x cycles=%llu",
          (int)st, c.r[15], (unsigned long long)c.cycles);

    /* WFI is privileged-only.  User mode must fault before the hook can make
     * any platform state advance. */
    calls = 0;
    m_w32(NULL, 0, wfi);
    arm_reset(&c, &bus);
    c.cpsr = ARM_MODE_USR;
    st = arm_step(&c);
    CHECK(st == ARM_UNDEFINED && calls == 0,
          "user WFI status=%d calls=%u expect undefined/0", (int)st, calls);

    /* A non-zero transferred value violates SBZ and must fail before changing
     * platform time. */
    calls = 0;
    m_w32(NULL, 0, 0xee072f90u);
    arm_reset(&c, &bus);
    c.cpsr = ARM_MODE_SYS;
    c.r[2] = 1u;
    st = arm_step(&c);
    CHECK(st == ARM_UNDEFINED && calls == 0,
          "non-zero WFI operand status=%d calls=%u expect undefined/0",
          (int)st, calls);

    /* An MRC and a non-zero Opcode_1 at the same c7 coordinates are not the
     * WFI operation and must not invoke the wait hook. */
    const uint32_t not_wfi[] = { 0xee170f90u, 0xee270f90u };
    for (unsigned i = 0; i < sizeof not_wfi / sizeof not_wfi[0]; i++) {
        calls = 0;
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, not_wfi[i]);
        arm_reset(&c, &bus);
        c.cpsr = ARM_MODE_SYS;
        st = arm_step(&c);
        CHECK(calls == 0, "non-WFI encoding %08x called hook %u times",
              not_wfi[i], calls);
    }
}

static void test_cortex_a8_cp15_selector_boundary(void) {
    /* These are the backed data-register coordinates in DDI0344K table3-3.
     * A legal selector does not by itself prove all of its control bits. */
    const unsigned limits[] = {0,3,3,1,0,2,3,0,0,0,0,0,0,5,0,0};
    for (unsigned crn = 0; crn < 16u; crn++) {
     for (unsigned opc1 = 0; opc1 < 8u; opc1++) {
      for (unsigned crm = 0; crm < 16u; crm++) {
       for (unsigned opc2 = 0; opc2 < 8u; opc2++) {
        bool backed = opc1 == 0u && crm == 0u && opc2 < limits[crn] && !(crn == 6u && opc2 == 1u);
        backed |= opc1 == 1u && crn == 9u && crm == 0u && opc2 == 2u; /* L2ACTLR */
        if (backed || crn == 7u || crn == 8u) continue; /* Maintenance has its own test. */
        for (unsigned load = 0; load < 2u; load++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr |= ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q;
            c.r[9] = 0x80000000u; c.cp15.tpidrurw = 0x12345678u;
            c.cp15.ttbr0 = 0x4000u; c.cp15.context_id = 0xabcdu;
            uint32_t flags = c.cpsr;
            arm_cp15_t before = c.cp15;
            uint32_t insn = 0xee009f10u | (load << 20) | (opc1 << 21) | (crn << 16) | (opc2 << 5) | crm;
            m_w32(NULL, 0, insn);
            CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.r[9] == 0x80000000u &&
                  c.cpsr == flags && memcmp(&c.cp15, &before, sizeof before) == 0 && c.a8_l2actlr == 0x42u,
                  "A8 aliased/unimplemented CP15 selector load=%u opc1=%u c%u,c%u,%u", load, opc1, crn, crm, opc2);
        }
       }
      }
     }
    }
    /* Keep real stateful thread-ID access in privileged and allowed User
     * modes. CP15 MRC to APSR is unpredictable (DDI0406C.b B3.15.2). */
    for (unsigned user = 0; user < 2u; user++) {
     for (unsigned op = 1; op <= 4; op++) {
      for (unsigned load = 0; load < 2u; load++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_N;
        c.r[9] = 0xabcdef80u;
        c.cp15.context_id = 0x12345678u; c.cp15.tpidrurw = 0x12345678u;
        c.cp15.tpidruro = 0x12345678u; c.cp15.tpidrprw = 0x12345678u;
        arm_cp15_t before = c.cp15;
        bool allowed = !user || op == 2u || (op == 3u && load);
        m_w32(NULL, 0, 0xee0d9f10u | (load << 20) | (op << 5));
        CHECK(arm_step(&c) == (allowed ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (allowed ? 4u : 0u) && c.r[9] == (allowed && load ? 0x12345678u : 0xabcdef80u),
              "A8 thread-ID permissions/result op=%u load=%u user=%u", op, load, user);
        uint32_t value = op == 1u ? c.cp15.context_id : op == 2u ? c.cp15.tpidrurw :
                         op == 3u ? c.cp15.tpidruro : c.cp15.tpidrprw;
        CHECK(value == (allowed && !load ? 0xabcdef80u : 0x12345678u), "A8 thread-ID write lost stored state");
        if (!allowed) CHECK(memcmp(&c.cp15, &before, sizeof before) == 0, "denied CP15 access changed state");
      }
     }
     for (unsigned nzcv = 0; nzcv < 16u; nzcv++) {
     arm_cpu_t c;
     CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
     c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_Q |
              (5u << 16) | ((15u ^ nzcv) << 28);
     uint32_t flags = c.cpsr;
     c.cp15.tpidrurw = (nzcv << 28) | 0x01234567u;
     m_w32(NULL, 0, 0xee1dff50u); /* MRC p15,0,APSR_nzcv,c13,c0,2 */
     CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == flags &&
           c.cp15.tpidrurw == ((nzcv << 28) | 0x01234567u),
           "A8 accepted unpredictable CP15 MRC to APSR");
     c.r[15] = 0; flags = c.cpsr;
     m_w32(NULL, 0, 0xee0dff50u); /* MCR cannot source PC. */
     CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == flags &&
           c.cp15.tpidrurw == ((nzcv << 28) | 0x01234567u),
           "A8 MCR accepted PC source");
     }
    }
}

static void test_cortex_a8_cp15_maintenance_boundary(void) {
    /* Explicitly enumerate the TRM's operations instead of duplicating the
     * production predicate. Current coherent memory has no dirty cache. */
    const unsigned caches[][2] = {{0,4},{5,0},{5,1},{5,4},{5,6},{5,7},{6,1},{6,2},
                                 {10,1},{10,2},{10,4},{10,5},{11,1},{14,1},{14,2}};
    for (unsigned crn = 7; crn <= 8; crn++) {
     for (unsigned opc1 = 0; opc1 < 2u; opc1++) {
      for (unsigned crm = 0; crm < 16u; crm++) {
       for (unsigned opc2 = 0; opc2 < 8u; opc2++) {
        for (unsigned load = 0; load < 2u; load++) {
         for (unsigned user = 0; user < 2u; user++) {
            unsigned calls = 0;
            arm_bus_t bus = g_bus; bus.ctx = &calls; bus.wait_for_interrupt = count_wfi;
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_N;
            c.r[9] = 0; c.excl_valid = true; c.excl_addr = 0x1234u;
            bool listed = false;
            if (crn == 7u) {
                for (unsigned i = 0; i < sizeof caches / sizeof caches[0]; i++)
                    if (caches[i][0] == crm && caches[i][1] == opc2) listed = true;
            } else listed = crm >= 5u && crm <= 7u && opc2 <= 2u;
            bool user_allowed = crn == 7u && ((crm == 5u && opc2 == 4u) ||
                                (crm == 10u && (opc2 == 4u || opc2 == 5u)));
            bool allowed = listed && !load && !opc1 && (!user || user_allowed);
            uint32_t flags = c.cpsr;
            uint32_t generation = c.tlb_gen;
            uint64_t flushes = c.tlb_flushes;
            m_w32(NULL, 0, 0xee009f10u | (load << 20) | (opc1 << 21) | (crn << 16) | (opc2 << 5) | crm);
            CHECK(arm_step(&c) == (allowed ? ARM_OK : ARM_UNDEFINED) && c.r[15] == (allowed ? 4u : 0u) &&
                  calls == 0u && c.cpsr == flags && c.excl_valid && c.excl_addr == 0x1234u && c.r[9] == 0,
                  "A8 maintenance load=%u opc1=%u c%u,c%u,%u user=%u lost permission/NOP semantics", load, opc1, crn, crm, opc2, user);
            CHECK(allowed && crn == 8u ? c.tlb_gen != generation && c.tlb_flushes == flushes + 1u :
                                       c.tlb_gen == generation && c.tlb_flushes == flushes,
                  "A8 TLB maintenance did not flush only on an accepted operation");
         }
        }
       }
      }
     }
    }
    const uint32_t ignored[] = {0u,1u,UINT32_MAX};
    for (unsigned i = 0; i < sizeof ignored / sizeof ignored[0]; i++) {
        unsigned calls = 0;
        arm_bus_t bus = g_bus; bus.ctx = &calls; bus.wait_for_interrupt = count_wfi;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.r[14] = ignored[i];
        m_w32(NULL, 0, 0xee07ef90u); /* Legacy WFI is a NOP on A8. */
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.r[14] == ignored[i] && calls == 0u,
              "A8 CP15 NOP used its operand or invoked wait");
    }
}

static void test_cortex_a8_system_control_register(void) {
    /* DDI0344K 3.2.25: low reset inputs, fixed read bits, and supported
     * memory/cache/vector controls. Reserved SBZP/SBOP violations and
     * unimplemented TE/TRE/EE modes must refuse before changing state. */
    static const unsigned writable[] = {0,1,2,11,12,13,29};
    static const unsigned refused[] = {7,8,9,10,15,19,20,25,26,28,30,31};
    static const unsigned ones[] = {3,4,5,6,16,18,22,23};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned bit = 0; bit < 32u + sizeof ones / sizeof ones[0]; bit++) {
      for (unsigned user = 0; user < 2u; user++) {
        memset(g_ram, 0, sizeof g_ram);
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        CHECK(c.cp15.sctlr == 0x00c50078u, "A8 SCTLR reset inherited ARM1176's zero value");
        c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_N |
                 ARM_CPSR_C | ARM_CPSR_Q | (thumb ? ARM_CPSR_T : 0u);
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        m_w32(NULL, 0x4000u, 0xc02u); /* Identity mapping if this write enables the MMU. */
        bool allowed = !user && bit < 32u, variable = false;
        for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++)
            if (bit == refused[i]) allowed = false;
        for (unsigned i = 0; i < sizeof writable / sizeof writable[0]; i++)
            if (bit == writable[i]) variable = true;
        c.r[4] = bit < 32u ? 0x00c50078u | (1u << bit) : 0x00c50078u & ~(1u << ones[bit - 32u]);
        uint32_t source = c.r[4], flags = c.cpsr, generation = c.tlb_gen;
        uint64_t flushes = c.tlb_flushes;
        if (thumb) { m_w16(NULL, 0, 0xee01u); m_w16(NULL, 2, 0x4f10u); }
        else m_w32(NULL, 0, 0xee014f10u);
        uint32_t expected = 0x00c50078u | (allowed && variable ? 1u << bit : 0u);
        CHECK(arm_step(&c) == (allowed ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (allowed ? 4u : 0u) && c.r[4] == source && c.cpsr == flags && c.cp15.sctlr == expected,
              "A8 SCTLR write policy/state thumb=%u bit=%u user=%u value=%08x actual=%08x", thumb, bit, user, source, c.cp15.sctlr);
        CHECK(allowed ? c.tlb_gen != generation && c.tlb_flushes == flushes + 1u :
                        c.tlb_gen == generation && c.tlb_flushes == flushes,
              "A8 SCTLR rejected write flushed translations");
        if (allowed) {
            if (thumb) { m_w16(NULL, 4, 0xee11u); m_w16(NULL, 6, 0x2f10u); }
            else m_w32(NULL, 4, 0xee112f10u);
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 8u && c.r[2] == expected && c.cpsr == flags,
                  "A8 SCTLR readback/fixed bits/next fetch lost stored semantics");
        }
      }
     }
    }
    /* The matching N88 entry reads SCTLR, sets I/Z, then writes it back. */
    const uint32_t entry[] = {0xee11bf10u,0xe38bbb06u,0xee01bf10u,0xee112f10u};
    for (unsigned i = 0; i < sizeof entry / sizeof entry[0]; i++) m_w32(NULL, 4u * i, entry[i]);
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    for (unsigned i = 0; i < 4u; i++) CHECK(arm_step(&c) == ARM_OK, "A8 SCTLR entry RMW failed");
    CHECK(c.cp15.sctlr == 0x00c51878u && c.r[2] == 0x00c51878u && c.r[15] == 16u,
          "A8 SCTLR RMW lost architectural fixed bits");
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned i = 0; i < sizeof legacy / sizeof legacy[0]; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, legacy[i]), "reset");
        CHECK(c.cp15.sctlr == 0u, "A8 SCTLR reset changed another profile");
        c.r[4] = UINT32_MAX; m_w32(NULL, 0, 0xee014f10u);
        CHECK(arm_step(&c) == ARM_OK && c.cp15.sctlr == UINT32_MAX,
              "A8 SCTLR write policy changed the legacy profile path");
    }
}

static void test_cortex_a8_auxiliary_control_register(void) {
    /* DDI0344K 3.2.26/table3-49. Reset enables L2, and the selected
     * L1RSTDISABLE/L2RSTDISABLE inputs are low. Their monitor bits are
     * read-only; bits29:21 and bit2 are reserved, not writable features. */
    static const uint32_t modes[] = {ARM_MODE_USR,ARM_MODE_SVC,ARM_MODE_SYS,
                                    ARM_MODE_IRQ,ARM_MODE_FIQ,ARM_MODE_ABT,ARM_MODE_UND};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned mode = 0; mode < sizeof modes / sizeof modes[0]; mode++) {
      for (unsigned bit = 0; bit <= 32u; bit++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        CHECK(c.cp15.actlr == 2u, "A8 ACTLR reset lost L2EN or inherited stale control fields");
        c.cpsr = modes[mode] | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_N | ARM_CPSR_C |
                 ARM_CPSR_Q | (5u << 16) | (thumb ? ARM_CPSR_T : 0u);
        c.excl_valid = true; c.excl_addr = 0x1234u;
        c.r[4] = bit == 32u ? 0u : 2u | (1u << bit);
        c.r[2] = 0xabcdef01u;
        arm_cp15_t expected = c.cp15;
        bool allowed = mode != 0u && bit != 2u && !(bit >= 21u && bit <= 29u);
        if (allowed) expected.actlr = bit >= 30u && bit < 32u ? 2u : c.r[4];
        uint32_t source = c.r[4], flags = c.cpsr, generation = c.tlb_gen;
        uint64_t flushes = c.tlb_flushes;
        if (thumb) { m_w16(NULL, 0, 0xee01u); m_w16(NULL, 2, 0x4f30u); }
        else m_w32(NULL, 0, 0xee014f30u);
        CHECK(arm_step(&c) == (allowed ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (allowed ? 4u : 0u) && c.r[4] == source && c.cpsr == flags,
              "A8 ACTLR write permission/retirement thumb=%u mode=%u bit=%u", thumb, mode, bit);
        CHECK(memcmp(&c.cp15, &expected, sizeof expected) == 0 &&
              c.excl_valid && c.excl_addr == 0x1234u && c.tlb_gen == generation && c.tlb_flushes == flushes,
              "A8 ACTLR write changed unrelated state or lost field policy thumb=%u mode=%u bit=%u", thumb, mode, bit);
        c.r[15] = 0;
        if (thumb) { m_w16(NULL, 0, 0xee11u); m_w16(NULL, 2, 0x2f30u); }
        else m_w32(NULL, 0, 0xee112f30u);
        CHECK(arm_step(&c) == (mode ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (mode ? 4u : 0u) && c.r[2] == (mode ? expected.actlr : 0xabcdef01u) &&
              c.cpsr == flags && memcmp(&c.cp15, &expected, sizeof expected) == 0,
              "A8 ACTLR readback/User refusal thumb=%u mode=%u bit=%u", thumb, mode, bit);
      }
     }
    }
    /* RMW sequences must preserve controls already set by earlier code. */
    const uint32_t entry[] = {0xee11bf30u,0xe38bb002u,0xee01bf30u,
                             0xee11bf30u,0xe38bb008u,0xee01bf30u};
    for (unsigned i = 0; i < sizeof entry / sizeof entry[0]; i++) m_w32(NULL, 4u * i, entry[i]);
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    for (unsigned i = 0; i < 6u; i++) CHECK(arm_step(&c) == ARM_OK, "A8 ACTLR entry RMW failed");
    CHECK(c.cp15.actlr == 10u && c.r[11] == 10u && c.r[15] == 24u,
          "A8 ACTLR L2EN/L1PE RMW lost control state");
    const uint32_t writes[] = {0x001ffffbu,0xc01ffffbu,0xc0000000u,0x102u,0x106u,0x200102u,2u};
    const uint32_t reads[] = {0x001ffffbu,0x001ffffbu,0u,0x102u,0x102u,0x102u,2u};
    for (unsigned i = 0; i < sizeof writes / sizeof writes[0]; i++) {
        c.r[15] = 0; c.r[4] = writes[i];
        m_w32(NULL, 0, 0xee014f30u);
        CHECK(arm_step(&c) == (i == 4u || i == 5u ? ARM_UNDEFINED : ARM_OK) && c.cp15.actlr == reads[i],
              "A8 ACTLR combined fields/clears or reserved-write preservation index=%u", i);
    }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned i = 0; i < sizeof legacy / sizeof legacy[0]; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, legacy[i]), "reset");
        CHECK(c.cp15.actlr == 0u, "A8 ACTLR reset changed another profile");
        c.r[4] = UINT32_MAX; m_w32(NULL, 0, 0xee014f30u);
        CHECK(arm_step(&c) == ARM_OK && c.cp15.actlr == UINT32_MAX,
              "A8 ACTLR write policy changed a legacy profile");
    }
}

static void test_cortex_a8_coprocessor_access_control(void) {
    /* DDI0344K 3.2.27 and DDI0406C.b B4.1.40: only CP10/CP11 are
     * implemented. Absent fields/optional disables are RAZ/WI. Refuse
     * reserved bit29/permission2 and mismatched floating-point permissions. */
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned user = 0; user < 2u; user++) {
      for (unsigned field = 0; field < 18u; field++) {
       for (unsigned permission = 0; permission < 4u; permission++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        CHECK(c.cp15.cpacr == 0u, "A8 CPACR reset allowed a coprocessor");
        uint32_t source = 0x00f00000u;
        if (field < 14u) {
            if (field == 10u || field == 11u) source = permission * 0x00500000u;
            else source |= permission << (2u * field);
        } else source |= (permission & 1u) << (field + 14u); /* bits28..31 */
        bool allowed = !user && !(field == 15u && (permission & 1u)) &&
                       !((field == 10u || field == 11u) && permission == 2u);
        c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_I | ARM_CPSR_F |
                 ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q | (thumb ? ARM_CPSR_T : 0u);
        c.cp15.cpacr = 0x00500000u;
        c.r[4] = source; c.r[2] = 0xabcdef01u;
        c.excl_valid = true; c.excl_addr = 0x1234u;
        arm_cp15_t expected = c.cp15;
        if (allowed) expected.cpacr = source & 0x00f00000u;
        uint32_t flags = c.cpsr, generation = c.tlb_gen;
        uint64_t flushes = c.tlb_flushes;
        if (thumb) { m_w16(NULL, 0, 0xee01u); m_w16(NULL, 2, 0x4f50u); }
        else m_w32(NULL, 0, 0xee014f50u);
        CHECK(arm_step(&c) == (allowed ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (allowed ? 4u : 0u) && c.r[4] == source && c.cpsr == flags &&
              memcmp(&c.cp15, &expected, sizeof expected) == 0,
              "A8 CPACR field/privilege policy thumb=%u user=%u field=%u permission=%u", thumb, user, field, permission);
        CHECK(c.tlb_gen == generation && c.tlb_flushes == flushes && c.excl_valid && c.excl_addr == 0x1234u,
              "A8 CPACR write changed translation or exclusive state");
        c.r[15] = 0;
        if (thumb) { m_w16(NULL, 0, 0xee11u); m_w16(NULL, 2, 0x2f50u); }
        else m_w32(NULL, 0, 0xee112f50u);
        CHECK(arm_step(&c) == (user ? ARM_UNDEFINED : ARM_OK) && c.r[15] == (user ? 0u : 4u) &&
              c.r[2] == (user ? 0xabcdef01u : expected.cpacr) && c.cpsr == flags,
              "A8 CPACR readback exposed absent coprocessors or User access");
       }
      }
      for (unsigned cp10 = 0; cp10 < 4u; cp10++) {
       for (unsigned cp11 = 0; cp11 < 4u; cp11++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_I | ARM_CPSR_F | (thumb ? ARM_CPSR_T : 0u);
        c.cp15.cpacr = 0x00500000u; c.r[4] = (cp10 << 20) | (cp11 << 22);
        bool allowed = !user && cp10 == cp11 && cp10 != 2u;
        if (thumb) { m_w16(NULL, 0, 0xee01u); m_w16(NULL, 2, 0x4f50u); }
        else m_w32(NULL, 0, 0xee014f50u);
        CHECK(arm_step(&c) == (allowed ? ARM_OK : ARM_UNDEFINED) &&
              c.cp15.cpacr == (allowed ? c.r[4] : 0x00500000u),
              "A8 CPACR accepted an invalid floating-point permission pair");
       }
      }
     }
    }
    /* Use a guest MCR to configure permissions, then execute a real VFP
     * access. Denied accesses must enter the guest Undefined handler. */
    static const unsigned permissions[] = {0,1,3};
    for (unsigned i = 0; i < sizeof permissions / sizeof permissions[0]; i++) {
     for (unsigned user = 0; user < 2u; user++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.r[4] = permissions[i] * 0x00500000u;
        m_w32(NULL, 0, 0xee014f50u);
        CHECK(arm_step(&c) == ARM_OK, "CPACR guest setup");
        c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_N;
        c.vfp_fpexc = ARM_FPEXC_EN; c.vfp_fpscr = 0x40000080u; /* FPSCR.Z/IDC */
        c.r[2] = 0xabcdef01u; c.r[15] = 0x100u;
        uint32_t flags = c.cpsr;
        bool allowed = permissions[i] == 3u || (permissions[i] == 1u && !user);
        m_w32(NULL, 0x100u, 0xeef12a10u); /* VMRS r2,FPSCR */
        CHECK(arm_step(&c) == ARM_OK, "CPACR-controlled VFP instruction halted");
        if (allowed) {
            CHECK(c.r[15] == 0x104u && c.r[2] == c.vfp_fpscr && c.cpsr == flags,
                  "CPACR did not grant the intended VFP access");
        } else {
            CHECK(c.r[15] == ARM_VEC_UNDEFINED && (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND &&
                  c.r[14] == 0x104u && c.spsr[ARM_BANK_UND] == flags && c.r[2] == 0xabcdef01u,
                  "CPACR denial did not preserve operands and enter guest Undefined");
        }
     }
    }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned i = 0; i < sizeof legacy / sizeof legacy[0]; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[i]), "reset");
        c.r[4] = UINT32_MAX; m_w32(NULL, 0, 0xee014f50u);
        CHECK(arm_step(&c) == ARM_OK && c.cp15.cpacr == UINT32_MAX,
              "A8 CPACR field rules changed a legacy profile");
    }
}

static void test_cortex_a8_l2_auxiliary_control(void) {
    /* DDI0344K 3.2.55, table3-3: reset 0x42, privileged Secure writes.
     * This CPU configuration has no parity/ECC RAM, so bit21 cannot set.
     * Test each defined field and every reserved bit, not just a boot value. */
    static const unsigned fields[] = {0,1,2,3,6,7,8,16,21,22,23,24,25,27,28,29};
    for (unsigned bit = 0; bit < 32u; bit++) {
        bool defined = false;
        for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; i++)
            if (bit == fields[i]) defined = true;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.r[1] = UINT32_MAX;
        m_w32(NULL, 0, 0xee391f50u); /* MRC p15,1,r1,c9,c0,2 */
        CHECK(arm_step(&c) == ARM_OK && c.r[1] == 0x42u && c.r[15] == 4u,
              "A8 L2ACTLR reset/read lost documented latency state");
        c.r[15] = 0; c.r[1] = 1u << bit;
        uint32_t flags = c.cpsr;
        uint32_t generation = c.tlb_gen;
        uint64_t flushes = c.tlb_flushes;
        arm_cp15_t before = c.cp15;
        m_w32(NULL, 0, 0xee291f50u); /* MCR p15,1,r1,c9,c0,2 */
        CHECK(arm_step(&c) == (defined ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (defined ? 4u : 0u) && c.r[1] == (1u << bit) && c.cpsr == flags,
              "A8 L2ACTLR field write permission/effects bit=%u", bit);
        CHECK(memcmp(&c.cp15, &before, sizeof before) == 0 &&
              c.tlb_gen == generation && c.tlb_flushes == flushes,
              "A8 L2ACTLR aliased a legacy register or flushed translations");
        c.r[15] = 0;
        m_w32(NULL, 0, 0xee392f50u);
        uint32_t expected = defined ? (bit == 21u ? 0u : 1u << bit) : 0x42u;
        CHECK(arm_step(&c) == ARM_OK && c.r[2] == expected,
              "A8 L2ACTLR bit=%u read=%08x expected=%08x", bit, c.r[2], expected);
    }
    /* Reset, read/modify/write and clearing persist independently of Rt.
     * The matching kernel requests 0x10600000; absent ECC RAM must read its
     * enable bit clear. Never report an error-protection unit from that write. */
    static const uint32_t writes[] = {0x02000042u,0x42u,0x10600000u,0x3be101cfu,0u};
    static const uint32_t reads[]  = {0x02000042u,0x42u,0x10400000u,0x3bc101cfu,0u};
    static const uint32_t modes[] = {ARM_MODE_SVC,ARM_MODE_SYS,ARM_MODE_IRQ,
                                    ARM_MODE_FIQ,ARM_MODE_ABT,ARM_MODE_UND};
    for (unsigned mode = 0; mode < sizeof modes / sizeof modes[0]; mode++) {
     for (unsigned rd = 0; rd < 16u; rd++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = modes[mode] | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_Q | (5u << 16) | 0xf0000000u;
        for (unsigned i = 0; i < sizeof writes / sizeof writes[0]; i++) {
            c.r[0] = writes[i]; c.r[15] = 0;
            uint32_t flags = c.cpsr;
            m_w32(NULL, 0, 0xee290f50u);
            CHECK(arm_step(&c) == ARM_OK && c.cpsr == flags, "A8 L2ACTLR write changed flags");
            c.r[15] = 0;
            m_w32(NULL, 0, 0xee390f50u | (rd << 12));
            CHECK(arm_step(&c) == (rd == 15u ? ARM_UNDEFINED : ARM_OK) && c.r[15] == (rd == 15u ? 0u : 4u) &&
                  (rd == 15u ? c.cpsr == flags :
                               c.r[rd] == reads[i] && c.cpsr == flags),
                  "A8 L2ACTLR stored read/APSR refusal mode=%u rd=%u value=%u", mode, rd, i);
        }
     }
    }
    for (unsigned load = 0; load < 2u; load++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = ARM_MODE_USR | ARM_CPSR_N; c.r[7] = 0x02000000u;
        uint32_t flags = c.cpsr;
        m_w32(NULL, 0, 0xee297f50u | (load << 20));
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u &&
              c.r[7] == 0x02000000u && c.cpsr == flags, "User L2ACTLR access accepted");
        c.cpsr = ARM_MODE_SVC;
        m_w32(NULL, 0, 0xee397f50u);
        CHECK(arm_step(&c) == ARM_OK && c.r[7] == 0x42u, "User L2ACTLR write leaked state");
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    m_w32(NULL, 0, 0xee29ff50u); /* MCR source PC is unpredictable. */
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u, "L2ACTLR accepted MCR PC");
    m_w32(NULL, 0, 0x0e29ff50u); /* Condition-failed invalid access has no effects. */
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u, "L2ACTLR bypassed condition check");
    c.r[15] = 0;
    m_w32(NULL, 0, 0xee390f50u);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x42u, "refused/skipped L2ACTLR write changed state");

    /* This profile remains in the Secure reset state. Do not grant L2 writes
     * while silently accepting an unimplemented transition to another world. */
    static const uint32_t transitions[] = {0xee010f11u, /* MCR SCR */
        0xe1600070u, /* SMC #0 */ 0xf1020016u, /* CPS #Monitor */
        0xe121f000u}; /* MSR CPSR_c,r0 with Monitor mode */
    for (unsigned i = 0; i < sizeof transitions / sizeof transitions[0]; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.r[0] = i == 3u ? 0x16u : 1u;
        uint32_t flags = c.cpsr;
        m_w32(NULL, 0, transitions[i]);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == flags,
              "A8 accepted an unimplemented security transition %08x", transitions[i]);
    }
    static const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned i = 0; i < sizeof legacy / sizeof legacy[0]; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, legacy[i]), "reset");
        c.r[0] = 0x02000000u;
        m_w32(NULL, 0, 0xee290f50u); m_w32(NULL, 4, 0xee391f50u);
        CHECK(arm_step(&c) == ARM_OK && arm_step(&c) == ARM_OK && c.r[1] == 0u,
              "A8 L2ACTLR changed another profile's legacy CP15 path");
    }
}

static void test_high_vectors(void) {
    /* Setting SCTLR.V (bit 13) moves the vector table to 0xFFFF0000, so a SWI
     * must vector to 0xFFFF0008 instead of 0x8. */
    uint32_t p[] = { 0xe3a00a02 /*MOV r0,#0x2000 (SCTLR.V)*/,
                     0xee010f10 /*MCR p15,0,r0,c1,c0,0*/,
                     0xef000000 /*SWI #0*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[15] == 0xffff0008u, "pc=%08x expect ffff0008 (high vectors)", c.r[15]);
}

/* ---------------------------------------------------------------- MMU tests */
/* Build a first-level table at 0x4000 mapping one 1 MB section, exactly as a
 * kernel would lay it out in RAM, then let the real walker translate. */
static void mmu_setup_section(arm_cpu_t *c, uint32_t va, uint32_t pa,
                              unsigned ap, unsigned domain) {
    const uint32_t l1 = 0x4000;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, l1 + ((va >> 20) << 2),
          (pa & 0xfff00000u) | (ap << 10) | (domain << 5) | 2u); /* section */
    arm_reset(c, &g_bus);
    c->cp15.ttbr0 = l1;
    c->cp15.dacr  = 1u << (domain * 2);       /* client: check AP */
    c->cp15.sctlr |= ARM_SCTLR_M;             /* MMU on */
}

static void test_mmu_disabled_is_identity(void) {
    arm_cpu_t c; arm_reset(&c, &g_bus);
    uint32_t pa = 0;
    uint32_t f = arm_mmu_translate(&c, 0xdeadb000u, ARM_ACCESS_READ, true, &pa);
    CHECK(f == 0, "fsr=%u expect 0 with MMU off", f);
    CHECK(pa == 0xdeadb000u, "pa=%08x expect identity", pa);
}

static void test_mmu_section_translation(void) {
    arm_cpu_t c; mmu_setup_section(&c, 0x80000000u, 0x00200000u, 3, 0);
    uint32_t pa = 0;
    uint32_t f = arm_mmu_translate(&c, 0x80001234u, ARM_ACCESS_READ, true, &pa);
    CHECK(f == 0, "fsr=%u expect 0", f);
    CHECK(pa == 0x00201234u, "pa=%08x expect 00201234", pa);
}

static void test_mmu_unmapped_faults(void) {
    arm_cpu_t c; mmu_setup_section(&c, 0x80000000u, 0x00200000u, 3, 0);
    uint32_t pa = 0;
    /* 0x90000000 has no descriptor at all. */
    uint32_t f = arm_mmu_translate(&c, 0x90000000u, ARM_ACCESS_READ, true, &pa);
    CHECK((f & 0xf) == ARM_FSR_SECTION_TRANSLATION,
          "fsr=%x expect section translation fault", f);
}

static void test_mmu_user_write_permission(void) {
    /* AP=10: privileged RW, user read-only. A user write must fault. */
    arm_cpu_t c; mmu_setup_section(&c, 0x80000000u, 0x00200000u, 2, 0);
    uint32_t pa = 0;
    CHECK(arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, false, &pa) == 0,
          "user read should be permitted with AP=10");
    uint32_t f = arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_WRITE, false, &pa);
    CHECK((f & 0xf) == ARM_FSR_SECTION_PERMISSION,
          "fsr=%x expect permission fault on user write", f);
}

static void test_mmu_small_page_translation(void) {
    /* Two-level walk: L1 coarse pointer -> L2 small page. */
    const uint32_t l1 = 0x4000, l2 = 0x5000;
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, l1 + ((0x80000000u >> 20) << 2), (l2 & 0xfffffc00u) | 1u); /* coarse */
    m_w32(NULL, l2 + (((0x80000000u >> 12) & 0xff) << 2),
          (0x00300000u & 0xfffff000u) | (3u << 4) | 2u);                   /* small page, AP=11 */
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = l1; c.cp15.dacr = 1u;
    c.cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP;
    uint32_t pa = 0;
    uint32_t f = arm_mmu_translate(&c, 0x80000abcu, ARM_ACCESS_READ, true, &pa);
    CHECK(f == 0, "fsr=%u expect 0 for small page", f);
    CHECK(pa == 0x00300abcu, "pa=%08x expect 00300abc", pa);
}

static void test_data_abort_taken(void) {
    /* With the MMU on and nothing mapped at the load address, LDR must raise a
     * data abort: ABT mode, vector 0x10, and DFAR recording the address. */
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    /* identity-map the low section so the fetch itself succeeds */
    m_w32(NULL, 0x4000 + 0, (0x00000000u) | (3u << 10) | 2u);
    /* 0x90000000 is in a different 1 MB section than the identity-mapped one,
     * so it has no descriptor at all. */
    uint32_t prog[] = { 0xe3a01209 /*MOV r1,#0x90000000*/, 0xe5912000 /*LDR r2,[r1]*/ };
    for (unsigned i = 0; i < 2; i++) m_w32(NULL, i * 4, prog[i]);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.cp15.ttbr0 = 0x4000; c.cp15.dacr = 1u; c.cp15.sctlr |= ARM_SCTLR_M;
    arm_step(&c); arm_step(&c);
    CHECK(c.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT,
          "mode=%02x expect ABT(17)", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.cp15.dfar == 0x90000000u, "dfar=%08x expect 90000000", c.cp15.dfar);
}

/* --- regressions from the adversarial audit ----------------------------- */

static void test_pld_is_a_nop_not_a_branch(void) {
    /* cond==0xF is the ARMv6 unconditional space. PLD must be a no-op; if it is
     * decoded as a conditional instruction it becomes "LDRB pc,[r1]" and
     * branches to whatever byte it loaded. */
    uint32_t p[] = { 0xe3a01a01 /*MOV r1,#0x1000*/,
                     0xf5d1f000 /*PLD [r1]*/,
                     0xe3a00007 /*MOV r0,#7*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 7, "r0=%u expect 7 (execution continued past PLD)", c.r[0]);
    CHECK(c.r[15] == 12, "pc=%08x expect 0c (PLD did not branch)", c.r[15]);
}

static void test_unconditional_space_traps(void) {
    /* Unimplemented cond==0xF encodings (e.g. SETEND) must trap, not execute. */
    uint32_t p[] = { 0xf1010200 /*SETEND BE*/ };
    arm_cpu_t c; arm_status_t st = run_status(&c, p, 1, 1);
    CHECK(st == ARM_UNDEFINED, "status=%d expect ARM_UNDEFINED for SETEND", (int)st);
}

static void test_clz_does_not_corrupt_cpsr(void) {
    /* CLZ sits in the same opcode space as MSR. With a loose MSR mask it would
     * rewrite CPSR (including the mode field) instead of trapping. */
    uint32_t p[] = { 0xe16f0f11 /*CLZ r0,r1*/ };
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, p[0]);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.r[1] = 1;
    uint32_t before = c.cpsr;
    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK, "status=%d expect ARM_OK — CLZ is implemented", (int)st);
    CHECK(c.r[0] == 31, "r0=%u expect 31 (clz of 1)", c.r[0]);
    CHECK(c.cpsr == before, "cpsr changed %08x -> %08x (CLZ decoded as MSR)",
          before, c.cpsr);
}

static void test_msr_still_works(void) {
    /* The tightened mask must not break real MSR. */
    uint32_t p[] = { 0xe3a000d2 /*MOV r0,#0xD2*/, 0xe121f000 /*MSR CPSR_c,r0*/ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_IRQ,
          "mode=%02x expect IRQ", c.cpsr & ARM_CPSR_MODE_MASK);
}

static void test_arm_media_extend_and_reverse(void) {
    /* The ARM media extend and byte-reverse families, which real XNU uses in
     * ordinary compiled code. (These used to trap; that assertion documented a
     * limitation that no longer exists.) */
    uint32_t p[] = { 0xe59f1010 /*LDR r1,[pc,#16] -> the literal at 0x18*/,
                     0xe6bf0f31 /*REV  r0,r1 */,
                     0xe6ef2071 /*UXTB r2,r1 */,
                     0xe6bf3071 /*SXTH r3,r1 */,
                     0xe6ff4071 /*UXTH r4,r1 */,
                     0xeafffffe /*B .        */,
                     0x11228344 /*literal    */ };
    arm_cpu_t c; load_and_run(&c, p, 7, 5);
    CHECK(c.r[1] == 0x11228344u, "r1=%08x expect 11228344", c.r[1]);
    CHECK(c.r[0] == 0x44832211u, "r0=%08x expect 44832211 (REV)", c.r[0]);
    CHECK(c.r[2] == 0x44, "r2=%08x expect 44 (UXTB)", c.r[2]);
    CHECK(c.r[3] == 0xffff8344u, "r3=%08x expect ffff8344 (SXTH sign-extends)", c.r[3]);
    CHECK(c.r[4] == 0x8344, "r4=%08x expect 8344 (UXTH)", c.r[4]);
}

static void test_arm_media_pair_extend(void) {
    /* cccc 0110 1 op Rn Rd rot00 0111 Rm; op 8 is signed, C unsigned.
     * Rn=PC names the plain form. These cases cover the exact real-userland
     * blocker plus both lane signs, every rotation, lane-local wraparound,
     * aliases, conditions, flag preservation and invalid PC operands. */
#define PAIR_EXT(cond, op, rn, rd, rot, rm) \
    (((cond) << 28) | 0x06000070u | ((op) << 20) | ((rn) << 16) | \
     ((rd) << 12) | ((rot) << 10) | (rm))
    arm_cpu_t c;
    arm_status_t st;
    const uint32_t blocker = 0xe6cf3073u;       /* UXTB16 r3,r3 */

    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, blocker);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS
           | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q | 0x000f0000u;
    c.r[3] = 0x00000100u;                  /* live register value at the stop */
    uint32_t flags = c.cpsr;
    st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[3] == 0u && c.r[15] == 4u,
          "real UXTB16 blocker status=%d r3=%08x pc=%08x",
          (int)st, c.r[3], c.r[15]);
    CHECK(c.cpsr == flags, "UXTB16 changed CPSR %08x -> %08x", flags, c.cpsr);

    struct pair_case { uint32_t insn, src, base, expect; const char *name; } cases[] = {
        { PAIR_EXT(0xeu,0xcu,15u,0u,0u,1u), 0x80ff7f01u, 0u,
          0x00ff0001u, "UXTB16 ror0" },
        { PAIR_EXT(0xeu,0x8u,15u,0u,0u,1u), 0x80ff7f01u, 0u,
          0xffff0001u, "SXTB16 ror0" },
        { PAIR_EXT(0xeu,0x8u,15u,0u,1u,1u), 0x80ff7f01u, 0u,
          0xff80007fu, "SXTB16 ror8" },
        { PAIR_EXT(0xeu,0x8u,15u,0u,2u,1u), 0x80ff7f01u, 0u,
          0x0001ffffu, "SXTB16 ror16" },
        { PAIR_EXT(0xeu,0xcu,15u,0u,3u,1u), 0x80ff7f01u, 0u,
          0x007f0080u, "UXTB16 ror24" },
        { PAIR_EXT(0xeu,0xcu,2u,0u,0u,1u),  0x00ff0020u, 0xfffffff0u,
          0x00fe0010u, "UXTAB16 lane-local wrap" },
        { PAIR_EXT(0xeu,0x8u,2u,0u,0u,1u),  0x008000ffu, 0x00010001u,
          0xff810000u, "SXTAB16 signed lanes" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, cases[i].insn);
        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS
               | ARM_CPSR_Z | ARM_CPSR_V | ARM_CPSR_Q | 0x000a0000u;
        c.r[1] = cases[i].src;
        c.r[2] = cases[i].base;
        flags = c.cpsr;
        st = arm_step(&c);
        CHECK(st == ARM_OK && c.r[0] == cases[i].expect,
              "%s status=%d got=%08x expect=%08x", cases[i].name,
              (int)st, c.r[0], cases[i].expect);
        CHECK(c.cpsr == flags, "%s changed CPSR %08x -> %08x",
              cases[i].name, flags, c.cpsr);
    }

    /* The scalar accumulate forms share the tightened family mask. Exercise
     * every op with a non-zero rotation so mask or dispatch changes cannot
     * regress them while the paired cases stay green. */
    struct scalar_case { uint32_t insn, src, base, expect; const char *name; } scalar[] = {
        { PAIR_EXT(0xeu,0xau,2u,0u,1u,1u), 0x80ff7f01u, 0x00000100u,
          0x0000017fu, "SXTAB ror8" },
        { PAIR_EXT(0xeu,0xbu,2u,0u,2u,1u), 0x80ff7f01u, 0x00010000u,
          0x000080ffu, "SXTAH ror16" },
        { PAIR_EXT(0xeu,0xeu,2u,0u,3u,1u), 0x80ff7f01u, 0xfffffff0u,
          0x00000070u, "UXTAB ror24" },
        { PAIR_EXT(0xeu,0xfu,2u,0u,1u,1u), 0x80ff7f01u, 0x00010000u,
          0x0001ff7fu, "UXTAH ror8" },
    };
    for (size_t i = 0; i < sizeof scalar / sizeof scalar[0]; i++) {
        memset(g_ram, 0, sizeof g_ram); m_w32(NULL, 0, scalar[i].insn);
        arm_reset(&c, &g_bus); c.r[1] = scalar[i].src; c.r[2] = scalar[i].base;
        c.cpsr |= ARM_CPSR_N | ARM_CPSR_V | ARM_CPSR_Q | 0x00050000u;
        flags = c.cpsr;
        st = arm_step(&c);
        CHECK(st == ARM_OK && c.r[0] == scalar[i].expect,
              "%s status=%d got=%08x expect=%08x", scalar[i].name,
              (int)st, c.r[0], scalar[i].expect);
        CHECK(c.cpsr == flags, "%s changed CPSR %08x -> %08x",
              scalar[i].name, flags, c.cpsr);
    }

    /* Rd aliases Rn: both source halves must be captured before the write. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, PAIR_EXT(0xeu,0xcu,2u,2u,0u,1u));
    arm_reset(&c, &g_bus); c.r[1] = 0x00020003u; c.r[2] = 0xfffffff0u;
    st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[2] == 0x0001fff3u,
          "UXTAB16 Rd==Rn status=%d r2=%08x", (int)st, c.r[2]);

    /* Rd aliases Rm: the complete rotated source must be captured first. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, PAIR_EXT(0xeu,0x8u,2u,1u,1u,1u));
    arm_reset(&c, &g_bus); c.r[1] = 0x80ff7f01u; c.r[2] = 0x00010001u;
    st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[1] == 0xff810080u,
          "SXTAB16 Rd==Rm status=%d r1=%08x", (int)st, c.r[1]);

    /* All operands may name one register; the old source and both base lanes
     * must be captured before the destination write. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, PAIR_EXT(0xeu,0xcu,1u,1u,0u,1u));
    arm_reset(&c, &g_bus); c.r[1] = 0x00020003u;
    st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[1] == 0x00040006u,
          "UXTAB16 Rn==Rd==Rm status=%d r1=%08x", (int)st, c.r[1]);

    /* Failed condition leaves destination, flags and PC progression ordinary. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, PAIR_EXT(0x0u,0xcu,15u,0u,0u,1u)); /* UXTB16EQ */
    arm_reset(&c, &g_bus); c.r[0] = 0xfeedfaceu; c.r[1] = 0xffffffffu;
    c.cpsr &= ~ARM_CPSR_Z;
    flags = c.cpsr;
    st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[0] == 0xfeedfaceu && c.r[15] == 4u &&
          c.cpsr == flags, "failed-condition UXTB16 mutated state");

    /* PC is not a legal destination or byte source for any extend form. Check
     * the paired encodings and the previously implemented scalar encodings. */
    uint32_t bad[] = {
        PAIR_EXT(0xeu,0x8u,15u,15u,0u,1u),
        PAIR_EXT(0xeu,0xcu,15u,0u,0u,15u),
        PAIR_EXT(0xeu,0xau,15u,15u,0u,1u),
        PAIR_EXT(0xeu,0xeu,15u,0u,0u,15u),
        0xe6cf2171u,                       /* reserved bits[9:8] != 00 */
        PAIR_EXT(0xeu,0x9u,15u,0u,0u,1u), /* unallocated op */
        PAIR_EXT(0xeu,0xdu,15u,0u,0u,1u), /* unallocated op */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        st = run_status(&c, &bad[i], 1, 1);
        CHECK(st == ARM_UNDEFINED, "bad extend encoding %08x status=%d",
              bad[i], (int)st);
    }
#undef PAIR_EXT
}

static void test_arm_media_reverse_edges(void) {
    struct reverse_case { uint32_t insn, expect; const char *name; } cases[] = {
        { 0xe6bf0f31u, 0x44832211u, "REV" },
        { 0xe6bf0fb1u, 0x22114483u, "REV16" },
        { 0xe6ff0fb1u, 0x00004483u, "REVSH positive" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        arm_cpu_t c;
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, cases[i].insn);
        arm_reset(&c, &g_bus);
        c.r[1] = 0x11228344u;
        c.cpsr |= ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q | 0x000f0000u;
        uint32_t flags = c.cpsr;
        arm_status_t st = arm_step(&c);
        CHECK(st == ARM_OK && c.r[0] == cases[i].expect,
              "%s status=%d got=%08x expect=%08x", cases[i].name,
              (int)st, c.r[0], cases[i].expect);
        CHECK(c.cpsr == flags, "%s changed CPSR %08x -> %08x",
              cases[i].name, flags, c.cpsr);

        uint32_t bad_rd = cases[i].insn | 0x0000f000u;
        uint32_t bad_rm = (cases[i].insn & ~0xfu) | 0xfu;
        CHECK(run_status(&c, &bad_rd, 1, 1) == ARM_UNDEFINED,
              "%s accepted Rd=PC encoding %08x", cases[i].name, bad_rd);
        CHECK(run_status(&c, &bad_rm, 1, 1) == ARM_UNDEFINED,
              "%s accepted Rm=PC encoding %08x", cases[i].name, bad_rm);
    }

    /* REVSH must sign-extend bit 15 of the byte-reversed halfword. */
    uint32_t revsh = 0xe6ff0fb1u;
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram); m_w32(NULL, 0, revsh);
    arm_reset(&c, &g_bus); c.r[1] = 0x11224483u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0xffff8344u,
          "REVSH negative got=%08x expect ffff8344", c.r[0]);

    /* Destination/source aliasing is legal, and a failed condition must be a
     * pure no-op apart from ordinary sequential PC progression. */
    uint32_t rev_alias = 0xe6bf1f31u;       /* REV r1,r1 */
    memset(g_ram, 0, sizeof g_ram); m_w32(NULL, 0, rev_alias);
    arm_reset(&c, &g_bus); c.r[1] = 0x11228344u;
    CHECK(arm_step(&c) == ARM_OK && c.r[1] == 0x44832211u,
          "REV Rd==Rm got=%08x", c.r[1]);

    uint32_t rev_eq = 0x06bf0f31u;          /* REVEQ r0,r1 */
    memset(g_ram, 0, sizeof g_ram); m_w32(NULL, 0, rev_eq);
    arm_reset(&c, &g_bus); c.cpsr &= ~ARM_CPSR_Z;
    c.r[0] = 0xfeedfaceu; c.r[1] = 0x11228344u;
    uint32_t before = c.cpsr;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0xfeedfaceu &&
          c.r[15] == 4u && c.cpsr == before,
          "failed-condition REV mutated state");
}

static void test_arm_media_saturate(void) {
#define SAT(cond, uns, field, rd, shift, asr, rn) \
    (((cond) << 28) | 0x06a00010u | ((uns) << 22) | ((field) << 16) | \
     ((rd) << 12) | ((shift) << 7) | ((asr) << 6) | (rn))
#define SAT16(cond, uns, field, rd, rn) \
    (((cond) << 28) | 0x06a00f30u | ((uns) << 22) | ((field) << 16) | \
     ((rd) << 12) | (rn))
    arm_cpu_t c;
    arm_status_t st;

    /* Both observed UI stops used this exact word. ASR #14 produces 256 here,
     * which USAT #8 must clamp to 255 and report through sticky Q. */
    const uint32_t blocker = 0xe6e83756u;       /* USAT r3,#8,r6,ASR #14 */
    memset(g_ram, 0, sizeof g_ram); m_w32(NULL, 0, blocker);
    arm_reset(&c, &g_bus);
    c.cpsr = ARM_MODE_SYS | ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C |
             ARM_CPSR_V | 0x000a0000u;
    c.r[6] = 0x00400000u;
    uint32_t preserved = c.cpsr;
    st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[3] == 0xffu && c.r[15] == 4u,
          "real USAT blocker status=%d r3=%08x pc=%08x",
          (int)st, c.r[3], c.r[15]);
    CHECK(c.cpsr == (preserved | ARM_CPSR_Q),
          "real USAT blocker flags=%08x expect=%08x",
          c.cpsr, preserved | ARM_CPSR_Q);

    static const struct {
        uint32_t insn, source, expect;
        bool expect_q;
        const char *what;
    } scalar[] = {
        { SAT(0xeu,1u,8u,0u,0u,0u,1u), 0xffffffffu, 0u, true,
          "USAT clamps a negative input to zero" },
        { SAT(0xeu,1u,8u,0u,0u,0u,1u), 0x000000ffu, 0x000000ffu, false,
          "USAT keeps its upper boundary" },
        { SAT(0xeu,1u,8u,0u,0u,0u,1u), 0x00000100u, 0x000000ffu, true,
          "USAT clamps above its upper boundary" },
        { SAT(0xeu,1u,0u,0u,0u,0u,1u), 1u, 0u, true,
          "USAT zero-bit range contains only zero" },
        { SAT(0xeu,0u,7u,0u,0u,0u,1u), 0xffffff7fu, 0xffffff80u, true,
          "SSAT clamps below minus 128" },
        { SAT(0xeu,0u,7u,0u,0u,0u,1u), 0xffffff80u, 0xffffff80u, false,
          "SSAT keeps its negative boundary" },
        { SAT(0xeu,0u,7u,0u,0u,0u,1u), 0x00000080u, 0x0000007fu, true,
          "SSAT clamps above 127" },
        { SAT(0xeu,0u,31u,0u,0u,0u,1u), 0x80000000u, 0x80000000u, false,
          "SSAT 32-bit range keeps INT_MIN" },
        { SAT(0xeu,1u,8u,0u,0u,1u,1u), 0x80000000u, 0u, true,
          "USAT ASR immediate zero means shift by 32" },
        { SAT(0xeu,1u,8u,0u,31u,0u,1u), 1u, 0u, true,
          "USAT saturates the signed result after LSL" },
    };
    for (size_t i = 0; i < sizeof scalar / sizeof scalar[0]; i++) {
        memset(g_ram, 0, sizeof g_ram); m_w32(NULL, 0, scalar[i].insn);
        arm_reset(&c, &g_bus);
        c.cpsr = ARM_MODE_SYS | ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C |
                 ARM_CPSR_V | 0x00050000u;
        preserved = c.cpsr;
        c.r[1] = scalar[i].source;
        st = arm_step(&c);
        CHECK(st == ARM_OK && c.r[0] == scalar[i].expect,
              "%s status=%d got=%08x expect=%08x", scalar[i].what,
              (int)st, c.r[0], scalar[i].expect);
        CHECK(c.cpsr == (preserved | (scalar[i].expect_q ? ARM_CPSR_Q : 0u)),
              "%s flags=%08x", scalar[i].what, c.cpsr);
    }

    /* Q is sticky and Rd may alias Rn. */
    uint32_t alias = SAT(0xeu,1u,8u,1u,0u,0u,1u);
    memset(g_ram, 0, sizeof g_ram); m_w32(NULL, 0, alias);
    arm_reset(&c, &g_bus); c.cpsr |= ARM_CPSR_Q | ARM_CPSR_Z;
    c.r[1] = 42u; preserved = c.cpsr;
    CHECK(arm_step(&c) == ARM_OK && c.r[1] == 42u && c.cpsr == preserved,
          "non-saturating aliased USAT changed value or sticky flags");

    static const struct {
        uint32_t insn, source, expect;
        bool expect_q;
        const char *what;
    } lanes[] = {
        { SAT16(0xeu,0u,7u,0u,1u), 0xff7f0080u, 0xff80007fu, true,
          "SSAT16 clamps each signed half independently" },
        { SAT16(0xeu,0u,7u,0u,1u), 0x007fffffu, 0x007fffffu, false,
          "SSAT16 keeps in-range signed halves" },
        { SAT16(0xeu,1u,8u,0u,1u), 0xffff012cu, 0x000000ffu, true,
          "USAT16 clamps negative and oversized halves" },
        { SAT16(0xeu,1u,0u,0u,1u), 0u, 0u, false,
          "USAT16 zero-bit range keeps zero" },
    };
    for (size_t i = 0; i < sizeof lanes / sizeof lanes[0]; i++) {
        memset(g_ram, 0, sizeof g_ram); m_w32(NULL, 0, lanes[i].insn);
        arm_reset(&c, &g_bus);
        c.cpsr = ARM_MODE_SYS | ARM_CPSR_N | ARM_CPSR_C | 0x000a0000u;
        preserved = c.cpsr; c.r[1] = lanes[i].source;
        st = arm_step(&c);
        CHECK(st == ARM_OK && c.r[0] == lanes[i].expect,
              "%s status=%d got=%08x expect=%08x", lanes[i].what,
              (int)st, c.r[0], lanes[i].expect);
        CHECK(c.cpsr == (preserved | (lanes[i].expect_q ? ARM_CPSR_Q : 0u)),
              "%s flags=%08x", lanes[i].what, c.cpsr);
    }

    /* Failed conditions are ordinary no-ops, and PC is not a legal source or
     * destination for any of the four saturation forms. */
    uint32_t eq = SAT(0u,1u,8u,0u,0u,0u,1u);
    memset(g_ram, 0, sizeof g_ram); m_w32(NULL, 0, eq);
    arm_reset(&c, &g_bus); c.cpsr &= ~ARM_CPSR_Z;
    c.r[0] = 0xfeedfaceu; c.r[1] = 0x100u; preserved = c.cpsr;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0xfeedfaceu &&
          c.r[15] == 4u && c.cpsr == preserved,
          "failed-condition USAT mutated architectural state");

    uint32_t bad[] = {
        SAT(0xeu,0u,7u,15u,0u,0u,1u),
        SAT(0xeu,1u,8u,0u,0u,0u,15u),
        SAT16(0xeu,0u,7u,15u,1u),
        SAT16(0xeu,1u,8u,0u,15u),
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(run_status(&c, &bad[i], 1, 1) == ARM_UNDEFINED,
              "saturate encoding %08x accepted PC operand", bad[i]);

#undef SAT16
#undef SAT
}

static void test_apx_makes_mapping_read_only(void) {
    /* APX=1, AP=01 is privileged read-only: a privileged write must fault. */
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x4000 + ((0x80000000u >> 20) << 2),
          0x00200000u | (1u << 15) | (1u << 10) | 2u);   /* APX=1, AP=01 */
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000; c.cp15.dacr = 1u;
    c.cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP;

    uint32_t pa = 0;
    CHECK(arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, true, &pa) == 0,
          "privileged read should be allowed with APX=1,AP=01");
    uint32_t f = arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_WRITE, true, &pa);
    CHECK((f & 0xf) == ARM_FSR_SECTION_PERMISSION,
          "fsr=%x expect permission fault on write to a read-only section", f);
}

static void test_xp0_ignores_extended_apx_bits(void) {
    arm_cpu_t c;
    uint32_t pa = 0;
    memset(g_ram, 0, sizeof g_ram);

    /* In the backwards-compatible format section bit 15 is not APX. Treating
     * it as APX silently turns a writable AP=11 section read-only. */
    m_w32(NULL, 0x4000 + (0x800u << 2),
          0x00200000u | (1u << 15) | (3u << 10) | 2u);
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000u;
    c.cp15.dacr = 1u;
    c.cp15.sctlr = ARM_SCTLR_M;                 /* XP=0 */
    CHECK(arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_WRITE, true, &pa) == 0,
          "XP=0 section bit 15 was incorrectly treated as APX");

    /* L2 type 3 is the legacy extended-small-page form. Its bit 9 is likewise
     * not APX, and one AP field controls the whole 4 KB page. */
    m_w32(NULL, 0x4000 + (0x801u << 2), 0x8000u | 1u);
    m_w32(NULL, 0x8000u, 0x00300000u | (1u << 9) | (3u << 4) | 3u);
    /* Probe VA[11:10]=01: a mistaken subpage decode would read the zero AP1
     * pair in bits[7:6], while the actual extended-small format still uses its
     * one AP=11 field in bits[5:4]. */
    CHECK(arm_mmu_translate(&c, 0x80100400u, ARM_ACCESS_WRITE, false, &pa) == 0,
          "XP=0 extended-small single AP/bit-9 semantics were misdecoded");
}

static void test_xp0_large_and_small_ap_subpages(void) {
    arm_cpu_t c;
    uint32_t pa = 0;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x4000 + (0x800u << 2), 0x8000u | 1u);

    /* AP0..AP3 = 11,00,01,10. A 64 KB descriptor is repeated in all sixteen
     * L2 slots, and VA[15:14] selects its 16 KB permission subpage. */
    uint32_t large = 0x00020000u | (3u << 4) | (0u << 6) |
                     (1u << 8) | (2u << 10) | 1u;
    for (unsigned i = 0; i < 16u; i++) m_w32(NULL, 0x8000u + i * 4u, large);
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000u;
    c.cp15.dacr = 1u;
    c.cp15.sctlr = ARM_SCTLR_M;                 /* XP=0 */
    CHECK(arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_WRITE, false, &pa) == 0,
          "large-page AP0=11 should allow user writes");
    CHECK((arm_mmu_translate(&c, 0x80004000u, ARM_ACCESS_READ, true, &pa) & 0xfu)
              == ARM_FSR_PAGE_PERMISSION,
          "large-page AP1=00 should deny privileged reads");
    CHECK(arm_mmu_translate(&c, 0x80008000u, ARM_ACCESS_WRITE, true, &pa) == 0,
          "large-page AP2=01 should allow privileged writes");
    CHECK((arm_mmu_translate(&c, 0x80008000u, ARM_ACCESS_READ, false, &pa) & 0xfu)
              == ARM_FSR_PAGE_PERMISSION,
          "large-page AP2=01 should deny user reads");
    CHECK(arm_mmu_translate(&c, 0x8000c000u, ARM_ACCESS_READ, false, &pa) == 0,
          "large-page AP3=10 should allow user reads");
    CHECK((arm_mmu_translate(&c, 0x8000c000u, ARM_ACCESS_WRITE, false, &pa) & 0xfu)
              == ARM_FSR_PAGE_PERMISSION,
          "large-page AP3=10 should deny user writes");

    /* The 4 KB legacy small page uses VA[11:10] to select four 1 KB AP fields. */
    m_w32(NULL, 0x8000u, 0x00030000u | (3u << 4) | (0u << 6) |
                              (1u << 8) | (2u << 10) | 2u);
    CHECK(arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_WRITE, false, &pa) == 0,
          "small-page AP0=11 should allow user writes");
    CHECK((arm_mmu_translate(&c, 0x80000400u, ARM_ACCESS_READ, true, &pa) & 0xfu)
              == ARM_FSR_PAGE_PERMISSION,
          "small-page AP1=00 should deny privileged reads");
    CHECK(arm_mmu_translate(&c, 0x80000800u, ARM_ACCESS_WRITE, true, &pa) == 0,
          "small-page AP2=01 should allow privileged writes");
    CHECK((arm_mmu_translate(&c, 0x80000800u, ARM_ACCESS_READ, false, &pa) & 0xfu)
              == ARM_FSR_PAGE_PERMISSION,
          "small-page AP2=01 should deny user reads");
    CHECK(arm_mmu_translate(&c, 0x80000c00u, ARM_ACCESS_READ, false, &pa) == 0,
          "small-page AP3=10 should allow user reads");
    CHECK((arm_mmu_translate(&c, 0x80000c00u, ARM_ACCESS_WRITE, false, &pa) & 0xfu)
              == ARM_FSR_PAGE_PERMISSION,
          "small-page AP3=10 should deny user writes");
}

static void test_sctlr_sr_legacy_permissions(void) {
    arm_cpu_t c;
    uint32_t pa = 0;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x4000 + (0x800u << 2), 0x00200000u | 2u); /* AP=00 */
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000u;
    c.cp15.dacr = 1u;
    c.cp15.sctlr = ARM_SCTLR_M;

    CHECK((arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, true, &pa) & 0xfu)
              == ARM_FSR_SECTION_PERMISSION,
          "AP=00 with S=R=0 should deny access");
    c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_S;
    CHECK(arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, true, &pa) == 0,
          "S alone should permit privileged reads");
    CHECK((arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_WRITE, true, &pa) & 0xfu)
              == ARM_FSR_SECTION_PERMISSION,
          "S alone should deny privileged writes");
    CHECK((arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, false, &pa) & 0xfu)
              == ARM_FSR_SECTION_PERMISSION,
          "S alone should deny user reads");
    c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_R;
    CHECK(arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, true, &pa) == 0 &&
          arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, false, &pa) == 0,
          "R alone should permit reads in both privilege levels");
    CHECK((arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_WRITE, false, &pa) & 0xfu)
              == ARM_FSR_SECTION_PERMISSION,
          "R alone should deny writes");
    c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_S | ARM_SCTLR_R;
    CHECK((arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, true, &pa) & 0xfu)
              == ARM_FSR_SECTION_PERMISSION,
          "reserved S=R=1 must not grant access");
}

static void test_arm1176_rejects_fine_page_tables(void) {
    arm_cpu_t c;
    uint32_t pa = 0;
    memset(g_ram, 0, sizeof g_ram);
    /* ARM1176 removed the old L1 fine-table descriptor. It is a translation
     * fault before any domain lookup, even when its obsolete domain bits name
     * a domain that DACR would reject. */
    m_w32(NULL, 0x4000 + (0x800u << 2), 0x8000u | (7u << 5) | 3u);
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000u;
    c.cp15.dacr = 0u;
    c.cp15.sctlr = ARM_SCTLR_M;
    uint32_t f = arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, true, &pa);
    CHECK((f & 0xfu) == ARM_FSR_SECTION_TRANSLATION,
          "L1 type 3 fsr=%x expect section translation fault", f);
    CHECK((f & 0xf0u) == 0u, "fine-table rejection leaked obsolete domain: fsr=%x", f);
}

static void test_page_translation_fault_precedes_page_domain_fault(void) {
    arm_cpu_t c;
    uint32_t pa = 0;
    const uint32_t l1 = 0x4000u, l2 = 0x8000u, domain = 7u;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, l1 + (0x800u << 2), l2 | (domain << 5) | 1u);
    /* The selected L2 descriptor remains zero (translation fault) while its
     * domain is disabled. Translation has higher fault priority. */
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = l1;
    c.cp15.dacr = 0u;
    c.cp15.sctlr = ARM_SCTLR_M;
    uint32_t f = arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, true, &pa);
    CHECK((f & 0xfu) == ARM_FSR_PAGE_TRANSLATION,
          "invalid L2 plus disabled domain produced fsr=%x, expect page translation", f);

    /* Once the L2 descriptor is valid, the same disabled domain is reported. */
    m_w32(NULL, l2, 0x00030000u | (3u << 4) | 2u);
    /* The descriptor just changed under a cache that now exists. ARMv6
     * requires CP15 c8 maintenance after a table edit for exactly this
     * reason; a guest that skips it reads stale on hardware too. */
    arm_mmu_tlb_flush(&c);
    f = arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, true, &pa);
    CHECK((f & 0xfu) == ARM_FSR_PAGE_DOMAIN,
          "valid L2 plus disabled domain produced fsr=%x, expect page domain", f);
}

static void test_cortex_a8_access_flag_retry_without_tlbi(void) {
    /* DDI0406C.b B3.7.4: AF=0 descriptors are never held in the TLB.
     * Software sets AF and retries without invalidating a faulting entry.
     * Cover all short-descriptor sizes, access kinds and domain FSR tags. */
    const arm_access_t accesses[] = {ARM_ACCESS_READ,ARM_ACCESS_WRITE,ARM_ACCESS_FETCH};
    for (unsigned kind = 0; kind < 4u; kind++) {
     for (unsigned domain = 0; domain < 16u; domain++) {
      if (kind == 3u && domain != 0u) continue; /* Supersections use domain 0. */
      for (unsigned access = 0; access < sizeof accesses / sizeof accesses[0]; access++) {
       for (unsigned priv = 0; priv < 2u; priv++) {
        memset(g_ram, 0, sizeof g_ram);
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | ARM_SCTLR_FA;
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u << (domain * 2u);
        bool page = kind == 1u || kind == 2u;
        uint32_t entry = page ? 0x8000u : 0x6000u;
        uint32_t flag = page ? 0x10u : 0x400u;
        uint32_t descriptor = kind == 0u ? 0x802u | (domain << 5) :
                              kind == 1u ? 0x22u : kind == 2u ? 0x21u : 0x40802u;
        unsigned copies = kind >= 2u ? 16u : 1u;
        if (page) m_w32(NULL, 0x6000u, 0x8001u | (domain << 5));
        for (unsigned i = 0; i < copies; i++) m_w32(NULL, entry + 4u * i, descriptor);
        uint32_t pa = 0xdeadbeefu;
        uint32_t expected_fsr = (page ? ARM_FSR_PAGE_ACCESS_FLAG : ARM_FSR_SECTION_ACCESS_FLAG) |
                               (domain << 4) | (accesses[access] == ARM_ACCESS_WRITE ? 0x800u : 0u);
        g_watch_addr = entry; g_watch_reads32 = g_watch_writes32 = 0u;
        CHECK(arm_mmu_translate(&c, 0x80000040u, accesses[access], priv != 0u, &pa) == expected_fsr &&
              pa == 0xdeadbeefu && g_watch_reads32 == 1u && g_watch_writes32 == 0u,
              "A8 AF fault status/tag/side effects kind=%u domain=%u access=%u priv=%u", kind, domain, access, priv);
        uint32_t generation = c.tlb_gen;
        uint64_t flushes = c.tlb_flushes;
        CHECK(arm_mmu_translate(&c, 0x80000040u, accesses[access], priv != 0u, &pa) == expected_fsr &&
              pa == 0xdeadbeefu && g_watch_reads32 == 2u && g_watch_writes32 == 0u,
              "A8 cached an AF-clear descriptor instead of walking again");
        /* Software updates the descriptor. No CP15 maintenance occurs. */
        g_watch_addr = UINT32_MAX;
        for (unsigned i = 0; i < copies; i++) m_w32(NULL, entry + 4u * i, descriptor | flag);
        g_watch_addr = entry;
        CHECK(arm_mmu_translate(&c, 0x80000040u, accesses[access], priv != 0u, &pa) == 0u && pa == 0x40u &&
              g_watch_reads32 == 3u && g_watch_writes32 == 0u,
              "A8 retry did not observe software setting AF without TLBI");
        CHECK(c.tlb_gen == generation && c.tlb_flushes == flushes && c.tlb_hits == 0u && c.tlb_misses == 3u,
              "A8 AF retries flushed unrelated translations or reported cache hits");
        CHECK(arm_mmu_translate(&c, 0x80000040u, accesses[access], priv != 0u, &pa) == 0u && pa == 0x40u &&
              g_watch_reads32 == 3u && c.tlb_hits == 1u && c.tlb_misses == 3u,
              "A8 AF fix disabled caching of valid translations");
        g_watch_addr = UINT32_MAX;
       }
      }
     }
    }
}

static void test_force_access_flag_faults_precede_domain_permissions(void) {
    arm_cpu_t c;
    uint32_t pa, fsr;

    /* Extended section AP[0]=0 is an access-flag fault when SCTLR.FA is set.
     * Even a Manager domain cannot bypass it, and a store retains DFSR.WnR. */
    memset(g_ram, 0, sizeof g_ram);
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000u;
    c.cp15.dacr = 3u << (5u * 2u);             /* domain 5: Manager */
    c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | ARM_SCTLR_FA;
    m_w32(NULL, 0x4000u + (0x800u << 2),
          0x00200000u | (5u << 5) | 2u);       /* section, AP[0]=0 */
    g_watch_addr = 0x00200100u;
    g_watch_reads32 = g_watch_writes32 = 0u;
    pa = 0xdeadbeefu;
    fsr = arm_mmu_translate(&c, 0x80000100u, ARM_ACCESS_WRITE, true, &pa);
    CHECK((fsr & 0xfu) == ARM_FSR_SECTION_ACCESS_FLAG &&
          (fsr & 0xf0u) == 0x50u && (fsr & (1u << 11)) != 0u &&
          pa == 0xdeadbeefu,
          "section access-flag fsr=%x pa=%08x", fsr, pa);
    CHECK(g_watch_reads32 == 0u && g_watch_writes32 == 0u,
          "section access-flag fault touched target memory");

    /* ARM1176 preserves the deprecated APX:AP=000 S/R escape when S and R are
     * opposite. It remains a permission mapping, not an access-flag fault. */
    c.cp15.dacr = 1u << (5u * 2u);             /* domain 5: Client */
    c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | ARM_SCTLR_FA | ARM_SCTLR_S;
    CHECK(arm_mmu_translate(&c, 0x80000100u, ARM_ACCESS_READ, true, &pa) == 0u,
          "S=1,R=0 privileged read was mistaken for an access-flag fault");
    fsr = arm_mmu_translate(&c, 0x80000100u, ARM_ACCESS_WRITE, true, &pa);
    CHECK((fsr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "S/R compatibility write fsr=%x, expect permission fault", fsr);
    c.cp15.sctlr |= ARM_SCTLR_R;                /* S=1,R=1: reserved, no escape */
    fsr = arm_mmu_translate(&c, 0x80000100u, ARM_ACCESS_READ, true, &pa);
    CHECK((fsr & 0xfu) == ARM_FSR_SECTION_ACCESS_FLAG,
          "S=1,R=1 suppressed access-flag fault: fsr=%x", fsr);

    /* FA only changes the XP=1 format. With FA clear, or XP clear, the same
     * Manager mapping is allowed and AP[0] resumes its ordinary meaning. */
    c.cp15.dacr = 3u << (5u * 2u);
    c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
    CHECK(arm_mmu_translate(&c, 0x80000100u, ARM_ACCESS_READ, true, &pa) == 0u &&
          pa == 0x00200100u,
          "FA-clear Manager section did not translate: pa=%08x", pa);
    c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_FA; /* XP clear */
    CHECK(arm_mmu_translate(&c, 0x80000100u, ARM_ACCESS_READ, true, &pa) == 0u,
          "XP-clear descriptor incorrectly treated AP[0] as an access flag");

    /* A valid second-level descriptor is classified before its access flag;
     * once valid, page access-flag outranks a disabled domain. */
    memset(g_ram, 0, sizeof g_ram);
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000u;
    c.cp15.dacr = 0u;                          /* every domain disabled */
    c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | ARM_SCTLR_FA;
    m_w32(NULL, 0x4000u + (0x800u << 2), 0x8000u | (7u << 5) | 1u);
    pa = 0xdeadbeefu;
    fsr = arm_mmu_translate(&c, 0x80001000u, ARM_ACCESS_READ, true, &pa);
    CHECK((fsr & 0xfu) == ARM_FSR_PAGE_TRANSLATION,
          "invalid L2 with FA produced fsr=%x, expect page translation", fsr);

    m_w32(NULL, 0x8004u, 0x00060000u | 2u);    /* small page, AP[0]=0 */
    /* The descriptor just changed under a cache that now exists. ARMv6
     * requires CP15 c8 maintenance after a table edit for exactly this
     * reason; a guest that skips it reads stale on hardware too. */
    arm_mmu_tlb_flush(&c);
    g_watch_addr = 0x00060000u;
    g_watch_reads32 = g_watch_writes32 = 0u;
    pa = 0xdeadbeefu;
    fsr = arm_mmu_translate(&c, 0x80001000u, ARM_ACCESS_READ, true, &pa);
    CHECK((fsr & 0xfu) == ARM_FSR_PAGE_ACCESS_FLAG &&
          (fsr & 0xf0u) == 0x70u && pa == 0xdeadbeefu,
          "page access-flag fsr=%x pa=%08x", fsr, pa);
    CHECK(g_watch_reads32 == 0u && g_watch_writes32 == 0u,
          "page access-flag fault touched target memory");
    g_watch_addr = 0xffffffffu;
}

static void test_fetch_cache_refill_requires_an_exact_live_witness(void) {
    arm_bus_t bus = g_bus;
    arm_cpu_t c;
    uint32_t pa = 0u;
    uint64_t hits, misses, flushes;
    const uint32_t mapped_va = UINT32_C(0x80000420);

    bus.host_ram = m_host_ram;
    memset(g_ram, 0, sizeof g_ram);
    arm_reset(&c, &bus);

    /* MMU-off identity mapping needs no TLB entry and must not invent one in
     * the accounting. */
    hits = c.tlb_hits;
    CHECK(arm_fetch_cache_try_refill(&c, 0x420u, true) &&
          c.fetch_host == g_ram + 0x400u && c.fetch_blk == 0x400u &&
          c.fetch_gen == c.tlb_gen && c.fetch_priv &&
          c.tlb_hits == hits,
          "MMU-off fetch refill did not use the exact identity block");

    /* One privileged FETCH translation seeds the exact 1 KiB TLB witness. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x4000u + (0x800u << 2),
          (3u << 10) | 2u);                 /* VA 0x80000000 -> PA 0 */
    arm_reset(&c, &bus);
    c.cp15.ttbr0 = 0x4000u;
    c.cp15.dacr = 1u;
    c.cp15.sctlr = ARM_SCTLR_M;
    CHECK(arm_mmu_translate(&c, mapped_va, ARM_ACCESS_FETCH, true, &pa) == 0u &&
          pa == 0x420u,
          "could not seed fetch TLB witness: pa=%08x", pa);
    c.fetch_host = NULL;
    hits = c.tlb_hits;
    misses = c.tlb_misses;
    flushes = c.tlb_flushes;
    CHECK(arm_fetch_cache_try_refill(&c, mapped_va, true) &&
          c.fetch_host == g_ram + 0x400u &&
          c.fetch_blk == (mapped_va & ~UINT32_C(0x3ff)) &&
          c.fetch_gen == c.tlb_gen && c.fetch_priv,
          "exact TLB witness did not refill the host fetch block");
    CHECK(c.tlb_hits == hits + 1u && c.tlb_misses == misses &&
          c.tlb_flushes == flushes,
          "successful refill did not replace exactly one interpreter TLB hit");
    hits = c.tlb_hits;
    CHECK(arm_fetch_cache_try_refill(&c, mapped_va, true) &&
          c.tlb_hits == hits,
          "already-live fetch cache counted a second TLB hit");

    /* Privilege and register-stamp differences must refuse without changing
     * either the cache or the counters. */
    c.fetch_host = NULL;
    hits = c.tlb_hits;
    CHECK(!arm_fetch_cache_try_refill(&c, mapped_va, false) &&
          !c.fetch_host && c.tlb_hits == hits,
          "privileged FETCH witness was reused for an unprivileged fetch");
    c.cp15.context_id ^= 1u;
    CHECK(!arm_fetch_cache_try_refill(&c, mapped_va, true) &&
          !c.fetch_host && c.tlb_hits == hits,
          "stale MMU register stamp was accepted");

    /* Restore a current stamp, then distinguish a genuine miss, a cached
     * fault and a successful translation whose target is not plain RAM. */
    c.cp15.context_id ^= 1u;
    CHECK(arm_mmu_translate(&c, mapped_va, ARM_ACCESS_FETCH, true, &pa) == 0u,
          "could not restore the current MMU stamp");
    c.fetch_host = NULL;
    hits = c.tlb_hits;
    misses = c.tlb_misses;
    CHECK(!arm_fetch_cache_try_refill(&c, 0x80000820u, true) &&
          !c.fetch_host && c.tlb_hits == hits && c.tlb_misses == misses,
          "lookup-only refill walked or counted a missing TLB entry");

    pa = 0xdeadbeefu;
    CHECK(arm_mmu_translate(&c, 0x90000420u, ARM_ACCESS_FETCH, true, &pa) != 0u &&
          pa == 0xdeadbeefu,
          "could not seed a cached prefetch fault");
    hits = c.tlb_hits;
    CHECK(!arm_fetch_cache_try_refill(&c, 0x90000420u, true) &&
          !c.fetch_host && c.tlb_hits == hits,
          "cached prefetch fault was consumed or double-counted");

    m_w32(NULL, 0x4000u + (0x900u << 2),
          UINT32_C(0x00200000) | (3u << 10) | 2u);
    arm_mmu_tlb_flush(&c);
    CHECK(arm_mmu_translate(&c, 0x90000420u, ARM_ACCESS_FETCH, true, &pa) == 0u &&
          pa == 0x00200420u,
          "could not seed a non-RAM successful translation: pa=%08x", pa);
    hits = c.tlb_hits;
    CHECK(!arm_fetch_cache_try_refill(&c, 0x90000420u, true) &&
          !c.fetch_host && c.tlb_hits == hits,
          "non-RAM translation produced a direct fetch pointer");
}

static void test_data_cache_refill_requires_an_exact_live_witness(void) {
    arm_bus_t bus = g_bus;
    arm_cpu_t c;
    uint32_t pa = 0u;
    uint64_t hits, misses, dread_hits, dread_misses;
    uint64_t dwrite_hits, dwrite_misses;
    const uint32_t mapped_va = UINT32_C(0x80000420);
    const unsigned read_slot =
        (unsigned)(((mapped_va >> 10) + ARM_DREAD_ENTRIES / 2u) &
                   (ARM_DREAD_ENTRIES - 1u));

    bus.host_ram = m_host_ram;
    bus.host_ram_write = m_host_ram_write;
    memset(g_ram, 0, sizeof g_ram);
    arm_reset(&c, &bus);

    /* MMU-off identity refill is derived state only: it installs the exact
     * block without inventing TLB or data-cache traffic. */
    hits = c.tlb_hits;
    dread_hits = c.dread_hits;
    dread_misses = c.dread_misses;
    CHECK(arm_data_cache_try_refill(&c, 0x420u, ARM_ACCESS_READ, true) &&
          c.dread[(0x420u >> 10) + ARM_DREAD_ENTRIES / 2u].host ==
              g_ram + 0x400u &&
          c.tlb_hits == hits && c.dread_hits == dread_hits &&
          c.dread_misses == dread_misses,
          "MMU-off data refill did not install the identity READ block");
    CHECK(!arm_data_cache_try_refill(&c, 0x420u, ARM_ACCESS_FETCH, true),
          "data refill accepted a FETCH access");

    /* Seed one exact privileged READ translation, then prove that neither a
     * different access class nor privilege can borrow it. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x4000u + (0x800u << 2),
          (3u << 10) | 2u);                 /* VA 0x80000000 -> PA 0 */
    arm_reset(&c, &bus);
    c.cp15.ttbr0 = 0x4000u;
    c.cp15.dacr = 1u;
    c.cp15.sctlr = ARM_SCTLR_M;
    CHECK(arm_mmu_translate(&c, mapped_va, ARM_ACCESS_READ, true, &pa) == 0u &&
          pa == 0x420u,
          "could not seed data READ TLB witness: pa=%08x", pa);
    memset(&c.dread[read_slot], 0, sizeof c.dread[read_slot]);
    hits = c.tlb_hits;
    misses = c.tlb_misses;
    dread_hits = c.dread_hits;
    dread_misses = c.dread_misses;
    CHECK(arm_data_cache_try_refill(&c, mapped_va, ARM_ACCESS_READ, true) &&
          c.dread[read_slot].host == g_ram + 0x400u &&
          c.dread[read_slot].tag ==
              ((mapped_va & ~ARM_DREAD_BLK_MASK) | 1u) &&
          c.dread[read_slot].gen == c.tlb_gen,
          "exact READ TLB witness did not refill DREAD");
    CHECK(c.tlb_hits == hits + 1u && c.tlb_misses == misses &&
          c.dread_hits == dread_hits && c.dread_misses == dread_misses,
          "READ refill changed anything beyond one displaced TLB hit");
    hits = c.tlb_hits;
    CHECK(arm_data_cache_try_refill(&c, mapped_va, ARM_ACCESS_READ, true) &&
          c.tlb_hits == hits,
          "already-live DREAD block counted another TLB hit");

    memset(&c.dread[read_slot], 0, sizeof c.dread[read_slot]);
    hits = c.tlb_hits;
    misses = c.tlb_misses;
    CHECK(!arm_data_cache_try_refill(&c, mapped_va, ARM_ACCESS_READ, false) &&
          !c.dread[(mapped_va >> 10) & (ARM_DREAD_ENTRIES - 1u)].host &&
          c.tlb_hits == hits && c.tlb_misses == misses,
          "privileged READ witness was reused as unprivileged");
    dwrite_hits = c.dwrite_hits;
    dwrite_misses = c.dwrite_misses;
    CHECK(!arm_data_cache_try_refill(&c, mapped_va, ARM_ACCESS_WRITE, true) &&
          !c.dwrite[read_slot].host && c.tlb_hits == hits &&
          c.tlb_misses == misses && c.dwrite_hits == dwrite_hits &&
          c.dwrite_misses == dwrite_misses,
          "READ witness was reused as WRITE or changed counters");

    /* A current WRITE witness still requires live observer-bypass consent. */
    CHECK(arm_mmu_translate(&c, mapped_va, ARM_ACCESS_WRITE, true, &pa) == 0u,
          "could not seed data WRITE TLB witness");
    hits = c.tlb_hits;
    bus.host_ram_write = NULL;
    CHECK(!arm_data_cache_try_refill(&c, mapped_va, ARM_ACCESS_WRITE, true) &&
          !c.dwrite[read_slot].host && c.tlb_hits == hits,
          "WRITE refill ignored revoked host_ram_write consent");
    bus.host_ram_write = m_host_ram_write;
    CHECK(arm_data_cache_try_refill(&c, mapped_va, ARM_ACCESS_WRITE, true) &&
          c.dwrite[read_slot].host == g_ram + 0x400u &&
          c.tlb_hits == hits + 1u,
          "exact WRITE witness did not refill DWRITE");

    /* Missing, faulting, stale-register and non-RAM witnesses all refuse
     * without a walk, cache mutation or counter change. */
    memset(&c.dread[read_slot], 0, sizeof c.dread[read_slot]);
    hits = c.tlb_hits;
    misses = c.tlb_misses;
    CHECK(!arm_data_cache_try_refill(&c, 0x80000820u,
                                     ARM_ACCESS_READ, true) &&
          c.tlb_hits == hits && c.tlb_misses == misses,
          "lookup-only data refill walked a missing TLB entry");
    pa = 0xdeadbeefu;
    CHECK(arm_mmu_translate(&c, 0x90000420u,
                            ARM_ACCESS_READ, true, &pa) != 0u &&
          pa == 0xdeadbeefu,
          "could not seed a cached data fault");
    hits = c.tlb_hits;
    CHECK(!arm_data_cache_try_refill(&c, 0x90000420u,
                                     ARM_ACCESS_READ, true) &&
          c.tlb_hits == hits,
          "cached data fault was consumed or double-counted");
    c.cp15.context_id ^= 1u;
    CHECK(!arm_data_cache_try_refill(&c, mapped_va, ARM_ACCESS_READ, true) &&
          c.tlb_hits == hits,
          "stale MMU register stamp was accepted for data refill");
    c.cp15.context_id ^= 1u;
    m_w32(NULL, 0x4000u + (0xa00u << 2),
          UINT32_C(0x00200000) | (3u << 10) | 2u);
    arm_mmu_tlb_flush(&c);
    CHECK(arm_mmu_translate(&c, 0xa0000420u,
                            ARM_ACCESS_READ, true, &pa) == 0u &&
          pa == 0x00200420u,
          "could not seed non-RAM data translation: pa=%08x", pa);
    hits = c.tlb_hits;
    CHECK(!arm_data_cache_try_refill(&c, 0xa0000420u,
                                     ARM_ACCESS_READ, true) &&
          c.tlb_hits == hits,
          "non-RAM data translation produced a direct pointer");
}

static void test_abort_restores_base_register(void) {
    /* Base Restored Abort Model: after a data abort the base and destination
     * registers must be unchanged so the handler can retry the instruction. */
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x4000 + 0, 0x00000000u | (3u << 10) | 2u);   /* identity map VA 0 */
    uint32_t prog[] = { 0xe4901004 };    /* LDR r1,[r0],#4  (post-indexed) */
    m_w32(NULL, 0, prog[0]);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.cp15.ttbr0 = 0x4000; c.cp15.dacr = 1u; c.cp15.sctlr |= ARM_SCTLR_M;
    c.r[0] = 0x00100000;                 /* unmapped */
    c.r[1] = 0x5a5a5a5a;
    arm_step(&c);
    CHECK(c.r[0] == 0x00100000u, "r0=%08x expect 00100000 (base restored)", c.r[0]);
    CHECK(c.r[1] == 0x5a5a5a5au, "r1=%08x expect 5a5a5a5a (dest untouched)", c.r[1]);
    CHECK(c.cp15.dfar == 0x00100000u, "dfar=%08x expect 00100000", c.cp15.dfar);
}

static void alignment_setup(arm_cpu_t *c, uint32_t insn) {
    g_watch_addr = 0xffffffffu;
    g_watch_reads32 = g_watch_writes32 = 0u;
    g_watch_reads16 = g_watch_writes16 = 0u;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, insn);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c->r[15] = 0u;
}

static void alignment_map_normal_identity(arm_cpu_t *c) {
    const uint32_t l1 = 0x4000u;
    m_w32(NULL, l1, (3u << 10) | 8u | 2u);       /* Normal, full access */
    c->cp15.ttbr0 = l1;
    c->cp15.dacr = 1u;                            /* domain 0 Client */
    c->cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP;
}

static void test_sctlr_a_faults_ordinary_unaligned_accesses(void) {
    arm_cpu_t c;
    alignment_setup(&c, 0xe5901000u);             /* LDR r1,[r0] */
    c.cp15.sctlr |= ARM_SCTLR_A | ARM_SCTLR_U;
    c.r[0] = 0x801u;
    c.r[1] = 0xdeadbeefu;
    g_watch_addr = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "unaligned LDR with A=1 did not take a data abort");
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT && c.cp15.dfar == 0x801u,
          "LDR alignment state dfsr=%x dfar=%08x", c.cp15.dfsr, c.cp15.dfar);
    CHECK((c.cp15.dfsr & (1u << 11)) == 0u && c.r[1] == 0xdeadbeefu,
          "faulting LDR changed WnR or destination");
    CHECK(g_watch_reads32 == 0u, "alignment-faulting LDR issued %u bus reads",
          g_watch_reads32);

    alignment_setup(&c, 0xe5801000u);             /* STR r1,[r0] */
    c.cp15.sctlr |= ARM_SCTLR_A | ARM_SCTLR_U;
    c.r[0] = 0x801u;
    c.r[1] = 0x11223344u;
    g_watch_addr = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "unaligned STR with A=1 did not take a data abort");
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT &&
          (c.cp15.dfsr & (1u << 11)) != 0u && c.cp15.dfar == 0x801u,
          "STR alignment state dfsr=%x dfar=%08x", c.cp15.dfsr, c.cp15.dfar);
    CHECK(g_watch_writes32 == 0u, "alignment-faulting STR issued %u bus writes",
          g_watch_writes32);
    g_watch_addr = 0xffffffffu;
}

static void test_sctlr_u_selects_legacy_or_armv6_unaligned_data(void) {
    arm_cpu_t c;

    /* Legacy U=0 word loads align down, then rotate right by the byte offset. */
    alignment_setup(&c, 0xe5901000u);             /* LDR r1,[r0] */
    m_w32(NULL, 0x800u, 0x11223344u);
    c.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
    c.r[0] = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[1] == 0x44112233u,
          "legacy unaligned LDR=%08x expect rotate-right 44112233", c.r[1]);

    /* Legacy word stores align down without rotation. */
    alignment_setup(&c, 0xe5801000u);             /* STR r1,[r0] */
    c.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
    c.r[0] = 0x803u;
    c.r[1] = 0xa1b2c3d4u;
    g_watch_addr = 0x800u;
    CHECK(arm_step(&c) == ARM_OK && m_r32(NULL, 0x800u) == 0xa1b2c3d4u,
          "legacy unaligned STR did not align down");
    CHECK(g_watch_writes32 == 1u, "legacy STR issued %u aligned writes expect 1",
          g_watch_writes32);

    /* Unlike words, an odd U=0/A=0 halfword is architecturally
     * UNPREDICTABLE. Refuse it before the align-down bus behavior leaks out as
     * a fabricated value. */
    alignment_setup(&c, 0xe1d010b0u);             /* LDRH r1,[r0] */
    m_w16(NULL, 0x800u, 0x1234u);
    c.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
    c.r[0] = 0x801u;
    c.r[1] = 0xdeadbeefu;
    g_watch_addr = 0x800u;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[1] == 0xdeadbeefu,
          "legacy odd LDRH should be refused without changing r1");
    CHECK(g_watch_reads16 == 0u, "undefined legacy LDRH issued %u bus reads",
          g_watch_reads16);

    /* U=1 enables the bytewise ARMv6 value in Normal memory. */
    alignment_setup(&c, 0xe5901000u);
    alignment_map_normal_identity(&c);
    g_ram[0x801u] = 0x11u; g_ram[0x802u] = 0x22u;
    g_ram[0x803u] = 0x33u; g_ram[0x804u] = 0x44u;
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[0] = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[1] == 0x44332211u,
          "ARMv6 U=1 LDR=%08x expect unaligned 44332211", c.r[1]);

    alignment_setup(&c, 0xe1d010b0u);
    alignment_map_normal_identity(&c);
    g_ram[0x801u] = 0x34u; g_ram[0x802u] = 0x12u;
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[0] = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[1] == 0x1234u,
          "ARMv6 U=1 LDRH=%08x expect unaligned 1234", c.r[1]);
    g_watch_addr = 0xffffffffu;
}

static void test_multiword_alignment_depends_on_sctlr_u_a(void) {
    arm_cpu_t c;

    /* U=1 makes an unaligned LDM an alignment fault even though A is clear. */
    alignment_setup(&c, 0xe8900006u);             /* LDMIA r0,{r1,r2} */
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[0] = 0x801u;
    c.r[1] = 0xaaaaaaaau; c.r[2] = 0xbbbbbbbbu;
    g_watch_addr = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "U=1 misaligned LDM did not take an alignment abort");
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT &&
          (c.cp15.dfsr & (1u << 11)) == 0u,
          "LDM alignment dfsr=%x", c.cp15.dfsr);
    CHECK(g_watch_reads32 == 0u && c.r[1] == 0xaaaaaaaau && c.r[2] == 0xbbbbbbbbu,
          "faulting LDM touched the bus or destination registers");

    /* U=0/A=0 keeps the legacy align-down behavior for a multiple transfer. */
    alignment_setup(&c, 0xe8900006u);
    m_w32(NULL, 0x800u, 0x11223344u);
    m_w32(NULL, 0x804u, 0x55667788u);
    c.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
    c.r[0] = 0x803u;
    CHECK(arm_step(&c) == ARM_OK && c.r[1] == 0x11223344u &&
          c.r[2] == 0x55667788u,
          "legacy LDM did not align start down: %08x/%08x", c.r[1], c.r[2]);

    /* The write form reports WnR and performs no first transaction. */
    alignment_setup(&c, 0xe8800006u);             /* STMIA r0,{r1,r2} */
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[0] = 0x801u;
    c.r[1] = 0x11223344u; c.r[2] = 0x55667788u;
    g_watch_addr = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "U=1 misaligned STM did not take an alignment abort");
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT &&
          (c.cp15.dfsr & (1u << 11)) != 0u && g_watch_writes32 == 0u,
          "STM alignment dfsr=%x writes=%u", c.cp15.dfsr, g_watch_writes32);

    /* Coprocessor transfers use the same strict U=1 word-alignment rule. */
    alignment_setup(&c, 0xed910a00u);             /* VLDR s0,[r1] */
    c.cp15.cpacr = 0xfu << 20;
    c.vfp_fpexc = ARM_FPEXC_EN;
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[1] = 0x801u;
    g_watch_addr = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT &&
          (c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT,
          "U=1 misaligned VLDR did not take an alignment abort");
    CHECK(g_watch_reads32 == 0u, "misaligned VLDR issued %u bus reads",
          g_watch_reads32);
    g_watch_addr = 0xffffffffu;
}

static void test_arm_multiword_base_writeback_restrictions(void) {
    arm_cpu_t c;

    alignment_setup(&c, 0xe8b10002u);           /* LDMIA r1!,{r1} */
    c.r[1] = 0x800u;
    g_watch_addr = 0x800u;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[1] == 0x800u,
          "LDM writeback with its base in the list must be UNPREDICTABLE");
    CHECK(g_watch_reads32 == 0u,
          "undefined base-in-list LDM issued %u reads", g_watch_reads32);

    alignment_setup(&c, 0xe8a10003u);           /* STMIA r1!,{r0,r1} */
    c.r[0] = 0x11223344u;
    c.r[1] = 0x800u;
    g_watch_addr = 0x800u;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[1] == 0x800u,
          "STM with a non-lowest writeback base must be UNPREDICTABLE");
    CHECK(g_watch_writes32 == 0u,
          "undefined non-lowest-base STM issued %u writes", g_watch_writes32);

    /* Rn in the list is defined for STM when it is the lowest-numbered
     * register: the original base is stored before normal writeback. */
    alignment_setup(&c, 0xe8a00003u);           /* STMIA r0!,{r0,r1} */
    c.r[0] = 0x800u;
    c.r[1] = 0xa1b2c3d4u;
    CHECK(arm_step(&c) == ARM_OK && m_r32(NULL, 0x800u) == 0x800u &&
          m_r32(NULL, 0x804u) == 0xa1b2c3d4u && c.r[0] == 0x808u,
          "defined lowest-base STM did not store the original base/write back");
    g_watch_addr = 0xffffffffu;
}

static void check_unpredictable_single_transfer(uint32_t insn,
                                                uint32_t target,
                                                const char *what) {
    arm_cpu_t c;
    uint32_t sentinel = 0x5a96c33cu;

    g_watch_addr = 0xffffffffu;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, insn);
    m_w32(NULL, target, sentinel);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.r[0] = 0x800u;
    c.r[1] = 0x800u;
    c.r[2] = 0x10u;
    c.r[15] = 0u;

    g_watch_addr = target;
    g_watch_reads8 = g_watch_writes8 = 0u;
    g_watch_reads16 = g_watch_writes16 = 0u;
    g_watch_reads32 = g_watch_writes32 = 0u;
    arm_status_t st = arm_step(&c);
    g_watch_addr = 0xffffffffu;

    uint32_t after;
    memcpy(&after, &g_ram[target & (RAM_SIZE - 1u)], sizeof after);
    CHECK(st == ARM_UNDEFINED, "%s returned status %d", what, (int)st);
    CHECK(g_watch_reads8 == 0u && g_watch_writes8 == 0u &&
          g_watch_reads16 == 0u && g_watch_writes16 == 0u &&
          g_watch_reads32 == 0u && g_watch_writes32 == 0u,
          "%s touched its target bus address (r8=%u w8=%u r16=%u w16=%u "
          "r32=%u w32=%u)", what, g_watch_reads8, g_watch_writes8,
          g_watch_reads16, g_watch_writes16, g_watch_reads32, g_watch_writes32);
    CHECK(c.r[0] == 0x800u && c.r[1] == 0x800u && c.r[2] == 0x10u &&
          c.r[15] == 0u && after == sentinel,
          "%s changed architectural state before trapping", what);
}

static void test_single_transfer_zero_post_same_base_load(void) {
    arm_cpu_t c;

    g_watch_addr = 0xffffffffu;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe4933000u); /* LDR r3,[r3],#0 */
    m_w32(NULL, 0x800u, 0x12345678u);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.r[3] = 0x800u;
    c.r[15] = 0u;

    g_watch_addr = 0x800u;
    g_watch_reads8 = g_watch_writes8 = 0u;
    g_watch_reads16 = g_watch_writes16 = 0u;
    g_watch_reads32 = g_watch_writes32 = 0u;
    arm_status_t st = arm_step(&c);
    g_watch_addr = 0xffffffffu;

    CHECK(st == ARM_OK && c.r[3] == 0x12345678u && c.r[15] == 4u,
          "zero-offset LDR Rd,[Rd],#0 status=%d rd=%08x pc=%08x",
          (int)st, c.r[3], c.r[15]);
    CHECK(g_watch_reads32 == 1u && g_watch_writes32 == 0u &&
          g_watch_reads8 == 0u && g_watch_writes8 == 0u &&
          g_watch_reads16 == 0u && g_watch_writes16 == 0u,
          "zero-offset LDR Rd,[Rd],#0 bus r8=%u w8=%u r16=%u w16=%u "
          "r32=%u w32=%u",
          g_watch_reads8, g_watch_writes8,
          g_watch_reads16, g_watch_writes16,
          g_watch_reads32, g_watch_writes32);
}

static void test_single_transfer_unpredictable_forms_trap_before_bus(void) {
    /* Addressing mode 2: any post-indexed form writes back, as does P=1,W=1.
     * Apart from the separately tested immediate-zero load, Rn==Rd is
     * ambiguous for both loads and stores. R15 cannot be a writeback base or
     * a register offset, and byte transfers reserve R15 as their data
     * register. */
    check_unpredictable_single_transfer(0xe4900004u, 0x800u,
                                        "LDR r0,[r0],#4");
    check_unpredictable_single_transfer(0xe4800004u, 0x800u,
                                        "STR r0,[r0],#4");
    check_unpredictable_single_transfer(0xe5b00004u, 0x804u,
                                        "LDR r0,[r0,#4]!");
    check_unpredictable_single_transfer(0xe5a00004u, 0x804u,
                                        "STR r0,[r0,#4]!");
    check_unpredictable_single_transfer(0xe49f0004u, 0x008u,
                                        "LDR r0,[pc],#4");
    check_unpredictable_single_transfer(0xe791000fu, 0x808u,
                                        "LDR r0,[r1,pc]");
    check_unpredictable_single_transfer(0xe5d1f000u, 0x800u,
                                        "LDRB pc,[r1]");
    check_unpredictable_single_transfer(0xe5c1f000u, 0x800u,
                                        "STRB pc,[r1]");
    check_unpredictable_single_transfer(0xe4b1f004u, 0x800u,
                                        "LDRT pc,[r1],#4");

    /* Addressing mode 3 has no P=0,W=1 translation form. Its register-offset
     * encoding reserves bits[11:8] and R15, and its writeback aliases have the
     * same ambiguity as mode 2. */
    check_unpredictable_single_transfer(0xe0f100b2u, 0x800u,
                                        "invalid P=0,W=1 LDRH");
    check_unpredictable_single_transfer(0xe19101b2u, 0x810u,
                                        "LDRH with nonzero SBZ bits");
    check_unpredictable_single_transfer(0xe19100bfu, 0x808u,
                                        "LDRH r0,[r1,pc]");
    check_unpredictable_single_transfer(0xe1f000b2u, 0x802u,
                                        "LDRH r0,[r0,#2]!");
    check_unpredictable_single_transfer(0xe0c000b2u, 0x800u,
                                        "STRH r0,[r0],#2");
    check_unpredictable_single_transfer(0xe1ff00b2u, 0x00au,
                                        "LDRH r0,[pc,#2]!");
}

/*
 * LDRD/STRD. These are ARM-mode only, which is why a Thumb-compiled daemon
 * never reached them, but ARM library code does. The pair is Rd and Rd+1 and
 * the transfer is two ordinary word accesses at addr and addr+4, so everything
 * about the addressing modes is shared with the halfword forms above.
 */
static void test_ldrd_strd_transfer_the_register_pair(void) {
    arm_cpu_t c;

    /* LDRD r0,r1,[r2]: low word into Rd, high word into Rd+1. */
    alignment_setup(&c, 0xe1c200d0u);             /* LDRD r0,r1,[r2] */
    m_w32(NULL, 0x800u, 0x11223344u);
    m_w32(NULL, 0x804u, 0x55667788u);
    c.cp15.sctlr |= ARM_SCTLR_U;
    c.r[2] = 0x800u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u &&
          c.r[0] == 0x11223344u && c.r[1] == 0x55667788u,
          "LDRD pair=%08x/%08x expect 11223344/55667788", c.r[0], c.r[1]);

    /* STRD r0,r1,[r2] writes BOTH words, in ascending address order. */
    alignment_setup(&c, 0xe1c200f0u);             /* STRD r0,r1,[r2] */
    c.cp15.sctlr |= ARM_SCTLR_U;
    c.r[0] = 0xa1b2c3d4u; c.r[1] = 0xe5f60718u; c.r[2] = 0x800u;
    CHECK(arm_step(&c) == ARM_OK && m_r32(NULL, 0x800u) == 0xa1b2c3d4u &&
          m_r32(NULL, 0x804u) == 0xe5f60718u,
          "STRD wrote %08x/%08x expect a1b2c3d4/e5f60718",
          m_r32(NULL, 0x800u), m_r32(NULL, 0x804u));

    /* The addressing-mode-3 machinery is the shared one: immediate offset up
     * and down, register offset, and both writeback forms. */
    alignment_setup(&c, 0xe1c200d8u);             /* LDRD r0,r1,[r2,#8] */
    m_w32(NULL, 0x808u, 0xdeadbeefu);
    m_w32(NULL, 0x80cu, 0xfeedfaceu);
    c.cp15.sctlr |= ARM_SCTLR_U;
    c.r[2] = 0x800u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0xdeadbeefu &&
          c.r[1] == 0xfeedfaceu && c.r[2] == 0x800u,
          "LDRD [r2,#8] pair=%08x/%08x base=%08x", c.r[0], c.r[1], c.r[2]);

    alignment_setup(&c, 0xe14200d8u);             /* LDRD r0,r1,[r2,#-8] */
    m_w32(NULL, 0x800u, 0x13579bdfu);
    m_w32(NULL, 0x804u, 0x02468aceu);
    c.cp15.sctlr |= ARM_SCTLR_U;
    c.r[2] = 0x808u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x13579bdfu &&
          c.r[1] == 0x02468aceu,
          "LDRD [r2,#-8] pair=%08x/%08x", c.r[0], c.r[1]);

    alignment_setup(&c, 0xe18200d3u);             /* LDRD r0,r1,[r2,r3] */
    m_w32(NULL, 0x810u, 0x0000cafeu);
    m_w32(NULL, 0x814u, 0x0000babeu);
    c.cp15.sctlr |= ARM_SCTLR_U;
    c.r[2] = 0x800u; c.r[3] = 0x10u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x0000cafeu &&
          c.r[1] == 0x0000babeu,
          "LDRD [r2,r3] pair=%08x/%08x", c.r[0], c.r[1]);

    alignment_setup(&c, 0xe0c200d8u);             /* LDRD r0,r1,[r2],#8 */
    m_w32(NULL, 0x800u, 0x00000001u);
    m_w32(NULL, 0x804u, 0x00000002u);
    c.cp15.sctlr |= ARM_SCTLR_U;
    c.r[2] = 0x800u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 1u && c.r[1] == 2u &&
          c.r[2] == 0x808u,
          "post-indexed LDRD pair=%08x/%08x base=%08x", c.r[0], c.r[1], c.r[2]);

    alignment_setup(&c, 0xe1e200f8u);             /* STRD r0,r1,[r2,#8]! */
    c.cp15.sctlr |= ARM_SCTLR_U;
    c.r[0] = 0x0f0f0f0fu; c.r[1] = 0xf0f0f0f0u; c.r[2] = 0x800u;
    CHECK(arm_step(&c) == ARM_OK && m_r32(NULL, 0x808u) == 0x0f0f0f0fu &&
          m_r32(NULL, 0x80cu) == 0xf0f0f0f0u && c.r[2] == 0x808u,
          "pre-indexed STRD wrote %08x/%08x base=%08x",
          m_r32(NULL, 0x808u), m_r32(NULL, 0x80cu), c.r[2]);
}

static void test_ldrd_strd_alignment_is_the_armv6_word_rule(void) {
    arm_cpu_t c;

    /*
     * The case most likely to be got wrong. ARMv5TE required a doubleword-
     * aligned address; ARMv6 relaxed that to the multiword rule, which is word
     * granularity. base+4 is word aligned but NOT doubleword aligned, and with
     * SCTLR.U=1, A=0 it must succeed rather than fault.
     */
    alignment_setup(&c, 0xe1c200d0u);             /* LDRD r0,r1,[r2] */
    m_w32(NULL, 0x804u, 0x0badf00du);
    m_w32(NULL, 0x808u, 0x8badbeefu);
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[2] = 0x804u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u &&
          c.r[0] == 0x0badf00du && c.r[1] == 0x8badbeefu,
          "word-but-not-doubleword-aligned LDRD pair=%08x/%08x pc=%08x",
          c.r[0], c.r[1], c.r[15]);

    alignment_setup(&c, 0xe1c200f0u);             /* STRD r0,r1,[r2] */
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[0] = 0x12345678u; c.r[1] = 0x9abcdef0u; c.r[2] = 0x804u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u &&
          m_r32(NULL, 0x804u) == 0x12345678u &&
          m_r32(NULL, 0x808u) == 0x9abcdef0u,
          "word-but-not-doubleword-aligned STRD wrote %08x/%08x pc=%08x",
          m_r32(NULL, 0x804u), m_r32(NULL, 0x808u), c.r[15]);

    /* A is the alignment CHECK, not a stricter alignment: a word-aligned pair
     * is still legal with A set, exactly as it is for LDM. */
    alignment_setup(&c, 0xe1c200d0u);
    m_w32(NULL, 0x804u, 0xaaaa5555u);
    m_w32(NULL, 0x808u, 0x5555aaaau);
    c.cp15.sctlr |= ARM_SCTLR_A | ARM_SCTLR_U;
    c.r[2] = 0x804u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0xaaaa5555u &&
          c.r[1] == 0x5555aaaau,
          "A=1 word-aligned LDRD pair=%08x/%08x", c.r[0], c.r[1]);

    /* base+2 is not word aligned: alignment abort, and no bus access at all. */
    alignment_setup(&c, 0xe1c200d0u);
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[0] = 0xdeadbeefu; c.r[1] = 0xfeedfaceu; c.r[2] = 0x802u;
    g_watch_addr = 0x802u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "halfword-aligned LDRD did not take an alignment abort");
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT &&
          (c.cp15.dfsr & (1u << 11)) == 0u && c.cp15.dfar == 0x802u,
          "LDRD alignment state dfsr=%x dfar=%08x", c.cp15.dfsr, c.cp15.dfar);
    CHECK(g_watch_reads32 == 0u && c.r[0] == 0xdeadbeefu &&
          c.r[1] == 0xfeedfaceu,
          "alignment-faulting LDRD issued %u reads or moved the pair",
          g_watch_reads32);

    /* The store form reports WnR and performs no transaction either. */
    alignment_setup(&c, 0xe1c200f0u);
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[0] = 0x11223344u; c.r[1] = 0x55667788u; c.r[2] = 0x802u;
    g_watch_addr = 0x802u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "halfword-aligned STRD did not take an alignment abort");
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT &&
          (c.cp15.dfsr & (1u << 11)) != 0u && c.cp15.dfar == 0x802u &&
          g_watch_writes32 == 0u,
          "STRD alignment dfsr=%x dfar=%08x writes=%u",
          c.cp15.dfsr, c.cp15.dfar, g_watch_writes32);

    /* Legacy U=0/A=0 aligns the transfer down, like any other multiword one —
     * and the aligned-down value must not leak into the writeback, which is
     * still base+offset. */
    alignment_setup(&c, 0xe1e200d3u);             /* LDRD r0,r1,[r2,#3]! */
    m_w32(NULL, 0x800u, 0x11223344u);
    m_w32(NULL, 0x804u, 0x55667788u);
    c.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
    c.r[2] = 0x800u;
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x11223344u &&
          c.r[1] == 0x55667788u,
          "legacy LDRD did not align the transfer down: %08x/%08x",
          c.r[0], c.r[1]);
    CHECK(c.r[2] == 0x803u,
          "base=%08x expect 803 — writeback is base+offset, not the "
          "aligned-down transfer address", c.r[2]);
    g_watch_addr = 0xffffffffu;
}

static void test_ldrd_strd_operand_restrictions_trap_before_bus(void) {
    /* Rd names a PAIR: it must be even, and R14 would make R15 its second
     * half. A writeback base may alias neither half. Every one is
     * UNPREDICTABLE and must be refused before any bus access. */
    check_unpredictable_single_transfer(0xe1c130d0u, 0x800u,
                                        "LDRD r3,r4,[r1] (odd Rd)");
    check_unpredictable_single_transfer(0xe1c130f0u, 0x800u,
                                        "STRD r3,r4,[r1] (odd Rd)");
    check_unpredictable_single_transfer(0xe1c1e0d0u, 0x800u,
                                        "LDRD r14,pc,[r1]");
    check_unpredictable_single_transfer(0xe1c1e0f0u, 0x800u,
                                        "STRD r14,pc,[r1]");
    check_unpredictable_single_transfer(0xe1c1f0d0u, 0x800u,
                                        "LDRD pc,[r1]");
    check_unpredictable_single_transfer(0xe1e100d4u, 0x804u,
                                        "LDRD r0,r1,[r1,#4]! (Rn == Rd+1)");
    check_unpredictable_single_transfer(0xe0c100f8u, 0x800u,
                                        "STRD r0,r1,[r1],#8 (Rn == Rd+1)");
    check_unpredictable_single_transfer(0xe0c000d8u, 0x800u,
                                        "LDRD r0,r1,[r0],#8 (Rn == Rd)");
    check_unpredictable_single_transfer(0xe0e100d4u, 0x800u,
                                        "LDRD with the invalid P=0,W=1 form");
    check_unpredictable_single_transfer(0xe18101d2u, 0x810u,
                                        "LDRD with nonzero SBZ bits");
}

static void test_exclusive_alignment_is_never_silently_fixed(void) {
    arm_cpu_t c;
    alignment_setup(&c, 0xe1901f9fu);             /* LDREX r1,[r0] */
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[0] = 0x801u;
    g_watch_addr = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT &&
          (c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT,
          "U=1 misaligned LDREX did not alignment-fault");
    CHECK(g_watch_reads32 == 0u && !c.excl_valid,
          "misaligned LDREX touched the bus or set the monitor");

    alignment_setup(&c, 0xe1b14f9fu);             /* LDREXD r4,r5,[r1] */
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    c.r[1] = 0x804u;                              /* word, not doubleword aligned */
    g_watch_addr = 0x804u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT &&
          (c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT,
          "word-aligned but not 64-bit-aligned LDREXD did not fault");
    CHECK(g_watch_reads32 == 0u && !c.excl_valid,
          "misaligned LDREXD touched the bus or set the monitor");

    /* In the legacy 00 configuration this form is UNPREDICTABLE, not an
     * ordinary word load that can be aligned down and rotated. */
    alignment_setup(&c, 0xe1901f9fu);
    c.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
    c.r[0] = 0x801u;
    g_watch_addr = 0x801u;
    CHECK(arm_step(&c) == ARM_UNDEFINED,
          "legacy misaligned LDREX should be refused as UNPREDICTABLE");
    CHECK(g_watch_reads32 == 0u && !c.excl_valid,
          "undefined LDREX touched the bus or set the monitor");
    g_watch_addr = 0xffffffffu;
}

static void check_unpredictable_atomic_operands(uint32_t insn, const char *what) {
    arm_cpu_t c;
    uint32_t before[16];

    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, insn);
    m_w32(NULL, 0x800u, 0x12345678u);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    for (unsigned i = 0; i < 15u; i++) c.r[i] = 0x10000000u + i;
    c.r[1] = 0x800u;                         /* base for every case */
    c.r[15] = 0u;
    c.excl_valid = true;
    c.excl_addr = 0x800u;
    memcpy(before, c.r, sizeof before);

    g_watch_addr = 0x800u;
    g_watch_reads8 = g_watch_writes8 = 0u;
    g_watch_reads16 = g_watch_writes16 = 0u;
    g_watch_reads32 = g_watch_writes32 = 0u;
    arm_status_t st = arm_step(&c);
    g_watch_addr = 0xffffffffu;

    CHECK(st == ARM_UNDEFINED, "%s returned status %d", what, (int)st);
    CHECK(memcmp(before, c.r, sizeof before) == 0 &&
          c.excl_valid && c.excl_addr == 0x800u,
          "%s changed registers or consumed the monitor", what);
    CHECK(g_watch_reads8 == 0u && g_watch_writes8 == 0u &&
          g_watch_reads16 == 0u && g_watch_writes16 == 0u &&
          g_watch_reads32 == 0u && g_watch_writes32 == 0u,
          "%s touched memory before operand validation", what);
}

static void test_atomic_operand_aliases_are_rejected_before_bus(void) {
    check_unpredictable_atomic_operands(0xe1811f90u, "STREX status==base");
    check_unpredictable_atomic_operands(0xe1810f90u, "STREX status==data");
    check_unpredictable_atomic_operands(0xe1a11f96u, "STREXD status==base");
    check_unpredictable_atomic_operands(0xe1a16f96u, "STREXD status==data-low");
    check_unpredictable_atomic_operands(0xe1a17f96u, "STREXD status==data-high");
    check_unpredictable_atomic_operands(0xe1c11f90u, "STREXB status==base");
    check_unpredictable_atomic_operands(0xe1c10f90u, "STREXB status==data");
    check_unpredictable_atomic_operands(0xe1e11f90u, "STREXH status==base");
    check_unpredictable_atomic_operands(0xe1e10f90u, "STREXH status==data");
    check_unpredictable_atomic_operands(0xe1012091u, "SWP base==data");
    check_unpredictable_atomic_operands(0xe1011090u, "SWP base==destination");
}

/* --------------------------------------------------------------- Thumb --- */

/* Load 16-bit Thumb halfwords at `base` and run from there in Thumb state. */
static void load_thumb(arm_cpu_t *c, const uint16_t *prog, size_t n, int steps) {
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < n; i++) m_w16(NULL, (uint32_t)(i*2), prog[i]);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS;
    c->cpsr |= ARM_CPSR_T;
    c->r[15] = 0;
    for (int i = 0; i < steps; i++) if (arm_step(c) != ARM_OK) break;
}

static void test_thumb_mov_add(void) {
    /* MOV r0,#40 ; MOV r1,#2 ; ADD r0,r0,r1 */
    uint16_t p[] = { 0x2028, 0x2102, 0x1840 };
    arm_cpu_t c; load_thumb(&c, p, 3, 3);
    CHECK(c.r[0] == 42, "r0=%u expect 42", c.r[0]);
    CHECK(c.r[15] == 6, "pc=%u expect 6 (2 bytes per instruction)", c.r[15]);
}

static void test_thumb_lsl_flags(void) {
    /* MOV r0,#1 ; LSL r1,r0,#31 -> N set */
    uint16_t p[] = { 0x2001, 0x07c1 };
    arm_cpu_t c; load_thumb(&c, p, 2, 2);
    CHECK(c.r[1] == 0x80000000u, "r1=%08x expect 80000000", c.r[1]);
    CHECK((c.cpsr & ARM_CPSR_N) != 0, "N should be set");
}

static void test_thumb_push_pop(void) {
    /* SP=0x900 ; r0=0xAA ; r1=0xBB ; PUSH {r0,r1} ; POP {r2,r3} */
    uint16_t p[] = { 0x20aa,        /* MOV r0,#0xAA */
                     0x21bb,        /* MOV r1,#0xBB */
                     0xb403,        /* PUSH {r0,r1} */
                     0xbc0c };      /* POP  {r2,r3} */
    arm_cpu_t c; load_thumb(&c, p, 4, 0);
    c.r[13] = 0x900;
    for (int i = 0; i < 4; i++) arm_step(&c);
    CHECK(c.r[2] == 0xaa, "r2=%08x expect aa", c.r[2]);
    CHECK(c.r[3] == 0xbb, "r3=%08x expect bb", c.r[3]);
    CHECK(c.r[13] == 0x900, "sp=%08x expect 900 (balanced)", c.r[13]);
}

static void thumb_alignment_setup(arm_cpu_t *c, uint16_t insn) {
    g_watch_addr = 0xffffffffu;
    g_watch_reads32 = g_watch_writes32 = 0u;
    memset(g_ram, 0, sizeof g_ram);
    m_w16(NULL, 0u, insn);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS | ARM_CPSR_T;
    c->r[15] = 0u;
}

static void test_thumb_multiword_alignment_uses_strict_rules(void) {
    arm_cpu_t c;

    /* Thumb LDMIA is a multiple transfer too: U=1 makes a misaligned base an
     * alignment abort before the first read, rather than an ordinary unaligned
     * LDR performed once per register. */
    thumb_alignment_setup(&c, 0xc802u);           /* LDMIA r0!,{r1} */
    c.cp15.sctlr = ARM_SCTLR_U;
    c.r[0] = 0x801u;
    c.r[1] = 0xdeadbeefu;
    g_watch_addr = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT &&
          (c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT,
          "misaligned Thumb LDMIA did not take an alignment abort");
    CHECK(g_watch_reads32 == 0u && c.r[0] == 0x801u &&
          c.r[1] == 0xdeadbeefu,
          "faulting Thumb LDMIA touched the bus, base, or destination");

    /* PUSH has a separate Thumb decoder path and must apply the same rule.
     * Its fault is a write and the pre-decremented first address is reported. */
    thumb_alignment_setup(&c, 0xb402u);           /* PUSH {r1} */
    c.cp15.sctlr = ARM_SCTLR_U;
    c.r[13] = 0x805u;
    c.r[1] = 0x11223344u;
    g_watch_addr = 0x801u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT &&
          (c.cp15.dfsr & 0xfu) == ARM_FSR_ALIGNMENT &&
          (c.cp15.dfsr & (1u << 11)) != 0u && c.cp15.dfar == 0x801u,
          "misaligned Thumb PUSH did not report a write alignment abort");
    CHECK(g_watch_writes32 == 0u && c.bank_r13[ARM_BANK_USR] == 0x805u,
          "faulting Thumb PUSH touched memory or changed SP");

    /* Legacy U=0/A=0 aligns the memory address down but computes writeback
     * from the original base, just like the ARM-state multiple form. */
    thumb_alignment_setup(&c, 0xc802u);           /* LDMIA r0!,{r1} */
    m_w32(NULL, 0x800u, 0xa1b2c3d4u);
    c.r[0] = 0x803u;
    g_watch_addr = 0x800u;
    CHECK(arm_step(&c) == ARM_OK && c.r[1] == 0xa1b2c3d4u &&
          c.r[0] == 0x807u && g_watch_reads32 == 1u,
          "legacy Thumb LDMIA did not align data and preserve writeback");

    /* Empty PUSH/POP lists are UNPREDICTABLE; refuse them without touching
     * the stack instead of treating them as benign no-ops. */
    thumb_alignment_setup(&c, 0xb400u);           /* PUSH {} */
    c.r[13] = 0x900u;
    g_watch_addr = 0x8fcu;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[13] == 0x900u &&
          g_watch_writes32 == 0u,
          "empty Thumb PUSH was not refused before memory/writeback");

    /* Like ARM STM, Thumb STMIA only defines a base-in-list store when the
     * base is the lowest-numbered register. */
    thumb_alignment_setup(&c, 0xc103u);           /* STMIA r1!,{r0,r1} */
    c.r[0] = 0x11223344u;
    c.r[1] = 0x800u;
    g_watch_addr = 0x800u;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[1] == 0x800u &&
          g_watch_writes32 == 0u,
          "Thumb STMIA accepted a non-lowest writeback base");

    thumb_alignment_setup(&c, 0xc003u);           /* STMIA r0!,{r0,r1} */
    c.r[0] = 0x800u;
    c.r[1] = 0x55667788u;
    CHECK(arm_step(&c) == ARM_OK && m_r32(NULL, 0x800u) == 0x800u &&
          m_r32(NULL, 0x804u) == 0x55667788u && c.r[0] == 0x808u,
          "defined Thumb lowest-base STMIA stored/writeback incorrectly");

    thumb_alignment_setup(&c, 0xbd00u);           /* POP {pc} */
    m_w32(NULL, 0x800u, 0x102u);
    c.r[13] = 0x800u;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u &&
          c.r[13] == 0x800u && (c.cpsr & ARM_CPSR_T) != 0u,
          "Thumb POP accepted an impossible ARM-state 0b10 target");
    g_watch_addr = 0xffffffffu;
}

static void test_thumb_conditional_branch(void) {
    /* MOV r0,#1 ; CMP r0,#1 ; BNE +4 (not taken) ; MOV r1,#7 */
    uint16_t p[] = { 0x2001, 0x2801, 0xd101, 0x2107 };
    arm_cpu_t c; load_thumb(&c, p, 4, 4);
    CHECK(c.r[1] == 7, "r1=%u expect 7 (BNE not taken)", c.r[1]);
}

static void test_arm_to_thumb_and_back(void) {
    /* ARM: set r0 = 0x10|1 and BX to it, landing in Thumb at 0x10.
     * Thumb at 0x10: MOV r1,#5 ; then BX r2 with r2 = 0x20 (back to ARM).
     * ARM at 0x20: MOV r3,#9. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x00, 0xe3a00011);      /* MOV r0,#0x11        */
    m_w32(NULL, 0x04, 0xe3a02020);      /* MOV r2,#0x20        */
    m_w32(NULL, 0x08, 0xe12fff10);      /* BX  r0  -> Thumb    */
    m_w16(NULL, 0x10, 0x2105);          /* Thumb: MOV r1,#5    */
    m_w16(NULL, 0x12, 0x4710);          /* Thumb: BX r2 -> ARM */
    m_w32(NULL, 0x20, 0xe3a03009);      /* ARM:   MOV r3,#9    */

    arm_cpu_t c; arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
    for (int i = 0; i < 6; i++) arm_step(&c);

    CHECK(c.r[1] == 5, "r1=%u expect 5 (Thumb code ran)", c.r[1]);
    CHECK(c.r[3] == 9, "r3=%u expect 9 (returned to ARM)", c.r[3]);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "T bit should be clear back in ARM state");
}

static void test_bx_blx_register_reject_invalid_targets_without_side_effects(void) {
    arm_cpu_t c;

    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0u, 0xe12fff3fu);              /* BLX pc: UNPREDICTABLE */
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.r[14] = 0xa5a5a5a5u;
    c.r[15] = 0u;
    uint32_t cpsr = c.cpsr;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[14] == 0xa5a5a5a5u &&
          c.r[15] == 0u && c.cpsr == cpsr,
          "ARM BLX pc changed LR/PC/state before trapping");

    memset(g_ram, 0, sizeof g_ram);
    m_w16(NULL, 0u, 0x47f8u);                  /* Thumb BLX pc */
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS | ARM_CPSR_T;
    c.r[14] = 0xa5a5a5a5u;
    c.r[15] = 0u;
    cpsr = c.cpsr;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[14] == 0xa5a5a5a5u &&
          c.r[15] == 0u && c.cpsr == cpsr,
          "Thumb BLX pc changed LR/PC/state before trapping");

    static const struct {
        uint32_t arm;
        uint16_t thumb;
        const char *what;
    } cases[] = {
        { 0xe12fff10u, 0x4700u, "BX" },
        { 0xe12fff30u, 0x4780u, "BLX" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0u, cases[i].arm);
        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
        c.r[0] = 0x102u;                         /* no valid ARM/Thumb state */
        c.r[14] = 0xa5a5a5a5u;
        c.r[15] = 0u;
        cpsr = c.cpsr;
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[14] == 0xa5a5a5a5u &&
              c.r[15] == 0u && c.cpsr == cpsr,
              "ARM %s accepted target 0x102 or mutated state", cases[i].what);

        memset(g_ram, 0, sizeof g_ram);
        m_w16(NULL, 0u, cases[i].thumb);
        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS | ARM_CPSR_T;
        c.r[0] = 0x102u;
        c.r[14] = 0xa5a5a5a5u;
        c.r[15] = 0u;
        cpsr = c.cpsr;
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[14] == 0xa5a5a5a5u &&
              c.r[15] == 0u && c.cpsr == cpsr,
              "Thumb %s accepted target 0x102 or mutated state", cases[i].what);
    }
}

static void test_thumb_bl_pair(void) {
    /* BL is a 32-bit pair in Thumb. The halves combine into one 22-bit offset:
     * target = (PC_of_first + 4) + (offset << 1). With offset 2 that is
     * 4 + 4 = 8. LR must be the address after the pair, with the Thumb bit set
     * so the eventual BX LR returns to Thumb state. */
    uint16_t p[] = { 0xf000, 0xf802 };
    arm_cpu_t c; load_thumb(&c, p, 2, 2);
    CHECK(c.r[15] == 0x08, "pc=%08x expect 08", c.r[15]);
    CHECK(c.r[14] == 0x05, "lr=%08x expect 05 (return addr | Thumb bit)", c.r[14]);
}

static void test_thumb_extend_and_reverse(void) {
    /* ARMv6 Thumb extend group. Real Apple LLB reaches UXTB within a few
     * thousand instructions, so these are required, not optional. */
    uint16_t p[] = { 0x21ff,   /* MOV  r1,#0xff        */
                     0xb2ca,   /* UXTB r2,r1  -> 0xff  */
                     0xb24b,   /* SXTB r3,r1  -> -1    */
                     0xb289 }; /* UXTH r1,r1  -> 0xff  */
    arm_cpu_t c; load_thumb(&c, p, 4, 4);
    CHECK(c.r[2] == 0xff, "r2=%08x expect ff (UXTB)", c.r[2]);
    CHECK(c.r[3] == 0xffffffffu, "r3=%08x expect ffffffff (SXTB)", c.r[3]);
    CHECK(c.r[1] == 0xff, "r1=%08x expect ff (UXTH)", c.r[1]);
}

static void test_thumb_rev(void) {
    /* Build 0x11223344 a byte at a time, then REV it. */
    uint16_t p[] = { 0x2011, 0x0200, 0x3022, 0x0200,
                     0x3033, 0x0200, 0x3044, 0xba01 };
    arm_cpu_t c; load_thumb(&c, p, 8, 8);
    CHECK(c.r[0] == 0x11223344u, "r0=%08x expect 11223344", c.r[0]);
    CHECK(c.r[1] == 0x44332211u, "r1=%08x expect 44332211 (REV)", c.r[1]);
}

static void test_thumb_cps(void) {
    /* CPSID i then CPSIE i must set and clear the CPSR I bit. */
    uint16_t p[] = { 0xb672, 0xb662 };
    arm_cpu_t c; load_thumb(&c, p, 2, 1);
    CHECK((c.cpsr & ARM_CPSR_I) != 0, "CPSID should mask IRQs");
    arm_step(&c);
    CHECK((c.cpsr & ARM_CPSR_I) == 0, "CPSIE should unmask IRQs");
}

static void test_mmu_supersection(void) {
    /* Regression from booting real XNU: a type-2 descriptor with bit 18 set is
     * a 16 MB SUPERsection, taking its base from bits[31:24] and the offset
     * from va[23:0] — a different split from the 1 MB section. Treating one as
     * a section silently resolves the wrong physical address, and the kernel
     * read garbage where a valid pointer lived. */
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    /* Bits[8:5] are not a domain in a supersection. Give them a value whose
     * corresponding DACR domain is disabled: a walker that decodes them as a
     * normal section domain will spuriously fault instead of using domain 0. */
    uint32_t super = 0x08000000u | (1u << 18) | (3u << 10) |
                     (5u << 5) | 2u;
    for (unsigned i = 0; i < 16; i++)
        m_w32(NULL, 0x4000 + ((((0xc0000000u >> 20) + i)) << 2), super);
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000; c.cp15.dacr = 1u; c.cp15.sctlr |= ARM_SCTLR_M;

    uint32_t pa = 0;
    CHECK(arm_mmu_translate(&c, 0xc020e220u, ARM_ACCESS_READ, true, &pa) == 0,
          "supersection translation faulted");
    CHECK(pa == 0x0820e220u,
          "pa=%08x expect 0820e220 (supersection uses va[23:0])", pa);

    /* A plain section must still use the 1 MB split. */
    uint32_t sect = 0x08000000u | (3u << 10) | 2u;
    m_w32(NULL, 0x4000 + ((0xc0000000u >> 20) << 2), sect);
    CHECK(arm_mmu_translate(&c, 0xc0001234u, ARM_ACCESS_READ, true, &pa) == 0,
          "section faulted");
    CHECK(pa == 0x08001234u, "pa=%08x expect 08001234 (plain section)", pa);
}

static void test_thumb_blx_suffix_is_not_a_branch(void) {
    /* Regression from booting real XNU: 0xE000-0xE7FF is an unconditional
     * branch but 0xE800-0xEFFF is the SECOND half of a BLX pair, which returns
     * to ARM state. Treating the whole 0xExxx range as a branch sent BLX to a
     * garbage address, and the kernel executed a pointer table as code. */
    uint16_t p[] = { 0xf000, 0xe802 };
    arm_cpu_t c; load_thumb(&c, p, 2, 2);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "BLX suffix must switch back to ARM state");
    CHECK((c.r[15] & 3u) == 0, "pc=%08x must be word-aligned after BLX", c.r[15]);
    CHECK(c.r[14] == 0x05, "lr=%08x expect 05 (return addr | Thumb bit)", c.r[14]);

    /* A plain unconditional Thumb branch must still branch. */
    uint16_t b[] = { 0xe000, 0x2007 };
    arm_cpu_t c2; load_thumb(&c2, b, 2, 2);
    CHECK((c2.cpsr & ARM_CPSR_T) != 0, "a plain branch must stay in Thumb");
}

static void test_user_bank_stm(void) {
    /* STM with the S bit transfers the USER bank whatever mode we are in —
     * how a kernel saves user context on exception entry. Real XNU uses
     * "STMIA sp,{r0-r14}^". */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    arm_set_mode(&c, ARM_MODE_USR);
    c.r[13] = 0xaaaa0000u; c.r[14] = 0xbbbb0000u;
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[13] = 0x800; c.r[14] = 0xcccc0000u;
    m_w32(NULL, 0, 0xe8cd6000u);          /* STMIA sp,{r13,r14}^  (S bit set) */
    c.r[15] = 0;
    arm_step(&c);
    CHECK(m_r32(NULL, 0x800) == 0xaaaa0000u,
          "stored r13=%08x expect the USER bank aaaa0000", m_r32(NULL, 0x800));
    CHECK(m_r32(NULL, 0x804) == 0xbbbb0000u,
          "stored r14=%08x expect the USER bank bbbb0000", m_r32(NULL, 0x804));
    CHECK(c.r[14] == 0xcccc0000u, "the SVC bank must be untouched");
}

/* ------------------------------------------------ TTBCR.N / TTBR1 split ---
 * ARM ARM (ARMv6) B4.9.4 "Selecting between TTBR0 and TTBR1":
 *
 *   N == 0  -> every VA walks TTBR0; TTBR1 is not used at all.
 *   N  > 0  -> if VA[31:32-N] are all zero the walk starts at TTBR0, whose
 *              table is 2^(14-N) bytes based at TTBR0[31:14-N] and indexed by
 *              VA[31-N:20] (12-N index bits); otherwise it starts at TTBR1,
 *              which is ALWAYS a full 16 KB table indexed by VA[31:20].
 *
 * The tests below plant a decoy descriptor wherever a plausible mis-implementation
 * (unmasked base, wrong index width, inverted or off-by-one selector, TTBR0
 * fall-back) would land, so a wrong walker resolves to a recognisable PA rather
 * than merely faulting.
 */

/* Point the CPU at a TTBR0/TTBR1 pair with a given TTBCR.N, MMU on, domain 0 a
 * client so the AP bits are really checked. Clears RAM — plant descriptors
 * after calling this, not before. */
static void mmu_setup_split(arm_cpu_t *c, unsigned n,
                            uint32_t ttbr0, uint32_t ttbr1) {
    memset(g_ram, 0, sizeof g_ram);
    arm_reset(c, &g_bus);
    c->cp15.ttbr0  = ttbr0;
    c->cp15.ttbr1  = ttbr1;
    c->cp15.ttbcr  = n;
    c->cp15.dacr   = 1u;                      /* domain 0: client */
    c->cp15.sctlr |= ARM_SCTLR_M;             /* MMU on */
}

/* Plant a 1 MB section (AP=11, domain 0) at an explicit first-level index. The
 * index is spelled out instead of derived from the VA because which table and
 * which index the walker picks is precisely what is under test. The base and
 * offset are OR-ed exactly as the walker combines them, so a decoy written at an
 * unmasked base lands where a mis-masked walker would read. */
static void mmu_put_section(uint32_t table, unsigned index, uint32_t pa) {
    m_w32(NULL, table | (index << 2), (pa & 0xfff00000u) | (3u << 10) | 2u);
}

/* Privileged read translation; returns the PA, or 0xffffffff if it faulted. */
static uint32_t mmu_xlate(arm_cpu_t *c, uint32_t va) {
    uint32_t pa = 0;
    return arm_mmu_translate(c, va, ARM_ACCESS_READ, true, &pa) ? 0xffffffffu : pa;
}

static void test_mmu_ttbcr_n0_ignores_ttbr1(void) {
    /* N == 0 is the reset state and must keep behaving exactly as the walker did
     * before the split existed: a single 4096-entry TTBR0 table covering the
     * whole 4 GB, based at TTBR0[31:14], with TTBR1 never consulted. */
    arm_cpu_t c; mmu_setup_split(&c, 0, 0x4200u, 0x8000u); /* base masks to 0x4000 */
    mmu_put_section(0x4000, 0x001, 0x00400000u);   /* VA 0x00100000 */
    mmu_put_section(0x4000, 0xc00, 0x00200000u);   /* VA 0xc0000000 */
    mmu_put_section(0x4000, 0xfff, 0x00500000u);   /* VA 0xfff00000, last entry */
    mmu_put_section(0x4200, 0x001, 0x00900000u);   /* decoy: unmasked TTBR0 base */
    mmu_put_section(0x8000, 0xc00, 0x00900000u);   /* decoy: TTBR1 must be ignored */
    mmu_put_section(0x8000, 0xfff, 0x00900000u);   /* decoy: TTBR1 must be ignored */

    uint32_t pa = mmu_xlate(&c, 0x00100abcu);
    CHECK(pa == 0x00400abcu, "pa=%08x expect 00400abc (low VA via TTBR0)", pa);
    pa = mmu_xlate(&c, 0xc0001234u);
    CHECK(pa == 0x00201234u, "pa=%08x expect 00201234 (N=0: high VA still TTBR0)", pa);
    pa = mmu_xlate(&c, 0xfff00010u);
    CHECK(pa == 0x00500010u, "pa=%08x expect 00500010 (N=0 table is 4096 entries)", pa);
}

static void test_mmu_ttbcr_n2_low_va_uses_shrunk_ttbr0(void) {
    /* N == 2 is what XNU-ARM runs with. TTBR0's table shrinks to 2^(14-2) = 4 KB
     * / 1024 entries, its base is TTBR0[31:12] (the low 12 bits of the register
     * are not part of the address), and it is indexed by VA[29:20] — ten bits. */
    arm_cpu_t c; mmu_setup_split(&c, 2, 0x5400u, 0x8000u); /* base masks to 0x5000 */
    mmu_put_section(0x5000, 0x001, 0x00700000u);   /* VA 0x00100000 */
    mmu_put_section(0x5000, 0x3ff, 0x00800000u);   /* VA 0x3ff00000, last entry */
    mmu_put_section(0x5400, 0x001, 0x00900000u);   /* decoy: unmasked TTBR0 base */

    uint32_t pa = mmu_xlate(&c, 0x00100abcu);
    CHECK(pa == 0x00700abcu,
          "pa=%08x expect 00700abc (N=2 TTBR0 base masks to a 4 KB boundary)", pa);
    pa = mmu_xlate(&c, 0x3ff00abcu);
    CHECK(pa == 0x00800abcu,
          "pa=%08x expect 00800abc (last entry of the 1024-entry TTBR0 table)", pa);
}

static void test_mmu_ttbcr_n2_kernel_va_uses_ttbr1(void) {
    /* With N == 2 everything at or above 0x40000000 walks TTBR1: all of kernel
     * text at 0xc0000000 and the 0xffff0000 exception-vector page. TTBR1's table
     * is a full 16 KB indexed by VA[31:20] regardless of N. */
    arm_cpu_t c; mmu_setup_split(&c, 2, 0x5000u, 0x8234u); /* base masks to 0x8000 */
    mmu_put_section(0x8000, 0xc00, 0x00200000u);   /* kernel text  0xc0000000 */
    mmu_put_section(0x8000, 0xfff, 0x00300000u);   /* vector page  0xffff0000 */
    mmu_put_section(0x8234, 0xc00, 0x00900000u);   /* decoy: unmasked TTBR1 base */
    /* 0xd0000000 has no TTBR1 entry. Give the TTBR0 table an entry at the index
     * a truncated walk would use (0xd00 & 0x3ff) so a fall-back is visible. */
    mmu_put_section(0x5000, 0x100, 0x00a00000u);

    uint32_t pa = mmu_xlate(&c, 0xc0000abcu);
    CHECK(pa == 0x00200abcu, "pa=%08x expect 00200abc (kernel text via TTBR1)", pa);
    /* 0xffff000c lies 0xf000c into the 1 MB section based at 0xfff00000. */
    pa = mmu_xlate(&c, 0xffff000cu);
    CHECK(pa == 0x003f000cu, "pa=%08x expect 003f000c (vector page via TTBR1)", pa);

    uint32_t p = 0;
    uint32_t f = arm_mmu_translate(&c, 0xd0000000u, ARM_ACCESS_READ, true, &p);
    CHECK((f & 0xf) == ARM_FSR_SECTION_TRANSLATION,
          "fsr=%x expect a translation fault: a TTBR1 miss must not fall back "
          "to TTBR0 (got pa=%08x)", f, p);
}

static void test_mmu_ttbcr_n2_split_boundary(void) {
    /* The selector with N == 2 is VA[31:30]: 0x3ff00000 is the last VA that
     * still walks TTBR0 and 0x40000000 is the first that crosses to TTBR1. */
    arm_cpu_t c; mmu_setup_split(&c, 2, 0x5000u, 0x8000u);
    mmu_put_section(0x5000, 0x3ff, 0x00100000u);   /* TTBR0 side of the line */
    mmu_put_section(0x8000, 0x400, 0x00200000u);   /* TTBR1 side of the line */
    /* 0x40000000's index truncated to ten bits is 0 — where the walk would land
     * if the index were shrunk but the table not switched. */
    mmu_put_section(0x5000, 0x000, 0x00900000u);
    /* and TTBR1 at the TTBR0-side index, in case the selector is inverted. */
    mmu_put_section(0x8000, 0x3ff, 0x00a00000u);

    uint32_t pa = mmu_xlate(&c, 0x3ff00abcu);
    CHECK(pa == 0x00100abcu,
          "pa=%08x expect 00100abc (0x3ff00000 is still TTBR0 with N=2)", pa);
    pa = mmu_xlate(&c, 0x40000abcu);
    CHECK(pa == 0x00200abcu,
          "pa=%08x expect 00200abc (0x40000000 is the first TTBR1 VA with N=2)", pa);
}

static void test_mmu_kernel_mapping_survives_ttbr0_pmap_switch(void) {
    /* THE regression the split exists to fix. XNU runs with N == 2, keeps the
     * kernel in TTBR1 and the *current user* pmap in TTBR0, and rewrites TTBR0
     * on every context switch (pmap_switch -> set_mmu_ttb). A walker that always
     * used TTBR0 lost kernel text and the 0xffff0000 vector page the instant
     * that happened, and the CPU stormed on prefetch aborts at 0xffff000c. */
    const uint32_t pmap_a = 0x5000u, pmap_b = 0x6000u, kernel = 0x8000u;
    arm_cpu_t c; mmu_setup_split(&c, 2, pmap_a, kernel);
    mmu_put_section(kernel, 0xc00, 0x00200000u);   /* kernel text  0xc0000000 */
    mmu_put_section(kernel, 0xfff, 0x00300000u);   /* vector page  0xffff0000 */
    mmu_put_section(pmap_a, 0x001, 0x00500000u);   /* user 0x00100000 under A */
    mmu_put_section(pmap_b, 0x001, 0x00600000u);   /* user 0x00100000 under B */

    uint32_t text_a = mmu_xlate(&c, 0xc0001000u);
    uint32_t vec_a  = mmu_xlate(&c, 0xffff000cu);
    uint32_t usr_a  = mmu_xlate(&c, 0x00100000u);
    CHECK(text_a == 0x00201000u, "pa=%08x expect 00201000 (kernel text, pmap A)", text_a);
    CHECK(vec_a  == 0x003f000cu, "pa=%08x expect 003f000c (vector page, pmap A)", vec_a);
    CHECK(usr_a  == 0x00500000u, "pa=%08x expect 00500000 (user VA via pmap A)", usr_a);

    c.cp15.ttbr0 = pmap_b;                          /* pmap_switch() to another task */

    uint32_t usr_b  = mmu_xlate(&c, 0x00100000u);
    CHECK(usr_b == 0x00600000u,
          "pa=%08x expect 00600000 — the TTBR0 switch must take effect for user VAs",
          usr_b);
    uint32_t text_b = mmu_xlate(&c, 0xc0001000u);
    uint32_t vec_b  = mmu_xlate(&c, 0xffff000cu);
    CHECK(text_b == text_a,
          "pa=%08x expect %08x — kernel text must survive a TTBR0 pmap switch",
          text_b, text_a);
    CHECK(vec_b == vec_a,
          "pa=%08x expect %08x — the vector page must survive a TTBR0 pmap switch",
          vec_b, vec_a);
}

static void test_mmu_ttbcr_n1_table_geometry(void) {
    /* N == 1: TTBR0's table is 2^(14-1) = 8 KB / 2048 entries based at
     * TTBR0[31:13] and indexed by VA[30:20]; the split falls at 0x80000000. */
    arm_cpu_t c; mmu_setup_split(&c, 1, 0x6800u, 0x8000u); /* base masks to 0x6000 */
    mmu_put_section(0x6000, 0x001, 0x00300000u);   /* VA 0x00100000 */
    mmu_put_section(0x6000, 0x7ff, 0x00100000u);   /* VA 0x7ff00000, last entry */
    mmu_put_section(0x8000, 0x800, 0x00200000u);   /* VA 0x80000000, first TTBR1 VA */
    mmu_put_section(0x6800, 0x001, 0x00900000u);   /* decoy: unmasked TTBR0 base */
    mmu_put_section(0x6000, 0x000, 0x00a00000u);   /* decoy: index shrunk, table not switched */

    uint32_t pa = mmu_xlate(&c, 0x00100abcu);
    CHECK(pa == 0x00300abcu,
          "pa=%08x expect 00300abc (N=1 TTBR0 base masks to an 8 KB boundary)", pa);
    pa = mmu_xlate(&c, 0x7ff00abcu);
    CHECK(pa == 0x00100abcu,
          "pa=%08x expect 00100abc (0x7ff00000 is the last TTBR0 VA with N=1)", pa);
    pa = mmu_xlate(&c, 0x80000abcu);
    CHECK(pa == 0x00200abcu,
          "pa=%08x expect 00200abc (0x80000000 is the first TTBR1 VA with N=1)", pa);
}

static void test_mmu_ttbcr_n3_table_geometry(void) {
    /* N == 3: TTBR0's table is 2^(14-3) = 2 KB / 512 entries based at
     * TTBR0[31:11] and indexed by VA[28:20]; the split falls at 0x20000000.
     * TTBR1's table stays a full 16 KB indexed by VA[31:20] whatever N is. */
    arm_cpu_t c; mmu_setup_split(&c, 3, 0x6400u, 0x8000u); /* base masks to 0x6000 */
    mmu_put_section(0x6000, 0x001, 0x00300000u);   /* VA 0x00100000 */
    mmu_put_section(0x6000, 0x1ff, 0x00100000u);   /* VA 0x1ff00000, last entry */
    mmu_put_section(0x8000, 0x200, 0x00200000u);   /* VA 0x20000000, first TTBR1 VA */
    mmu_put_section(0x8000, 0xfff, 0x00400000u);   /* VA 0xfff00000 via TTBR1 */
    mmu_put_section(0x6400, 0x001, 0x00900000u);   /* decoy: unmasked TTBR0 base */
    mmu_put_section(0x6000, 0x000, 0x00a00000u);   /* decoy: index shrunk, table not switched */

    uint32_t pa = mmu_xlate(&c, 0x00100abcu);
    CHECK(pa == 0x00300abcu,
          "pa=%08x expect 00300abc (N=3 TTBR0 base masks to a 2 KB boundary)", pa);
    pa = mmu_xlate(&c, 0x1ff00abcu);
    CHECK(pa == 0x00100abcu,
          "pa=%08x expect 00100abc (0x1ff00000 is the last TTBR0 VA with N=3)", pa);
    pa = mmu_xlate(&c, 0x20000abcu);
    CHECK(pa == 0x00200abcu,
          "pa=%08x expect 00200abc (0x20000000 is the first TTBR1 VA with N=3)", pa);
    pa = mmu_xlate(&c, 0xffff000cu);
    CHECK(pa == 0x004f000cu,
          "pa=%08x expect 004f000c (TTBR1 is a full 16 KB table for every N)", pa);
}

static void test_mmu_ttbcr_pd_bits_suppress_selected_walk(void) {
    uint32_t pa, fsr;
    arm_cpu_t c;

    /* PD0 disables a low-VA TTBR0 walk even when a valid descriptor exists. */
    mmu_setup_split(&c, 2u, 0x5000u, 0x8000u);
    mmu_put_section(0x5000u, 1u, 0x00300000u);
    c.cp15.ttbcr |= 1u << 4;
    g_watch_addr = 0x5004u;
    g_watch_reads32 = 0u;
    pa = 0xdeadbeefu;
    fsr = arm_mmu_translate(&c, 0x00100000u, ARM_ACCESS_READ, true, &pa);
    CHECK((fsr & 0xfu) == ARM_FSR_SECTION_TRANSLATION &&
          g_watch_reads32 == 0u && pa == 0xdeadbeefu,
          "PD0 fsr=%x reads=%u pa=%08x", fsr, g_watch_reads32, pa);

    /* PD0 applies only to TTBR0. The same setting must not block TTBR1. */
    mmu_put_section(0x8000u, 0xc00u, 0x00400000u);
    pa = mmu_xlate(&c, 0xc0000123u);
    CHECK(pa == 0x00400123u, "PD0 blocked TTBR1: pa=%08x", pa);

    /* Conversely PD1 suppresses the high-VA TTBR1 walk without touching its
     * first-level descriptor, but leaves the low-VA TTBR0 walk operational. */
    mmu_setup_split(&c, 2u, 0x5000u, 0x8000u);
    mmu_put_section(0x5000u, 1u, 0x00300000u);
    mmu_put_section(0x8000u, 0xc00u, 0x00400000u);
    c.cp15.ttbcr |= 1u << 5;
    g_watch_addr = 0xb000u;
    g_watch_reads32 = 0u;
    pa = 0xdeadbeefu;
    fsr = arm_mmu_translate(&c, 0xc0000123u, ARM_ACCESS_WRITE, true, &pa);
    CHECK((fsr & 0xfu) == ARM_FSR_SECTION_TRANSLATION &&
          (fsr & (1u << 11)) != 0u && g_watch_reads32 == 0u &&
          pa == 0xdeadbeefu,
          "PD1 fsr=%x reads=%u pa=%08x", fsr, g_watch_reads32, pa);
    pa = mmu_xlate(&c, 0x00100000u);
    CHECK(pa == 0x00300000u, "PD1 blocked TTBR0: pa=%08x", pa);

    g_watch_addr = 0xffffffffu;
}

/* ------------------------------------------------- DFSR / IFSR completeness -
 * The fault status registers are the whole conversation between this CPU and
 * XNU's abort handlers, and a single missing bit in them cost a full diagnosis
 * cycle (DFSR.WnR, commit 85c4653). These tests pin down every field the
 * kernel is known to read.
 *
 * What xnu-1357.5.30 actually reads, from the shipped kernel:
 *   _fleh_dataabt (0xc0068338)  SUB lr,lr,#8 ; MRC p15,0,r5,c5,c0,0 (DFSR)
 *                                            ; MRC p15,0,r6,c6,c0,0 (DFAR)
 *   _fleh_prefabt (0xc006828c)  SUB lr,lr,#4 ; MRC p15,0,r1,c6,c0,2 (IFAR)
 *                                            ; MRC p15,0,r5,c5,c0,1 (IFSR)
 *   _sleh_abort   (0xc006c538)  status = fsr & 0x40f, and for a data abort
 *                               TST fsr,#0x800 — DFSR.WnR — selecting
 *                               fault_type 3 (read|write) instead of 1 (read).
 */

/* Two 1 MB sections through one L1 table at 0x4000: an identity map for the
 * low megabyte (so the instruction fetch itself always succeeds) and a test
 * mapping at 0x80000000 with the caller's AP. 0x80100000 is deliberately left
 * without a descriptor. `mode` is the mode to execute in. */
static void fsr_setup(arm_cpu_t *c, const uint32_t *prog, size_t words,
                      unsigned ap, uint32_t mode) {
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i * 4), prog[i]);
    m_w32(NULL, 0x4000 + (0x000u << 2), 0x00000000u | (3u << 10) | 2u);
    m_w32(NULL, 0x4000 + (0x800u << 2), 0x00000000u | (ap  << 10) | 2u);
    arm_reset(c, &g_bus);
    c->cp15.ttbr0 = 0x4000; c->cp15.dacr = 1u; c->cp15.sctlr |= ARM_SCTLR_M;
    arm_set_mode(c, mode);
    /* Reset leaves CPSR.A set. Clear it so the assertions below prove that
     * *exception entry* sets it, rather than passing on a leftover. */
    c->cpsr &= ~ARM_CPSR_A;
    c->r[15] = 0;
}

static void test_dfsr_wnr_write_vs_read(void) {
    /* At the translator itself: the SAME fault, reached by a read and by a
     * write, must differ in exactly one bit. AP=00 is "no access", so both
     * directions fault with the same status and the only difference is WnR. */
    arm_cpu_t c; mmu_setup_section(&c, 0x80000000u, 0x00200000u, 0, 0);
    uint32_t pa = 0;
    uint32_t rd = arm_mmu_translate(&c, 0x80000abcu, ARM_ACCESS_READ, true, &pa);
    uint32_t wr = arm_mmu_translate(&c, 0x80000abcu, ARM_ACCESS_WRITE, true, &pa);
    CHECK((rd & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "fsr=%08x expect a section permission fault on read", rd);
    CHECK((wr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "fsr=%08x expect a section permission fault on write", wr);
    CHECK((rd & (1u << 11)) == 0, "fsr=%08x: WnR must be CLEAR for a read", rd);
    CHECK((wr & (1u << 11)) != 0, "fsr=%08x: WnR must be SET for a write", wr);
    CHECK((rd ^ wr) == (1u << 11),
          "read fsr %08x and write fsr %08x must differ in bit 11 alone", rd, wr);

    /* WnR is orthogonal to the status code: a translation fault carries it too,
     * and so does an unprivileged access. */
    uint32_t tw = arm_mmu_translate(&c, 0x90000000u, ARM_ACCESS_WRITE, false, &pa);
    CHECK((tw & 0xfu) == ARM_FSR_SECTION_TRANSLATION,
          "fsr=%08x expect a translation fault", tw);
    CHECK((tw & (1u << 11)) != 0,
          "fsr=%08x: WnR must be SET on a write translation fault too", tw);

    /* And no field above bit 11 is ever invented: ExT (bit 12) and the
     * extended status bit (bit 10) have no source in this machine. */
    CHECK((wr & ~0x8ffu) == 0,
          "fsr=%08x has bits set outside status/domain/WnR", wr);
}

static void test_data_abort_write_sets_dfsr_wnr(void) {
    /* End to end, the way XNU sees it: an unprivileged store to a
     * privileged-only page. This is the exact shape of the copyout that
     * livelocked ~2.8 million times with WnR missing. */
    uint32_t p[] = { 0xe3a01102 /*MOV r1,#0x80000000*/,
                     0xe5812000 /*STR r2,[r1]        */ };
    arm_cpu_t c; fsr_setup(&c, p, 2, 1 /*AP=01: privileged RW only*/, ARM_MODE_USR);
    arm_step(&c);
    uint32_t before = c.cpsr;
    arm_step(&c);

    CHECK(c.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", c.r[15]);
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "dfsr=%08x expect a section permission fault", c.cp15.dfsr);
    CHECK((c.cp15.dfsr & (1u << 11)) != 0,
          "dfsr=%08x: bit 11 (WnR) must be SET for a store", c.cp15.dfsr);
    CHECK(c.cp15.dfar == 0x80000000u, "dfar=%08x expect 80000000", c.cp15.dfar);

    /* Abort-mode entry state, checked against what fleh_dataabt assumes:
     * it does SUB lr,lr,#8 to recover the faulting instruction's address, and
     * then LDRs the instruction word from it to classify the access. */
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT,
          "mode=%02x expect ABT", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.r[14] == 4u + 8u, "lr=%08x expect 0c (faulting insn 0x04 + 8)", c.r[14]);
    CHECK(c.spsr[ARM_BANK_ABT] == before,
          "spsr=%08x expect the pre-fault cpsr %08x", c.spsr[ARM_BANK_ABT], before);
    CHECK((c.cpsr & ARM_CPSR_I) != 0, "IRQs must be masked on abort entry");
    CHECK((c.cpsr & ARM_CPSR_A) != 0,
          "cpsr=%08x: ARMv6 sets CPSR.A on data-abort entry", c.cpsr);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "exceptions must enter in ARM state");

    /* The same page, same instruction shape, read instead of write. */
    uint32_t q[] = { 0xe3a01102 /*MOV r1,#0x80000000*/,
                     0xe5912000 /*LDR r2,[r1]        */ };
    arm_cpu_t d; fsr_setup(&d, q, 2, 1, ARM_MODE_USR);
    arm_step(&d); arm_step(&d);
    CHECK(d.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", d.r[15]);
    CHECK((d.cp15.dfsr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "dfsr=%08x expect a section permission fault", d.cp15.dfsr);
    CHECK((d.cp15.dfsr & (1u << 11)) == 0,
          "dfsr=%08x: bit 11 (WnR) must be CLEAR for a load", d.cp15.dfsr);
}

static void test_prefetch_abort_never_sets_wnr(void) {
    /* IFSR has no WnR field. The fetch path must translate as a read, and it
     * must leave DFSR/DFAR — which belong to the data side — untouched, because
     * a prefetch abort taken while a data fault is still recorded would
     * otherwise rewrite the kernel's view of the earlier fault. */
    uint32_t p[] = { 0xe3a00102 /*MOV r0,#0x80000000*/,
                     0xe1a0f000 /*MOV pc,r0         */ };
    arm_cpu_t c; fsr_setup(&c, p, 2, 3, ARM_MODE_SYS);
    c.cp15.dfsr = 0xdeadbeefu; c.cp15.dfar = 0xcafebabeu;   /* sentinels */
    arm_step(&c);
    /* 0x80100000 is not an encodable ARM immediate, so aim the branch there by
     * hand: it is the megabyte just past the mapping, with no descriptor. */
    c.r[0] = 0x80100000u;
    arm_step(&c);                            /* MOV pc,r0 */
    CHECK(c.r[15] == 0x80100000u, "pc=%08x expect 80100000 before the fetch", c.r[15]);
    uint32_t before = c.cpsr;
    arm_step(&c);                            /* the fetch aborts */

    CHECK(c.r[15] == ARM_VEC_PREFETCH, "pc=%08x expect 0c (prefetch abort)", c.r[15]);
    CHECK((c.cp15.ifsr & 0xfu) == ARM_FSR_SECTION_TRANSLATION,
          "ifsr=%08x expect a section translation fault", c.cp15.ifsr);
    CHECK((c.cp15.ifsr & (1u << 11)) == 0,
          "ifsr=%08x: the instruction-fetch path must never set bit 11 — "
          "IFSR has no WnR field", c.cp15.ifsr);
    CHECK(c.cp15.ifar == 0x80100000u, "ifar=%08x expect 80100000", c.cp15.ifar);
    CHECK(c.cp15.dfsr == 0xdeadbeefu,
          "dfsr=%08x: a prefetch abort must not touch the data-side FSR", c.cp15.dfsr);
    CHECK(c.cp15.dfar == 0xcafebabeu,
          "dfar=%08x: a prefetch abort must not touch the data-side FAR", c.cp15.dfar);
    /* fleh_prefabt does SUB lr,lr,#4 to recover the faulting address. */
    CHECK(c.r[14] == 0x80100000u + 4u, "lr=%08x expect 80100004", c.r[14]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT,
          "mode=%02x expect ABT", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.spsr[ARM_BANK_ABT] == before,
          "spsr=%08x expect the pre-fault cpsr %08x", c.spsr[ARM_BANK_ABT], before);
    CHECK((c.cpsr & ARM_CPSR_A) != 0,
          "cpsr=%08x: ARMv6 sets CPSR.A on prefetch-abort entry", c.cpsr);
}

static void test_dfar_is_the_faulting_word_of_a_block_transfer(void) {
    /* An LDM that runs off the end of a mapped page must report the address of
     * the word that actually faulted, not the base of the transfer. sleh_abort
     * page-aligns DFAR (fp & 0xfffff000) and hands it to arm_fast_fault, so
     * reporting the base would map in the page the transfer already had and
     * re-execute the same instruction forever. */
    uint32_t p[] = { 0xe59f1008 /*LDR  r1,[pc,#8] -> the literal at 0x10 */,
                     0xe8910007 /*LDMIA r1,{r0,r1,r2}                    */,
                     0xeafffffe /*B .                                    */,
                     0x00000000 /*(pad)                                  */,
                     0x800ffff8 /*literal: 8 bytes before the page end   */ };
    arm_cpu_t c; fsr_setup(&c, p, 5, 3, ARM_MODE_SYS);
    c.r[0] = 0x11111111u; c.r[2] = 0x33333333u;
    arm_step(&c);
    CHECK(c.r[1] == 0x800ffff8u, "r1=%08x expect 800ffff8", c.r[1]);
    arm_step(&c);

    CHECK(c.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", c.r[15]);
    CHECK(c.cp15.dfar == 0x80100000u,
          "dfar=%08x expect 80100000 — the third word of the LDM, not the base "
          "0x800ffff8", c.cp15.dfar);
    CHECK((c.cp15.dfsr & (1u << 11)) == 0,
          "dfsr=%08x: an LDM is a read, WnR must be clear", c.cp15.dfsr);
    /* Base Restored Abort Model: nothing in the register file moved. */
    CHECK(c.r[0] == 0x11111111u, "r0=%08x expect 11111111 (base-restored)", c.r[0]);
    CHECK(c.r[1] == 0x800ffff8u, "r1=%08x expect 800ffff8 (base-restored)", c.r[1]);
    CHECK(c.r[2] == 0x33333333u, "r2=%08x expect 33333333 (base-restored)", c.r[2]);

    /* The mirror-image STM must set WnR and report the same address. */
    uint32_t q[] = { 0xe59f1008, 0xe8810007 /*STMIA r1,{r0,r1,r2}*/,
                     0xeafffffe, 0x00000000, 0x800ffff8 };
    arm_cpu_t d; fsr_setup(&d, q, 5, 3, ARM_MODE_SYS);
    arm_step(&d); arm_step(&d);
    CHECK(d.cp15.dfar == 0x80100000u,
          "dfar=%08x expect 80100000 for the storing form too", d.cp15.dfar);
    CHECK((d.cp15.dfsr & (1u << 11)) != 0,
          "dfsr=%08x: an STM is a write, WnR must be set", d.cp15.dfsr);
}

static void test_cps_is_a_nop_in_user_mode(void) {
    /* "CPS is a no-op if executed in User mode" (ARM ARM A4.1.16). Executing it
     * anyway lets a user program run "CPS #0x13" and continue in SVC with its
     * own registers. Nothing in a kernel-only boot can expose this, because XNU
     * only ever reaches CPS from a privileged mode. */
    uint32_t p[] = { 0xf1020013 /*CPS #0x13 (to SVC)*/,
                     0xf1080080 /*CPSIE i           */,
                     0xe3a00007 /*MOV r0,#7         */ };
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    for (unsigned i = 0; i < 3; i++) m_w32(NULL, i * 4, p[i]);
    arm_set_mode(&c, ARM_MODE_USR);
    c.cpsr |= ARM_CPSR_I;
    c.r[15] = 0;
    for (int i = 0; i < 3; i++) arm_step(&c);

    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR,
          "mode=%02x expect USR — CPS must not change mode from User mode",
          c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK((c.cpsr & ARM_CPSR_I) != 0,
          "cpsr=%08x: CPSIE from User mode must not unmask interrupts", c.cpsr);
    CHECK(c.r[0] == 7, "r0=%u expect 7 — CPS is a no-op, not a trap", c.r[0]);

    /* From a privileged mode it must still work, or the kernel cannot mask
     * interrupts at all. */
    arm_cpu_t d; arm_reset(&d, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xf1020013u);
    arm_set_mode(&d, ARM_MODE_SYS);
    d.r[15] = 0;
    arm_step(&d);
    CHECK((d.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "mode=%02x expect SVC — privileged CPS must still change mode",
          d.cpsr & ARM_CPSR_MODE_MASK);
}

/* ------------------------------------------------- lazy VFP / undefined ---
 *
 * XNU clears FPEXC.EN and enables VFP one thread at a time: the first VFP
 * instruction a thread runs is SUPPOSED to be undefined, and _sleh_undef turns
 * VFP on and re-runs it. So a VFP encoding with VFP off must vector the guest
 * — but the same encoding with VFP already ON must still stop the machine,
 * because at that point the kernel's own handler would call it an illegal
 * instruction and we would have converted "not implemented" into a SIGILL.
 */

/* Run one instruction with the CPU privileged, VFP granted by CPACR, and
 * FPEXC.EN as asked. Returns the step status. */
static arm_status_t run_vfp(arm_cpu_t *c, uint32_t insn, bool en) {
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, insn);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS;
    c->cp15.cpacr  = 0xfu << ARM_CPACR_CP10_SHIFT;   /* full access to both */
    c->vfp_fpexc   = en ? ARM_FPEXC_EN : 0u;
    c->r[15] = 0;
    return arm_step(c);
}

static void test_vfp_disabled_vectors_the_guest(void) {
    /* Every encoding _sleh_undef (0xc006c368) matches, one per mask. */
    struct { uint32_t insn; const char *what; } v[] = {
        { 0xf2000d40u, "VADD.F32 (Advanced SIMD data processing)" },
        { 0xee300a00u, "VADD.F32 s0,s0,s0 (VFP data processing)"  },
        { 0xed901a00u, "VLDR s2,[r0] (VFP load/store)"            },
        { 0xecb10a20u, "VLDMIA r1!,{s0-s31} (VFP load/store)"     },
        { 0xf4200a0fu, "VLD1.8 (SIMD element/structure ld/st)"    },
        { 0xee100a10u, "VMOV r0,s0 (VFP 32-bit transfer)"         },
        { 0xee274b10u, "FMDHR d7,r4 (VFPv2 D-word transfer)"       },
        { 0xec510b10u, "VMOV r0,r1,d0 (VFP 64-bit transfer)"      },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        arm_cpu_t c;
        arm_status_t st = run_vfp(&c, v[i].insn, false);
        CHECK(st == ARM_OK && c.r[15] == ARM_VEC_UNDEFINED,
              "%s with FPEXC.EN=0: status=%d pc=%08x, expect a guest vector to 0x04",
              v[i].what, (int)st, c.r[15]);
    }
}

static void test_vfp_enabled_still_halts(void) {
    /* The other half of the rule, and the half that keeps us honest: once the
     * kernel has enabled VFP, an encoding we cannot execute has to stop the
     * machine and name itself. _sleh_undef would deliver EXC_BAD_INSTRUCTION
     * here, so vectoring would destroy the diagnostic AND kill the process.
     *
     * VFPv2 itself is now implemented (core/src/arm/vfp.c), so the examples
     * here are the encodings that remain genuinely absent from this part: NEON
     * — which the ARM1176 does not have at all — and VFPv3 additions. The VFP
     * unit's own coverage is tested in core/tests/test_vfp.c. */
    struct { uint32_t insn; const char *what; } v[] = {
        { 0xf2000d40u, "VADD.F32 (Advanced SIMD; no NEON on the ARM1176)" },
        { 0xf4200a0fu, "VLD1.8 (Advanced SIMD element/structure load)"    },
        { 0xeeb00b00u, "VMOV.F64 d0,#imm (VFPv3)"                        },
        { 0xee400b10u, "VMOV.8 d0[0],r0 (Advanced SIMD scalar transfer)"   },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        arm_cpu_t c;
        arm_status_t st = run_vfp(&c, v[i].insn, true);
        CHECK(st == ARM_UNDEFINED,
              "%s with FPEXC.EN=1: status=%d, expect ARM_UNDEFINED — we would "
              "have to compute a real floating-point result", v[i].what, (int)st);
    }
}

static void test_non_vfp_undefined_still_halts(void) {
    /* Encodings outside the six masks must be untouched by all of this, or the
     * lazy-VFP path becomes a silent swallow-everything. 0xE7FFDEFE is the
     * ARM breakpoint: _sleh_undef recognises it too, but it means the guest
     * trapped deliberately and we want to see where. */
    struct { uint32_t insn; const char *what; } v[] = {
        { 0xe7ffdefeu, "BKPT (0xE7FFDEFE)"           },
        { 0xf1010200u, "SETEND BE"                   },
        { 0xee000d10u, "MCR p13 (a coprocessor we do not model)" },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        arm_cpu_t c;
        arm_status_t st = run_vfp(&c, v[i].insn, false);
        CHECK(st == ARM_UNDEFINED,
              "%s: status=%d, expect ARM_UNDEFINED even with VFP disabled",
              v[i].what, (int)st);
    }
}

static void test_vfp_undef_entry_state(void) {
    /* The exception the guest actually receives. _fleh_undef does
     * "MRS sp,spsr; TST sp,#0x20; SUBEQ lr,lr,#4" — so LR must be the faulting
     * PC + 4 in ARM state, or the kernel resumes at the wrong instruction. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x100u, 0xee300a00u);            /* VADD.F32 s0,s0,s0 */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_USR);
    c.cpsr &= ~(ARM_CPSR_I | ARM_CPSR_A);
    c.cp15.cpacr = 0xfu << ARM_CPACR_CP10_SHIFT;
    c.vfp_fpexc  = 0;
    c.r[15] = 0x100u;
    uint32_t before = c.cpsr;
    arm_status_t st = arm_step(&c);

    CHECK(st == ARM_OK, "status=%d expect ARM_OK (the guest handles this)", (int)st);
    CHECK(c.r[15] == ARM_VEC_UNDEFINED, "pc=%08x expect the 0x04 vector", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND,
          "mode=%02x expect Undefined (0x1b)", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.r[14] == 0x104u,
          "lr=%08x expect 0x104 — _fleh_undef subtracts 4 to find the faulting PC",
          c.r[14]);
    CHECK(c.spsr[ARM_BANK_UND] == before,
          "spsr=%08x expect the CPSR at fault time %08x", c.spsr[ARM_BANK_UND], before);
    CHECK((c.cpsr & ARM_CPSR_I) != 0, "cpsr=%08x: entry must mask IRQ", c.cpsr);
    CHECK((c.cpsr & ARM_CPSR_A) == 0,
          "cpsr=%08x: Undefined must NOT set CPSR.A (only aborts and interrupts do)",
          c.cpsr);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "cpsr=%08x: entry is always ARM state", c.cpsr);

    /* SCTLR.V moves the whole table, this vector included. */
    arm_cpu_t d; arm_reset(&d, &g_bus);
    d.cpsr = (d.cpsr & ~0x1fu) | ARM_MODE_SYS;
    d.cp15.sctlr |= ARM_SCTLR_V;
    d.cp15.cpacr  = 0xfu << ARM_CPACR_CP10_SHIFT;
    d.vfp_fpexc   = 0;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xee300a00u);
    d.r[15] = 0;
    arm_step(&d);
    CHECK(d.r[15] == 0xffff0004u,
          "pc=%08x expect 0xffff0004 with high vectors", d.r[15]);
}

static void test_vfp_sysreg_access_follows_fpexc(void) {
    /* FPEXC and FPSID stay readable with VFP off — that is what makes lazy
     * enabling possible, and _get_vfp_enabled (0xc006994c) does exactly this
     * VMRS with EN clear. FPSCR does not: accessing it with EN==0 is
     * UNDEFINED, and _vfp_switch is careful to set FPEXC.EN first. */
    arm_cpu_t c;
    arm_status_t st = run_vfp(&c, 0xeef80a10u, false);   /* VMRS r0, FPEXC */
    CHECK(st == ARM_OK && c.r[0] == 0,
          "VMRS FPEXC with EN=0: status=%d r0=%08x, must read back, not trap",
          (int)st, c.r[0]);

    st = run_vfp(&c, 0xeef00a10u, false);                /* VMRS r0, FPSID */
    CHECK(st == ARM_OK && c.r[0] == ARM1176_FPSID,
          "VMRS FPSID with EN=0: status=%d r0=%08x expect %08x",
          (int)st, c.r[0], ARM1176_FPSID);

    st = run_vfp(&c, 0xeef10a10u, false);                /* VMRS r0, FPSCR */
    CHECK(st == ARM_OK && c.r[15] == ARM_VEC_UNDEFINED,
          "VMRS FPSCR with EN=0: status=%d pc=%08x, must take the guest's "
          "Undefined exception", (int)st, c.r[15]);

    st = run_vfp(&c, 0xeef10a10u, true);                 /* VMRS r0, FPSCR */
    CHECK(st == ARM_OK && c.r[15] == 4u,
          "VMRS FPSCR with EN=1: status=%d pc=%08x, must simply execute",
          (int)st, c.r[15]);

    /* Setting FPEXC.EN is the whole point of the handler; it must stick. */
    st = run_vfp(&c, 0xeee80a10u, false);                /* VMSR FPEXC, r0 */
    CHECK(st == ARM_OK, "VMSR FPEXC with EN=0: status=%d, must be permitted", (int)st);
}

static void test_vfp_cpacr_denial_vectors(void) {
    /* CPACR gates CP10/CP11 ahead of FPEXC. XNU's _init_vfp writes 0xf<<20 to
     * grant both; before that runs, or if a field is set to "privileged only",
     * the access is UNDEFINED and the guest must see it. */
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xee300a00u);                  /* VADD.F32 */
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
    c.cp15.cpacr = 0;                             /* access denied */
    c.vfp_fpexc  = ARM_FPEXC_EN;                  /* enabled, but unreachable */
    c.r[15] = 0;
    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[15] == ARM_VEC_UNDEFINED,
          "CPACR=0 with FPEXC.EN=1: status=%d pc=%08x, CPACR must win",
          (int)st, c.r[15]);

    /* "Privileged only" (0b01 in both fields) denies User mode and permits SYS. */
    arm_cpu_t d; arm_reset(&d, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xeef10a10u);                  /* VMRS r0, FPSCR */
    arm_set_mode(&d, ARM_MODE_USR);
    d.cp15.cpacr = (1u << ARM_CPACR_CP10_SHIFT) | (1u << ARM_CPACR_CP11_SHIFT);
    d.vfp_fpexc  = ARM_FPEXC_EN;
    d.r[15] = 0;
    st = arm_step(&d);
    CHECK(st == ARM_OK && d.r[15] == ARM_VEC_UNDEFINED,
          "CPACR=privileged-only from User mode: status=%d pc=%08x, expect the "
          "guest vector", (int)st, d.r[15]);

    arm_cpu_t e; arm_reset(&e, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xeef10a10u);
    e.cpsr = (e.cpsr & ~0x1fu) | ARM_MODE_SYS;
    e.cp15.cpacr = (1u << ARM_CPACR_CP10_SHIFT) | (1u << ARM_CPACR_CP11_SHIFT);
    e.vfp_fpexc  = ARM_FPEXC_EN;
    e.r[15] = 0;
    st = arm_step(&e);
    CHECK(st == ARM_OK && e.r[15] == 4u,
          "CPACR=privileged-only from SYS: status=%d pc=%08x, expect it to run",
          (int)st, e.r[15]);
}

static void test_vfp_lazy_trap_cannot_loop(void) {
    /* The property that makes vectoring safe: the trap is one-shot. Enable VFP
     * the way _vfp_switch does and the SAME instruction now halts loudly
     * instead of vectoring again, so a kernel that enables VFP and retries
     * gets a diagnostic rather than an infinite exception loop. */
    /* 0xeeb00b00 is VMOV.F64 d0,#imm — in the VFP encoding space (so the
     * disabled case vectors) but a VFPv3 addition the VFP11 does not have (so
     * the enabled case halts). It has to be an encoding we do not implement:
     * one we DO implement simply executes on the retry, which is the whole
     * point of the milestone and is covered by test_vfp.c. */
    arm_cpu_t c;
    arm_status_t st = run_vfp(&c, 0xeeb00b00u, false);
    CHECK(st == ARM_OK && c.r[15] == ARM_VEC_UNDEFINED, "first pass must vector");

    c.vfp_fpexc |= ARM_FPEXC_EN;                  /* what _vfp_switch does */
    arm_set_mode(&c, ARM_MODE_SYS);
    c.r[15] = 0;
    st = arm_step(&c);
    CHECK(st == ARM_UNDEFINED,
          "status=%d after the guest enabled VFP: the retry must halt, not "
          "vector a second time", (int)st);
}

/* ================= the user <-> kernel transition =========================
 *
 * Everything below is checked against xnu-1357.5.30's own code, not only
 * against the ARM ARM, because the kernel's assembly is the other half of the
 * contract. The encodings used here are the literal words in firmware/
 * kernel.macho:
 *
 *   _fleh_swi              0xc00680a0  STM sp,{r0-r14}^   0xe8cd7fff
 *                          0xc00680b8  SRSIA sp,#0x13     0xf8cd0513
 *   _thread_exception_return
 *                          0xc0068774  LDM sp,{r0-r14}^   0xe8dd7fff
 *                          0xc006877c  MOVS pc,lr         0xe1b0f00e
 *   _fleh_fiq_generic      0xc00685e4  SUBSPL pc,lr,#4    0x525ef004
 *   _copyin                0xc0069cb0  LDRT r3,[r0],#4    0xe4b03004
 *   _copyout               0xc0069d38  STRT r3,[r1],#4    0xe4a13004
 *   _thread_set_cthread_self
 *                          0xc0061da0  MCR p15,0,r0,c13,c0,3
 *
 * Note what _fleh_swi does NOT do: it never looks at the SWI immediate. The
 * syscall number arrives in r12 ("CMN ip,#3" is its first instruction) and
 * _unix_syscall re-reads it from the saved frame at +0x30, which is r12's slot
 * in the STM^ above. The arguments come from the saved r0-r6 (it computes
 * "&regs->r[0]" or "&regs->r[1]" and rejects anything with more than 7 args) —
 * never from the user stack. So the whole syscall ABI rests on the `^` block
 * transfers below reading and writing the USER bank.
 */

/* Put the core in User mode at `pc` with the MMU off and a program loaded at 0. */
static void user_mode_at(arm_cpu_t *c, const uint32_t *prog, size_t words,
                         uint32_t pc) {
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i * 4), prog[i]);
    arm_reset(c, &g_bus);
    arm_set_mode(c, ARM_MODE_USR);
    c->cpsr &= ~(ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A);
    c->r[15] = pc;
}

static arm_bus_t bus_with_svc_probe(svc_probe_t *probe) {
    arm_bus_t bus = g_bus;
    arm_bus_set_privileged_svc_handler(&bus, probe_privileged_svc, probe);
    return bus;
}

static void test_privileged_svc_hook_handles_a32(void) {
    const uint32_t svc = 0xefabc123u;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x100u, svc);

    svc_probe_t probe = {0};
    probe.result = ARM_SVC_HANDLED;
    probe.mutate = true;
    arm_bus_t bus = bus_with_svc_probe(&probe);
    arm_cpu_t c;
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_SYS);
    c.cpsr = ARM_MODE_SYS | ARM_CPSR_Z | ARM_CPSR_C;
    c.r[0] = 0x12345678u;
    c.r[7] = 0x77777777u;
    c.r[15] = 0x100u;
    c.cp15.context_id = 0x13572468u;
    c.excl_valid = true;
    c.excl_addr = 0x400u;
    c.spsr[ARM_BANK_SVC] = 0xa5a5a5a5u;
    c.bank_r14[ARM_BANK_SVC] = 0x5a5a5a5au;
    probe.expected_cpu = &c;

    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK && probe.calls == 1u,
          "status=%d calls=%u expect handled OK/1", (int)st, probe.calls);
    CHECK(probe.saw_expected_cpu, "hook did not receive the live CPU object");
    CHECK(probe.seen_pc == 0x100u && probe.seen_cpu_pc == 0x100u,
          "pc arg=%08x cpu.pc=%08x expect exact current PC 00000100",
          probe.seen_pc, probe.seen_cpu_pc);
    CHECK(probe.seen_encoding == svc,
          "encoding=%08x expect raw A32 word %08x", probe.seen_encoding, svc);
    CHECK(probe.seen_cpsr == (ARM_MODE_SYS | ARM_CPSR_Z | ARM_CPSR_C),
          "cpsr seen=%08x expect pre-exception SYS state", probe.seen_cpsr);
    CHECK(probe.seen_r0 == 0x12345678u,
          "r0 seen=%08x expect complete pre-hook register state", probe.seen_r0);
    CHECK(c.r[15] == 0x104u && c.cycles == 1u,
          "pc=%08x cycles=%llu expect sequentially retired A32 SVC",
          c.r[15], (unsigned long long)c.cycles);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS &&
          (c.cpsr & ARM_CPSR_N) != 0u,
          "handled SVC must stay in SYS and commit hook CPSR changes (%08x)",
          c.cpsr);
    CHECK(c.r[0] == 0xfeed0001u && c.r[7] == 0xfeed0007u &&
          c.cp15.context_id == 0xfeedc013u,
          "handled hook register writes were not committed");
    CHECK(c.spsr[ARM_BANK_SVC] == 0xa5a5a5a5u &&
          c.bank_r14[ARM_BANK_SVC] == 0x5a5a5a5au,
          "handled host service must not enter or alter the SVC exception bank");
}

static void test_privileged_svc_hook_redirects_a32_to_thumb(void) {
    const uint32_t svc = 0xefabc123u;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x100u, svc);
    m_w16(NULL, 0x202u, 0x225au);             /* MOVS r2,#0x5a (Thumb) */

    svc_probe_t probe = {0};
    probe.result = ARM_SVC_REDIRECTED;
    probe.mutate = true;
    probe.redirect = true;
    probe.redirect_thumb = true;
    probe.redirect_pc = 0x202u;
    arm_bus_t bus = bus_with_svc_probe(&probe);
    arm_cpu_t c;
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_SYS);
    c.cpsr = ARM_MODE_SYS | ARM_CPSR_Z | ARM_CPSR_C;
    c.r[0] = 0x12345678u;
    c.r[7] = 0x77777777u;
    c.r[15] = 0x100u;
    c.cp15.context_id = 0x13572468u;
    c.excl_valid = true;
    c.excl_addr = 0x400u;
    c.spsr[ARM_BANK_SVC] = 0xa5a5a5a5u;
    c.bank_r14[ARM_BANK_SVC] = 0x5a5a5a5au;
    c.cycles = 40u;
    probe.expected_cpu = &c;

    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK && probe.calls == 1u && probe.saw_expected_cpu,
          "A32 redirect status=%d calls=%u", (int)st, probe.calls);
    CHECK(probe.seen_pc == 0x100u && probe.seen_cpu_pc == 0x100u &&
          probe.seen_encoding == svc,
          "A32 redirect hook saw pc=%08x cpu.pc=%08x encoding=%08x",
          probe.seen_pc, probe.seen_cpu_pc, probe.seen_encoding);
    CHECK(c.r[15] == 0x202u && (c.cpsr & ARM_CPSR_T) != 0u &&
          c.cycles == 41u,
          "A32 redirect pc=%08x cpsr=%08x cycles=%llu expect Thumb 202/41",
          c.r[15], c.cpsr, (unsigned long long)c.cycles);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS &&
          (c.cpsr & ARM_CPSR_N) != 0u &&
          c.r[0] == 0xfeed0001u && c.r[7] == 0xfeed0007u &&
          c.cp15.context_id == 0xfeedc013u &&
          !c.excl_valid && c.excl_addr == 0xfeedeeeeu,
          "A32 redirect did not commit the callback's complete CPU mutation");
    CHECK(c.spsr[ARM_BANK_SVC] == 0xa5a5a5a5u &&
          c.bank_r14[ARM_BANK_SVC] == 0x5a5a5a5au,
          "A32 redirect incorrectly entered or altered the SVC bank");

    st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[2] == 0x5au && c.r[15] == 0x204u &&
          (c.cpsr & ARM_CPSR_T) != 0u && c.cycles == 42u &&
          probe.calls == 1u,
          "redirect target did not fetch as Thumb: status=%d r2=%08x pc=%08x "
          "cpsr=%08x cycles=%llu calls=%u",
          (int)st, c.r[2], c.r[15], c.cpsr,
          (unsigned long long)c.cycles, probe.calls);
}

static void test_privileged_svc_hook_nonhandled_is_transactional(void) {
    static const arm_svc_result_t results[] = {
        ARM_SVC_UNHANDLED,
        (arm_svc_result_t)7
    };
    const uint32_t svc = 0xef765432u;

    for (size_t i = 0; i < sizeof results / sizeof results[0]; i++) {
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, svc);
        svc_probe_t probe = {0};
        probe.result = results[i];
        probe.mutate = true;
        arm_bus_t bus = bus_with_svc_probe(&probe);
        arm_cpu_t c;
        arm_reset(&c, &bus);
        arm_set_mode(&c, ARM_MODE_IRQ);
        const uint32_t old_cpsr = ARM_MODE_IRQ | ARM_CPSR_C;
        c.cpsr = old_cpsr;
        c.r[0] = 0x01020304u;
        c.r[7] = 0x07070707u;
        c.cp15.context_id = 0x89abcdefu;
        c.excl_valid = true;
        c.excl_addr = 0x400u;
        probe.expected_cpu = &c;

        arm_status_t st = arm_step(&c);
        CHECK(st == ARM_OK && probe.calls == 1u,
              "case %u status=%d calls=%u expect callback then guest SVC",
              (unsigned)i, (int)st, probe.calls);
        CHECK(probe.saw_expected_cpu && probe.seen_pc == 0u &&
              probe.seen_cpu_pc == 0u && probe.seen_encoding == svc,
              "case %u callback did not receive exact CPU/PC/encoding",
              (unsigned)i);
        CHECK(c.r[15] == ARM_VEC_SWI &&
              (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC && c.r[14] == 4u &&
              c.cycles == 1u,
              "case %u did not fail closed through the ordinary SVC vector",
              (unsigned)i);
        CHECK(c.spsr[ARM_BANK_SVC] == old_cpsr,
              "case %u SPSR=%08x expect pristine pre-hook CPSR %08x",
              (unsigned)i, c.spsr[ARM_BANK_SVC], old_cpsr);
        CHECK(c.r[0] == 0x01020304u && c.r[7] == 0x07070707u &&
              c.cp15.context_id == 0x89abcdefu,
              "case %u leaked a declined/unknown hook's CPU mutations",
              (unsigned)i);
        CHECK(!c.excl_valid,
              "case %u must still clear the monitor on ordinary exception entry",
              (unsigned)i);
    }
}

static void test_privileged_svc_hook_a32_error_halts_transactionally(void) {
    const uint32_t svc = 0xef2468acu;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, svc);
    svc_probe_t probe = {0};
    probe.result = ARM_SVC_ERROR;
    probe.mutate = true;
    arm_bus_t bus = bus_with_svc_probe(&probe);
    arm_cpu_t c;
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_IRQ);
    const uint32_t old_cpsr = ARM_MODE_IRQ | ARM_CPSR_C;
    c.cpsr = old_cpsr;
    c.r[0] = 0x01020304u;
    c.r[7] = 0x07070707u;
    c.r[14] = 0x14141414u;
    c.cp15.context_id = 0x89abcdefu;
    c.excl_valid = true;
    c.excl_addr = 0x400u;
    c.spsr[ARM_BANK_SVC] = 0xa5a5a5a5u;
    c.bank_r14[ARM_BANK_SVC] = 0x51515151u;
    c.cycles = UINT64_MAX;              /* increment wraps; ERROR must undo it */
    probe.expected_cpu = &c;

    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_HALT && probe.calls == 1u,
          "A32 ERROR status=%d calls=%u expect HALT/1", (int)st, probe.calls);
    CHECK(probe.saw_expected_cpu && probe.seen_pc == 0u &&
          probe.seen_cpu_pc == 0u && probe.seen_encoding == svc,
          "A32 ERROR callback did not receive exact CPU/PC/encoding");
    CHECK(c.r[15] == 0u && c.cycles == UINT64_MAX && c.cpsr == old_cpsr,
          "A32 ERROR pc=%08x cycles=%llu cpsr=%08x must not retire the SVC",
          c.r[15], (unsigned long long)c.cycles, c.cpsr);
    CHECK(c.r[0] == 0x01020304u && c.r[7] == 0x07070707u &&
          c.r[14] == 0x14141414u && c.cp15.context_id == 0x89abcdefu,
          "A32 ERROR leaked callback register mutations");
    CHECK(c.excl_valid && c.excl_addr == 0x400u,
          "A32 ERROR must restore the pre-hook exclusive monitor");
    CHECK(c.spsr[ARM_BANK_SVC] == 0xa5a5a5a5u &&
          c.bank_r14[ARM_BANK_SVC] == 0x51515151u,
          "A32 ERROR must not enter or alter the guest SVC exception bank");
}

static void test_privileged_svc_hook_obeys_a32_guards(void) {
    svc_probe_t probe = {0};
    probe.result = ARM_SVC_HANDLED;
    arm_bus_t bus = bus_with_svc_probe(&probe);

    /* User SVC is always owned by the guest, even when a host callback says it
     * would handle that encoding. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xef000080u);
    arm_cpu_t c;
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_USR);
    c.cpsr = ARM_MODE_USR;
    probe.expected_cpu = &c;
    arm_step(&c);
    CHECK(probe.calls == 0u, "user-mode A32 SVC called the privileged hook");
    CHECK(c.r[15] == ARM_VEC_SWI &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC && c.r[14] == 4u,
          "user A32 SVC must retain the ordinary guest exception path");

    /* EQ SVC with Z clear is a condition-failed instruction, not a service
     * request. It retires without either callback or exception. */
    probe.calls = 0;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0x0f13579bu);
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_SYS);
    c.cpsr = ARM_MODE_SYS;                    /* Z clear: EQ fails */
    probe.expected_cpu = &c;
    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK && probe.calls == 0u && c.r[15] == 4u,
          "condition-failed SVC status=%d calls=%u pc=%08x expect OK/0/4",
          (int)st, probe.calls, c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS,
          "condition-failed SVC incorrectly entered mode %02x",
          c.cpsr & ARM_CPSR_MODE_MASK);

    /* Clearing the bus hook must clear its independent context too and restore
     * the byte-for-byte default privileged SVC path. */
    arm_bus_set_privileged_svc_handler(&bus, NULL, &probe);
    CHECK(bus.privileged_svc_handler == NULL && bus.privileged_svc_ctx == NULL,
          "clearing the SVC hook left a handler or dangling context");
    probe.calls = 0;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xef000000u);
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_SYS);
    c.cpsr = ARM_MODE_SYS;
    st = arm_step(&c);
    CHECK(st == ARM_OK && probe.calls == 0u && c.r[15] == ARM_VEC_SWI &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "cleared hook did not restore the ordinary privileged SVC path");
}

static void test_privileged_svc_hook_handles_thumb(void) {
    const uint16_t svc = 0xdfa5u;
    memset(g_ram, 0, sizeof g_ram);
    m_w16(NULL, 0x100u, svc);
    svc_probe_t probe = {0};
    probe.result = ARM_SVC_HANDLED;
    probe.mutate = true;
    arm_bus_t bus = bus_with_svc_probe(&probe);
    arm_cpu_t c;
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_SYS);
    c.cpsr = ARM_MODE_SYS | ARM_CPSR_T | ARM_CPSR_Z;
    c.r[0] = 0x2468ace0u;
    c.r[15] = 0x100u;
    c.spsr[ARM_BANK_SVC] = 0x55aa55aau;
    probe.expected_cpu = &c;

    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK && probe.calls == 1u && probe.saw_expected_cpu,
          "Thumb handled status=%d calls=%u", (int)st, probe.calls);
    CHECK(probe.seen_pc == 0x100u && probe.seen_cpu_pc == 0x100u &&
          probe.seen_encoding == (uint32_t)svc,
          "Thumb hook saw pc=%08x cpu.pc=%08x encoding=%08x",
          probe.seen_pc, probe.seen_cpu_pc, probe.seen_encoding);
    CHECK((probe.seen_cpsr & (ARM_CPSR_MODE_MASK | ARM_CPSR_T)) ==
          (ARM_MODE_SYS | ARM_CPSR_T) && probe.seen_r0 == 0x2468ace0u,
          "Thumb hook did not see the exact ISA/mode/register state");
    CHECK(c.r[15] == 0x102u && c.cycles == 1u &&
          (c.cpsr & (ARM_CPSR_MODE_MASK | ARM_CPSR_T)) ==
          (ARM_MODE_SYS | ARM_CPSR_T),
          "handled Thumb SVC did not retire sequentially in Thumb/SYS");
    CHECK(c.r[0] == 0xfeed0001u && c.cp15.context_id == 0xfeedc013u,
          "handled Thumb hook writes were not committed");
    CHECK(c.spsr[ARM_BANK_SVC] == 0x55aa55aau,
          "handled Thumb SVC incorrectly entered the SVC exception bank");
}

static void test_privileged_svc_hook_redirects_thumb_to_a32(void) {
    const uint16_t svc = 0xdfa5u;
    memset(g_ram, 0, sizeof g_ram);
    m_w16(NULL, 0x100u, svc);
    m_w32(NULL, 0x200u, 0xe3a02066u);         /* MOV r2,#0x66 (A32) */

    svc_probe_t probe = {0};
    probe.result = ARM_SVC_REDIRECTED;
    probe.mutate = true;
    probe.redirect = true;
    probe.redirect_thumb = false;
    probe.redirect_pc = 0x200u;
    arm_bus_t bus = bus_with_svc_probe(&probe);
    arm_cpu_t c;
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_SYS);
    c.cpsr = ARM_MODE_SYS | ARM_CPSR_T | ARM_CPSR_C;
    c.r[0] = 0x2468ace0u;
    c.r[15] = 0x100u;
    c.spsr[ARM_BANK_SVC] = 0x55aa55aau;
    c.cycles = 90u;
    probe.expected_cpu = &c;

    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK && probe.calls == 1u && probe.saw_expected_cpu,
          "Thumb redirect status=%d calls=%u", (int)st, probe.calls);
    CHECK(probe.seen_pc == 0x100u && probe.seen_cpu_pc == 0x100u &&
          probe.seen_encoding == (uint32_t)svc &&
          (probe.seen_cpsr & ARM_CPSR_T) != 0u,
          "Thumb redirect hook saw pc=%08x cpu.pc=%08x encoding=%08x cpsr=%08x",
          probe.seen_pc, probe.seen_cpu_pc, probe.seen_encoding, probe.seen_cpsr);
    CHECK(c.r[15] == 0x200u && (c.cpsr & ARM_CPSR_T) == 0u &&
          c.cycles == 91u,
          "Thumb redirect pc=%08x cpsr=%08x cycles=%llu expect A32 200/91",
          c.r[15], c.cpsr, (unsigned long long)c.cycles);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS &&
          (c.cpsr & ARM_CPSR_N) != 0u &&
          c.r[0] == 0xfeed0001u && c.r[7] == 0xfeed0007u &&
          c.cp15.context_id == 0xfeedc013u &&
          !c.excl_valid && c.excl_addr == 0xfeedeeeeu,
          "Thumb redirect did not commit the callback's complete CPU mutation");
    CHECK(c.spsr[ARM_BANK_SVC] == 0x55aa55aau,
          "Thumb redirect incorrectly entered the SVC exception bank");

    st = arm_step(&c);
    CHECK(st == ARM_OK && c.r[2] == 0x66u && c.r[15] == 0x204u &&
          (c.cpsr & ARM_CPSR_T) == 0u && c.cycles == 92u &&
          probe.calls == 1u,
          "redirect target did not fetch as A32: status=%d r2=%08x pc=%08x "
          "cpsr=%08x cycles=%llu calls=%u",
          (int)st, c.r[2], c.r[15], c.cpsr,
          (unsigned long long)c.cycles, probe.calls);
}

static void test_privileged_svc_hook_thumb_error_and_user_guard(void) {
    const uint16_t svc = 0xdf3cu;
    svc_probe_t probe = {0};
    probe.result = ARM_SVC_ERROR;
    probe.mutate = true;
    arm_bus_t bus = bus_with_svc_probe(&probe);

    memset(g_ram, 0, sizeof g_ram);
    m_w16(NULL, 0x100u, svc);
    arm_cpu_t c;
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_SYS);
    const uint32_t old_cpsr = ARM_MODE_SYS | ARM_CPSR_T | ARM_CPSR_C;
    c.cpsr = old_cpsr;
    c.r[0] = 0x11223344u;
    c.r[7] = 0x55667788u;
    c.r[14] = 0x14141414u;
    c.r[15] = 0x100u;
    c.cp15.context_id = 0x10203040u;
    c.excl_valid = true;
    c.excl_addr = 0x400u;
    c.spsr[ARM_BANK_SVC] = 0xa5a5a5a5u;
    c.bank_r14[ARM_BANK_SVC] = 0x51515151u;
    c.cycles = UINT64_MAX;              /* increment wraps; ERROR must undo it */
    probe.expected_cpu = &c;
    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_HALT && probe.calls == 1u &&
          probe.seen_encoding == (uint32_t)svc,
          "Thumb ERROR status=%d calls=%u encoding=%08x expect HALT/1/%04x",
          (int)st, probe.calls, probe.seen_encoding, svc);
    CHECK(c.r[15] == 0x100u && c.cycles == UINT64_MAX && c.cpsr == old_cpsr,
          "Thumb ERROR pc=%08x cycles=%llu cpsr=%08x must not retire the SVC",
          c.r[15], (unsigned long long)c.cycles, c.cpsr);
    CHECK(c.r[0] == 0x11223344u && c.r[7] == 0x55667788u &&
          c.r[14] == 0x14141414u && c.cp15.context_id == 0x10203040u,
          "Thumb ERROR leaked callback register mutations");
    CHECK(c.excl_valid && c.excl_addr == 0x400u,
          "Thumb ERROR must restore the pre-hook exclusive monitor");
    CHECK(c.spsr[ARM_BANK_SVC] == 0xa5a5a5a5u &&
          c.bank_r14[ARM_BANK_SVC] == 0x51515151u,
          "Thumb ERROR must not enter or alter the guest SVC exception bank");

    /* Thumb has no condition field, but it has the same hard User-mode gate. */
    memset(g_ram, 0, sizeof g_ram);
    m_w16(NULL, 0x100u, svc);
    probe.calls = 0;
    probe.result = ARM_SVC_HANDLED;
    arm_reset(&c, &bus);
    arm_set_mode(&c, ARM_MODE_USR);
    c.cpsr = ARM_MODE_USR | ARM_CPSR_T;
    c.r[15] = 0x100u;
    probe.expected_cpu = &c;
    st = arm_step(&c);
    CHECK(probe.calls == 0u, "user-mode Thumb SVC called the privileged hook");
    CHECK(st == ARM_OK && c.r[15] == ARM_VEC_SWI && c.r[14] == 0x102u &&
          (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "user Thumb SVC must retain the ordinary guest exception path");
}

static void test_swi_entry_state_from_user_mode(void) {
    /* The full ARM ARM (ARMv6, A2.6.4) entry contract for SWI, taken from User
     * mode — the only mode it will ever be taken from once launchd runs. */
    uint32_t p[0x44] = {0};
    p[0x40] = 0xef000080u;                    /* 0x100: SWI #0x80 */
    arm_cpu_t c; user_mode_at(&c, p, 0x44, 0x100);
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[13] = 0x900; c.r[14] = 0xdeadbeefu;   /* SVC bank, must be overwritten */
    arm_set_mode(&c, ARM_MODE_USR);
    c.r[13] = 0xaaaa0000u; c.r[14] = 0xbbbb0000u;
    c.r[12] = 4;                              /* the syscall number XNU reads */
    arm_step(&c);

    CHECK(c.r[15] == ARM_VEC_SWI, "pc=%08x expect 08", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "mode=%02x expect SVC", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.r[14] == 0x104, "lr_svc=%08x expect 104 (the instruction AFTER the "
          "SWI; _unix_syscall rewinds this by 4 for ERESTART)", c.r[14]);
    CHECK(c.r[13] == 0x900, "sp_svc=%08x expect the SVC bank, not the user's", c.r[13]);
    CHECK(c.spsr[ARM_BANK_SVC] == (ARM_MODE_USR),
          "spsr_svc=%08x expect the pre-switch User CPSR", c.spsr[ARM_BANK_SVC]);
    CHECK((c.cpsr & ARM_CPSR_I) != 0, "I must be set on entry");
    CHECK((c.cpsr & ARM_CPSR_A) == 0,
          "A must NOT be set: SWI and Undefined are the two vectors that leave "
          "it alone (A2.6). Setting it here would put a bit in every SPSR XNU "
          "saves that hardware would not have put there");
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "T must be cleared: handlers run in ARM state");
    CHECK((c.cpsr & ARM_CPSR_E) == 0, "E must come from SCTLR.EE, which is clear");
    CHECK(c.r[12] == 4, "r12 is not banked and must survive the switch to SVC");
}

static void test_thumb_swi_lr_is_the_next_halfword(void) {
    /* In Thumb state the SWI is two bytes, so the return address is PC+2, not
     * PC+4. Getting this wrong resumes the process one instruction early. */
    uint32_t p[0x44] = {0};
    m_w32(NULL, 0, 0);
    arm_cpu_t c; user_mode_at(&c, p, 0x44, 0x100);
    m_w16(NULL, 0x100, 0xdf80u);              /* SWI #0x80, Thumb */
    c.cpsr |= ARM_CPSR_T;
    arm_step(&c);

    CHECK(c.r[15] == ARM_VEC_SWI, "pc=%08x expect 08", c.r[15]);
    CHECK(c.r[14] == 0x102, "lr_svc=%08x expect 102 (PC+2 in Thumb)", c.r[14]);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "the handler runs in ARM state");
    CHECK((c.spsr[ARM_BANK_SVC] & ARM_CPSR_T) != 0,
          "SPSR.T must record that the caller was Thumb — _fleh_undef and "
          "_sleh_undef key off exactly this bit");
    CHECK((c.spsr[ARM_BANK_SVC] & ARM_CPSR_MODE_MASK) == ARM_MODE_USR,
          "SPSR mode must be User; _fleh_* branch on \"TST spsr,#0xf\"");
}

static void test_irq_sets_a_but_swi_does_not(void) {
    /* The distinction take_exception draws, pinned from both sides so neither
     * half can regress alone. */
    uint32_t p[] = { 0xef000000u };           /* SWI #0 */
    arm_cpu_t c; user_mode_at(&c, p, 1, 0);
    arm_step(&c);
    CHECK((c.cpsr & ARM_CPSR_A) == 0, "SWI must leave A alone");

    arm_cpu_t d; user_mode_at(&d, p, 1, 0);
    d.irq_line = true;
    arm_step(&d);
    CHECK(d.r[15] == ARM_VEC_IRQ, "pc=%08x expect 18 (IRQ)", d.r[15]);
    CHECK((d.cpsr & ARM_CPSR_A) != 0, "IRQ entry must set A");
    CHECK((d.cpsr & ARM_CPSR_F) == 0, "IRQ entry must NOT mask FIQ");
}

static void test_exception_entry_takes_cpsr_e_from_sctlr_ee(void) {
    /* CPSR.E <- SCTLR.EE, not "E is preserved". We never set EE, so the bit
     * must come back clear even when the interrupted code had set it. */
    uint32_t p[] = { 0xe3a00c02u,   /* MOV r0,#0x200  (the E bit)  */
                     0xe122f000u,   /* MSR CPSR_x, r0 -> sets E    */
                     0xef000000u }; /* SWI #0                      */
    arm_cpu_t c; load_and_run(&c, p, 3, 2);
    CHECK((c.cpsr & ARM_CPSR_E) != 0, "MSR should have set E for this test");
    arm_step(&c);
    CHECK((c.cpsr & ARM_CPSR_E) == 0, "exception entry must clear E from SCTLR.EE");
    CHECK((c.spsr[ARM_BANK_SVC] & ARM_CPSR_E) != 0,
          "the SPSR must still record that the caller had E set");
}

static void test_xnu_syscall_frame_round_trip(void) {
    /*
     * The real thing: SWI from User mode, _fleh_swi's "STM sp,{r0-r14}^",
     * the handler scribbling on the registers, _thread_exception_return's
     * "LDM sp,{r0-r14}^" and "MOVS pc,lr". Every user register must come back,
     * the SVC bank must be untouched throughout, and the frame in memory must
     * hold the USER r13/r14 rather than the handler's.
     */
    uint32_t p[0x44] = {0};
    p[0x02] = 0xe8cd7fffu;   /* 0x08: STM sp,{r0-r14}^   (the SWI vector) */
    p[0x03] = 0xe1a00000u;   /* 0x0c: NOP (the mandatory gap after ^)     */
    p[0x04] = 0xe3a00000u;   /* 0x10: MOV r0,#0   — clobber               */
    p[0x05] = 0xe3a0c000u;   /* 0x14: MOV r12,#0  — clobber               */
    p[0x06] = 0xe8dd7fffu;   /* 0x18: LDM sp,{r0-r14}^                    */
    p[0x07] = 0xe1a00000u;   /* 0x1c: NOP                                 */
    p[0x08] = 0xe1b0f00eu;   /* 0x20: MOVS pc,lr                          */
    p[0x40] = 0xef000000u;   /* 0x100: SWI #0                             */

    arm_cpu_t c; user_mode_at(&c, p, 0x44, 0x100);
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[13] = 0x900;                       /* the per-thread save area   */
    c.r[14] = 0xcccc0000u;                 /* SVC LR, must not leak out  */
    arm_set_mode(&c, ARM_MODE_USR);
    for (unsigned i = 0; i < 13; i++) c.r[i] = 0x1000u + i;
    c.r[13] = 0xaaaa0000u; c.r[14] = 0xbbbb0000u;

    for (int i = 0; i < 8; i++) arm_step(&c);

    /* The frame the kernel reads: r12 at +0x30, user sp at +0x34, user lr +0x38. */
    CHECK(m_r32(NULL, 0x900 + 0x00) == 0x1000u, "frame r0=%08x", m_r32(NULL, 0x900));
    CHECK(m_r32(NULL, 0x900 + 0x30) == 0x100cu,
          "frame+0x30=%08x expect r12 — this is the slot _unix_syscall reads the "
          "syscall number from", m_r32(NULL, 0x900 + 0x30));
    CHECK(m_r32(NULL, 0x900 + 0x34) == 0xaaaa0000u,
          "frame+0x34=%08x expect the USER sp, not sp_svc", m_r32(NULL, 0x900 + 0x34));
    CHECK(m_r32(NULL, 0x900 + 0x38) == 0xbbbb0000u,
          "frame+0x38=%08x expect the USER lr", m_r32(NULL, 0x900 + 0x38));

    /* And the return. */
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR,
          "mode=%02x expect back in User", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.r[15] == 0x104, "pc=%08x expect 104", c.r[15]);
    CHECK(c.r[0] == 0x1000u && c.r[12] == 0x100cu,
          "r0=%08x r12=%08x: the clobbered registers must be restored",
          c.r[0], c.r[12]);
    CHECK(c.r[13] == 0xaaaa0000u && c.r[14] == 0xbbbb0000u,
          "sp=%08x lr=%08x expect the user bank back", c.r[13], c.r[14]);
    arm_set_mode(&c, ARM_MODE_SVC);
    CHECK(c.r[13] == 0x900 && c.r[14] == 0x104,
          "sp_svc=%08x lr_svc=%08x: the LDM^ must write the USER bank ONLY. "
          "lr_svc still has to hold the resume address the SWI put there — "
          "_thread_exception_return reloads it from the frame BEFORE the LDM^ "
          "and then feeds it straight to MOVS pc,lr, so a LDM^ that wrote the "
          "current bank would return to the user's r14 instead", c.r[13], c.r[14]);
}

static void test_syscall_error_flag_reaches_user_through_the_spsr(void) {
    /*
     * How a failed syscall tells the process. _unix_syscall does
     * "LDR r3,[r6,#0x40]; ORR r3,r3,#0x20000000; STR r3,[r6,#0x40]" — it sets
     * the C flag in the SAVED CPSR — and _thread_exception_return reloads that
     * word into SPSR_svc with "MSR spsr_fsxc,r4" before "MOVS pc,lr". So the
     * flags a user process sees after a syscall travel entirely through the
     * SPSR: an exception return that recomputed NZCV from its own arithmetic,
     * or that restored only the control byte, would make every syscall look
     * successful to libSystem's "bcs cerror".
     */
    uint32_t p[] = { 0xe16ff000u,   /* 0x00: MSR spsr_fsxc, r0 */
                     0xe1b0f00eu }; /* 0x04: MOVS pc, lr       */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, p[0]); m_w32(NULL, 4, p[1]);
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[0]  = ARM_MODE_USR | ARM_CPSR_C;   /* the frame's cpsr word, C set */
    c.r[14] = 0x00001020u;
    c.cpsr |= ARM_CPSR_Z;                  /* handler flags, must not survive */
    c.cpsr &= ~ARM_CPSR_C;
    c.r[15] = 0;
    arm_step(&c); arm_step(&c);

    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR,
          "mode=%02x expect User", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK((c.cpsr & ARM_CPSR_C) != 0,
          "cpsr=%08x: C must arrive from the SPSR — this is the syscall error "
          "indication", c.cpsr);
    CHECK((c.cpsr & ARM_CPSR_Z) == 0,
          "cpsr=%08x: the handler's own flags must not leak into user mode", c.cpsr);
    CHECK(c.r[15] == 0x00001020u, "pc=%08x expect 1020", c.r[15]);
}

static void test_user_bank_ldm_writes_the_user_bank(void) {
    /* The load direction of the same rule, isolated. */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    arm_set_mode(&c, ARM_MODE_USR);
    c.r[13] = 0x1111; c.r[14] = 0x2222;
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[13] = 0x800; c.r[14] = 0xcccc0000u;
    m_w32(NULL, 0x800, 0x33330000u);      /* -> user r13 */
    m_w32(NULL, 0x804, 0x44440000u);      /* -> user r14 */
    m_w32(NULL, 0, 0xe8dd6000u);          /* LDM sp,{r13,r14}^ */
    c.r[15] = 0;
    arm_step(&c);

    CHECK(c.r[13] == 0x800 && c.r[14] == 0xcccc0000u,
          "sp_svc=%08x lr_svc=%08x must be untouched", c.r[13], c.r[14]);
    arm_set_mode(&c, ARM_MODE_USR);
    CHECK(c.r[13] == 0x33330000u && c.r[14] == 0x44440000u,
          "user sp=%08x lr=%08x expect the loaded values", c.r[13], c.r[14]);
}

static void test_user_bank_stm_from_fiq_uses_the_user_r8_r12(void) {
    /* FIQ is the mode where the `^` form has the most to get wrong: it banks
     * r8-r12 as well as r13/r14, so a save that reads the live registers would
     * write FIQ's private copies into the interrupted thread's frame and hand
     * them back to it on resume. */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    arm_set_mode(&c, ARM_MODE_USR);
    for (unsigned i = 8; i <= 12; i++) c.r[i] = 0xd0000000u + i;
    c.r[13] = 0xaaaa0000u; c.r[14] = 0xbbbb0000u;
    arm_set_mode(&c, ARM_MODE_FIQ);
    for (unsigned i = 8; i <= 12; i++) c.r[i] = 0xf0000000u + i;
    c.r[13] = 0x800; c.r[14] = 0xcccc0000u;
    m_w32(NULL, 0, 0xe8cd7f00u);          /* STMIA sp,{r8-r14}^ */
    c.r[15] = 0;
    arm_step(&c);

    for (unsigned i = 0; i < 5; i++)
        CHECK(m_r32(NULL, 0x800 + i * 4) == 0xd0000000u + 8 + i,
              "frame r%u=%08x expect the USER bank, not FIQ's", 8 + i,
              m_r32(NULL, 0x800 + i * 4));
    CHECK(m_r32(NULL, 0x814) == 0xaaaa0000u, "frame sp=%08x expect user sp",
          m_r32(NULL, 0x814));
    CHECK(m_r32(NULL, 0x818) == 0xbbbb0000u, "frame lr=%08x expect user lr",
          m_r32(NULL, 0x818));
    CHECK(c.r[8] == 0xf0000008u && c.r[14] == 0xcccc0000u,
          "the FIQ bank must be untouched");
}

static void test_user_bank_transfer_with_writeback_traps(void) {
    /* LDM(2)/STM(2) forbid writeback (ARM ARM A4.1.21/A4.1.42): W==1 is
     * UNPREDICTABLE, and the two plausible answers differ exactly when Rn is
     * r13 — the only base XNU uses with this form. Trap rather than pick one. */
    uint32_t st[] = { 0xe8ed7fffu };      /* STMIA sp!,{r0-r14}^ */
    uint32_t ld[] = { 0xe8fd7fffu };      /* LDMIA sp!,{r0-r14}^ */
    arm_cpu_t c;
    CHECK(run_status(&c, st, 1, 1) == ARM_UNDEFINED, "STM^ with writeback must trap");
    CHECK(run_status(&c, ld, 1, 1) == ARM_UNDEFINED, "LDM^ with writeback must trap");
    /* ...but the exception-return LDM, which legitimately writes back, must not. */
    arm_cpu_t d;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xe8fd8000u);          /* LDMIA sp!,{pc}^ */
    m_w32(NULL, 0x800, 0x00001020u);
    arm_reset(&d, &g_bus);
    arm_set_mode(&d, ARM_MODE_SVC);
    d.spsr[ARM_BANK_SVC] = ARM_MODE_USR;
    d.r[13] = 0x800;
    d.r[15] = 0;
    CHECK(arm_step(&d) == ARM_OK,
          "LDM {pc}^ with writeback is LDM(3), where writeback IS allowed");
    CHECK(d.r[15] == 0x00001020u, "pc=%08x expect the return address", d.r[15]);
    CHECK((d.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR,
          "mode=%02x expect User restored from SPSR", d.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(d.bank_r13[ARM_BANK_SVC] == 0x804,
          "sp_svc=%08x expect 804: the writeback lands in the HANDLER's banked "
          "Rn, and only then does CPSR<-SPSR rebank us", d.bank_r13[ARM_BANK_SVC]);
}

static void test_user_bank_transfers_reject_user_and_system_modes(void) {
    static const uint32_t insns[] = {
        0xe8d00002u,                            /* LDMIA r0,{r1}^ */
        0xe8c00002u,                            /* STMIA r0,{r1}^ */
    };
    static const uint32_t modes[] = { ARM_MODE_USR, ARM_MODE_SYS };

    for (unsigned mi = 0; mi < 2u; mi++) {
        for (unsigned ii = 0; ii < 2u; ii++) {
            arm_cpu_t c;
            g_watch_addr = 0xffffffffu;
            g_watch_reads32 = g_watch_writes32 = 0u;
            memset(g_ram, 0, sizeof g_ram);
            m_w32(NULL, 0u, insns[ii]);
            arm_reset(&c, &g_bus);
            arm_set_mode(&c, modes[mi]);
            c.r[0] = 0x800u;
            c.r[1] = 0x11223344u;
            c.r[15] = 0u;
            g_watch_addr = 0x800u;
            CHECK(arm_step(&c) == ARM_UNDEFINED,
                  "%s^ in mode %02x was not refused as UNPREDICTABLE",
                  ii == 0u ? "LDM" : "STM", modes[mi]);
            CHECK(g_watch_reads32 == 0u && g_watch_writes32 == 0u,
                  "%s^ in mode %02x touched memory before rejection",
                  ii == 0u ? "LDM" : "STM", modes[mi]);
        }
    }
    g_watch_addr = 0xffffffffu;
}

static void test_srs_stores_lr_and_spsr_of_the_current_mode(void) {
    /* _fleh_swi+0x18 is "SRSIA sp,#0x13" (0xf8cd0513): it appends the user PC
     * (in lr_svc) and the user CPSR (in spsr_svc) to the frame at +0x3c/+0x40,
     * which is exactly where _thread_exception_return reads them back from. */
    uint32_t p[] = { 0xf8cd0513u };
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, p[0]);
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[13] = 0x93c; c.r[14] = 0x00000104u;
    c.spsr[ARM_BANK_SVC] = ARM_MODE_USR | ARM_CPSR_T;
    c.r[15] = 0;
    arm_step(&c);

    CHECK(m_r32(NULL, 0x93c) == 0x00000104u, "[sp]=%08x expect lr_svc", m_r32(NULL, 0x93c));
    CHECK(m_r32(NULL, 0x940) == (ARM_MODE_USR | ARM_CPSR_T),
          "[sp+4]=%08x expect spsr_svc", m_r32(NULL, 0x940));
    CHECK(c.r[13] == 0x93c, "W==0 in this encoding: sp must not move");
}

static void test_subs_pc_lr_4_returns_from_fiq(void) {
    /* _fleh_fiq_generic ends with "SUBSPL pc,lr,#4" — the third exception-return
     * form, and the only one that arrives at the resume address by arithmetic. */
    uint32_t p[] = { 0xe25ef004u };       /* SUBS pc,lr,#4 */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, p[0]);
    arm_set_mode(&c, ARM_MODE_FIQ);
    c.spsr[ARM_BANK_FIQ] = ARM_MODE_USR;
    c.r[14] = 0x00001020u;
    c.r[15] = 0;
    arm_step(&c);
    CHECK(c.r[15] == 0x0000101cu, "pc=%08x expect 101c (lr-4)", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR,
          "mode=%02x expect User restored from SPSR", c.cpsr & ARM_CPSR_MODE_MASK);
}

/* --- copyin / copyout: the T forms under real user-pmap conditions -------- */

/* Identity-map section 0 as AP=11 (so the fetch works from any mode) and map
 * VA 0x80000000 -> PA 0 as AP=01: privileged RW, user NO ACCESS. That is what
 * a kernel-only page looks like to copyin. */
static void kernel_only_page_setup(arm_cpu_t *c, const uint32_t *prog, size_t words) {
    const uint32_t l1 = 0x4000;
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i * 4), prog[i]);
    m_w32(NULL, l1 + 0,           0x00000000u | (3u << 10) | 2u);  /* AP=11 */
    m_w32(NULL, l1 + (0x800 << 2), 0x00000000u | (1u << 10) | 2u); /* AP=01 */
    arm_reset(c, &g_bus);
    c->cp15.ttbr0 = l1;
    c->cp15.dacr  = 1u;                       /* domain 0 client: check AP */
    c->cp15.sctlr |= ARM_SCTLR_M;
    arm_set_mode(c, ARM_MODE_SVC);
    c->r[15] = 0;
}

static void test_ldrt_from_svc_translates_as_unprivileged(void) {
    /* copyin's inner loop is "LDRT r3,[r0],#4" issued from SVC with the user
     * pmap in TTBR0. A plain LDR of the same address succeeds; the T form must
     * fault, because that is the whole mechanism protecting the kernel from
     * dereferencing a pointer the process could not have followed itself. */
    uint32_t p[] = { 0xe3a01102u,   /* MOV r1,#0x80000000 */
                     0xe3811c03u,   /* ORR r1,r1,#0x300   */
                     0xe5910000u,   /* LDR  r0,[r1]       (privileged: ok)   */
                     0xe4b12000u }; /* LDRT r2,[r1]       (unprivileged: fault) */
    arm_cpu_t c; kernel_only_page_setup(&c, p, 4);
    m_w32(NULL, 0x300, 0x5a5aa5a5u);
    arm_step(&c); arm_step(&c); arm_step(&c);
    CHECK(c.r[0] == 0x5a5aa5a5u,
          "r0=%08x: a plain LDR from SVC must read the kernel-only page", c.r[0]);
    arm_step(&c);
    CHECK(c.r[15] == ARM_VEC_DATA_ABORT,
          "pc=%08x expect 10: LDRT from a privileged mode translates as "
          "unprivileged and must abort here", c.r[15]);
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "dfsr=%08x expect a section permission fault", c.cp15.dfsr);
    CHECK((c.cp15.dfsr & (1u << 11)) == 0, "WnR must be clear for a read");
    CHECK(c.cp15.dfar == 0x80000300u, "dfar=%08x expect 80000300", c.cp15.dfar);
}

static void test_strt_from_svc_translates_as_unprivileged(void) {
    /* copyout's mirror image, and the one that also has to get DFSR.WnR right. */
    uint32_t p[] = { 0xe3a01102u,   /* MOV r1,#0x80000000 */
                     0xe3811c03u,   /* ORR r1,r1,#0x300   */
                     0xe5810000u,   /* STR  r0,[r1]       (privileged: ok)   */
                     0xe4a12000u }; /* STRT r2,[r1]       (unprivileged: fault) */
    arm_cpu_t c; kernel_only_page_setup(&c, p, 4);
    c.r[0] = 0x11223344u;
    arm_step(&c); arm_step(&c); arm_step(&c);
    CHECK(m_r32(NULL, 0x300) == 0x11223344u, "a plain STR from SVC must land");
    arm_step(&c);
    CHECK(c.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (STRT must abort)", c.r[15]);
    CHECK((c.cp15.dfsr & (1u << 11)) != 0, "dfsr=%08x: WnR must be set for a write",
          c.cp15.dfsr);
}

static void test_user_mode_load_uses_user_permissions(void) {
    /* And the same page reached from genuine User mode, so `priv` is confirmed
     * to come from the CURRENT mode and not only from the instruction form. */
    uint32_t p[] = { 0xe3a01102u,   /* MOV r1,#0x80000000 */
                     0xe5910000u }; /* LDR r0,[r1]        */
    arm_cpu_t c; kernel_only_page_setup(&c, p, 2);
    arm_set_mode(&c, ARM_MODE_USR);
    arm_step(&c); arm_step(&c);
    CHECK(c.r[15] == ARM_VEC_DATA_ABORT,
          "pc=%08x expect 10: a plain LDR in User mode must fault on a "
          "kernel-only page", c.r[15]);
}

/* --- CP15 is privileged ---------------------------------------------------- */

static arm_status_t run_one_in_user(arm_cpu_t *c, uint32_t insn) {
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, insn);
    arm_reset(c, &g_bus);
    arm_set_mode(c, ARM_MODE_USR);
    c->r[15] = 0;
    return arm_step(c);
}

static void test_cp15_is_privileged_from_user_mode(void) {
    /*
     * CP15 is not a free-for-all. Without this check a user process can write
     * TTBR0 and point translation at a page table it owns, or clear SCTLR.M,
     * or make DACR call every domain a manager — and our MMU would obey. Same
     * shape as the CPS-in-User-mode hole: invisible for the entire kernel-only
     * boot, live from launchd's first instruction.
     */
    arm_cpu_t c;
    CHECK(run_one_in_user(&c, 0xee110f10u) == ARM_UNDEFINED, "MRC SCTLR must trap");
    CHECK(run_one_in_user(&c, 0xee010f10u) == ARM_UNDEFINED, "MCR SCTLR must trap");
    CHECK(run_one_in_user(&c, 0xee020f10u) == ARM_UNDEFINED, "MCR TTBR0 must trap");
    CHECK(run_one_in_user(&c, 0xee030f10u) == ARM_UNDEFINED, "MCR DACR must trap");
    CHECK(run_one_in_user(&c, 0xee1d0f90u) == ARM_UNDEFINED, "MRC TPIDRPRW must trap");
    CHECK(run_one_in_user(&c, 0xee110f30u) == ARM_UNDEFINED, "MRC CP14 must trap");
    /* The same accesses from a privileged mode must still work. */
    uint32_t p[] = { 0xee110f10u };
    arm_cpu_t d; load_and_run(&d, p, 1, 1);
    CHECK(d.r[0] == d.cp15.sctlr, "SCTLR must still be readable from SYS mode");
}

static void test_cp15_thread_id_registers_stay_user_accessible(void) {
    /* The three CP15 accesses the ARM1176 does grant User mode. TPIDRURO is the
     * load-bearing one: _thread_set_cthread_self (0xc0061da0) writes it with
     * "MCR p15,0,r0,c13,c0,3" and libSystem reads it back from User mode. */
    arm_cpu_t c;
    CHECK(run_one_in_user(&c, 0xee1d0f70u) == ARM_OK, "MRC TPIDRURO must be allowed");
    CHECK(run_one_in_user(&c, 0xee0d0f70u) == ARM_UNDEFINED,
          "MCR TPIDRURO is READ-only from User mode");
    CHECK(run_one_in_user(&c, 0xee1d0f50u) == ARM_OK, "MRC TPIDRURW must be allowed");
    CHECK(run_one_in_user(&c, 0xee0d0f50u) == ARM_OK, "MCR TPIDRURW must be allowed");
    CHECK(run_one_in_user(&c, 0xee071f9au) == ARM_OK,
          "the c7 barrier/cache operations stay accessible from User mode");

    /* And the value really round-trips through the kernel-side write. */
    arm_cpu_t d;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xee1d0f70u);            /* MRC p15,0,r0,c13,c0,3 */
    arm_reset(&d, &g_bus);
    d.cp15.tpidruro = 0xfeedface;
    arm_set_mode(&d, ARM_MODE_USR);
    d.r[15] = 0;
    arm_step(&d);
    CHECK(d.r[0] == 0xfeedfaceu, "r0=%08x expect the cthread_self value", d.r[0]);
}

/* --- page-crossing unaligned accesses, and XN --------------------------- */

/*
 * Two 4 KB small pages behind one coarse table, at VA 0x80000000 and
 * 0x80001000, whose physical frames the caller chooses — and chooses to be far
 * apart, because that is the whole point: a walker that translates only the
 * base of a straddling access reads or writes across the *physical* boundary,
 * which is silently the wrong memory whenever the frames are not adjacent.
 * A frame of 0 leaves that page with no second-level descriptor at all.
 * `xn1` sets execute-never (bit 0 of a small-page descriptor) on the second.
 *
 * The low megabyte is identity-mapped by a section so the instruction fetch
 * always succeeds, and SCTLR.XP is set because that is the configuration XNU
 * runs in and the only one in which XN exists.
 */
static void split_setup(arm_cpu_t *c, const uint32_t *prog, size_t words,
                        uint32_t pa0, uint32_t pa1, bool xn1) {
    const uint32_t l1 = 0x4000, l2 = 0x5000;
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i * 4), prog[i]);
    m_w32(NULL, l1 + (0x000u << 2), 0x00000000u | (3u << 10) | 8u | 2u);/* Normal identity */
    m_w32(NULL, l1 + (0x800u << 2), (l2 & 0xfffffc00u) | 1u);         /* coarse   */
    if (pa0) m_w32(NULL, l2 + (0u << 2),
                    (pa0 & 0xfffff000u) | (3u << 4) | 8u | 2u);
    if (pa1) m_w32(NULL, l2 + (1u << 2),
                    (pa1 & 0xfffff000u) | (3u << 4) | 8u | 2u | (xn1 ? 1u : 0u));
    arm_reset(c, &g_bus);
    c->cp15.ttbr0  = l1;
    c->cp15.dacr   = 1u;                       /* domain 0: client */
    c->cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP | ARM_SCTLR_U;
    c->cpsr = (c->cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c->r[15] = 0;
}

/*
 * ARMv6 parallel add/subtract. Run32 stopped on 0xe611ef9e, SADD8 lr, r1, lr,
 * in SpringBoard's path to its first frame. The whole family is implemented
 * together because the classes differ only in how a lane result is reduced, so
 * covering one and trapping its neighbours would hide the next stop behind an
 * identical shape.
 *
 * Each case builds r1/r2, executes one encoding, and checks the packed result
 * and the GE flags. Lane independence is the property that matters most: a
 * carry or borrow must never cross a lane boundary.
 */
static void test_parallel_add_sub_family(void) {
    arm_cpu_t c;
    static const struct {
        uint32_t insn, a, b, expect, ge;
        bool check_ge;
        const char *what;
    } CASES[] = {
        /* Every encoding below is Rd = r2, Rn = r1, Rm = r0. */
        /* SADD8: -128+127, -1+1, 1+2, 127+1 -- the last wraps to 0x80 but its
         * full-precision sum is still non-negative, so GE3 is set. */
        { 0xe6112f90u, 0x7f01ff80u, 0x0102017fu, 0x800300ffu, 0xeu, true,
          "SADD8 lanes and GE" },
        /* The exact encoding run32 stopped on: SADD8 lr, r1, lr. */
        { 0xe611ef9eu, 0x00000001u, 0u, 0u, 0u, false, "SADD8 lr,r1,lr decodes" },
        /* SADD16: -1+1 in the low lane must not carry into the high lane. */
        { 0xe6112f10u, 0x0001ffffu, 0x00010001u, 0x00020000u, 0xfu, true,
          "SADD16 no carry across lanes" },
        /* SSUB16 with a negative low lane clears the low GE pair. */
        { 0xe6112f70u, 0x00000000u, 0x00000001u, 0x0000ffffu, 0xcu, true,
          "SSUB16 GE from lane sign" },
        /* UADD8 sets GE from the unsigned carry out of each byte. */
        { 0xe6512f90u, 0x80ff0100u, 0x8001ff00u, 0x00000000u, 0xeu, true,
          "UADD8 GE from carry" },
        /* USUB8: GE means "no borrow", i.e. a >= b per byte. */
        { 0xe6512ff0u, 0x02000200u, 0x01010100u, 0x01ff0100u, 0xbu, true,
          "USUB8 GE from no-borrow" },
        /* UQADD8 saturates each byte at 0xff rather than wrapping. */
        { 0xe6612f90u, 0x80ff0100u, 0x8001ff00u, 0xffffff00u, 0u, false,
          "UQADD8 saturates" },
        /* QADD16 saturates a signed halfword at 0x7fff. */
        { 0xe6212f10u, 0x7fff0000u, 0x00010000u, 0x7fff0000u, 0u, false,
          "QADD16 saturates" },
        /* SHADD8 halves each lane: (4+2)>>1 == 3. */
        { 0xe6312f90u, 0x00000004u, 0x00000002u, 0x00000003u, 0u, false,
          "SHADD8 halves" },
        /* UHADD16 halves without sign extension: (0xffff+1)>>1 == 0x8000. */
        { 0xe6712f10u, 0x0000ffffu, 0x00000001u, 0x00008000u, 0u, false,
          "UHADD16 halves unsigned" },
        /* SASX: low = a.lo - b.hi = 3-2 = 1; high = a.hi + b.lo = 5+1 = 6. */
        { 0xe6112f30u, 0x00050003u, 0x00020001u, 0x00060001u, 0xfu, true,
          "SASX exchange" },
        /* SSAX: low = a.lo + b.hi = 3+2 = 5; high = a.hi - b.lo = 5-1 = 4. */
        { 0xe6112f50u, 0x00050003u, 0x00020001u, 0x00040005u, 0xfu, true,
          "SSAX exchange" },
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint32_t prog[] = { CASES[i].insn };
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, prog[0]);
        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
        c.r[1]  = CASES[i].a;      /* Rn */
        c.r[0]  = CASES[i].b;      /* Rm */
        c.r[14] = CASES[i].b;
        CHECK(arm_step(&c) == ARM_OK, "%s: refused", CASES[i].what);
        if (CASES[i].check_ge) {
            CHECK(c.r[2] == CASES[i].expect, "%s: got %08x expected %08x",
                  CASES[i].what, c.r[2], CASES[i].expect);
            CHECK(((c.cpsr >> 16) & 0xfu) == CASES[i].ge,
                  "%s: GE %x expected %x", CASES[i].what,
                  (c.cpsr >> 16) & 0xfu, CASES[i].ge);
        }
    }

    /* The saturating and halving classes must leave GE alone. */
    {
        uint32_t prog[] = { 0xe6612f90u };            /* UQADD8 r2, r1, r0 */
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, prog[0]);
        arm_reset(&c, &g_bus);
        c.cpsr = ((c.cpsr & ~0x1fu) | ARM_MODE_SYS) | (0xfu << 16);
        CHECK(arm_step(&c) == ARM_OK, "UQADD8 refused");
        CHECK(((c.cpsr >> 16) & 0xfu) == 0xfu,
              "a saturating class must not write GE");
    }

    /* PC operands are UNPREDICTABLE and must refuse rather than branch. */
    {
        uint32_t prog[] = { 0xe611ff90u };            /* SADD8 pc, r1, r0 */
        CHECK(run_status(&c, prog, 1, 1) == ARM_UNDEFINED,
              "SADD8 with Rd == PC must refuse");
    }
}

/*
 * PKHBT / PKHTB / SEL -- the media instructions r205 stopped on.
 *
 * r205 halted with UNDEFINED INSTRUCTION on 0xe6844853, `PKHTB r4, r4, r3,
 * ASR #16`, at 0x33922c78 in pid 38, about 340 M instructions after a tap that
 * opened Notes. Every earlier boot missed it because nothing had run
 * halfword-packing code; the decoder's catch-all named PKH in a comment and
 * returned UNDEFINED.
 *
 * SEL is tested here rather than after the next stop because the parallel
 * add/sub family above already WRITES the GE flags and SEL is what reads them:
 * producer present, consumer trapping, is a one-instruction hole in the middle
 * of otherwise working code.
 */
static void test_pack_halfword_and_select(void) {
    arm_cpu_t c;
    static const struct {
        uint32_t insn, rn, rm, ge, expect;
        const char *what;
    } CASES[] = {
        /* PKHTB r2, r1, r0, ASR #16: top half from Rn, low half from Rm >> 16.
         * Rm is negative, so the arithmetic shift fills with ones and the low
         * half is its top half. */
        { 0xe6812850u, 0xaaaa1111u, 0xbbbb2222u, 0u, 0xaaaabbbbu,
          "PKHTB takes Rn top and shifted Rm bottom" },
        /* PKHBT r2, r1, r0, LSL #8: low half from Rn, top half from Rm << 8. */
        { 0xe6812410u, 0xaaaa1111u, 0x0000bb22u, 0u, 0x00bb1111u,
          "PKHBT takes Rn bottom and shifted Rm top" },
        /* PKHTB with imm5 == 0 encodes ASR #32, not "no shift": every bit of
         * the result becomes Rm's sign, so the low half is all ones here. */
        { 0xe6812050u, 0xaaaa1111u, 0x80000000u, 0u, 0xaaaaffffu,
          "PKHTB imm5 zero is ASR #32" },
        /* ...and the same encoding with a non-negative Rm gives all zeroes,
         * which distinguishes ASR #32 from a shift that was skipped. */
        { 0xe6812050u, 0xaaaa1111u, 0x7fffffffu, 0u, 0xaaaa0000u,
          "PKHTB ASR #32 of a positive Rm is zero" },
        /* SEL r2, r1, r0: each GE bit picks that byte lane from Rn, else Rm. */
        { 0xe6812fb0u, 0xaabbccddu, 0x11223344u, 0xau, 0xaa22cc44u,
          "SEL picks lanes by GE" },
        { 0xe6812fb0u, 0xaabbccddu, 0x11223344u, 0xfu, 0xaabbccddu,
          "SEL all GE set takes Rn entirely" },
        { 0xe6812fb0u, 0xaabbccddu, 0x11223344u, 0x0u, 0x11223344u,
          "SEL no GE set takes Rm entirely" },
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint32_t prog[] = { CASES[i].insn };
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, prog[0]);
        arm_reset(&c, &g_bus);
        c.cpsr = ((c.cpsr & ~0x1fu) | ARM_MODE_SYS) | (CASES[i].ge << 16);
        c.r[1] = CASES[i].rn;
        c.r[0] = CASES[i].rm;
        CHECK(arm_step(&c) == ARM_OK, "%s: refused", CASES[i].what);
        CHECK(c.r[2] == CASES[i].expect, "%s: got %08x expected %08x",
              CASES[i].what, c.r[2], CASES[i].expect);
    }

    /* The exact encoding that stopped r205, with its own registers. */
    {
        uint32_t prog[] = { 0xe6844853u };      /* PKHTB r4, r4, r3, ASR #16 */
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, prog[0]);
        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
        c.r[4] = 0x638bff00u;                   /* r4 as r205 recorded it */
        c.r[3] = 0x556289ffu;                   /* r3 as r205 recorded it */
        CHECK(arm_step(&c) == ARM_OK, "the r205 encoding must not be UNDEFINED");
        CHECK(c.r[4] == 0x638b5562u, "r205 encoding: got %08x expected %08x",
              c.r[4], 0x638b5562u);
    }

    /* PC operands are UNPREDICTABLE and must refuse rather than branch. */
    {
        uint32_t prog[] = { 0xe681f850u };      /* PKHTB pc, r1, r0, ASR #16 */
        CHECK(run_status(&c, prog, 1, 1) == ARM_UNDEFINED,
              "PKHTB with Rd == PC must refuse");
    }
    {
        uint32_t prog[] = { 0xe681ffb0u };      /* SEL pc, r1, r0 */
        CHECK(run_status(&c, prog, 1, 1) == ARM_UNDEFINED,
              "SEL with Rd == PC must refuse");
    }
}

/*
 * The ARMv6 signed dual multiplies, and the multiply-accumulate media space
 * around them. r207 stopped on SMLAD (0xe7088e12) one billion instructions
 * further than r205's PKHTB, which is what argued for implementing the whole
 * family rather than discovering each member at forty minutes a run.
 */
static void test_signed_dual_multiply_family(void) {
    arm_cpu_t c;
    static const struct {
        uint32_t insn, rn, rm, ra, expect;
        bool     expect_q;
        const char *what;
    } CASES[] = {
        /* SMLAD r2,r1,r0,r3: 3*5 + 2*4 + 100 = 123. */
        { 0xe7023011u, 0x00020003u, 0x00040005u, 100u, 123u, false,
          "SMLAD adds both products and the accumulator" },
        /* SMUAD is the Ra == 15 encoding: same products, no accumulator. */
        { 0xe702f011u, 0x00020003u, 0x00040005u, 0u, 23u, false,
          "SMUAD has no accumulator" },
        /* SMLSD subtracts the high product from the low one. */
        { 0xe7023051u, 0x00020003u, 0x00040005u, 100u, 107u, false,
          "SMLSD subtracts the high product" },
        { 0xe702f051u, 0x00020003u, 0x00040005u, 0u, 7u, false,
          "SMUSD subtracts without an accumulator" },
        /* The X form swaps Rm's halves first: 3*4 + 2*5 + 100 = 122. */
        { 0xe7023031u, 0x00020003u, 0x00040005u, 100u, 122u, false,
          "SMLADX swaps the Rm halves" },
        /* Halves are SIGNED: 2*4 + (-1)*3 = 5, not 2*4 + 65535*3. */
        { 0xe702f011u, 0xffff0002u, 0x00030004u, 0u, 5u, false,
          "dual products sign-extend their halves" },
        /* Q is set when the full-precision result does not fit in 32 bits. */
        { 0xe7023011u, 0x7fff7fffu, 0x7fff7fffu, 0x7fffffffu, 0xfffe0001u, true,
          "SMLAD sets Q on overflow and keeps the low word" },
        /* SMMUL keeps the TOP word of a 32x32 product. */
        { 0xe752f011u, 0x40000000u, 0x40000000u, 0u, 0x10000000u, false,
          "SMMUL keeps the high word" },
        /* SMMLA adds Ra at bit 32 before taking the top word. */
        { 0xe7523011u, 0x40000000u, 0x40000000u, 1u, 0x10000001u, false,
          "SMMLA accumulates into the high word" },
        /* USAD8 sums |a-b| per byte: 3+1+1+3 = 8. */
        { 0xe782f011u, 0x01020304u, 0x04030201u, 0u, 8u, false,
          "USAD8 sums absolute byte differences" },
        /* USADA8 adds Ra to that sum. */
        { 0xe7823011u, 0x01020304u, 0x04030201u, 10u, 18u, false,
          "USADA8 adds the accumulator" },
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint32_t prog[] = { CASES[i].insn };
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, prog[0]);
        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
        c.cpsr &= ~ARM_CPSR_Q;
        c.r[1] = CASES[i].rn;
        c.r[0] = CASES[i].rm;
        c.r[3] = CASES[i].ra;
        CHECK(arm_step(&c) == ARM_OK, "%s: refused", CASES[i].what);
        CHECK(c.r[2] == CASES[i].expect, "%s: got %08x expected %08x",
              CASES[i].what, c.r[2], CASES[i].expect);
        CHECK(((c.cpsr & ARM_CPSR_Q) != 0u) == CASES[i].expect_q,
              "%s: Q was %d", CASES[i].what,
              (c.cpsr & ARM_CPSR_Q) != 0u);
    }

    /* The exact encoding and register values r207 recorded. */
    {
        uint32_t prog[] = { 0xe7088e12u };      /* SMLAD r8, r2, lr, r8 */
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, prog[0]);
        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
        c.r[2]  = 0x00020001u;
        c.r[14] = 0x10c23294u;
        c.r[8]  = 0x00002000u;
        CHECK(arm_step(&c) == ARM_OK, "the r207 encoding must not be UNDEFINED");
        CHECK(c.r[8] == 0x00007418u, "r207 encoding: got %08x expected %08x",
              c.r[8], 0x00007418u);
    }

    /* Q is sticky: a later non-overflowing operation must not clear it. */
    {
        uint32_t prog[] = { 0xe702f011u };      /* SMUAD r2, r1, r0 */
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, prog[0]);
        arm_reset(&c, &g_bus);
        c.cpsr = ((c.cpsr & ~0x1fu) | ARM_MODE_SYS) | ARM_CPSR_Q;
        c.r[1] = 0x00010001u; c.r[0] = 0x00010001u;
        CHECK(arm_step(&c) == ARM_OK, "SMUAD refused");
        CHECK((c.cpsr & ARM_CPSR_Q) != 0u, "Q must be sticky");
    }

    /* PC operands are UNPREDICTABLE and must refuse rather than branch. */
    {
        uint32_t prog[] = { 0xe70f3011u };      /* SMLAD pc, r1, r0, r3 */
        CHECK(run_status(&c, prog, 1, 1) == ARM_UNDEFINED,
              "SMLAD with Rd == PC must refuse");
    }
}

static void test_integer_divide_requires_swift(void) {
    /* SDIV/UDIV do not exist on the ARM1176. The first half of this test is the
     * important half: it pins the CURRENT target's behaviour, so adding a second
     * machine profile cannot quietly start executing an instruction that the
     * real hardware under iPhone OS 3 would have trapped. */
    arm_cpu_t c;
    static const struct {
        uint32_t insn, n, m, expect;
        const char *what;
    } CASES[] = {
        /* Rd = r2 (19:16), Rm = r0 (11:8), Rn = r1 (3:0). Bit 21 selects
         * unsigned, so SDIV is 0xe712f011 and UDIV is 0xe732f011. */
        { 0xe732f011u,  7u, 2u, 3u, "UDIV truncates toward zero" },
        { 0xe732f011u, 10u, 5u, 2u, "UDIV exact" },
        { 0xe732f011u,  1u, 0u, 0u, "UDIV by zero yields zero" },
        { 0xe732f011u, UINT32_C(0xffffffff), 2u, UINT32_C(0x7fffffff),
          "UDIV treats operands as unsigned" },
        { 0xe712f011u,  7u, 2u, 3u, "SDIV positive truncates toward zero" },
        /* C99 division truncates toward zero, which is what the architecture
         * specifies -- -7/2 is -3, not -4. */
        { 0xe712f011u, (uint32_t)-7, 2u, (uint32_t)-3,
          "SDIV negative truncates toward zero, not down" },
        { 0xe712f011u, (uint32_t)-7, (uint32_t)-2, 3u, "SDIV both negative" },
        { 0xe712f011u,  1u, 0u, 0u, "SDIV by zero yields zero" },
        /* INT_MIN / -1 overflows; the architecture defines the result as
         * INT_MIN rather than trapping, and C would call it undefined. */
        { 0xe712f011u, UINT32_C(0x80000000), UINT32_C(0xffffffff),
          UINT32_C(0x80000000), "SDIV INT_MIN/-1 wraps to INT_MIN" },
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        /* ARMv6: every one of these must still be refused. */
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, CASES[i].insn);
        arm_reset(&c, &g_bus);
        /* Legacy reset selects ARM1176; keep the intended profile explicit. */
        c.arch = ARM_ARCH_V6_ARM1176;
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
        c.r[1] = CASES[i].n;
        c.r[0] = CASES[i].m;
        CHECK(arm_step(&c) == ARM_UNDEFINED,
              "%s: must be UNDEFINED on the ARM1176", CASES[i].what);

        /* ARMv7: the same encoding executes and produces the defined result. */
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, CASES[i].insn);
        arm_reset(&c, &g_bus);
        c.arch = ARM_ARCH_V7_SWIFT;
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
        c.r[1] = CASES[i].n;
        c.r[0] = CASES[i].m;
        CHECK(arm_step(&c) == ARM_OK, "%s: refused on ARMv7", CASES[i].what);
        CHECK(c.r[2] == CASES[i].expect, "%s: got %08x expected %08x",
              CASES[i].what, c.r[2], CASES[i].expect);
    }

    /* PC operands are UNPREDICTABLE in every position, even on ARMv7. */
    {
        static const struct { uint32_t insn; const char *what; } PCS[] = {
            { 0xe71ff011u, "Rd == PC" },
            { 0xe712f01fu, "Rn == PC" },
            { 0xe712ff11u, "Rm == PC" },
        };
        for (size_t i = 0; i < sizeof PCS / sizeof PCS[0]; i++) {
            memset(g_ram, 0, sizeof g_ram);
            m_w32(NULL, 0, PCS[i].insn);
            arm_reset(&c, &g_bus);
            c.arch = ARM_ARCH_V7_SWIFT;
            c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
            CHECK(arm_step(&c) == ARM_UNDEFINED,
                  "UDIV with %s must refuse", PCS[i].what);
        }
    }
}

static void test_movw_movt_are_armv7_only(void) {
    /* MOVW/MOVT are ARMv6T2; the ARM1176 is ARMv6K and does not have them.
     * ARMv7 compilers build every 32-bit constant this way, so they matter far
     * more than their size suggests. */
    arm_cpu_t c;

    /* MOVW r3, #0xbeef  -> imm4 = 0xb (19:16), imm12 = 0xeef, Rd = r3. */
    const uint32_t movw = 0xe30b3eefu;
    /* MOVT r3, #0xdead  -> imm4 = 0xd, imm12 = 0xead, Rd = r3. */
    const uint32_t movt = 0xe34d3eadu;

    /* ARMv6 must still refuse both. */
    for (int i = 0; i < 2; i++) {
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, i ? movt : movw);
        arm_reset(&c, &g_bus);
        c.arch = ARM_ARCH_V6_ARM1176;
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
        CHECK(arm_step(&c) == ARM_UNDEFINED,
              "%s must be UNDEFINED on the ARM1176", i ? "MOVT" : "MOVW");
    }

    /* ARMv7: MOVW zero-extends, then MOVT fills the top half and must leave the
     * bottom half alone -- the whole point of the pair. */
    {
        uint32_t prog[] = { movw, movt };
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, prog[0]);
        m_w32(NULL, 4, prog[1]);
        arm_reset(&c, &g_bus);
        c.arch = ARM_ARCH_V7_SWIFT;
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
        c.r[3] = 0xffffffffu;          /* prove MOVW zero-extends, not merges */

        CHECK(arm_step(&c) == ARM_OK, "MOVW refused on ARMv7");
        CHECK(c.r[3] == 0x0000beefu, "MOVW gave %08x expected 0000beef", c.r[3]);
        CHECK(arm_step(&c) == ARM_OK, "MOVT refused on ARMv7");
        CHECK(c.r[3] == 0xdeadbeefu, "MOVT gave %08x expected deadbeef", c.r[3]);
    }

    /* Neither form writes flags, and Rd == PC is UNPREDICTABLE. */
    {
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, movw);
        arm_reset(&c, &g_bus);
        c.arch = ARM_ARCH_V7_SWIFT;
        c.cpsr = ((c.cpsr & ~0x1fu) | ARM_MODE_SYS) | ARM_CPSR_Z | ARM_CPSR_C;
        CHECK(arm_step(&c) == ARM_OK, "MOVW refused");
        CHECK((c.cpsr & (ARM_CPSR_Z | ARM_CPSR_C)) == (ARM_CPSR_Z | ARM_CPSR_C),
              "MOVW must not write flags");
    }
    {
        memset(g_ram, 0, sizeof g_ram);
        m_w32(NULL, 0, 0xe30bfeefu);              /* MOVW pc, #0xbeef */
        arm_reset(&c, &g_bus);
        c.arch = ARM_ARCH_V7_SWIFT;
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
        CHECK(arm_step(&c) == ARM_UNDEFINED, "MOVW with Rd == PC must refuse");
    }
}

static void test_srs_and_rfe_stop_after_the_first_fault(void) {
    /* The first frame word is in an unmapped page while the second is a mapped,
     * watched device-like address. Once the first access faults, neither SRS nor
     * RFE may issue the second bus transaction. */
    uint32_t srs[] = { 0xf8cd0513u };             /* SRSIA sp,#SVC */
    arm_cpu_t c;
    split_setup(&c, srs, 1, 0u, 0x60000u, false);
    arm_set_mode(&c, ARM_MODE_IRQ);
    c.bank_r13[ARM_BANK_SVC] = 0x80000ffcu;
    c.r[14] = 0x11223344u;
    c.spsr[ARM_BANK_IRQ] = ARM_MODE_USR;
    c.r[15] = 0;
    g_watch_addr = 0x60000u; g_watch_reads32 = g_watch_writes32 = 0;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "SRS first-word fault must take a data abort");
    CHECK(g_watch_writes32 == 0u,
          "SRS issued %u second-word writes after its first word faulted",
          g_watch_writes32);

    uint32_t rfe[] = { 0xf8900a00u };             /* RFEIA r0 */
    arm_cpu_t d;
    split_setup(&d, rfe, 1, 0u, 0x60000u, false);
    arm_set_mode(&d, ARM_MODE_IRQ);
    d.r[0] = 0x80000ffcu;
    d.r[15] = 0;
    g_watch_addr = 0x60000u; g_watch_reads32 = g_watch_writes32 = 0;
    CHECK(arm_step(&d) == ARM_OK && d.r[15] == ARM_VEC_DATA_ABORT,
          "RFE first-word fault must take a data abort");
    CHECK(g_watch_reads32 == 0u,
          "RFE issued %u second-word reads after its first word faulted",
          g_watch_reads32);
    g_watch_addr = 0xffffffffu;
}

static void test_ldrd_strd_stop_after_the_first_faulting_word(void) {
    /* Same property for the doubleword pair, and the same trap: the second word
     * of an LDRD/STRD lies in the next page as often as not. VA 0x80000ffc is
     * in the absent page; the second word at 0x80001000 is mapped and watched,
     * so a stray access shows up as a bus read/write that must not exist. */
    uint32_t ldrd[] = { 0xe1c200d0u };            /* LDRD r0,r1,[r2] */
    arm_cpu_t c;
    split_setup(&c, ldrd, 1, 0u /* first page absent */, 0x60000u, false);
    c.r[0] = 0xdeadbeefu; c.r[1] = 0xfeedfaceu; c.r[2] = 0x80000ffcu;
    c.r[15] = 0;
    g_watch_addr = 0x60000u; g_watch_reads32 = g_watch_writes32 = 0;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "LDRD first-word fault must take a data abort");
    CHECK(g_watch_reads32 == 0u,
          "LDRD issued %u second-word reads after its first word faulted",
          g_watch_reads32);
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_PAGE_TRANSLATION &&
          (c.cp15.dfsr & (1u << 11)) == 0u && c.cp15.dfar == 0x80000ffcu,
          "LDRD fault state dfsr=%08x dfar=%08x", c.cp15.dfsr, c.cp15.dfar);
    CHECK(c.r[0] == 0xdeadbeefu && c.r[1] == 0xfeedfaceu &&
          c.r[2] == 0x80000ffcu,
          "faulting LDRD moved the pair or the base: %08x/%08x base=%08x",
          c.r[0], c.r[1], c.r[2]);

    /* The mirror: the SECOND word faults. DFAR names it, and the base-restored
     * abort model still forbids committing the half that did arrive. */
    arm_cpu_t d;
    split_setup(&d, ldrd, 1, 0x30000u, 0u /* second page absent */, false);
    m_w32(NULL, 0x30ffcu, 0x11223344u);
    d.r[0] = 0xdeadbeefu; d.r[1] = 0xfeedfaceu; d.r[2] = 0x80000ffcu;
    d.r[15] = 0;
    CHECK(arm_step(&d) == ARM_OK && d.r[15] == ARM_VEC_DATA_ABORT &&
          d.cp15.dfar == 0x80001000u,
          "LDRD second-word fault pc=%08x dfar=%08x", d.r[15], d.cp15.dfar);
    CHECK(d.r[0] == 0xdeadbeefu && d.r[1] == 0xfeedfaceu &&
          d.r[2] == 0x80000ffcu,
          "LDRD committed a half pair across a fault: %08x/%08x base=%08x",
          d.r[0], d.r[1], d.r[2]);

    uint32_t strd[] = { 0xe1c200f0u };            /* STRD r0,r1,[r2] */
    arm_cpu_t e;
    split_setup(&e, strd, 1, 0u, 0x60000u, false);
    e.r[0] = 0x11223344u; e.r[1] = 0x55667788u; e.r[2] = 0x80000ffcu;
    e.r[15] = 0;
    g_watch_addr = 0x60000u; g_watch_reads32 = g_watch_writes32 = 0;
    CHECK(arm_step(&e) == ARM_OK && e.r[15] == ARM_VEC_DATA_ABORT,
          "STRD first-word fault must take a data abort");
    CHECK(g_watch_writes32 == 0u,
          "STRD issued %u second-word writes after its first word faulted",
          g_watch_writes32);
    CHECK((e.cp15.dfsr & 0xfu) == ARM_FSR_PAGE_TRANSLATION &&
          (e.cp15.dfsr & (1u << 11)) != 0u && e.cp15.dfar == 0x80000ffcu,
          "STRD fault state dfsr=%08x dfar=%08x — a store must set WnR",
          e.cp15.dfsr, e.cp15.dfar);
    CHECK(e.r[2] == 0x80000ffcu, "faulting STRD moved its base to %08x", e.r[2]);
    g_watch_addr = 0xffffffffu;
}

static void test_unaligned_access_spanning_two_pages(void) {
    /* VA 0x80000ffe..0x80001001: two bytes in the frame at PA 0x30000 and two
     * in the frame at PA 0x60000. Translating the base once and doing a single
     * 32-bit bus read at 0x30ffe picks up 0x31000/0x31001 for the top half —
     * memory belonging to whatever else happens to sit after that frame, which
     * is why a decoy is planted there: it returned 0x8899bbaa before the fix. */
    uint32_t p[] = { 0xe59f1008 /*LDR r1,[pc,#8] -> literal at 0x10*/,
                     0xe5910000 /*LDR r0,[r1]                      */,
                     0xeafffffe /*B .                              */,
                     0x00000000,
                     0x80000ffeu };
    arm_cpu_t c; split_setup(&c, p, 5, 0x30000u, 0x60000u, false);
    m_w8(NULL, 0x30ffeu, 0xaa); m_w8(NULL, 0x30fffu, 0xbb);
    m_w8(NULL, 0x60000u, 0xcc); m_w8(NULL, 0x60001u, 0xdd);
    m_w8(NULL, 0x31000u, 0x99); m_w8(NULL, 0x31001u, 0x88);  /* the decoy */
    arm_step(&c); arm_step(&c);
    CHECK(c.r[0] == 0xddccbbaau,
          "r0=%08x expect ddccbbaa — the two halves come from PA 0x30ffe and "
          "PA 0x60000, not from one run of bytes at 0x30ffe", c.r[0]);

    /* The storing mirror: each half must land in its own frame, and nothing may
     * be written past the end of the first one. */
    uint32_t q[] = { 0xe59f1008, 0xe5810000 /*STR r0,[r1]*/, 0xeafffffe,
                     0x00000000, 0x80000ffeu };
    arm_cpu_t d; split_setup(&d, q, 5, 0x30000u, 0x60000u, false);
    arm_step(&d);
    d.r[0] = 0x11223344u;
    arm_step(&d);
    CHECK(m_r8(NULL, 0x30ffeu) == 0x44 && m_r8(NULL, 0x30fffu) == 0x33,
          "%02x %02x expect 44 33 in the first frame",
          m_r8(NULL, 0x30ffeu), m_r8(NULL, 0x30fffu));
    CHECK(m_r8(NULL, 0x60000u) == 0x22 && m_r8(NULL, 0x60001u) == 0x11,
          "%02x %02x expect 22 11 in the SECOND frame",
          m_r8(NULL, 0x60000u), m_r8(NULL, 0x60001u));
    CHECK(m_r8(NULL, 0x31000u) == 0 && m_r8(NULL, 0x31001u) == 0,
          "the store must not run past the end of the first physical frame");

    /* A halfword straddling the same boundary, and — the other side of the
     * test — a word ending exactly ON the boundary, which does NOT cross and
     * must still be served by a single whole-word bus access. */
    uint32_t h[] = { 0xe59f1008, 0xe1d100b0 /*LDRH r0,[r1]*/, 0xeafffffe,
                     0x00000000, 0x80000fffu };
    arm_cpu_t e; split_setup(&e, h, 5, 0x30000u, 0x60000u, false);
    m_w8(NULL, 0x30fffu, 0xbb); m_w8(NULL, 0x60000u, 0xcc);
    arm_step(&e); arm_step(&e);
    CHECK(e.r[0] == 0xccbbu, "r0=%08x expect ccbb for a straddling LDRH", e.r[0]);

    uint32_t w[] = { 0xe59f1008, 0xe5910000, 0xeafffffe, 0x00000000, 0x80000ffcu };
    arm_cpu_t g; split_setup(&g, w, 5, 0x30000u, 0x60000u, false);
    m_w32(NULL, 0x30ffcu, 0x12345678u);
    arm_step(&g); arm_step(&g);
    CHECK(g.r[0] == 0x12345678u,
          "r0=%08x expect 12345678 — 0xffc..0xfff is the last word wholly inside "
          "the page and must not be split", g.r[0]);
}

static void test_unaligned_access_faulting_on_the_second_page(void) {
    /* The half that faults is the one that must be reported. sleh_abort page-
     * aligns DFAR and repairs that page, so a fault on the second page reported
     * against the base address maps in the page the access already had and
     * re-executes the same instruction forever. */
    uint32_t p[] = { 0xe59f1008, 0xe5910000 /*LDR r0,[r1]*/, 0xeafffffe,
                     0x00000000, 0x80000ffeu };
    arm_cpu_t c; split_setup(&c, p, 5, 0x30000u, 0u /*second page absent*/, false);
    arm_step(&c);
    c.r[0] = 0xdeadbeefu;
    arm_step(&c);
    CHECK(c.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", c.r[15]);
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_PAGE_TRANSLATION,
          "dfsr=%08x expect a page translation fault", c.cp15.dfsr);
    CHECK((c.cp15.dfsr & (1u << 11)) == 0, "dfsr=%08x: a load must not set WnR",
          c.cp15.dfsr);
    CHECK(c.cp15.dfar == 0x80001000u,
          "dfar=%08x expect 80001000 — the first byte that lies in the SECOND "
          "page, not the base 0x80000ffe", c.cp15.dfar);
    CHECK(c.r[0] == 0xdeadbeefu,
          "r0=%08x: base-restored abort model — the destination must not move",
          c.r[0]);
    CHECK(c.r[1] == 0x80000ffeu, "r1=%08x expect the base unchanged", c.r[1]);

    /* A store that faults on its second page. DFAR and WnR as above — and the
     * bytes that landed in the first page STAY there. Hardware issues the two
     * halves as separate bus transactions and cannot retract one that has
     * completed; the architecture restores the base *register*, not memory.
     * Re-execution after the handler maps the page rewrites the same bytes. */
    uint32_t q[] = { 0xe59f1008, 0xe5810000 /*STR r0,[r1]*/, 0xeafffffe,
                     0x00000000, 0x80000ffeu };
    arm_cpu_t d; split_setup(&d, q, 5, 0x30000u, 0u, false);
    arm_step(&d);
    d.r[0] = 0x11223344u;
    arm_step(&d);
    CHECK(d.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", d.r[15]);
    CHECK(d.cp15.dfar == 0x80001000u,
          "dfar=%08x expect 80001000 for the storing form too", d.cp15.dfar);
    CHECK((d.cp15.dfsr & (1u << 11)) != 0,
          "dfsr=%08x: a store must set WnR", d.cp15.dfsr);
    CHECK(m_r8(NULL, 0x30ffeu) == 0x44 && m_r8(NULL, 0x30fffu) == 0x33,
          "%02x %02x expect 44 33: the first page's bytes are already committed "
          "when the second page aborts", m_r8(NULL, 0x30ffeu), m_r8(NULL, 0x30fffu));
    CHECK(d.r[1] == 0x80000ffeu, "r1=%08x expect the base unchanged", d.r[1]);

    /* And when it is the FIRST page that is missing, the base is the right
     * answer and nothing is written anywhere. */
    arm_cpu_t e; split_setup(&e, q, 5, 0u, 0x60000u, false);
    arm_step(&e);
    e.r[0] = 0x11223344u;
    arm_step(&e);
    CHECK(e.cp15.dfar == 0x80000ffeu,
          "dfar=%08x expect 80000ffe when the first page is the bad one",
          e.cp15.dfar);
    CHECK(m_r8(NULL, 0x60000u) == 0 && m_r8(NULL, 0x60001u) == 0,
          "a fault on the first half must not let the second half through");
}

/*
 * XN — execute never.
 *
 * Confirmed live in xnu-1357.5.30 before being implemented: _nx_enabled
 * (0xc020e1b8) is a __DATA global initialised to 1 that nothing in the image
 * writes, and Thumb _pmap_enter (0xc006056c) builds its small-page template as
 * "pa | 2" but reaches an ORR that makes it "pa | 3" — XN set — whenever the
 * mapping is not VM_PROT_EXECUTE and both _nx_enabled and the per-pmap flag at
 * pmap+0x420 are non-zero. _pmap_disable_NX (0xc005fe44) is five instructions
 * that store 0 to that flag, and it is the only opt-out.
 */
static void test_xn_blocks_fetch_from_a_small_page(void) {
    uint32_t p[] = { 0xe59f0008 /*LDR r0,[pc,#8] -> literal at 0x10*/,
                     0xe1a0f000 /*MOV pc,r0                        */,
                     0xeafffffe /*B .                              */,
                     0x00000000,
                     0x80001000u };

    /* The permitted case first, so the fault below is known to be the XN bit
     * and not the mapping being broken: same page, XN clear, executes. */
    arm_cpu_t c; split_setup(&c, p, 5, 0x30000u, 0x60000u, false);
    m_w32(NULL, 0x60000u, 0xe3a02007u);             /* MOV r2,#7 at the target */
    arm_step(&c); arm_step(&c); arm_step(&c);
    CHECK(c.r[2] == 7, "r2=%u expect 7 — a page without XN must execute", c.r[2]);

    /* The same mapping with XN set. */
    arm_cpu_t d; split_setup(&d, p, 5, 0x30000u, 0x60000u, true);
    m_w32(NULL, 0x60000u, 0xe3a02007u);
    d.cp15.dfsr = 0xdeadbeefu; d.cp15.dfar = 0xcafebabeu;
    arm_step(&d);
    d.r[2] = 0x5a5a5a5au;
    arm_step(&d);
    CHECK(d.r[15] == 0x80001000u, "pc=%08x expect 80001000 before the fetch", d.r[15]);
    arm_step(&d);
    CHECK(d.r[15] == ARM_VEC_PREFETCH, "pc=%08x expect 0c (prefetch abort)", d.r[15]);
    CHECK((d.cp15.ifsr & 0xfu) == ARM_FSR_PAGE_PERMISSION,
          "ifsr=%08x expect a page permission fault — XN is reported as a "
          "permission fault", d.cp15.ifsr);
    CHECK((d.cp15.ifsr & (1u << 11)) == 0,
          "ifsr=%08x: IFSR has no WnR field", d.cp15.ifsr);
    CHECK(d.cp15.ifar == 0x80001000u, "ifar=%08x expect 80001000", d.cp15.ifar);
    CHECK(d.cp15.dfsr == 0xdeadbeefu && d.cp15.dfar == 0xcafebabeu,
          "an XN fault is a prefetch abort and must not touch the data side");
    CHECK(d.r[2] == 0x5a5a5a5au,
          "r2=%08x: the execute-never page must not have executed", d.r[2]);

    /* XN is a fetch-only attribute: the same page stays readable and writable.
     * If it did not, marking the heap non-executable would make it unusable. */
    uint32_t pa = 0;
    CHECK(arm_mmu_translate(&d, 0x80001000u, ARM_ACCESS_READ, true, &pa) == 0
          && pa == 0x60000u,
          "pa=%08x: an XN page must still translate for a read", pa);
    CHECK(arm_mmu_translate(&d, 0x80001000u, ARM_ACCESS_WRITE, true, &pa) == 0,
          "an XN page must still translate for a write");
    CHECK((arm_mmu_translate(&d, 0x80001000u, ARM_ACCESS_FETCH, true, &pa) & 0xfu)
          == ARM_FSR_PAGE_PERMISSION,
          "only ARM_ACCESS_FETCH may see the XN bit");

    /* A manager domain cannot generate a permission fault at all, and an XN
     * violation is a permission fault, so it is not checked there either. */
    d.cp15.dacr = 3u;
    CHECK(arm_mmu_translate(&d, 0x80001000u, ARM_ACCESS_FETCH, true, &pa) == 0,
          "a manager domain must not raise an XN fault");
}

static void test_xn_on_a_section_and_the_xp_gate(void) {
    /* Sections carry XN in bit 4. The section planted at index 0x800 maps
     * 0x80000000 onto physical 0, so 0x80000008 is the MOV r2,#7 below. */
    uint32_t p[] = { 0xe59f0008 /*LDR r0,[pc,#8]*/, 0xe1a0f000 /*MOV pc,r0*/,
                     0xe3a02007 /*MOV r2,#7     */, 0xeafffffe /*B .      */,
                     0x80000008u };
    struct { bool xn, xp; } cases[] = { {false, true}, {true, true}, {true, false} };
    for (unsigned k = 0; k < 3; k++) {
        arm_cpu_t c;
        memset(g_ram, 0, sizeof g_ram);
        for (unsigned i = 0; i < 5; i++) m_w32(NULL, i * 4, p[i]);
        m_w32(NULL, 0x4000 + (0x000u << 2), (3u << 10) | 2u);
        m_w32(NULL, 0x4000 + (0x800u << 2),
              (3u << 10) | 2u | (cases[k].xn ? (1u << 4) : 0u));
        arm_reset(&c, &g_bus);
        c.cp15.ttbr0  = 0x4000;
        c.cp15.dacr   = 1u;
        c.cp15.sctlr |= ARM_SCTLR_M | (cases[k].xp ? ARM_SCTLR_XP : 0u);
        c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
        c.r[15] = 0;
        c.r[2] = 0x5a5a5a5au;
        arm_step(&c); arm_step(&c); arm_step(&c);

        if (cases[k].xn && cases[k].xp) {
            CHECK(c.r[15] == ARM_VEC_PREFETCH,
                  "pc=%08x expect 0c: an XN section must abort on fetch", c.r[15]);
            CHECK((c.cp15.ifsr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
                  "ifsr=%08x expect a SECTION permission fault", c.cp15.ifsr);
            CHECK(c.cp15.ifar == 0x80000008u, "ifar=%08x expect 80000008",
                  c.cp15.ifar);
        } else {
            /* xn && !xp is the inert case: with SCTLR.XP clear the descriptors
             * are the ARMv5-compatible layout, where bit 4 is not XN at all, so
             * reading it as XN would invent a fault the hardware never takes. */
            CHECK(c.r[2] == 7,
                  "r2=%08x expect 7 (xn=%d xp=%d): the fetch must be permitted",
                  c.r[2], (int)cases[k].xn, (int)cases[k].xp);
        }
    }
}

static void test_direct_write_cache_requires_explicit_consent(void) {
    static const uint32_t prog[] = {
        0xe5801000u, /* STR r1,[r0] */
        0xe5801000u,
        0xe5801000u,
        0xe5801000u,
        0xe5801000u,
    };
    arm_bus_t bus = g_bus;
    arm_cpu_t c;
    uint32_t value = 0u;

    memset(g_ram, 0, sizeof g_ram);
    for (unsigned i = 0u; i < sizeof prog / sizeof prog[0]; i++)
        m_w32(NULL, i * 4u, prog[i]);
    bus.host_ram_write = m_host_ram_write;
    arm_reset(&c, &bus);
    c.cpsr = (c.cpsr & ~UINT32_C(0x1f)) | ARM_MODE_SYS;
    c.r[0] = UINT32_C(0x8000);
    c.r[1] = UINT32_C(0x11223344);
    g_watch_addr = c.r[0];
    g_watch_writes32 = 0u;

    CHECK(arm_step(&c) == ARM_OK && arm_step(&c) == ARM_OK,
          "consented direct stores execute");
    memcpy(&value, &g_ram[c.r[0]], sizeof value);
    CHECK(value == c.r[1], "direct-store value=%08x", value);
    CHECK(g_watch_writes32 == 1u,
          "first store fills through bus, second bypasses it: writes=%u",
          g_watch_writes32);
    CHECK(c.dwrite_misses == 1u && c.dwrite_hits == 1u,
          "direct-write counters miss/hit=%llu/%llu",
          (unsigned long long)c.dwrite_misses,
          (unsigned long long)c.dwrite_hits);

    /* Revoking the callback must defeat an already-filled pointer immediately,
     * without depending on a TLB flush or cache clear. */
    bus.host_ram_write = NULL;
    c.r[1] = UINT32_C(0x55667788);
    CHECK(arm_step(&c) == ARM_OK && g_watch_writes32 == 2u,
          "revoked consent returned to the bus, writes=%u",
          g_watch_writes32);

    /* Re-enable and change the translation generation. The old host pointer is
     * still present but must miss on generation before the following hit. */
    bus.host_ram_write = m_host_ram_write;
    arm_mmu_tlb_flush(&c);
    c.r[1] = UINT32_C(0xaabbccdd);
    CHECK(arm_step(&c) == ARM_OK && arm_step(&c) == ARM_OK,
          "generation-refilled direct stores execute");
    CHECK(g_watch_writes32 == 3u,
          "generation miss used the bus exactly once, writes=%u",
          g_watch_writes32);
    CHECK(c.dwrite_misses == 2u && c.dwrite_hits == 2u,
          "post-flush miss/hit=%llu/%llu",
          (unsigned long long)c.dwrite_misses,
          (unsigned long long)c.dwrite_hits);

    arm_reset(&c, &bus);
    bool empty = true;
    for (unsigned i = 0u; i < ARM_DREAD_ENTRIES; i++)
        if (c.dwrite[i].host) empty = false;
    CHECK(empty, "reset clears every process-local DWRITE pointer");
    g_watch_addr = UINT32_MAX;
}

static void test_reset_initializes_the_default_profile(void) {
    arm_cpu_t c;
    memset(&c, 0xa5, sizeof c);
    arm_reset(&c, &g_bus);
    CHECK(c.arch == ARM_ARCH_V6_ARM1176,
          "reset left the CPU profile uninitialized");
    CHECK(c.r[15] == 0u && c.bus == &g_bus,
          "default reset lost its PC or bus");
    CHECK(c.a8_l2actlr == 0u, "default reset left inactive Cortex-A8 state");
}

static void test_explicit_profile_reset_and_invalid_configuration(void) {
    static const arm_arch_t profiles[] = {
        ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_SWIFT, ARM_ARCH_V7_CORTEX_A8
    };
    arm_cpu_t c, before;
    for (size_t i = 0; i < sizeof profiles / sizeof profiles[0]; i++) {
        memset(&c, 0xa5, sizeof c);
        CHECK(arm_reset_profile(&c, &g_bus, profiles[i]), "valid reset refused");
        CHECK(c.arch == profiles[i] && c.bus == &g_bus && c.r[15] == 0u &&
              c.cp15.sctlr == (profiles[i] == ARM_ARCH_V7_CORTEX_A8 ? 0x00c50078u : 0u) &&
              c.cp15.actlr == (profiles[i] == ARM_ARCH_V7_CORTEX_A8 ? 2u : 0u) &&
              c.tlb_gen == 1u && !c.excl_valid &&
              c.vfp_fpexc == 0u && c.cycles == 0u,
              "explicit profile reset left stale state");
        CHECK(c.a8_l2actlr == (profiles[i] == ARM_ARCH_V7_CORTEX_A8 ? 0x42u : 0u),
              "explicit reset lost profile-specific L2 reset state");
        c.r[3] = 0xabcdef01u;
        c.a8_l2actlr = 0x02000000u;
        c.cp15.actlr = UINT32_MAX;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[i]) &&
              c.arch == profiles[i] && c.r[3] == 0u &&
              c.cp15.actlr == (profiles[i] == ARM_ARCH_V7_CORTEX_A8 ? 2u : 0u) &&
              c.a8_l2actlr == (profiles[i] == ARM_ARCH_V7_CORTEX_A8 ? 0x42u : 0u),
              "repeated explicit reset lost profile or register reset");
    }
    memcpy(&before, &c, sizeof c);
    CHECK(!arm_reset_profile(&c, &g_bus, (arm_arch_t)99),
          "unknown CPU profile accepted");
    CHECK(memcmp(&before, &c, sizeof c) == 0,
          "invalid reset modified the CPU");
    CHECK(!arm_reset_profile(NULL, &g_bus, ARM_ARCH_V7_CORTEX_A8),
          "null CPU accepted");

    /* An invalid configuration must stop before fetch or IRQ entry, even
     * when the pending instruction would be common to every valid core. */
    c.arch = (arm_arch_t)99;
    c.irq_line = true;
    c.cpsr &= ~ARM_CPSR_I;
    memcpy(&before, &c, sizeof c);
    m_w32(NULL, 0, 0xe3a0002au); /* MOV r0,#42 */
    g_watch_addr = 0u; g_watch_reads32 = 0;
    CHECK(arm_step(&c) == ARM_UNDEFINED && g_watch_reads32 == 0u &&
          memcmp(&before, &c, sizeof c) == 0,
          "invalid profile fetched or changed CPU state");
    g_watch_addr = 0xffffffffu;
    arm_reset(&c, &g_bus);
    CHECK(c.arch == ARM_ARCH_V6_ARM1176,
          "legacy reset inherited a previous profile");
}

static void test_profile_instruction_boundaries(void) {
    static const arm_arch_t profiles[] = {
        ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_SWIFT, ARM_ARCH_V7_CORTEX_A8,
        (arm_arch_t)-1, (arm_arch_t)3, (arm_arch_t)0x7fffffff
    };
    static const uint32_t encodings[] = {
        0xe30b3eefu, 0xe34d3eadu, 0xe713f011u, 0xe733f011u
    }; /* MOVW r3,#beef; MOVT r3,#dead; SDIV/UDIV r3,r1,r0 */
    for (size_t p = 0; p < sizeof profiles / sizeof profiles[0]; p++) {
        for (size_t i = 0; i < sizeof encodings / sizeof encodings[0]; i++) {
            arm_cpu_t c;
            arm_reset(&c, &g_bus);
            c.arch = profiles[p];
            c.r[0] = 2u; c.r[1] = 7u; c.r[3] = 0x11223344u;
            c.cpsr |= ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q;
            uint32_t cpsr = c.cpsr;
            m_w32(NULL, 0, encodings[i]);
            bool supported = profiles[p] == ARM_ARCH_V7_SWIFT ||
                (i < 2 && profiles[p] == ARM_ARCH_V7_CORTEX_A8);
            arm_status_t status = arm_step(&c);
            CHECK(status == (supported ? ARM_OK : ARM_UNDEFINED),
                  "profile %d instruction %08x returned %d",
                  (int)profiles[p], encodings[i], status);
            uint32_t result = i == 0 ? 0x0000beefu :
                              i == 1 ? 0xdead3344u : 3u;
            CHECK(c.r[3] == (supported ? result : 0x11223344u),
                  "profile %d instruction %08x wrote the wrong result",
                  (int)profiles[p], encodings[i]);
            CHECK(c.cpsr == cpsr && c.r[15] == (supported ? 4u : 0u),
                  "profile boundary changed flags or advanced a rejected PC");
        }
    }
}

static void test_a32_barrier_profile_boundaries(void) {
    static const arm_arch_t profiles[] = {
        ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_SWIFT, ARM_ARCH_V7_CORTEX_A8,
        (arm_arch_t)99
    };
    static const uint32_t barriers[] = {0xf57ff040u, 0xf57ff050u, 0xf57ff060u};
    for (unsigned p = 0; p < sizeof profiles / sizeof profiles[0]; p++) {
        for (unsigned b = 0; b < sizeof barriers / sizeof barriers[0]; b++) {
            /* DDI0406C.b requires reserved options to execute as SY too. */
            for (unsigned option = 0; option < 16; option++) {
                for (unsigned user = 0; user < 2; user++) {
                    arm_cpu_t c;
                    arm_reset(&c, &g_bus);
                    c.arch = profiles[p];
                    c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) |
                             ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q;
                    c.r[0] = 0x12345678u;
                    c.excl_valid = true;
                    c.excl_addr = 0x200u;
                    uint32_t cpsr = c.cpsr;
                    m_w32(NULL, 0, barriers[b] | option);
                    bool supported = profiles[p] == ARM_ARCH_V7_SWIFT ||
                                     profiles[p] == ARM_ARCH_V7_CORTEX_A8;
                    arm_status_t status = arm_step(&c);
                    CHECK(status == (supported ? ARM_OK : ARM_UNDEFINED) &&
                          c.r[15] == (supported ? 4u : 0u),
                          "profile %u accepted/refused barrier %08x incorrectly",
                          (unsigned)profiles[p], barriers[b] | option);
                    CHECK(c.r[0] == 0x12345678u && c.cpsr == cpsr &&
                          c.excl_valid && c.excl_addr == 0x200u && c.tlb_gen == 1u,
                          "barrier changed flags/registers/exclusive monitor/TLB");
                }
            }
        }
    }
}

static void test_a32_barriers_observe_completed_stores(void) {
    const uint32_t program[] = {
        0xe5801000u, /* STR r1,[r0] replaces the upcoming instruction */
        0xf57ff05fu, /* DMB SY */
        0xf57ff04fu, /* DSB SY */
        0xf57ff06fu, /* ISB SY */
        0xe3a04000u  /* MOV r4,#0 before the store */
    };
    for (unsigned host = 0; host < 2; host++) {
        arm_bus_t bus = g_bus;
        if (host) bus.host_ram = m_host_ram;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        memcpy(g_ram, program, sizeof program);
        c.r[0] = 16u; c.r[1] = 0xe3a0402au; /* MOV r4,#42 */
        g_watch_addr = 16u;
        g_watch_writes32 = 0u;
        for (unsigned step = 0; step < 5; step++) {
            CHECK(arm_step(&c) == ARM_OK, "barrier program stopped at %u", step);
            CHECK(g_watch_writes32 == 1u && m_r32(NULL, 16u) == 0xe3a0402au,
                  "preceding store had not completed across the barrier");
        }
        CHECK(c.r[4] == 42u && c.r[15] == 20u && c.cycles == 5u,
              "ISB did not fetch the updated instruction, host=%u", host);
        g_watch_addr = 0xffffffffu;
    }
}

static void test_thumb2_movw_movt_and_unknown_width(void) {
    static const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    static const uint16_t values[] = {
        0, 1, 0x80, 0x100, 0x400, 0x800, 0x1000, 0x8000, 0x9e64, 0xffff
    };
    for (unsigned p = 0; p < sizeof profiles / sizeof profiles[0]; p++) {
        for (unsigned top = 0; top < 2; top++) {
            for (unsigned rd = 0; rd < 16; rd++) {
                for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
                    uint32_t imm = values[i];
                    uint16_t first = (uint16_t)((top ? 0xf2c0u : 0xf240u) |
                                               (imm >> 12) | ((imm & 0x800u) >> 1));
                    uint16_t second = (uint16_t)(((imm & 0x700u) << 4) |
                                                 (rd << 8) | (imm & 0xffu));
                    arm_cpu_t c;
                    CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
                    c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q;
                    c.r[rd] = 0x11223344u;
                    c.r[15] = 0;
                    uint32_t before[16]; memcpy(before, c.r, sizeof before);
                    uint32_t cpsr = c.cpsr;
                    m_w16(NULL, 0, first); m_w16(NULL, 2, second);
                    bool valid = rd != 13 && rd != 15;
                    CHECK(arm_step(&c) == (valid ? ARM_OK : ARM_UNDEFINED),
                          "Thumb MOV%s r%u immediate %04x status", top ? "T" : "W", rd, imm);
                    if (valid) {
                        before[rd] = top ? (imm << 16) | 0x3344u : imm;
                        before[15] = 4;
                    }
                    CHECK(memcmp(before, c.r, sizeof before) == 0 &&
                          c.cpsr == cpsr && c.cycles == 1u,
                          "Thumb MOVW/MOVT was split or changed other state");
                }
            }
        }
        const uint16_t unknown[][2] = {
            {0xf380u, 0x8000u}, /* unsupported wide system register operation */
            {0xe800u, 0xffffu}, {0xf800u, 0xffffu},
            {0xf000u, 0xc001u}  /* BLX with reserved H bit */
        };
        for (unsigned i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
            c.cpsr |= ARM_CPSR_T;
            c.r[14] = 0x12345679u;
            m_w16(NULL, 0, unknown[i][0]); m_w16(NULL, 2, unknown[i][1]);
            CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u &&
                  c.r[14] == 0x12345679u && (c.cpsr & ARM_CPSR_T),
                  "unknown wide instruction aliased a legacy branch half");
        }
    }

    arm_cpu_t legacy;
    arm_reset(&legacy, &g_bus);
    legacy.cpsr |= ARM_CPSR_T;
    m_w16(NULL, 0, 0xf000u); m_w16(NULL, 2, 0xf802u);
    CHECK(arm_step(&legacy) == ARM_OK && legacy.r[15] == 2u && legacy.r[14] == 4u,
          "ARM1176 legacy BL prefix changed");
    CHECK(arm_step(&legacy) == ARM_OK && legacy.r[15] == 8u && legacy.r[14] == 5u,
          "ARM1176 legacy BL suffix changed");
}

static void test_thumb2_fetch_across_page_boundary(void) {
    for (unsigned host = 0; host < 2; host++) {
      for (unsigned call = 0; call < 2; call++) {
        for (unsigned fault = 0; fault < 4; fault++) {
            memset(g_ram, 0, sizeof g_ram);
            arm_bus_t bus = g_bus;
            if (host) bus.host_ram = m_host_ram;
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
            c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
            c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N;
            c.r[15] = 0xffeu; c.r[4] = 0x12345678u; c.r[14] = 0x777u;
            uint32_t cpsr = c.cpsr;
            m_w32(NULL, 0x4000u, 0x6001u); /* coarse table */
            m_w32(NULL, 0x6000u, 0x8032u); /* user executable page 0 -> PA 0x8000 */
            m_w32(NULL, 0x6004u, fault == 1 ? 0u : fault == 2 ? 0xa033u :
                                      fault == 3 ? 0xa012u : 0xa032u);
            m_w16(NULL, 0x8ffeu, call ? 0xf000u : 0xf649u);
            m_w16(NULL, 0xa000u, call ? 0xf802u : 0x6464u); /* BL +4 or MOVW r4,#9e64 */
            m_w16(NULL, 0x9000u, 0x0400u); /* wrong physical-contiguity assumption */
            CHECK(arm_step(&c) == ARM_OK, "fetch should retire or vector an abort");
            if (!fault) {
                CHECK(c.r[4] == (call ? 0x12345678u : 0x9e64u) &&
                      c.r[15] == (call ? 0x1006u : 0x1002u) &&
                      c.r[14] == (call ? 0x1003u : 0x777u) && c.cpsr == cpsr,
                      "wide fetch did not translate its second halfword");
            } else {
                CHECK(c.r[4] == 0x12345678u && c.r[15] == ARM_VEC_PREFETCH &&
                      c.r[14] == 0x1002u && c.cp15.ifar == 0x1000u &&
                      (c.cp15.ifsr & 15u) == (fault == 1 ? ARM_FSR_PAGE_TRANSLATION :
                                                         ARM_FSR_PAGE_PERMISSION),
                      "second-halfword fault changed result or lost fault address");
                CHECK(c.bank_r14[arm_bank_of_mode(ARM_MODE_USR)] == 0x777u,
                      "aborted wide instruction modified User LR");
            }
            CHECK(c.cycles == 1u, "wide instruction charged multiple steps");
        }
      }
    }
}

static void test_thumb2_modified_immediate_moves(void) {
    /* Independent expected constants cover every replication form and the
     * edges of the rotated form in DDI0406C.b table A6-11. */
    static const struct { uint16_t imm12; uint32_t value; } cases[] = {
        {0x000u, 0u}, {0x080u, 0x80u}, {0x0ffu, 0xffu},
        {0x112u, 0x00120012u}, {0x234u, 0x34003400u},
        {0x3abu, 0xababababu}, {0x3ffu, 0xffffffffu},
        {0x400u, 0x80000000u}, {0x47fu, 0xff000000u},
        {0x480u, 0x40000000u}, {0x7ffu, 0x01fe0000u},
        {0x800u, 0x00800000u}, {0xf80u, 0x100u}, {0xfffu, 0x1feu}
    };
    for (unsigned flag = 0; flag < 2; flag++) {
        for (unsigned carry = 0; carry < 2; carry++) {
            for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
                arm_cpu_t c;
                CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
                c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_V | ARM_CPSR_Q;
                if (carry) c.cpsr |= ARM_CPSR_C;
                uint32_t flags = c.cpsr;
                c.r[1] = 0x12345678u;
                c.r[14] = 0x777u;
                uint16_t imm = cases[i].imm12;
                m_w16(NULL, 0, (uint16_t)(0xf04fu | (flag << 4) | ((imm & 0x800u) >> 1)));
                m_w16(NULL, 2, (uint16_t)(0x100u | ((imm & 0x700u) << 4) | (imm & 0xffu)));
                if (flag) {
                    flags &= ~(ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C);
                    if (cases[i].value & 0x80000000u) flags |= ARM_CPSR_N;
                    if (!cases[i].value) flags |= ARM_CPSR_Z;
                    if (imm < 0x400u ? carry : cases[i].value >> 31) flags |= ARM_CPSR_C;
                }
                CHECK(arm_step(&c) == ARM_OK && c.r[1] == cases[i].value &&
                      c.r[14] == 0x777u && c.r[15] == 4u && c.cycles == 1u && c.cpsr == flags,
                      "MOV%s.W immediate %03x, carry %u: result=%08x flags=%08x",
                      flag ? "S" : "", imm, carry, c.r[1], c.cpsr);
            }
        }
    }
    for (unsigned bad = 0; bad < 5; bad++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_C;
        uint32_t flags = c.cpsr;
        c.r[1] = 0x12345678u; c.r[13] = 0x888u;
        uint16_t second = bad < 3 ? (uint16_t)(((bad + 1u) << 12) | 0x100u) :
                                   bad == 3 ? 0x0d01u : 0x0f01u;
        m_w16(NULL, 0, 0xf05fu); m_w16(NULL, 2, second);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[1] == 0x12345678u &&
              c.r[13] == 0x888u && c.r[15] == 0u && c.cpsr == flags,
              "unpredictable zero replication or SP/PC destination accepted");
    }
}

static void test_armv7_has_no_legacy_unaligned_mode(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_CORTEX_A8,
                                  ARM_ARCH_V7_SWIFT};
    for (unsigned p = 0; p < 3; p++) {
        for (unsigned u = 0; u < 2; u++) {
            for (unsigned a = 0; a < 2; a++) {
                arm_cpu_t c;
                CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
                c.cp15.sctlr = (u ? ARM_SCTLR_U : 0u) | (a ? ARM_SCTLR_A : 0u);
                c.r[0] = 0x101u; c.r[1] = 0x44332211u;
                memset(g_ram + 0x100u, 0xee, 8);
                m_w32(NULL, 0, 0xe5801000u); /* STR r1,[r0] */
                m_w32(NULL, 4, 0xe5903000u); /* LDR r3,[r0] */
                CHECK(arm_step(&c) == ARM_OK, "word store status");
                if (a) {
                    CHECK(c.r[15] == ARM_VEC_DATA_ABORT &&
                          c.cp15.dfar == 0x101u && c.cp15.dfsr == (ARM_FSR_ALIGNMENT | (1u << 11)) &&
                          m_r32(NULL, 0x100u) == 0xeeeeeeeeu,
                          "alignment checking failed before a store");
                } else {
                    bool modern = u || profiles[p] != ARM_ARCH_V6_ARM1176;
                    CHECK(g_ram[modern ? 0x101u : 0x100u] == 0x11u &&
                          g_ram[modern ? 0x104u : 0x103u] == 0x44u &&
                          g_ram[modern ? 0x100u : 0x104u] == 0xeeu,
                          "profile %u inherited the wrong unaligned store mode", p);
                    CHECK(arm_step(&c) == ARM_OK &&
                          c.r[3] == (modern ? 0x44332211u : 0x11443322u),
                          "profile %u inherited the wrong unaligned load mode", p);
                    m_w32(NULL, 8, 0xe1d030b0u); /* LDRH r3,[r0] */
                    CHECK(arm_step(&c) == (modern ? ARM_OK : ARM_UNDEFINED) &&
                          (!modern || c.r[3] == 0x2211u),
                          "odd halfword was incorrectly treated as ARM1176 legacy");
                }
            }
        }
    }
}

static void test_thumb2_str_immediate(void) {
    const uint16_t offsets[] = {0u, 0x224u, 0xfffu};
    for (unsigned host = 0; host < 2; host++) {
        for (unsigned i = 0; i < 3; i++) {
            arm_bus_t bus = g_bus;
            if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C;
            c.r[4] = 0x10000u; c.r[2] = 0x44332211u;
            uint32_t cpsr = c.cpsr;
            memset(g_ram + 0x10000u, 0xee, 0x1008u);
            m_w16(NULL, 0, 0xf8c4u);
            m_w16(NULL, 2, (uint16_t)(0x2000u | offsets[i]));
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
                  c.r[4] == 0x10000u && c.r[2] == 0x44332211u && c.cpsr == cpsr,
                  "STR.W did not retire without writeback or flag changes");
            CHECK(m_r32(NULL, 0x10000u + offsets[i]) == 0x44332211u &&
                  g_ram[0x10004u + offsets[i]] == 0xeeu,
                  "STR.W used the wrong immediate or unaligned address");
        }
        arm_bus_t bus = g_bus;
        if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T;
        c.r[13] = 0x100u; c.r[14] = 0xfeedfaceu;
        m_w16(NULL, 0, 0xf8cdu); m_w16(NULL, 2, 0xe000u); /* STR.W lr,[sp] */
        m_w16(NULL, 4, 0xf8cdu); m_w16(NULL, 6, 0xd004u); /* STR.W sp,[sp,#4] */
        CHECK(arm_step(&c) == ARM_OK && arm_step(&c) == ARM_OK &&
              m_r32(NULL, 0x100u) == 0xfeedfaceu && m_r32(NULL, 0x104u) == 0x100u &&
              c.r[13] == 0x100u, "SP/LR store registers or base alias mishandled");
    }
    for (unsigned bad = 0; bad < 2; bad++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T;
        c.r[4] = 0x100u;
        m_w16(NULL, 0, bad ? 0xf8cfu : 0xf8c4u);
        m_w16(NULL, 2, bad ? 0x2000u : 0xf000u);
        uint32_t instruction = m_r32(NULL, 0);
        m_w32(NULL, 0x100u, 0xeeeeeeeeu);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u &&
              m_r32(NULL, 0) == instruction && m_r32(NULL, 0x100u) == 0xeeeeeeeeu,
              "PC base/source accepted by STR.W");
    }
}

static void test_thumb2_str_page_crossing_and_aborts(void) {
    for (unsigned host = 0; host < 2; host++) {
        /* Success, first/second page absent, second page denied, alignment. */
        for (unsigned fault = 0; fault < 5; fault++) {
            memset(g_ram, 0xee, sizeof g_ram);
            arm_bus_t bus = g_bus;
            if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP |
                          (fault == 4 ? ARM_SCTLR_A : 0u); /* raw U=0 */
            c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
            c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C;
            c.r[4] = 0x1ffeu; c.r[2] = 0x44332211u; c.r[14] = 0x777u;
            uint32_t cpsr = c.cpsr;
            m_w32(NULL, 0x4000u, 0x6001u);
            m_w32(NULL, 0x6000u, 0x803eu); /* executable, user RW, normal memory */
            m_w32(NULL, 0x6004u, fault == 1 ? 0u : 0xa03eu);
            m_w32(NULL, 0x6008u, fault == 2 ? 0u : fault == 3 ? 0xc01eu : 0xc03eu);
            m_w16(NULL, 0x8000u, 0xf8c4u); m_w16(NULL, 0x8002u, 0x2000u);
            CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u &&
                  c.r[4] == 0x1ffeu && c.r[2] == 0x44332211u,
                  "wide store changed its operands or retirement count");
            if (!fault) {
                CHECK(c.r[15] == 4u && c.cpsr == cpsr &&
                      m_r16(NULL, 0xaffeu) == 0x2211u && m_r16(NULL, 0xc000u) == 0x4433u,
                      "wide store did not independently translate both pages");
            } else {
                uint32_t far = (fault == 1 || fault == 4) ? 0x1ffeu : 0x2000u;
                uint32_t fsr = fault == 4 ? ARM_FSR_ALIGNMENT : fault == 3 ?
                               ARM_FSR_PAGE_PERMISSION : ARM_FSR_PAGE_TRANSLATION;
                CHECK(c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 8u &&
                      c.cp15.dfar == far && c.cp15.dfsr == (fsr | (1u << 11)) &&
                      c.spsr[ARM_BANK_ABT] == cpsr && c.bank_r14[ARM_BANK_USR] == 0x777u,
                      "wide store abort lost fault VA, WnR, first-instruction LR or saved state");
                CHECK(m_r16(NULL, 0xaffeu) == ((fault == 2 || fault == 3) ? 0x2211u : 0xeeeeu) &&
                      m_r16(NULL, 0xc000u) == 0xeeeeu,
                      "store abort committed the wrong bytes");
            }
            CHECK(m_r16(NULL, 0xaffcu) == 0xeeeeu && m_r32(NULL, 0xb000u) == 0xeeeeeeeeu &&
                  m_r16(NULL, 0xc002u) == 0xeeeeu, "store crossed physical bounds");
        }
    }
}

static void test_armv7_multiword_and_sync_alignment(void) {
    static const struct { uint32_t insn; bool write; } cases[] = {
        {0xe8b00006u, false}, /* LDMIA r0!,{r1,r2} */
        {0xe8a00006u, true},  /* STMIA r0!,{r1,r2} */
        {0xe1901f9fu, false}, /* LDREX r1,[r0] */
        {0xe1802f91u, true},  /* STREX r2,r1,[r0] */
        {0xe1002091u, true},  /* SWP r2,r1,[r0] */
    };
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    for (unsigned p = 0; p < 2; p++) {
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
            c.cp15.sctlr = 0u; /* Neither U nor A permits legacy behavior on v7. */
            c.r[0] = 0x101u; c.r[1] = 0x12345678u; c.r[2] = 0x87654321u;
            m_w32(NULL, 0, cases[i].insn);
            memset(g_ram + 0x100u, 0xee, 12);
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT &&
                  c.cp15.dfar == 0x101u &&
                  c.cp15.dfsr == (ARM_FSR_ALIGNMENT | (cases[i].write ? (1u << 11) : 0u)),
                  "v7 multiword/exclusive/SWP access did not alignment-fault");
            CHECK(c.r[0] == 0x101u && c.r[1] == 0x12345678u && c.r[2] == 0x87654321u &&
                  m_r32(NULL, 0x100u) == 0xeeeeeeeeu && m_r32(NULL, 0x104u) == 0xeeeeeeeeu,
                  "faulting multiword/sync access changed registers or memory");
        }
    }
}

static void test_thumb2_modified_immediate_arithmetic(void) {
    static const struct {
        uint16_t first, second;
        uint32_t a, cin, result, nzcv;
    } cases[] = {
        {0xf504u, 0x7090u, 0x803020e0u, 0u, 0x80302200u, 0u}, /* real ADD.W */
        {0xf114u, 0x0301u, 0xffffffffu, 0u, 0u, ARM_CPSR_Z | ARM_CPSR_C},
        {0xf114u, 0x0301u, 0x7fffffffu, 0u, 0x80000000u, ARM_CPSR_N | ARM_CPSR_V},
        {0xf154u, 0x0300u, 0xffffffffu, ARM_CPSR_C, 0u, ARM_CPSR_Z | ARM_CPSR_C},
        {0xf154u, 0x0300u, 0x7fffffffu, ARM_CPSR_C, 0x80000000u, ARM_CPSR_N | ARM_CPSR_V},
        {0xf154u, 0x0300u, 0x7fffffffu, 0u, 0x7fffffffu, 0u},
        {0xf1b4u, 0x0301u, 0u, ARM_CPSR_C, 0xffffffffu, ARM_CPSR_N},
        {0xf1b4u, 0x0301u, 0x80000000u, 0u, 0x7fffffffu, ARM_CPSR_C | ARM_CPSR_V},
        {0xf1b4u, 0x0301u, 1u, 0u, 0u, ARM_CPSR_Z | ARM_CPSR_C},
        {0xf174u, 0x0300u, 0u, 0u, 0xffffffffu, ARM_CPSR_N},
        {0xf174u, 0x0300u, 0u, ARM_CPSR_C, 0u, ARM_CPSR_Z | ARM_CPSR_C},
        {0xf174u, 0x0300u, 0x80000000u, 0u, 0x7fffffffu, ARM_CPSR_C | ARM_CPSR_V},
        {0xf1d4u, 0x0300u, 1u, 0u, 0xffffffffu, ARM_CPSR_N},
        {0xf1d4u, 0x0300u, 0x80000000u, ARM_CPSR_C, 0x80000000u, ARM_CPSR_N | ARM_CPSR_V},
        {0xf1d4u, 0x4300u, 0xffffffffu, 0u, 0x80000001u, ARM_CPSR_N}, /* 0x80000000 - Rn */
        {0xf514u, 0x7380u, 0xffffffffu, 0u, 0xffu, ARM_CPSR_C}, /* rotated 0x100 */
        {0xf114u, 0x33ffu, 1u, 0u, 0u, ARM_CPSR_Z | ARM_CPSR_C}, /* replicated -1 */
    };
    const uint32_t flags = ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        for (unsigned set = 0; set < 2; set++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_Q | ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_V | cases[i].cin;
            c.r[4] = cases[i].a;
            uint32_t before = c.cpsr;
            m_w16(NULL, 0, (uint16_t)((cases[i].first & ~0x10u) | (set ? 0x10u : 0u)));
            m_w16(NULL, 2, cases[i].second);
            unsigned rd = (cases[i].second >> 8) & 15u;
            CHECK(arm_step(&c) == ARM_OK && c.r[rd] == cases[i].result &&
                  c.r[4] == cases[i].a && c.r[15] == 4u && c.cycles == 1u,
                  "modified arithmetic case %u set=%u result=%08x", i, set, c.r[rd]);
            uint32_t expected_flags = i == 0 ? ARM_CPSR_N : cases[i].nzcv;
            CHECK(c.cpsr == (set ? ((before & ~flags) | expected_flags) : before),
                  "modified arithmetic case %u set=%u flags=%08x", i, set, c.cpsr);
        }
    }
    /* SP forms and compare aliases have different register restrictions. */
    for (unsigned subtract = 0; subtract < 2; subtract++) {
        for (unsigned rd_index = 0; rd_index < 3; rd_index++) {
            const unsigned rd_list[] = {13u, 14u, 15u};
            unsigned rd = rd_list[rd_index];
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr |= ARM_CPSR_T;
            c.r[13] = 0x100u;
            m_w16(NULL, 0, subtract ? 0xf1bdu : 0xf11du);
            m_w16(NULL, 2, (uint16_t)((rd << 8) | 1u));
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u &&
                  (rd == 15u || c.r[rd] == (subtract ? 0xffu : 0x101u)) &&
                  (rd == 13u || c.r[13] == 0x100u) &&
                  (c.cpsr & flags) == (subtract ? ARM_CPSR_C : 0u),
                  "SP arithmetic/compare alias changed PC or used the wrong source");
        }
    }
    static const uint16_t invalid[][2] = {
        {0xf104u, 0x0d01u}, {0xf10fu, 0x0301u}, {0xf104u, 0x0f01u},
        {0xf1a4u, 0x0d01u}, {0xf1bfu, 0x0f01u},
        {0xf15du, 0x0301u}, {0xf154u, 0x0f01u}, {0xf154u, 0x0d01u},
        {0xf17fu, 0x0301u}, {0xf174u, 0x0d01u}, {0xf174u, 0x0f01u},
        {0xf1ddu, 0x0301u}, {0xf1d4u, 0x0f01u}, {0xf1d4u, 0x0d01u},
        {0xf114u, 0x1300u}, {0xf154u, 0x2300u}, {0xf1b4u, 0x3300u}, /* zero replication */
        {0xf124u, 0x0301u}, {0xf184u, 0x0301u}, {0xf1e4u, 0x0301u}, /* unallocated op */
    };
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | flags;
        c.r[3] = 0x12345678u; c.r[13] = 0x100u;
        uint32_t before = c.cpsr;
        m_w16(NULL, 0, invalid[i][0]); m_w16(NULL, 2, invalid[i][1]);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u &&
              c.r[3] == 0x12345678u && c.r[13] == 0x100u && c.cpsr == before,
              "invalid arithmetic %u changed state", i);
    }
}

static void test_thumb2_wide_branches(void) {
    static const struct { uint16_t first, second; uint32_t offset; } cases[] = {
        {0xf000u, 0xb800u, 0u}, {0xf000u, 0x9000u, 0x00c00000u},
        {0xf000u, 0x9800u, 0x00800000u}, {0xf000u, 0xb000u, 0x00400000u},
        {0xf400u, 0x9000u, 0xff000000u}, {0xf400u, 0x9800u, 0xff400000u},
        {0xf400u, 0xb000u, 0xff800000u}, {0xf400u, 0xb800u, 0xffc00000u},
        {0xf3ffu, 0x97ffu, 0x00fffffeu}, {0xf7ffu, 0xbfffu, 0xfffffffeu},
        {0xf000u, 0xf802u, 4u}, {0xf578u, 0xfb04u, 0xffd78608u}, /* actual kernel BL */
        {0xf400u, 0xd000u, 0xff000000u}, {0xf3ffu, 0xd7ffu, 0x00fffffeu},
        {0xf7ffu, 0xffffu, 0xfffffffeu},
        {0xf000u, 0xe800u, 0u}, {0xf400u, 0xc000u, 0xff000000u},
        {0xf3ffu, 0xc7feu, 0x00fffffcu}, {0xf7ffu, 0xeffeu, 0xfffffffcu},
    };
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    for (unsigned p = 0; p < 2; p++) {
        for (unsigned aligned = 0; aligned < 2; aligned++) {
            for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
                arm_cpu_t c;
                CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
                c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_V | ARM_CPSR_Q;
                uint32_t cpsr = c.cpsr, pc = aligned ? 0x104u : 0x102u;
                c.r[15] = pc; c.r[14] = 0x777u;
                m_w16(NULL, pc, cases[i].first); m_w16(NULL, pc + 2u, cases[i].second);
                bool call = (cases[i].second & 0x4000u) != 0u;
                bool exchange = call && !(cases[i].second & 0x1000u);
                uint32_t base = exchange ? ((pc + 4u) & ~3u) : pc + 4u;
                CHECK(arm_step(&c) == ARM_OK && c.r[15] == base + cases[i].offset &&
                      c.r[14] == (call ? ((pc + 4u) | 1u) : 0x777u) && c.cycles == 1u &&
                      c.cpsr == (exchange ? (cpsr & ~ARM_CPSR_T) : cpsr),
                      "wide branch %u mishandled offset, LR, alignment or instruction state", i);
            }
        }
    }
    static const struct { uint32_t yes, no; } conditions[] = {
        {ARM_CPSR_Z, 0u}, {0u, ARM_CPSR_Z}, {ARM_CPSR_C, 0u}, {0u, ARM_CPSR_C},
        {ARM_CPSR_N, 0u}, {0u, ARM_CPSR_N}, {ARM_CPSR_V, 0u}, {0u, ARM_CPSR_V},
        {ARM_CPSR_C, ARM_CPSR_C | ARM_CPSR_Z}, {ARM_CPSR_Z, ARM_CPSR_C},
        {ARM_CPSR_N | ARM_CPSR_V, ARM_CPSR_N}, {ARM_CPSR_N, ARM_CPSR_N | ARM_CPSR_V},
        {0u, ARM_CPSR_Z}, {ARM_CPSR_Z, 0u},
    };
    for (unsigned cond = 0; cond < 14; cond++) {
        for (unsigned taken = 0; taken < 2; taken++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr = ARM_MODE_SVC | ARM_CPSR_T | (taken ? conditions[cond].yes : conditions[cond].no);
            uint32_t before = c.cpsr;
            c.r[14] = 0x777u;
            m_w16(NULL, 0, (uint16_t)(0xf000u | (cond << 6))); m_w16(NULL, 2, 0x8002u);
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == (taken ? 8u : 4u) &&
                  c.r[14] == 0x777u && c.cpsr == before,
                  "wide conditional branch condition %u taken=%u", cond, taken);
        }
    }
    static const struct { uint16_t first, second; uint32_t target; } conditional[] = {
        {0xf000u, 0x8000u, 4u}, {0xf000u, 0x8800u, 0x80004u},
        {0xf000u, 0xa000u, 0x40004u}, {0xf03fu, 0xafffu, 0x100002u},
        {0xf400u, 0x8000u, 0xfff00004u}, {0xf400u, 0x8800u, 0xfff80004u},
        {0xf400u, 0xa000u, 0xfff40004u}, {0xf43fu, 0xafffu, 2u},
    };
    for (unsigned i = 0; i < sizeof conditional / sizeof conditional[0]; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_Z;
        m_w16(NULL, 0, conditional[i].first); m_w16(NULL, 2, conditional[i].second);
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == conditional[i].target,
              "conditional branch %u used BL's inverted J bits or sign width", i);
    }
    /* Halfword-aligned BLX enters ARM and BX LR returns after both halves. */
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.r[15] = 2u; c.cpsr |= ARM_CPSR_T;
    m_w16(NULL, 2, 0xf000u); m_w16(NULL, 4, 0xe806u);
    m_w16(NULL, 6, 0x2307u); /* MOVS r3,#7 after return */
    m_w32(NULL, 0x10u, 0xe3a0202au); m_w32(NULL, 0x14u, 0xe12fff1eu);
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x10u && c.r[14] == 7u &&
          !(c.cpsr & ARM_CPSR_T), "BLX did not enter ARM");
    CHECK(arm_step(&c) == ARM_OK && arm_step(&c) == ARM_OK &&
          c.r[15] == 6u && (c.cpsr & ARM_CPSR_T) && c.r[2] == 42u,
          "ARM callee did not return to Thumb");
    CHECK(arm_step(&c) == ARM_OK && c.r[3] == 7u && c.cycles == 4u,
          "interworking lost an instruction or returned into the second half");
}

static void test_thumb2_multiple_transfers(void) {
    const unsigned regs[] = {0u, 3u, 8u, 12u, 14u};
    for (unsigned host = 0; host < 2; host++) {
      for (unsigned db = 0; db < 2; db++) {
       for (unsigned load = 0; load < 2; load++) {
        for (unsigned wb = 0; wb < 2; wb++) {
            arm_bus_t bus = g_bus;
            if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V;
            uint32_t before = c.cpsr, address = db ? 0x1006cu : 0x10080u;
            c.r[5] = 0x10080u;
            memset(g_ram + 0x10068u, 0xee, 0x30u);
            for (unsigned i = 0; i < 5; i++) {
                c.r[regs[i]] = 0x10000000u + regs[i];
                if (load) m_w32(NULL, address + 4u * i, 0xa5000000u + i);
            }
            m_w16(NULL, 0, (uint16_t)((db ? 0xe900u : 0xe880u) | (wb << 5) | (load << 4) | 5u));
            m_w16(NULL, 2, 0x5109u); /* r0,r3,r8,r12,lr */
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
                  c.cpsr == before && c.r[5] == (wb ? (db ? 0x1006cu : 0x10094u) : 0x10080u),
                  "multiple transfer direction or writeback mismatch");
            for (unsigned i = 0; i < 5; i++) {
                CHECK(c.r[regs[i]] == (load ? 0xa5000000u + i : 0x10000000u + regs[i]) &&
                      m_r32(NULL, address + 4u * i) == (load ? 0xa5000000u + i : 0x10000000u + regs[i]),
                      "multiple transfer register ordering mismatch at %u", i);
            }
            CHECK(m_r32(NULL, address - 4u) == 0xeeeeeeeeu &&
                  m_r32(NULL, address + 20u) == 0xeeeeeeeeu, "multiple transfer wrote outside its list");
        }
       }
      }
    }
    /* The real POP restores LR without branching; PUSH uses decrement-before. */
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T; c.r[13] = 0x200u;
    for (unsigned i = 4; i < 8; i++) c.r[i] = 0x100u + i;
    c.r[14] = 0x777u;
    m_w16(NULL, 0, 0xe92du); m_w16(NULL, 2, 0x40f0u); /* PUSH.W {r4-r7,lr} */
    m_w16(NULL, 4, 0xe8bdu); m_w16(NULL, 6, 0x40f0u); /* POP.W {r4-r7,lr} */
    CHECK(arm_step(&c) == ARM_OK && c.r[13] == 0x1ecu &&
          m_r32(NULL, 0x1ecu) == 0x104u && m_r32(NULL, 0x1fcu) == 0x777u,
          "PUSH alias did not use SP and decrement-before ordering");
    c.r[4] = c.r[7] = c.r[14] = 0u;
    CHECK(arm_step(&c) == ARM_OK && c.r[13] == 0x200u && c.r[4] == 0x104u &&
          c.r[7] == 0x107u && c.r[14] == 0x777u && c.r[15] == 8u,
          "POP with LR incorrectly branched or failed to restore the stack");
    for (unsigned target = 0; target < 3; target++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[13] = 0x100u; c.r[4] = 0x1234u;
        m_w16(NULL, 0, 0xe8bdu); m_w16(NULL, 2, 0x8010u); /* POP.W {r4,pc} */
        m_w32(NULL, 0x100u, 0x5678u); m_w32(NULL, 0x104u, 0x20u + target);
        CHECK(arm_step(&c) == (target == 2 ? ARM_UNDEFINED : ARM_OK) &&
              c.r[15] == (target == 2 ? 0u : 0x20u) &&
              c.r[4] == (target == 2 ? 0x1234u : 0x5678u) &&
              c.r[13] == (target == 2 ? 0x100u : 0x108u) &&
              !!(c.cpsr & ARM_CPSR_T) == (target != 0u),
              "POP PC did not interwork or reject a misaligned ARM target transactionally");
    }
    /* No-writeback base-in-list loads are defined; the loaded base wins. */
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T; c.r[0] = 0x100u;
    m_w16(NULL, 0, 0xe890u); m_w16(NULL, 2, 0x0003u);
    m_w32(NULL, 0x100u, 0x1234u); m_w32(NULL, 0x104u, 0x5678u);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x1234u && c.r[1] == 0x5678u,
          "LDM without writeback lost its loaded base register");
    static const uint16_t invalid[][2] = {
        {0xe8b0u, 0u}, {0xe8b0u, 2u}, {0xe8afu, 3u}, /* count/base */
        {0xe8b0u, 0x2002u}, {0xe8b0u, 0xc000u}, /* SP / PC+LR */
        {0xe8a0u, 3u}, {0xe8b0u, 3u}, /* Thumb forbids either writeback alias */
        {0xe920u, 3u}, {0xe930u, 3u},
        {0xe8a0u, 0x8002u}, {0xe920u, 0x8002u}, /* STM PC */
        {0xe8a0u, 0x2002u}, {0xe920u, 0x2002u}, /* STM SP */
    };
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[0] = 0x100u; c.r[1] = 0x1234u;
        m_w16(NULL, 0, invalid[i][0]); m_w16(NULL, 2, invalid[i][1]);
        m_w32(NULL, 0x100u, 0xeeeeeeeeu); m_w32(NULL, 0xf8u, 0xeeeeeeeeu);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.r[0] == 0x100u &&
              c.r[1] == 0x1234u && m_r32(NULL, 0x100u) == 0xeeeeeeeeu &&
              m_r32(NULL, 0xf8u) == 0xeeeeeeeeu, "invalid multiple-transfer list %u changed state", i);
    }
}

static void test_thumb2_multiple_transfer_aborts(void) {
    for (unsigned load = 0; load < 2; load++) {
      for (unsigned db = 0; db < 2; db++) {
       for (unsigned fault = 0; fault < 3; fault++) {
        memset(g_ram, 0, sizeof g_ram);
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP; /* raw U=A=0 still requires alignment */
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N;
        c.r[5] = (db ? 0x2004u : 0x1ffcu) + (fault == 2 ? 1u : 0u);
        c.r[0] = 0x11111111u; c.r[1] = 0x22222222u;
        uint32_t base = c.r[5], cpsr = c.cpsr;
        m_w32(NULL, 0x4000u, 0x6001u);
        m_w32(NULL, 0x6000u, 0x803eu); m_w32(NULL, 0x6004u, 0xa03eu);
        m_w32(NULL, 0x6008u, fault == 1 ? 0u : 0xc03eu);
        m_w16(NULL, 0x8000u, (uint16_t)((db ? 0xe920u : 0xe8a0u) | (load << 4) | 5u));
        m_w16(NULL, 0x8002u, 3u);
        m_w32(NULL, 0xaffcu, 0x33333333u); m_w32(NULL, 0xc000u, 0x44444444u);
        CHECK(arm_step(&c) == ARM_OK, "multiple transfer failed instead of retiring/vectoring");
        if (!fault) {
            CHECK(c.r[5] == (db ? 0x1ffcu : 0x2004u) && c.r[15] == 4u &&
                  c.r[0] == (load ? 0x33333333u : 0x11111111u) &&
                  c.r[1] == (load ? 0x44444444u : 0x22222222u) &&
                  m_r32(NULL, 0xaffcu) == (load ? 0x33333333u : 0x11111111u) &&
                  m_r32(NULL, 0xc000u) == (load ? 0x44444444u : 0x22222222u),
                  "multiple transfer assumed contiguous physical pages");
        } else {
            CHECK(c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 8u && c.r[5] == base &&
                  c.r[0] == 0x11111111u && c.r[1] == 0x22222222u &&
                  c.spsr[ARM_BANK_ABT] == cpsr && c.cp15.dfar == (fault == 2 ? 0x1ffdu : 0x2000u) &&
                  c.cp15.dfsr == ((fault == 2 ? ARM_FSR_ALIGNMENT : ARM_FSR_PAGE_TRANSLATION) |
                                  (load ? 0u : (1u << 11))),
                  "multiple-transfer abort committed a load/writeback or lost exception state");
            CHECK(m_r32(NULL, 0xaffcu) == (!load && fault == 1 ? 0x11111111u : 0x33333333u) &&
                  m_r32(NULL, 0xc000u) == 0x44444444u,
                  "multiple-transfer abort committed the wrong memory words");
        }
       }
      }
    }
}

static void test_thumb2_word_loads(void) {
    for (unsigned host = 0; host < 2; host++) {
      for (unsigned half_aligned = 0; half_aligned < 2; half_aligned++) {
       for (unsigned subtract = 0; subtract < 2; subtract++) {
        for (unsigned large = 0; large < 2; large++) {
            arm_bus_t bus = g_bus;
            if (host) bus.host_ram = m_host_ram;
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V;
            uint32_t cpsr = c.cpsr, pc = half_aligned ? 0x2002u : 0x2000u;
            uint32_t offset = large ? 0xfffu : 0x10u;
            uint32_t address = subtract ? 0x2004u - offset : 0x2004u + offset;
            c.r[15] = pc;
            m_w16(NULL, pc, subtract ? 0xf85fu : 0xf8dfu);
            m_w16(NULL, pc + 2u, (uint16_t)(0xc000u | offset)); /* LDR.W ip,[pc,+/-imm12] */
            m_w32(NULL, address, 0x76543210u);
            CHECK(arm_step(&c) == ARM_OK && c.r[12] == 0x76543210u &&
                  c.r[15] == pc + 4u && c.cpsr == cpsr && c.cycles == 1u,
                  "literal load used the wrong PC alignment, sign or byte offset");
        }
       }
      }
      const unsigned destinations[] = {2u, 5u, 13u, 14u};
      for (unsigned i = 0; i < 4; i++) {
        arm_bus_t bus = g_bus;
        if (host) bus.host_ram = m_host_ram;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[5] = 0x10000u;
        m_w16(NULL, 0, 0xf8d5u); m_w16(NULL, 2, (uint16_t)((destinations[i] << 12) | 0xfffu));
        m_w32(NULL, 0x10fffu, 0x12345678u);
        CHECK(arm_step(&c) == ARM_OK && c.r[destinations[i]] == 0x12345678u &&
              (destinations[i] == 5u || c.r[5] == 0x10000u) && c.r[15] == 4u,
              "wide load rejected SP/LR/base alias or aligned down the address");
      }
    }
    for (unsigned literal = 0; literal < 2; literal++) {
      for (unsigned target = 0; target < 3; target++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C;
        uint32_t before = c.cpsr;
        c.r[15] = 0x100u; c.r[5] = 0x110u;
        m_w16(NULL, 0x100u, literal ? 0xf8dfu : 0xf8d5u);
        m_w16(NULL, 0x102u, literal ? 0xf00cu : 0xf000u);
        m_w32(NULL, 0x110u, 0x20u + target);
        CHECK(arm_step(&c) == (target == 2 ? ARM_UNDEFINED : ARM_OK) &&
              c.r[15] == (target == 2 ? 0x100u : 0x20u) && c.r[5] == 0x110u &&
              c.cpsr == (target == 0 ? (before & ~ARM_CPSR_T) : before),
              "wide LDR PC did not interwork or reject an unaligned ARM target");
      }
      for (unsigned check_alignment = 0; check_alignment < 2; check_alignment++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.cp15.sctlr = check_alignment ? ARM_SCTLR_A : 0u;
        c.r[15] = 0x100u; c.r[5] = 0x111u;
        m_w16(NULL, 0x100u, literal ? 0xf8dfu : 0xf8d5u);
        m_w16(NULL, 0x102u, literal ? 0xf00du : 0xf000u);
        g_watch_addr = 0x111u; g_watch_reads32 = 0u;
        CHECK(arm_step(&c) == (check_alignment ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (check_alignment ? ARM_VEC_DATA_ABORT : 0x100u) &&
              g_watch_reads32 == 0u && (!check_alignment ||
              (c.cp15.dfar == 0x111u && c.cp15.dfsr == ARM_FSR_ALIGNMENT)),
              "unaligned LDR PC accessed memory or lost the alignment fault");
        g_watch_addr = 0xffffffffu;
      }
    }
}

static void test_thumb2_word_load_aborts(void) {
    for (unsigned host = 0; host < 2; host++) {
      for (unsigned fault = 0; fault < 4; fault++) {
        memset(g_ram, 0, sizeof g_ram);
        arm_bus_t bus = g_bus;
        if (host) bus.host_ram = m_host_ram;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N;
        c.r[4] = 0x1ffeu; c.r[2] = 0x12345678u;
        uint32_t cpsr = c.cpsr;
        m_w32(NULL, 0x4000u, 0x6001u);
        m_w32(NULL, 0x6000u, 0x803eu);
        m_w32(NULL, 0x6004u, fault == 1 ? 0u : 0xa03eu);
        m_w32(NULL, 0x6008u, fault == 2 ? 0u : fault == 3 ? 0xc01eu : 0xc03eu);
        m_w16(NULL, 0x8000u, 0xf8d4u); m_w16(NULL, 0x8002u, 0x2000u);
        m_w16(NULL, 0xaffeu, 0x2211u); m_w16(NULL, 0xc000u, 0x4433u);
        CHECK(arm_step(&c) == ARM_OK && c.r[4] == 0x1ffeu, "wide load status or base changed");
        if (!fault) {
            CHECK(c.r[2] == 0x44332211u && c.r[15] == 4u && c.cpsr == cpsr,
                  "wide load assumed contiguous pages");
        } else {
            CHECK(c.r[2] == 0x12345678u && c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 8u &&
                  c.cp15.dfar == (fault == 1 ? 0x1ffeu : 0x2000u) &&
                  c.cp15.dfsr == (fault == 3 ? ARM_FSR_PAGE_PERMISSION : ARM_FSR_PAGE_TRANSLATION) &&
                  c.spsr[ARM_BANK_ABT] == cpsr,
                  "wide load abort committed its result or lost DFAR/WnR/saved state");
        }
      }
    }
}

static void test_thumb2_small_stores(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    const uint16_t offsets[] = {0u, 0x64u, 0xfffu};
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned host = 0; host < 2; host++) {
      for (unsigned half = 0; half < 2; half++) {
       for (unsigned i = 0; i < 3; i++) {
        arm_bus_t bus = g_bus;
        if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, profiles[p]), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V | ARM_CPSR_Q;
        c.cp15.sctlr = 0u; /* ARMv7 unaligned support even when raw U=0. */
        c.r[13] = 0x10001u; c.r[14] = 0x44332211u;
        uint32_t cpsr = c.cpsr, address = c.r[13] + offsets[i];
        memset(g_ram + 0x10000u, 0xee, 0x1008u);
        m_w16(NULL, 0, (uint16_t)(half ? 0xf8adu : 0xf88du)); /* Rt=LR, Rn=SP */
        m_w16(NULL, 2, (uint16_t)(0xe000u | offsets[i]));
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
              c.r[13] == 0x10001u && c.r[14] == 0x44332211u && c.cpsr == cpsr,
              "STRB/H.W changed operands, flags or retirement");
        CHECK(g_ram[address - 1u] == 0xeeu && g_ram[address] == 0x11u &&
              g_ram[address + 1u] == (half ? 0x22u : 0xeeu) && g_ram[address + 2u] == 0xeeu,
              "STRB/H.W offset/size wrong or adjacent byte overwritten");
       }
      }
     }
    }
    for (unsigned half = 0; half < 2; half++) {
     for (unsigned bad = 0; bad < 3; bad++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T;
        c.r[4] = 0x100u; c.r[2] = 0x12345678u;
        m_w16(NULL, 0, (uint16_t)((half ? 0xf8a0u : 0xf880u) | (bad == 2 ? 15u : 4u)));
        m_w16(NULL, 2, (uint16_t)((bad == 0 ? 13u : bad == 1 ? 15u : 2u) << 12));
        m_w32(NULL, 0x100u, 0xeeeeeeeeu);
        uint32_t instruction = m_r32(NULL, 0), cpsr = c.cpsr;
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == cpsr &&
              m_r32(NULL, 0) == instruction && m_r32(NULL, 0x100u) == 0xeeeeeeeeu,
              "STRB/H.W accepted SP/PC source or PC base");
     }
    }
}

static void test_thumb2_small_store_aborts(void) {
    for (unsigned host = 0; host < 2; host++) {
     for (unsigned half = 0; half < 2; half++) {
      for (unsigned fault = 0; fault < 5; fault++) {
        arm_bus_t bus = g_bus;
        if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
        arm_cpu_t c;
        memset(g_ram, 0xee, sizeof g_ram);
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | (fault == 4 ? ARM_SCTLR_A : 0u);
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N;
        c.r[4] = 0x1ffeu; c.r[2] = 0x44332211u;
        uint32_t cpsr = c.cpsr;
        m_w32(NULL, 0x4000u, 0x6001u);
        m_w32(NULL, 0x6000u, 0x803eu);
        m_w32(NULL, 0x6004u, fault == 1 ? 0u : 0xa03eu);
        m_w32(NULL, 0x6008u, fault == 2 ? 0u : fault == 3 ? 0xc01eu : 0xc03eu);
        m_w16(NULL, 0x8000u, (uint16_t)(half ? 0xf8a4u : 0xf884u));
        m_w16(NULL, 0x8002u, 0x2001u); /* address 0x1fff; halfword crosses pages */
        bool abort = fault == 1u || (half && fault != 0u);
        CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u && c.r[4] == 0x1ffeu &&
              c.r[2] == 0x44332211u, "STRB/H.W failed to retire with operands intact");
        if (abort) {
            uint32_t fsr = fault == 4 ? ARM_FSR_ALIGNMENT : fault == 3 ?
                           ARM_FSR_PAGE_PERMISSION : ARM_FSR_PAGE_TRANSLATION;
            CHECK(c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 8u &&
                  c.cp15.dfar == (fault == 1 || fault == 4 ? 0x1fffu : 0x2000u) &&
                  c.cp15.dfsr == (fsr | (1u << 11)) && c.spsr[ARM_BANK_ABT] == cpsr,
                  "STRB/H.W abort lost byte address, WnR, LR or saved flags");
        } else {
            CHECK(c.r[15] == 4u && c.cpsr == cpsr,
                  "byte access incorrectly used halfword alignment or next-page permission");
        }
        bool first_written = !abort || (half && (fault == 2 || fault == 3));
        CHECK(g_ram[0xafffu] == (first_written ? 0x11u : 0xeeu) &&
              g_ram[0xc000u] == (half && !abort ? 0x22u : 0xeeu) &&
              g_ram[0xaffeu] == 0xeeu && g_ram[0xb000u] == 0xeeu && g_ram[0xc001u] == 0xeeu,
              "STRB/H.W committed wrong bytes across an abort or physical page boundary");
      }
     }
    }
}

static void test_thumb2_indexed_transfers(void) {
    const uint16_t opcodes[] = {0xf800u, 0xf820u, 0xf840u, 0xf850u};
    const uint16_t offsets[] = {0u, 4u, 255u};
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2; profile++) {
     for (unsigned host = 0; host < 2; host++) {
      for (unsigned op = 0; op < 4; op++) {
       for (unsigned puw = 0; puw < 8; puw++) {
        for (unsigned imm = 0; imm < 3; imm++) {
            arm_bus_t bus = g_bus;
            if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &bus, profiles[profile]), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V;
            c.cp15.sctlr = 0u;
            c.r[4] = 0x1100u; c.r[8] = 0x44332211u;
            bool pre = (puw & 4u) != 0u, add = (puw & 2u) != 0u, wb = (puw & 1u) != 0u;
            bool valid = (pre || wb) && puw != 6u; /* 110 is unprivileged, not ordinary. */
            uint32_t adjusted = add ? 0x1100u + offsets[imm] : 0x1100u - offsets[imm];
            uint32_t address = pre ? adjusted : 0x1100u, cpsr = c.cpsr;
            memset(g_ram + 0x1000u, 0xee, 0x208u);
            if (op == 3u) m_w32(NULL, address, 0x12345678u);
            m_w16(NULL, 0, (uint16_t)(opcodes[op] | 4u));
            m_w16(NULL, 2, (uint16_t)(0x8800u | (puw << 8) | offsets[imm]));
            CHECK(arm_step(&c) == (valid ? ARM_OK : ARM_UNDEFINED) && c.cycles == 1u &&
                  c.r[15] == (valid ? 4u : 0u) && c.cpsr == cpsr &&
                  c.r[4] == (valid && wb ? adjusted : 0x1100u) &&
                  c.r[8] == (valid && op == 3u ? 0x12345678u : 0x44332211u),
                  "indexed wide transfer op=%u PUW=%u immediate=%u has wrong state", op, puw, offsets[imm]);
            uint32_t expected = op == 3u ? 0x12345678u : !valid ? 0xeeeeeeeeu :
                                op == 0u ? 0xeeeeee11u : op == 1u ? 0xeeee2211u : 0x44332211u;
            CHECK(m_r32(NULL, address) == expected && g_ram[address - 1u] == 0xeeu &&
                  g_ram[address + 4u] == 0xeeu, "indexed transfer wrote the wrong address or width");
        }
       }
      }
     }
    }
    for (unsigned op = 0; op < 4; op++) {
     for (unsigned bad = 0; bad < 4; bad++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T;
        c.r[4] = 0x100u;
        /* Overlap; PC store; SP byte/halfword store. For loads, the latter
         * cases exercise the unprivileged and P=W=0 encodings instead. */
        unsigned rt = bad == 0 ? 4u : bad == 1 ? 15u : 13u;
        unsigned puw = 7u;
        if (op == 3u && bad) { rt = 8u; puw = bad == 1 ? 6u : 0u; }
        if (op == 2u && bad == 2u) continue; /* STR permits SP as source. */
        if (bad == 3u) {
            if (op == 3u) continue; /* Rn=PC is the separately tested LDR literal. */
            rt = 8u;
        }
        m_w16(NULL, 0, (uint16_t)(opcodes[op] | (bad == 3u ? 15u : 4u)));
        m_w16(NULL, 2, (uint16_t)((rt << 12) | 0x800u | (puw << 8) | 4u));
        m_w32(NULL, 0x104u, 0xeeeeeeeeu);
        uint32_t cpsr = c.cpsr;
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.r[4] == 0x100u &&
              c.cpsr == cpsr && m_r32(NULL, 0x104u) == 0xeeeeeeeeu,
              "indexed transfer accepted restricted source, overlap or unprivileged alias");
     }
    }
    /* Actual single-register PUSH and POP encodings, with SP writeback. */
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T; c.r[13] = 0x200u; c.r[8] = 0x13579bdfu;
    m_w16(NULL, 0, 0xf84du); m_w16(NULL, 2, 0x8d04u);
    m_w16(NULL, 4, 0xf85du); m_w16(NULL, 6, 0x8b04u);
    CHECK(arm_step(&c) == ARM_OK && c.r[13] == 0x1fcu &&
          m_r32(NULL, 0x1fcu) == 0x13579bdfu, "single-register PUSH failed");
    c.r[8] = 0u;
    CHECK(arm_step(&c) == ARM_OK && c.r[13] == 0x200u && c.r[8] == 0x13579bdfu &&
          c.r[15] == 8u, "single-register POP failed");
    c.r[4] = 0x200u;
    m_w16(NULL, 8, 0xf844u); m_w16(NULL, 10, 0xdc04u); /* STR sp,[r4,#-4] */
    m_w16(NULL, 12, 0xf854u); m_w16(NULL, 14, 0xdc04u); /* LDR sp,[r4,#-4] */
    CHECK(arm_step(&c) == ARM_OK && m_r32(NULL, 0x1fcu) == 0x200u,
          "indexed word store rejected SP source");
    c.r[13] = 0u;
    CHECK(arm_step(&c) == ARM_OK && c.r[13] == 0x200u && c.r[4] == 0x200u,
          "indexed word load rejected SP destination or changed base without writeback");
    for (unsigned target = 0; target < 4; target++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[13] = 0x200u;
        m_w16(NULL, 0, 0xf85du); m_w16(NULL, 2, 0xfb04u); /* POP {pc} */
        m_w32(NULL, 0x200u, 0x100u | target);
        uint32_t cpsr = c.cpsr;
        CHECK(arm_step(&c) == (target == 2u ? ARM_UNDEFINED : ARM_OK) &&
              c.r[13] == (target == 2u ? 0x200u : 0x204u) &&
              c.r[15] == (target == 2u ? 0u : (0x100u | target) & ~1u) &&
              c.cpsr == (target == 0u ? cpsr & ~ARM_CPSR_T : cpsr),
              "POP PC failed interworking or committed writeback for an invalid target");
    }
}

static void test_thumb2_indexed_transfer_aborts(void) {
    for (unsigned host = 0; host < 2; host++) {
     for (unsigned load = 0; load < 2; load++) {
      for (unsigned pre = 0; pre < 2; pre++) {
       for (unsigned fault = 0; fault < 4; fault++) {
        arm_cpu_t c;
        arm_bus_t bus = g_bus;
        if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
        memset(g_ram, 0xee, sizeof g_ram);
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C;
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | (fault == 3 ? ARM_SCTLR_A : 0u);
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        c.r[4] = pre ? 0x1ffdu : 0x1ffeu; c.r[8] = 0x44332211u;
        uint32_t cpsr = c.cpsr, base = c.r[4];
        m_w32(NULL, 0x4000u, 0x6001u);
        m_w32(NULL, 0x6000u, 0x803eu);
        m_w32(NULL, 0x6004u, fault == 0u ? 0u : 0xa03eu);
        m_w32(NULL, 0x6008u, fault == 1u ? 0u : 0xc01eu); /* user denied */
        m_w16(NULL, 0x8000u, (uint16_t)(load ? 0xf854u : 0xf844u));
        m_w16(NULL, 0x8002u, (uint16_t)(pre ? 0x8f01u : 0x8b01u));
        uint32_t fsr = fault == 3u ? ARM_FSR_ALIGNMENT : fault == 2u ?
                       ARM_FSR_PAGE_PERMISSION : ARM_FSR_PAGE_TRANSLATION;
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 8u &&
              c.r[4] == base && c.r[8] == 0x44332211u && c.spsr[ARM_BANK_ABT] == cpsr &&
              c.cp15.dfar == (fault == 0u || fault == 3u ? 0x1ffeu : 0x2000u) &&
              c.cp15.dfsr == (fsr | (load ? 0u : (1u << 11))),
              "indexed transfer abort committed writeback/result or lost fault state");
        CHECK(m_r16(NULL, 0xaffeu) == (!load && (fault == 1u || fault == 2u) ? 0x2211u : 0xeeeeu) &&
              m_r32(NULL, 0xc000u) == 0xeeeeeeeeu,
              "indexed transfer abort committed incorrect memory bytes");
       }
      }
     }
    }
}

#define TEST_IT_MASK 0x0600fc00u
static uint32_t test_it_bits(unsigned state) {
    return ((state & 0xfcu) << 8) | ((state & 3u) << 25);
}

static void put_vfp_system_transfer(unsigned thumb, uint32_t pc, unsigned load,
                                     unsigned sysreg, unsigned rt) {
    uint32_t insn = 0xeee00a10u | (load << 20) | (sysreg << 16) | (rt << 12);
    if (thumb) {
        m_w16(NULL, pc, (uint16_t)(insn >> 16));
        m_w16(NULL, pc + 2u, (uint16_t)insn);
    } else m_w32(NULL, pc, insn);
}

static void test_cortex_a8_vfp_system_access(void) {
    /* DDI0344K 13.4; DDI0406C.b B9.3.21/22. Non-FPSCR accesses stay
     * privileged even with EN=1. Unimplemented identity reads must stop,
     * including with EN=0, instead of leaking VFP11's identity or repeatedly
     * entering a lazy-enable handler that cannot supply missing hardware. */
    static const unsigned permissions[] = {0,1,3};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned user = 0; user < 2u; user++) {
      for (unsigned enabled = 0; enabled < 2u; enabled++) {
       for (unsigned acc = 0; acc < 3u; acc++) {
        for (unsigned sysreg = 0; sysreg < 16u; sysreg++) {
         for (unsigned load = 0; load < 2u; load++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            CHECK(c.vfp_fpexc == 0u && c.vfp_fpscr == 0u, "A8 FP reset");
            c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_N |
                     ARM_CPSR_C | ARM_CPSR_Q | ARM_CPSR_F | (9u << 16) |
                     (thumb ? ARM_CPSR_T | test_it_bits(0x1cu) : 0u); /* NE */
            c.cp15.cpacr = permissions[acc] * 0x00500000u;
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            c.vfp_fpscr = 0x48000080u; /* Z, QC, IDC */
            for (unsigned n = 0; n < 32u; n++) c.vfp_s[n] = 0x12340000u + n;
            uint32_t fp[32]; memcpy(fp, c.vfp_s, sizeof fp);
            uint32_t source = sysreg == 8u ? (enabled ? 0u : ARM_FPEXC_EN) : 0x98000095u;
            c.r[4] = source; c.r[15] = 0x100u;
            c.excl_valid = true; c.excl_addr = 0x8000u;
            uint32_t flags = c.cpsr, generation = c.tlb_gen;
            arm_cp15_t cp15 = c.cp15;
            put_vfp_system_transfer(thumb, 0x100u, load, sysreg, 4u);
            bool legal = sysreg == 0u || sysreg == 1u || sysreg == 8u ||
                         (load && (sysreg == 6u || sysreg == 7u));
            bool access = permissions[acc] == 3u || (permissions[acc] == 1u && !user);
            bool exception = legal && (!access || (user && sysreg != 1u) ||
                                       (!enabled && sysreg == 1u));
            bool complete = legal && !exception && !(load && sysreg != 1u && sysreg != 8u);
            arm_status_t status = arm_step(&c);
            CHECK(status == (complete || exception ? ARM_OK : ARM_UNDEFINED),
                  "A8 FP disposition T=%u U=%u EN=%u acc=%u reg=%u L=%u status=%d",
                  thumb, user, enabled, permissions[acc], sysreg, load, status);
            uint32_t expected_r4 = complete && load ?
                (sysreg == 1u ? 0x48000080u : enabled ? ARM_FPEXC_EN : 0u) : source;
            CHECK(c.r[4] == expected_r4 &&
                  c.vfp_fpscr == (complete && !load && sysreg == 1u ? source : 0x48000080u) &&
                  c.vfp_fpexc == (complete && !load && sysreg == 8u ? source : enabled ? ARM_FPEXC_EN : 0u) &&
                  memcmp(fp, c.vfp_s, sizeof fp) == 0 && memcmp(&cp15, &c.cp15, sizeof cp15) == 0 &&
                  c.tlb_gen == generation && c.cycles == 1u,
                  "A8 FP operand/control effects T=%u U=%u EN=%u acc=%u reg=%u L=%u",
                  thumb, user, enabled, permissions[acc], sysreg, load);
            if (exception) {
                uint32_t handler_flags = (flags & ~(ARM_CPSR_MODE_MASK | ARM_CPSR_T | TEST_IT_MASK)) |
                                         ARM_MODE_UND | ARM_CPSR_I;
                CHECK(c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == (thumb ? 0x102u : 0x104u) &&
                      c.spsr[ARM_BANK_UND] == flags && c.cpsr == handler_flags && !c.excl_valid,
                      "A8 FP denial lost precise Undefined state T=%u reg=%u L=%u", thumb, sysreg, load);
            } else {
                uint32_t after = complete && thumb ? (flags & ~TEST_IT_MASK) | test_it_bits(0x18u) : flags;
                CHECK(c.r[15] == (complete ? 0x104u : 0x100u) && c.cpsr == after &&
                      c.excl_valid && c.excl_addr == 0x8000u,
                      "A8 FP completion/refusal changed PC/IT/monitor T=%u reg=%u L=%u", thumb, sysreg, load);
            }
         }
        }
       }
      }
     }
    }
}

static void test_cortex_a8_vfp_control_fields(void) {
    /* List the documented fields independently of the production mask.
     * FPEXC extra state is not implemented: EX and non-EN requests must stop
     * before modifying the live enable state, not manufacture saved state. */
    static const unsigned fpscr_bits[] = {0,1,2,3,4,7,16,17,18,20,21,22,23,24,25,27,28,29,30,31};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned fpexc = 0; fpexc < 2u; fpexc++) {
      for (unsigned bit = 0; bit <= 32u; bit++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = ARM_MODE_SVC | ARM_CPSR_N | ARM_CPSR_Q | (thumb ? ARM_CPSR_T : 0u);
        c.cp15.cpacr = 0x00f00000u;
        c.vfp_fpexc = ARM_FPEXC_EN; c.vfp_fpscr = 0x48000080u;
        c.r[4] = bit == 32u ? 0u : 1u << bit; c.r[15] = 0x100u;
        uint32_t flags = c.cpsr;
        bool allowed = bit == 32u || (fpexc && bit == 30u);
        if (!fpexc) for (unsigned n = 0; n < sizeof fpscr_bits / sizeof fpscr_bits[0]; n++)
            if (bit == fpscr_bits[n]) allowed = true;
        put_vfp_system_transfer(thumb, 0x100u, 0u, fpexc ? 8u : 1u, 4u);
        CHECK(arm_step(&c) == (allowed ? ARM_OK : ARM_UNDEFINED) &&
              c.vfp_fpscr == (!fpexc && allowed ? c.r[4] : 0x48000080u) &&
              c.vfp_fpexc == (fpexc && allowed ? c.r[4] : ARM_FPEXC_EN) &&
              c.r[15] == (allowed ? 0x104u : 0x100u) && c.cpsr == flags,
              "A8 FP field T=%u FPEXC=%u bit=%u", thumb, fpexc, bit);
        if (allowed) {
            put_vfp_system_transfer(thumb, 0x104u, 1u, fpexc ? 8u : 1u, 2u);
            CHECK(arm_step(&c) == ARM_OK && c.r[2] == c.r[4] && c.cpsr == flags,
                  "A8 FP field failed guest readback T=%u FPEXC=%u bit=%u", thumb, fpexc, bit);
        }
      }
     }
    }
}

static void test_cortex_a8_vfp_core_registers(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned fpexc = 0; fpexc < 2u; fpexc++) {
      for (unsigned load = 0; load < 2u; load++) {
       for (unsigned rt = 0; rt < 16u; rt++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = ARM_MODE_SYS | ARM_CPSR_N | ARM_CPSR_Q | (10u << 16) |
                 (thumb ? ARM_CPSR_T | test_it_bits(0x1cu) : 0u);
        c.cp15.cpacr = 0x00f00000u;
        c.vfp_fpexc = ARM_FPEXC_EN; c.vfp_fpscr = 0x48000080u;
        for (unsigned n = 0; n < 15u; n++) c.r[n] = 0x12340000u + n;
        if (rt != 15u) c.r[rt] = fpexc ? 0u : 0x98000095u;
        c.r[15] = 0x100u;
        uint32_t regs[16]; memcpy(regs, c.r, sizeof regs);
        uint32_t flags = c.cpsr;
        bool apsr = load && !fpexc && rt == 15u;
        bool allowed = !(thumb && rt == 13u) && (rt != 15u || apsr);
        put_vfp_system_transfer(thumb, 0x100u, load, fpexc ? 8u : 1u, rt);
        CHECK(arm_step(&c) == (allowed ? ARM_OK : ARM_UNDEFINED),
              "A8 VMRS/VMSR Rt legality T=%u FPEXC=%u L=%u Rt=%u", thumb, fpexc, load, rt);
        if (allowed) {
            if (load && !apsr) regs[rt] = fpexc ? ARM_FPEXC_EN : 0x48000080u;
            if (apsr) flags = (flags & 0x0fffffffu) | 0x40000000u;
            if (thumb) flags = (flags & ~TEST_IT_MASK) | test_it_bits(0x18u);
            regs[15] = 0x104u;
        }
        CHECK(memcmp(regs, c.r, sizeof regs) == 0 && c.cpsr == flags &&
              c.vfp_fpscr == (allowed && !load && !fpexc ? 0x98000095u : 0x48000080u) &&
              c.vfp_fpexc == (allowed && !load && fpexc ? 0u : ARM_FPEXC_EN),
              "A8 VMRS/VMSR register/flag effects T=%u FPEXC=%u L=%u Rt=%u", thumb, fpexc, load, rt);
       }
      }
     }
    }
    /* Every flag combination, with QC deliberately different from APSR.Q. */
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned nzcv = 0; nzcv < 16u; nzcv++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = ARM_MODE_USR | ARM_CPSR_Q | (5u << 16) | (thumb ? ARM_CPSR_T : 0u);
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        c.vfp_fpscr = (nzcv << 28) | 0x80u;
        uint32_t flags = c.cpsr;
        put_vfp_system_transfer(thumb, 0u, 1u, 1u, 15u);
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cpsr == (flags | (nzcv << 28)),
              "VMRS APSR transferred fields beyond NZCV T=%u flags=%u", thumb, nzcv);
     }
    }
}

static void test_cortex_a8_vfp_undefined_retry(void) {
    /* B1.9.2: Thumb Undefined LR is fault PC+2 even for a 32-bit instruction.
     * Guest code enables FPEXC, returns with SUBS pc,lr,#2, and re-executes
     * the exact IT slot. VMRS then changes the condition of the next slot. */
    for (unsigned user = 0; user < 2u; user++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SYS) | ARM_CPSR_T | ARM_CPSR_Z |
                 ARM_CPSR_Q | (9u << 16);
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpscr = 0x80000000u;
        c.r[15] = 0x100u; c.r[5] = ARM_FPEXC_EN; c.r[2] = 0x12345678u;
        m_w16(NULL, 0x100u, 0xbf04u); /* ITT EQ */
        put_vfp_system_transfer(1u, 0x102u, 1u, 1u, 15u);
        m_w16(NULL, 0x106u, 0x2201u); /* MOVS r2,#1, second EQ slot */
        put_vfp_system_transfer(0u, ARM_VEC_UNDEFINED, 0u, 8u, 5u);
        m_w32(NULL, 8u, 0xe25ef002u); /* SUBS pc,lr,#2 */
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x102u &&
              (c.cpsr & TEST_IT_MASK) == test_it_bits(0x04u), "FP retry IT setup");
        uint32_t interrupted = c.cpsr;
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == 0x104u &&
              c.spsr[ARM_BANK_UND] == interrupted && !(c.cpsr & (ARM_CPSR_T | TEST_IT_MASK)) &&
              c.vfp_fpexc == 0u, "disabled Thumb FPSCR did not enter precise Undefined handler");
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 8u && c.vfp_fpexc == ARM_FPEXC_EN,
              "guest Undefined handler did not enable VFP");
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x102u && c.cpsr == interrupted,
              "Undefined return did not restore Thumb instruction and original IT slot");
        uint32_t after = (interrupted & ~(0xf0000000u | TEST_IT_MASK)) |
                         ARM_CPSR_N | test_it_bits(0x08u);
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x106u && c.cpsr == after,
              "VMRS retry did not transfer NZCV and retire its IT slot once");
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x108u && c.r[2] == 0x12345678u &&
              c.cpsr == (after & ~TEST_IT_MASK) && c.cycles == 6u,
              "post-retry IT condition did not see new FPSCR comparison flags");
    }
}

/* Actual translating accesses, including a D31 transfer split across unrelated
 * physical frames. S31 uses the same start address but must touch one word. */
static void test_cortex_a8_vfp_single_memory_aborts(void) {
    for (unsigned host = 0; host < 2u; host++) {
     for (unsigned thumb = 0; thumb < 2u; thumb++) {
      for (unsigned load = 0; load < 2u; load++) {
       for (unsigned dbl = 0; dbl < 2u; dbl++) {
        for (unsigned fault = 0; fault < 7u; fault++) {
            arm_bus_t bus = g_bus;
            if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
            arm_cpu_t c;
            memset(g_ram, 0xee, sizeof g_ram);
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | (fault == 6u ? ARM_SCTLR_A : 0u);
            c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
            c.vfp_fpscr = 0x0bc00080u;
            c.cpsr = ARM_MODE_USR | ARM_CPSR_N | ARM_CPSR_C |
                     (thumb ? ARM_CPSR_T | test_it_bits(0x18u) : 0u);
            c.r[4] = fault >= 5u ? 0x1ff9u : 0x1ff8u;
            c.vfp_s[31] = 0x44332211u;
            c.a8_vfp_hi[15] = UINT64_C(0x8877665544332211);
            uint32_t flags = c.cpsr, base = c.r[4];
            m_w32(NULL, 0x4000u, 0x6001u);
            m_w32(NULL, 0x6000u, 0x803eu);
            m_w32(NULL, 0x6004u, fault == 1u ? 0u : fault == 3u ? 0xa01eu : 0xa03eu);
            m_w32(NULL, 0x6008u, fault == 2u ? 0u : fault == 4u ? 0xc01eu : 0xc03eu);
            uint32_t insn = 0xedc4fa01u | (load << 20) | (dbl << 8); /* [r4,#4] */
            if (thumb) {
                m_w16(NULL, 0x8000u, (uint16_t)(insn >> 16)); m_w16(NULL, 0x8002u, (uint16_t)insn);
            } else m_w32(NULL, 0x8000u, insn);
            m_w32(NULL, 0xaffcu, 0x12345678u); m_w32(NULL, 0xc000u, 0x9abcdef0u);
            g_watch_addr = 0xc000u; g_watch_reads32 = g_watch_writes32 = 0u;
            bool aborts = fault && (dbl || (fault != 2u && fault != 4u));
            CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u && c.r[4] == base,
                  "single FP transfer did not retire once or changed base T=%u L=%u D=%u fault=%u",
                  thumb, load, dbl, fault);
            if (aborts) {
                uint32_t fsr = fault >= 5u ? ARM_FSR_ALIGNMENT : fault >= 3u ? ARM_FSR_PAGE_PERMISSION :
                                                                                           ARM_FSR_PAGE_TRANSLATION;
                uint32_t address = fault >= 5u ? 0x1ffdu : fault == 1u || fault == 3u ? 0x1ffcu : 0x2000u;
                CHECK(c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 8u && c.spsr[ARM_BANK_ABT] == flags &&
                      c.cp15.dfar == address && c.cp15.dfsr == (fsr | (load ? 0u : 1u << 11)) &&
                      !(c.cpsr & TEST_IT_MASK) && g_watch_reads32 == 0u && g_watch_writes32 == 0u,
                      "single FP abort lost first fault/IT or touched second frame T=%u L=%u D=%u fault=%u",
                      thumb, load, dbl, fault);
            } else {
                CHECK(c.r[15] == 4u && c.cpsr == (flags & ~TEST_IT_MASK),
                      "single FP transfer lost PC/IT/flags T=%u L=%u D=%u fault=%u", thumb, load, dbl, fault);
                if (!dbl) CHECK(g_watch_reads32 == 0u && g_watch_writes32 == 0u, "S load/store accessed second word");
            }
            CHECK(c.vfp_s[31] == (load && !dbl && !aborts ? 0x12345678u : 0x44332211u) &&
                  c.a8_vfp_hi[15] == (load && dbl && !aborts ? UINT64_C(0x9abcdef012345678) :
                                                             UINT64_C(0x8877665544332211)) &&
                  c.vfp_fpexc == ARM_FPEXC_EN && c.vfp_fpscr == 0x0bc00080u,
                  "single FP fault partially changed a register or controls T=%u L=%u D=%u fault=%u",
                  thumb, load, dbl, fault);
            g_watch_addr = 0xffffffffu;
            bool first_written = !load && (fault == 0u || fault == 2u || fault == 4u);
            CHECK(m_r32(NULL, 0xaffcu) == (first_written ? 0x44332211u : 0x12345678u) &&
                  m_r32(NULL, 0xc000u) == (!load && dbl && !aborts ? 0x88776655u : 0x9abcdef0u) &&
                  m_r32(NULL, 0xb000u) == 0xeeeeeeeeu,
                  "single FP store lost completed first word or touched wrong frame T=%u D=%u fault=%u",
                  thumb, dbl, fault);
        }
       }
      }
     }
    }
}

static void test_cortex_a8_vfp_multiple_memory_aborts(void) {
    for (unsigned host = 0; host < 2u; host++) {
     for (unsigned thumb = 0; thumb < 2u; thumb++) {
      for (unsigned load = 0; load < 2u; load++) {
       for (unsigned dbl = 0; dbl < 2u; dbl++) {
        for (unsigned db = 0; db < 2u; db++) {
         for (unsigned fault = 0; fault < 7u; fault++) {
            arm_bus_t bus = g_bus;
            if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
            arm_cpu_t c;
            memset(g_ram, 0xee, sizeof g_ram);
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | (fault == 6u ? ARM_SCTLR_A : 0u);
            c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
            c.cpsr = ARM_MODE_USR | ARM_CPSR_C | (thumb ? ARM_CPSR_T | test_it_bits(0x1cu) : 0u);
            uint32_t address = (dbl ? 0x1ff4u : 0x1ffcu) + (fault >= 5u ? 1u : 0u);
            uint32_t bytes = dbl ? 16u : 8u;
            c.r[4] = address + (db ? bytes : 0u);
            uint32_t flags = c.cpsr, base = c.r[4];
            c.vfp_s[30] = 0x11111111u; c.vfp_s[31] = 0x22222222u;
            c.a8_vfp_hi[14] = UINT64_C(0x2222222211111111);
            c.a8_vfp_hi[15] = UINT64_C(0x4444444433333333);
            m_w32(NULL, 0x4000u, 0x6001u); m_w32(NULL, 0x6000u, 0x803eu);
            m_w32(NULL, 0x6004u, fault == 1u ? 0u : fault == 3u ? 0xa01eu : 0xa03eu);
            m_w32(NULL, 0x6008u, fault == 2u ? 0u : fault == 4u ? 0xc01eu : 0xc03eu);
            uint32_t insn = (db ? 0xed240000u : 0xeca40000u) | (load << 20) |
                            (dbl ? 0x0040eb04u : 0x0000fa02u); /* D30-D31 or S30-S31, R4! */
            if (thumb) { m_w16(NULL, 0x8000u, (uint16_t)(insn >> 16)); m_w16(NULL, 0x8002u, (uint16_t)insn); }
            else m_w32(NULL, 0x8000u, insn);
            uint32_t aligned = address & ~3u;
            for (unsigned w = 0; w < bytes / 4u; w++)
                m_w32(NULL, aligned + w * 4u < 0x2000u ? 0xa000u + ((aligned + w * 4u) & 0xfffu) : 0xc000u,
                      0xabc00001u + w);
            g_watch_addr = 0xc000u; g_watch_reads32 = g_watch_writes32 = 0u;
            CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u, "multiple FP transfer disposition");
            if (fault) {
                uint32_t fsr = fault >= 5u ? ARM_FSR_ALIGNMENT : fault >= 3u ? ARM_FSR_PAGE_PERMISSION :
                                                                                           ARM_FSR_PAGE_TRANSLATION;
                uint32_t far = fault == 2u || fault == 4u ? 0x2000u : address;
                CHECK(c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 8u && c.spsr[ARM_BANK_ABT] == flags &&
                      c.r[4] == base && c.cp15.dfar == far && c.cp15.dfsr == (fsr | (load ? 0u : 1u << 11)) &&
                      !(c.cpsr & TEST_IT_MASK) && g_watch_reads32 == 0u && g_watch_writes32 == 0u,
                      "multiple FP abort lost base/first fault/IT T=%u L=%u D=%u DB=%u fault=%u",
                      thumb, load, dbl, db, fault);
            } else CHECK(c.r[15] == 4u && c.r[4] == (db ? address : base + bytes) &&
                         c.cpsr == (thumb ? (flags & ~TEST_IT_MASK) | test_it_bits(0x18u) : flags),
                         "multiple FP success lost writeback/IT");
            bool first_complete = load && (fault == 0u || fault == 2u || fault == 4u);
            CHECK(c.vfp_s[30] == (!dbl && first_complete ? 0xabc00001u : 0x11111111u) &&
                  c.vfp_s[31] == (!dbl && load && !fault ? 0xabc00002u : 0x22222222u) &&
                  c.a8_vfp_hi[14] == (dbl && first_complete ? UINT64_C(0xabc00002abc00001) :
                                                                               UINT64_C(0x2222222211111111)) &&
                  c.a8_vfp_hi[15] == (dbl && load && !fault ? UINT64_C(0xabc00004abc00003) :
                                                                           UINT64_C(0x4444444433333333)),
                  "multiple FP load lost completed register or partially replaced failed D register");
            g_watch_addr = 0xffffffffu;
            for (unsigned w = 0; w < bytes / 4u; w++) {
                uint32_t va = aligned + w * 4u, pa = va < 0x2000u ? 0xa000u + (va & 0xfffu) : 0xc000u;
                bool written = !load && (!fault || ((fault == 2u || fault == 4u) && va < 0x2000u));
                CHECK(m_r32(NULL, pa) == (written ? 0x11111111u * (w + 1u) : 0xabc00001u + w),
                      "multiple FP store changed wrong partial word w=%u fault=%u", w, fault);
            }
         }
        }
       }
      }
     }
    }
}

static void test_cortex_a8_vfp_bitwise_fetch_and_retry(void) {
    static const uint32_t insns[] = {0xeef7fb00u,0xeef0fb60u,0xeef0fbe0u,0xeef1fb60u};
    static const uint64_t expected[] = {
        UINT64_C(0x3ff0000000000000), UINT64_C(0xfff0000000000001),
        UINT64_C(0x7ff0000000000001), UINT64_C(0x7ff0000000000001)
    };
    for (unsigned host = 0; host < 2u; host++)
     for (unsigned op = 0; op < 4u; op++)
      for (unsigned fault = 0; fault < 4u; fault++) {
        memset(g_ram, 0, sizeof g_ram);
        arm_bus_t bus = g_bus; if (host) bus.host_ram = m_host_ram;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u; c.cp15.cpacr = 0x00f00000u;
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N | test_it_bits(0x1cu);
        c.vfp_fpscr = 0x4bc00080u; c.vfp_fpexc = fault ? 0u : ARM_FPEXC_EN;
        c.a8_vfp_hi[0] = UINT64_C(0xfff0000000000001);
        c.a8_vfp_hi[15] = UINT64_C(0x123456789abcdef0);
        c.r[15] = 0xffeu;
        uint32_t flags = c.cpsr;
        m_w32(NULL, 0x4000u, 0x6001u); m_w32(NULL, 0x6000u, 0x8032u);
        m_w32(NULL, 0x6004u, fault == 1u ? 0u : fault == 2u ? 0xa033u : fault == 3u ? 0xa012u : 0xa032u);
        m_w16(NULL, 0x8ffeu, (uint16_t)(insns[op] >> 16));
        m_w16(NULL, 0xa000u, (uint16_t)insns[op]);
        m_w16(NULL, 0x9000u, 0u); /* unrelated physical neighbor */
        CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u, "Thumb raw FP fetch disposition");
        CHECK(c.vfp_fpscr == 0x4bc00080u && c.a8_vfp_hi[0] == UINT64_C(0xfff0000000000001),
              "Thumb raw FP fetch changed controls/source");
        if (!fault) {
            CHECK(c.r[15] == 0x1002u && c.a8_vfp_hi[15] == expected[op] &&
                  c.cpsr == ((flags & ~TEST_IT_MASK) | test_it_bits(0x18u)),
                  "Thumb raw FP used physical neighbor or lost IT retirement");
        } else {
            CHECK(c.r[15] == ARM_VEC_PREFETCH && c.r[14] == 0x1002u && c.cp15.ifar == 0x1000u &&
                  c.spsr[ARM_BANK_ABT] == flags && !(c.cpsr & (ARM_CPSR_T | TEST_IT_MASK)) &&
                  c.a8_vfp_hi[15] == UINT64_C(0x123456789abcdef0) && c.vfp_fpexc == 0u &&
                  (c.cp15.ifsr & 15u) == (fault == 1u ? ARM_FSR_PAGE_TRANSLATION : ARM_FSR_PAGE_PERMISSION),
                  "Thumb raw FP availability/effects preceded second-half fetch");
        }
      }
    for (unsigned op = 0; op < 4u; op++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_Z | ARM_CPSR_Q;
        c.cp15.cpacr = 0x00f00000u;
        c.a8_vfp_hi[0] = UINT64_C(0xfff0000000000001);
        c.r[15] = 0x100u; c.r[5] = ARM_FPEXC_EN; c.r[2] = 0x12345678u;
        m_w16(NULL, 0x100u, 0xbf04u); /* ITT EQ */
        m_w16(NULL, 0x102u, (uint16_t)(insns[op] >> 16));
        m_w16(NULL, 0x104u, (uint16_t)insns[op]);
        m_w16(NULL, 0x106u, 0x2201u); /* MOVS r2,#1, second EQ slot */
        put_vfp_system_transfer(0u, ARM_VEC_UNDEFINED, 0u, 8u, 5u);
        m_w32(NULL, 8u, 0xe25ef002u); /* SUBS pc,lr,#2 */
        CHECK(arm_step(&c) == ARM_OK, "raw FP retry IT setup");
        uint32_t interrupted = c.cpsr;
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == 0x104u &&
              c.spsr[ARM_BANK_UND] == interrupted && c.a8_vfp_hi[15] == 0u, "raw FP lazy exception");
        CHECK(arm_step(&c) == ARM_OK && c.vfp_fpexc == ARM_FPEXC_EN, "raw FP handler enable");
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x102u && c.cpsr == interrupted, "raw FP exception return");
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x106u && c.a8_vfp_hi[15] == expected[op] &&
              c.cpsr == ((interrupted & ~TEST_IT_MASK) | test_it_bits(0x08u)), "raw FP exact retry/IT retirement");
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x108u && c.r[2] == 1u &&
              c.cpsr == (interrupted & ~TEST_IT_MASK) && c.cycles == 6u, "raw FP changed following IT condition");
    }
}

static void test_cortex_a8_vfp_fetch_and_refusals(void) {
    for (unsigned host = 0; host < 2u; host++) {
     for (unsigned load = 0; load < 2u; load++) {
      for (unsigned fault = 0; fault < 4u; fault++) {
        memset(g_ram, 0, sizeof g_ram);
        arm_bus_t bus = g_bus; if (host) bus.host_ram = m_host_ram;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u; c.cp15.cpacr = 0x00f00000u;
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N | test_it_bits(0x1cu);
        c.vfp_fpscr = 0x48000080u; c.vfp_fpexc = fault ? 0u : ARM_FPEXC_EN;
        c.r[15] = 0xffeu; c.r[4] = 0x98000095u;
        uint32_t flags = c.cpsr;
        m_w32(NULL, 0x4000u, 0x6001u); m_w32(NULL, 0x6000u, 0x8032u);
        m_w32(NULL, 0x6004u, fault == 1u ? 0u : fault == 2u ? 0xa033u : fault == 3u ? 0xa012u : 0xa032u);
        m_w16(NULL, 0x8ffeu, (uint16_t)(0xeee1u | (load << 4)));
        m_w16(NULL, 0xa000u, 0x4a10u); m_w16(NULL, 0x9000u, 0x5a10u); /* wrong physical neighbor */
        CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u, "Thumb FP fetch disposition");
        if (!fault) {
            CHECK(c.r[15] == 0x1002u && c.r[4] == (load ? 0x48000080u : 0x98000095u) &&
                  c.vfp_fpscr == (load ? 0x48000080u : 0x98000095u) &&
                  c.cpsr == ((flags & ~TEST_IT_MASK) | test_it_bits(0x18u)),
                  "Thumb FP lost noncontiguous second-half fetch or IT retirement");
        } else {
            CHECK(c.r[15] == ARM_VEC_PREFETCH && c.r[14] == 0x1002u &&
                  c.cp15.ifar == 0x1000u && c.spsr[ARM_BANK_ABT] == flags &&
                  !(c.cpsr & (ARM_CPSR_T | TEST_IT_MASK)) &&
                  c.vfp_fpscr == 0x48000080u && c.vfp_fpexc == 0u && c.r[4] == 0x98000095u &&
                  (c.cp15.ifsr & 15u) == (fault == 1u ? ARM_FSR_PAGE_TRANSLATION : ARM_FSR_PAGE_PERMISSION),
                  "Thumb FP effects/availability preceded complete instruction fetch");
        }
      }
     }
    }
    /* Reserved VMRS/VMSR bits must not reach a backed register. Refusals
     * stay capability stops even with EN=0; a failed condition suppresses
     * the whole transfer, including the availability/permission check. */
    static const unsigned reserved[] = {0,1,2,3,5,6,7};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned enabled = 0; enabled < 2u; enabled++) {
      for (unsigned skip = 0; skip < 2u; skip++) {
       for (unsigned load = 0; load < 2u; load++) {
        for (unsigned n = 0; n <= sizeof reserved / sizeof reserved[0]; n++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr = ARM_MODE_SVC | ARM_CPSR_N |
                     (thumb ? ARM_CPSR_T | test_it_bits(skip ? 0x08u : 0x18u) : 0u);
            c.cp15.cpacr = skip ? 0u : 0x00f00000u;
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            c.vfp_fpscr = 0x48000080u; c.r[4] = 0u; c.r[15] = 0x100u;
            uint32_t flags = c.cpsr;
            uint32_t insn = 0xeee84a10u | (load << 20); /* FPEXC valid with either EN */
            bool valid = n == sizeof reserved / sizeof reserved[0];
            if (!valid) insn |= 1u << reserved[n];
            if (thumb) { m_w16(NULL, 0x100u, (uint16_t)(insn >> 16)); m_w16(NULL, 0x102u, (uint16_t)insn); }
            else m_w32(NULL, 0x100u, skip ? insn & 0x0fffffffu : insn); /* EQ false */
            CHECK(arm_step(&c) == (skip || valid ? ARM_OK : ARM_UNDEFINED) &&
                  c.r[15] == (skip || valid ? 0x104u : 0x100u) &&
                  c.cpsr == (thumb && (skip || valid) ? flags & ~TEST_IT_MASK : flags) &&
                  c.vfp_fpscr == 0x48000080u &&
                  c.vfp_fpexc == (!skip && valid && !load ? 0u : enabled ? ARM_FPEXC_EN : 0u) &&
                  c.r[4] == (!skip && valid && load && enabled ? ARM_FPEXC_EN : 0u),
                  "FP reserved/conditional transfer T=%u EN=%u skip=%u L=%u n=%u", thumb, enabled, skip, load, n);
        }
       }
      }
     }
    }
    const uint32_t neighbors[] = {0xeef14b10u,0xee514b10u,0xeef14aa0u,0xfef14a10u};
    for (unsigned n = 0; n < sizeof neighbors / sizeof neighbors[0]; n++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        c.vfp_fpscr = 0x48000080u; c.r[4] = 0x12345678u;
        m_w16(NULL, 0u, (uint16_t)(neighbors[n] >> 16)); m_w16(NULL, 2u, (uint16_t)neighbors[n]);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[4] == 0x12345678u && c.r[15] == 0u &&
              c.vfp_fpscr == 0x48000080u, "Thumb system transfer aliased unrelated encoding n=%u", n);
    }
    /* The shared A32 VMOV remains a data-register transfer, not FPSCR. */
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
    c.vfp_fpscr = 0x48000080u; c.vfp_s[2] = 0x12345678u;
    m_w32(NULL, 0u, 0xee114a10u);
    CHECK(arm_step(&c) == ARM_OK && c.r[4] == 0x12345678u && c.vfp_fpscr == 0x48000080u,
          "A8 VFP system decoder swallowed VMOV r4,s2");
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned n = 0; n < sizeof legacy / sizeof legacy[0]; n++) {
        CHECK(arm_reset_profile(&c, &g_bus, legacy[n]), "reset");
        c.cpsr = ARM_MODE_USR; c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        put_vfp_system_transfer(0u, 0u, 1u, 0u, 4u);
        CHECK(arm_step(&c) == ARM_OK && c.r[4] == ARM1176_FPSID, "A8 FPSID permission changed legacy VFP");
        c.cpsr = ARM_MODE_SVC; c.r[4] = UINT32_MAX;
        put_vfp_system_transfer(0u, 4u, 0u, 1u, 4u);
        put_vfp_system_transfer(0u, 8u, 0u, 8u, 4u);
        CHECK(arm_step(&c) == ARM_OK && c.vfp_fpscr == 0xf3f79f9fu, "A8 FPSCR fields changed legacy VFP");
        CHECK(arm_step(&c) == ARM_OK && c.vfp_fpexc == UINT32_MAX, "A8 FPEXC fields changed legacy VFP");
    }
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_SWIFT), "reset");
    c.cpsr |= ARM_CPSR_T; c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
    put_vfp_system_transfer(1u, 0u, 1u, 1u, 4u);
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u, "A8 Thumb VFP enabled unaudited Swift path");
}

typedef struct wfi_probe {
    arm_cpu_t *cpu;
    unsigned calls;
    bool irq, fiq, result;
    uint32_t seen_pc, seen_cpsr;
} wfi_probe_t;

static bool wake_wfi(void *ctx) {
    wfi_probe_t *p = ctx;
    p->calls++;
    p->seen_pc = p->cpu->r[15]; p->seen_cpsr = p->cpu->cpsr;
    if (p->irq) p->cpu->irq_line = true;
    if (p->fiq) p->cpu->fiq_line = true;
    m_w32(NULL, 0x100u, 0x12345678u);
    return p->result;
}

static unsigned put_wfi(unsigned encoding, uint32_t pc) {
    if (!encoding) m_w32(NULL, pc, 0xe320f003u); /* A1 */
    else if (encoding == 1u) m_w16(NULL, pc, 0xbf30u); /* T1 */
    else { m_w16(NULL, pc, 0xf3afu); m_w16(NULL, pc + 2u, 0x8003u); } /* T2 */
    return encoding == 1u ? 2u : 4u;
}

static void test_cortex_a8_wfi(void) {
    /* DDI0406C.b A8.8.425/B1.8.14: all three encodings permit User mode;
     * pending IRQ/FIQ wakes regardless of CPSR masks. DDI0344K3.2.26
     * ACTLR.WFINOP bypasses the wait. A missing/nonprogressing hook returns. */
    for (unsigned encoding = 0; encoding < 3u; encoding++) {
     for (unsigned user = 0; user < 2u; user++) {
      for (unsigned pending = 0; pending < 4u; pending++) {
       for (unsigned nop = 0; nop < 2u; nop++) {
        for (unsigned hook = 0; hook < 3u; hook++) {
            memset(g_ram, 0, sizeof g_ram);
            arm_cpu_t c;
            wfi_probe_t p = {.cpu = &c, .result = hook == 2u};
            arm_bus_t bus = g_bus; bus.ctx = &p;
            bus.wait_for_interrupt = hook ? wake_wfi : NULL;
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_I | ARM_CPSR_F |
                     ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q | (encoding ? ARM_CPSR_T : 0u);
            c.cp15.actlr |= nop << 8;
            c.irq_line = (pending & 1u) != 0u; c.fiq_line = (pending & 2u) != 0u;
            c.r[0] = 0xabcdef01u; c.r[14] = 0xaabbccddu; c.excl_valid = true; c.excl_addr = 0x200u;
            uint32_t flags = c.cpsr;
            unsigned width = put_wfi(encoding, 0), calls = hook && !nop && !pending ? 1u : 0u;
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == width && c.cycles == 1u && p.calls == calls,
                  "WFI did not wait/retire correctly encoding=%u user=%u pending=%u nop=%u hook=%u calls=%u",
                  encoding, user, pending, nop, hook, p.calls);
            CHECK(c.cpsr == flags && c.r[0] == 0xabcdef01u && c.r[14] == 0xaabbccddu &&
                  c.excl_valid && c.excl_addr == 0x200u && m_r32(NULL, 0x100u) == (calls ? 0x12345678u : 0u),
                  "WFI changed CPU state or lost synchronous platform work");
            CHECK(!calls || (p.seen_pc == 0u && p.seen_cpsr == flags), "WFI callback observed premature retirement");
        }
       }
      }
     }
     for (unsigned pass = 0; pass < 2u; pass++) {
        arm_cpu_t c;
        wfi_probe_t p = {.cpu = &c};
        arm_bus_t bus = g_bus; bus.ctx = &p; bus.wait_for_interrupt = wake_wfi;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_N | (pass ? ARM_CPSR_Z : 0u) | (encoding ? ARM_CPSR_T : 0u);
        uint32_t flags = c.cpsr;
        unsigned width = put_wfi(encoding, encoding ? 2u : 0u);
        if (encoding) {
            m_w16(NULL, 0, 0xbf0cu); /* ITE EQ; WFI may precede the last slot. */
            CHECK(arm_step(&c) == ARM_OK && (c.cpsr & TEST_IT_MASK) == test_it_bits(0x0cu), "ITE setup");
        } else m_w32(NULL, 0, 0x0320f003u); /* WFIEQ */
        CHECK(arm_step(&c) == ARM_OK && p.calls == pass && c.r[15] == width + (encoding ? 2u : 0u) &&
              c.cpsr == (flags | (encoding ? test_it_bits(0x18u) : 0u)),
              "conditional WFI lost condition/IT progression encoding=%u pass=%u", encoding, pass);
     }
     for (unsigned fiq = 0; fiq < 2u; fiq++) {
      for (unsigned masked = 0; masked < 2u; masked++) {
        arm_cpu_t c;
        wfi_probe_t p = {.cpu = &c, .irq = !fiq, .fiq = fiq != 0u, .result = true};
        arm_bus_t bus = g_bus; bus.ctx = &p; bus.wait_for_interrupt = wake_wfi;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = ARM_MODE_USR | ARM_CPSR_N | ARM_CPSR_C | (encoding ? ARM_CPSR_T | test_it_bits(0x1cu) : 0u) |
                 (masked ? ARM_CPSR_I | ARM_CPSR_F : 0u);
        uint32_t flags = c.cpsr;
        uint32_t retired_flags = encoding ? (flags & ~TEST_IT_MASK) | test_it_bits(0x18u) : flags;
        unsigned width = put_wfi(encoding, 0);
        if (encoding) m_w16(NULL, width, 0x235au); else m_w32(NULL, width, 0xe3a0305au);
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == width && c.cpsr == retired_flags && p.calls == 1u &&
              p.seen_cpsr == flags && m_r32(NULL, 0x100u) == 0x12345678u,
              "WFI lost wake event/IT progression or vectored before retirement");
        CHECK(arm_step(&c) == ARM_OK, "first step after WFI failed");
        if (masked) {
            CHECK(c.r[3] == 0x5au && c.r[15] == width + (encoding ? 2u : 4u) &&
                  (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR, "masked WFI wake did not resume the next instruction");
        } else {
            CHECK(c.r[15] == (fiq ? ARM_VEC_FIQ : ARM_VEC_IRQ) && c.r[14] == width + 4u &&
                  c.spsr[fiq ? ARM_BANK_FIQ : ARM_BANK_IRQ] == retired_flags && !(c.cpsr & (ARM_CPSR_T | TEST_IT_MASK)),
                  "WFI wake lost interrupt vector/return link/Thumb state");
        }
      }
     }
    }
    /* Existing profile paths remain separate: A32 used a no-op and Swift
     * Thumb WFI remains unsupported. ARM1176 keeps its CP15 wait tests. */
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++) {
        unsigned calls = 0;
        arm_bus_t bus = g_bus; bus.ctx = &calls; bus.wait_for_interrupt = count_wfi;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, legacy[profile]), "reset");
        put_wfi(0, 0);
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && !calls, "A8 WFI changed legacy A32 behavior");
        for (unsigned encoding = 1; encoding < 3u; encoding++) {
            CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_SWIFT), "reset"); c.cpsr |= ARM_CPSR_T;
            put_wfi(encoding, 0);
            CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && !calls, "A8 WFI leaked into Swift Thumb");
        }
    }
    /* Nearby hints/reserved bits must not acquire a WFI platform side effect. */
    const uint32_t a32_neighbors[] = {0xe320f000u,0xe320f001u,0xe320f002u,0xe320f004u,0xe320f013u,0xe320f103u};
    const uint32_t t32_neighbors[] = {0xf3af8000u,0xf3af8001u,0xf3af8002u,0xf3af8004u,0xf3af8013u,0xf3af8103u};
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
     for (unsigned i = 0; i < sizeof a32_neighbors / sizeof a32_neighbors[0]; i++) {
        unsigned calls = 0;
        arm_bus_t bus = g_bus; bus.ctx = &calls; bus.wait_for_interrupt = count_wfi;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        if (thumb) {
            c.cpsr |= ARM_CPSR_T;
            m_w16(NULL, 0, (uint16_t)(t32_neighbors[i] >> 16)); m_w16(NULL, 2, (uint16_t)t32_neighbors[i]);
        } else m_w32(NULL, 0, a32_neighbors[i]);
        (void)arm_step(&c);
        CHECK(!calls, "neighbor instruction unexpectedly invoked WFI hook thumb=%u index=%u", thumb, i);
     }
    }
}

static void test_cortex_a8_actlr_guest_wait_control(void) {
    /* Program the control through real guest CP15 writes. No direct ACTLR
     * mutation between WFI operations may supply the intended behavior. */
    for (unsigned encoding = 0; encoding < 3u; encoding++) {
        unsigned calls = 0;
        arm_bus_t bus = g_bus; bus.ctx = &calls; bus.wait_for_interrupt = count_wfi;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_N | ARM_CPSR_C | (encoding ? ARM_CPSR_T : 0u);
        c.r[4] = 0x102u; c.r[5] = 2u;
        uint32_t flags = c.cpsr;
        unsigned width = put_wfi(encoding, 4u);
        if (encoding) {
            m_w16(NULL, 0, 0xee01u); m_w16(NULL, 2, 0x4f30u);
            m_w16(NULL, 4u + width, 0xee01u); m_w16(NULL, 6u + width, 0x5f30u);
        } else {
            m_w32(NULL, 0, 0xee014f30u); m_w32(NULL, 4u + width, 0xee015f30u);
        }
        put_wfi(encoding, 8u + width);
        CHECK(arm_step(&c) == ARM_OK && c.cp15.actlr == 0x102u && !calls, "guest WFINOP enable");
        CHECK(arm_step(&c) == ARM_OK && !calls && c.r[15] == 4u + width, "WFINOP did not bypass wait");
        CHECK(arm_step(&c) == ARM_OK && c.cp15.actlr == 2u && !calls, "guest WFINOP clear");
        CHECK(arm_step(&c) == ARM_OK && calls == 1u && c.r[15] == 8u + 2u * width && c.cpsr == flags,
              "guest ACTLR write did not restore WFI wait/retirement");
    }
    /* A refused CP15 write in an active IT block cannot partly change the
     * wait policy, consume the slot or suppress the next valid operation. */
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T | ARM_CPSR_Z;
    c.r[4] = 0x106u;
    m_w16(NULL, 0, 0xbf0cu); m_w16(NULL, 2, 0xee01u); m_w16(NULL, 4, 0x4f30u);
    CHECK(arm_step(&c) == ARM_OK, "ACTLR IT setup");
    uint32_t flags = c.cpsr;
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 2u && c.cpsr == flags && c.cp15.actlr == 2u,
          "reserved ACTLR write changed IT or wait state before refusal");
    c.r[4] = 0x102u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 6u && c.cp15.actlr == 0x102u &&
          c.cpsr == ((flags & ~TEST_IT_MASK) | test_it_bits(0x18u)), "valid ACTLR retry lost IT retirement");
}

static void test_cortex_a8_wfi_fetch_boundary(void) {
    for (unsigned host = 0; host < 2u; host++) {
     for (unsigned fault = 0; fault < 4u; fault++) {
        memset(g_ram, 0, sizeof g_ram);
        unsigned calls = 0;
        arm_bus_t bus = g_bus; bus.ctx = &calls; bus.wait_for_interrupt = count_wfi;
        if (host) bus.host_ram = m_host_ram;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr |= ARM_SCTLR_M; c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N | test_it_bits(0x1cu); /* NE passes */
        c.r[15] = 0xffeu;
        uint32_t flags = c.cpsr;
        m_w32(NULL, 0x4000u, 0x6001u); m_w32(NULL, 0x6000u, 0x8032u);
        m_w32(NULL, 0x6004u, fault == 1u ? 0u : fault == 2u ? 0xa033u : fault == 3u ? 0xa012u : 0xa032u);
        m_w16(NULL, 0x8ffeu, 0xf3afu); m_w16(NULL, 0xa000u, 0x8003u); m_w16(NULL, 0x9000u, 0x8000u);
        CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u && calls == (fault ? 0u : 1u),
              "WFI hook ran before complete instruction fetch or missed nonadjacent backing");
        if (!fault) {
            CHECK(c.r[15] == 0x1002u && c.cpsr == ((flags & ~TEST_IT_MASK) | test_it_bits(0x18u)),
                  "wide WFI lost cross-page PC/IT progression");
        } else {
            CHECK(c.r[15] == ARM_VEC_PREFETCH && c.r[14] == 0x1002u && c.cp15.ifar == 0x1000u &&
                  c.spsr[ARM_BANK_ABT] == flags && !(c.cpsr & TEST_IT_MASK) &&
                  (c.cp15.ifsr & 15u) == (fault == 1u ? ARM_FSR_PAGE_TRANSLATION : ARM_FSR_PAGE_PERMISSION),
                  "wide WFI fetch failure lost precise abort/IT state");
        }
     }
    }
}

static void test_thumb_it_encodings_and_sequences(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 3; profile++) {
     for (unsigned cond = 0; cond < 16; cond++) {
      arm_cpu_t hint;
      CHECK(arm_reset_profile(&hint, &g_bus, profiles[profile]), "reset");
      hint.cpsr |= ARM_CPSR_T | ARM_CPSR_N;
      uint32_t before_hint = hint.cpsr;
      m_w16(NULL, 0, (uint16_t)(0xbf00u | (cond << 4)));
      bool nop = (profile != 0u && cond == 0u) || (profile == 1u && cond == 3u);
      CHECK(arm_step(&hint) == (nop ? ARM_OK : ARM_UNDEFINED) && hint.cpsr == before_hint &&
            hint.r[15] == (nop ? 2u : 0u), "zero IT mask did not select the separate hint space");
      for (unsigned mask = 1; mask < 16; mask++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[profile]), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V;
        uint32_t cpsr = c.cpsr;
        bool valid = profile != 0u && cond != 15u && (cond != 14u || (mask & (mask - 1u)) == 0u);
        m_w16(NULL, 0, (uint16_t)(0xbf00u | (cond << 4) | mask));
        CHECK(arm_step(&c) == (valid ? ARM_OK : ARM_UNDEFINED) && c.cycles == 1u &&
              c.r[15] == (valid ? 2u : 0u) &&
              c.cpsr == (cpsr | (valid ? test_it_bits((cond << 4) | mask) : 0u)),
              "IT encoding cond=%u mask=%u profile=%u changed flags or wrong state", cond, mask, profile);
      }
     }
    }
    const uint8_t invalid_states[] = {0x10u, 0xf8u, 0xe3u, 0x60u};
    for (unsigned i = 0; i < sizeof invalid_states; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | test_it_bits(invalid_states[i]);
        uint32_t before = c.cpsr;
        m_w16(NULL, 0, 0x2001u);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.r[0] == 0u && c.cpsr == before,
              "reserved restored IT state executed an instruction");
    }
    /* Explicit state sequences from the IT encoding table, not the core's
     * shift helper. Executed MOVs must not disturb the condition flags. */
    static const struct { uint16_t insn; unsigned count; uint8_t state[5], execute; uint32_t flags; } cases[] = {
        {0xbf0cu, 2u, {0x0cu,0x18u,0}, 1u, ARM_CPSR_Z}, /* ITE EQ: T,F */
        {0xbf0cu, 2u, {0x0cu,0x18u,0}, 2u, 0u},         /* ITE EQ: F,T */
        {0xbf04u, 2u, {0x04u,0x08u,0}, 3u, ARM_CPSR_Z}, /* ITT EQ */
        {0xbf1au, 3u, {0x1au,0x14u,0x08u,0}, 3u, 0u},  /* ITTE NE */
        {0xbfa7u, 4u, {0xa7u,0xaeu,0xbcu,0xb8u,0}, 3u, 0u}, /* ITTEE GE */
        {0xbfe1u, 4u, {0xe1u,0xe2u,0xe4u,0xe8u,0}, 15u, ARM_CPSR_N | ARM_CPSR_Z},
    };
    for (unsigned p = 1; p < 3; p++) {
     for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
        c.cpsr = ARM_MODE_SVC | ARM_CPSR_T | ARM_CPSR_C | cases[i].flags;
        uint32_t cpsr = c.cpsr;
        m_w16(NULL, 0, cases[i].insn);
        CHECK(arm_step(&c) == ARM_OK && c.cpsr == (cpsr | test_it_bits(cases[i].state[0])), "IT start");
        for (unsigned slot = 0; slot < cases[i].count; slot++) {
            c.r[slot] = 0xdeadbeefu;
            m_w16(NULL, 2u + slot * 2u, (uint16_t)(0x2001u | (slot << 8)));
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u + slot * 2u && c.cycles == slot + 2u &&
                  c.r[slot] == ((cases[i].execute & (1u << slot)) ? 1u : 0xdeadbeefu) &&
                  c.cpsr == (cpsr | test_it_bits(cases[i].state[slot + 1u])),
                  "IT sequence %u slot %u has wrong execution, flags or next state", i, slot);
        }
     }
    }
}

static void test_thumb_it_narrow_flags(void) {
    const uint16_t implicit[] = {
        0x0048u, 0x0848u, 0x1048u, 0x1888u, 0x1a88u, 0x2000u, 0x3001u, 0x3801u,
        0x4008u, 0x4048u, 0x4088u, 0x40c8u, 0x4108u, 0x4148u, 0x4188u, 0x41c8u,
        0x4248u, 0x4308u, 0x4348u, 0x4388u, 0x43c8u,
    };
    const uint16_t explicit_flags[] = {0x2800u, 0x4208u, 0x4288u, 0x42c8u}; /* CMP/TST/CMN */
    for (unsigned kind = 0; kind < 2; kind++) {
     unsigned count = kind ? sizeof explicit_flags / sizeof explicit_flags[0] : sizeof implicit / sizeof implicit[0];
     for (unsigned i = 0; i < count; i++) {
        arm_cpu_t ordinary, inside;
        CHECK(arm_reset_profile(&ordinary, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        ordinary.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V;
        ordinary.r[0] = 0x80000000u; ordinary.r[1] = 1u; ordinary.r[2] = 0x7fffffffu;
        inside = ordinary;
        inside.cpsr |= test_it_bits(0xe8u); /* one AL slot */
        uint32_t flags = ordinary.cpsr;
        m_w16(NULL, 0, kind ? explicit_flags[i] : implicit[i]);
        CHECK(arm_step(&ordinary) == ARM_OK && arm_step(&inside) == ARM_OK &&
              memcmp(ordinary.r, inside.r, sizeof ordinary.r) == 0 &&
              inside.cpsr == (kind ? ordinary.cpsr : flags),
              "IT narrow opcode %04x changed implicit flags or suppressed explicit flags",
              kind ? explicit_flags[i] : implicit[i]);
     }
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T | ARM_CPSR_Z;
    m_w16(NULL, 0, 0xbf06u); /* ITTE EQ */
    m_w16(NULL, 2, 0xf05fu); m_w16(NULL, 4, 0x0001u); /* MOVS.W r0,#1 changes Z */
    m_w16(NULL, 6, 0x2102u); m_w16(NULL, 8, 0x2203u);
    CHECK(arm_step(&c) == ARM_OK && arm_step(&c) == ARM_OK && !(c.cpsr & ARM_CPSR_Z) &&
          arm_step(&c) == ARM_OK && arm_step(&c) == ARM_OK && c.r[0] == 1u &&
          c.r[1] == 0u && c.r[2] == 3u && !(c.cpsr & TEST_IT_MASK) && c.r[15] == 10u,
          "wide explicit flags did not affect later conditions in the same IT block");
}

static void test_thumb_it_exceptions(void) {
    /* Retry state is saved for IRQ/FIQ and data abort; SVC saves the next
     * slot. Existing A32 exception returns must restore the complete state. */
    for (unsigned exception = 0; exception < 4; exception++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_Z | test_it_bits(0x04u);
        c.r[15] = 0x100u; c.r[0] = 0x301u;
        uint32_t old = c.cpsr;
        unsigned bank = exception == 0 ? ARM_BANK_IRQ : exception == 1 ? ARM_BANK_FIQ :
                        exception == 2 ? ARM_BANK_ABT : ARM_BANK_SVC;
        uint32_t vector = exception == 0 ? ARM_VEC_IRQ : exception == 1 ? ARM_VEC_FIQ :
                          exception == 2 ? ARM_VEC_DATA_ABORT : ARM_VEC_SWI;
        c.irq_line = exception == 0; c.fiq_line = exception == 1;
        if (exception == 2) c.cp15.sctlr = ARM_SCTLR_A;
        m_w16(NULL, 0x100u, exception == 3 ? 0xdf00u : exception == 2 ? 0x6801u : 0x2201u);
        uint32_t saved = exception == 3 ? (old & ~TEST_IT_MASK) | test_it_bits(0x08u) : old;
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == vector && c.spsr[bank] == saved &&
              !(c.cpsr & TEST_IT_MASK) && !(c.cpsr & ARM_CPSR_T),
              "IT exception %u saved or cleared the wrong state", exception);
        c.irq_line = false; c.fiq_line = false;
        m_w32(NULL, vector, exception == 3 ? 0xe1b0f00eu : exception == 2 ? 0xe25ef008u : 0xe25ef004u);
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == (exception == 3 ? 0x102u : 0x100u) && c.cpsr == saved,
              "IT exception %u return did not restore retry/resume state", exception);
    }
}

static void test_thumb_it_placement_and_skips(void) {
    static const struct { uint16_t first, second; bool wide, never; } restricted[] = {
        {0xbf08u,0,false,true}, {0xb100u,0,false,true}, {0xb672u,0,false,true},
        {0xb650u,0,false,true}, {0xd000u,0,false,true}, {0xf000u,0x8000u,true,true},
        {0xe000u,0,false,false}, {0x4700u,0,false,false}, {0x4780u,0,false,false},
        {0x4487u,0,false,false}, {0x4687u,0,false,false}, {0xbd00u,0,false,false},
        {0xf000u,0xb800u,true,false}, {0xf000u,0xf800u,true,false}, {0xf000u,0xe800u,true,false},
        {0xf8d4u,0xf000u,true,false}, {0xf854u,0xfb04u,true,false}, {0xe8b4u,0x8001u,true,false},
    };
    for (unsigned last = 0; last < 2; last++) {
     for (unsigned i = 0; i < sizeof restricted / sizeof restricted[0]; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | test_it_bits(last ? 0x08u : 0x04u); /* EQ false */
        c.r[4] = 0x300u; c.r[13] = 0x300u; c.r[14] = 0x777u;
        uint32_t before = c.cpsr;
        m_w16(NULL, 0, restricted[i].first); m_w16(NULL, 2, restricted[i].second);
        bool valid = last && !restricted[i].never;
        g_watch_addr = 0x300u; g_watch_reads32 = g_watch_writes32 = 0u;
        CHECK(arm_step(&c) == (valid ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (valid ? (restricted[i].wide ? 4u : 2u) : 0u) &&
              c.cpsr == (valid ? before & ~TEST_IT_MASK : before) &&
              c.r[4] == 0x300u && c.r[13] == 0x300u && c.r[14] == 0x777u &&
              !g_watch_reads32 && !g_watch_writes32,
              "IT placement %u last=%u was accepted illegally or touched data", i, last);
        g_watch_addr = 0xffffffffu;
     }
    }
    static const struct { uint16_t first, second; bool wide; uint32_t target; } branches[] = {
        {0xe001u,0,false,6u}, {0xf000u,0xf801u,true,6u}, {0xf8d4u,0xf000u,true,0x200u},
    };
    for (unsigned i = 0; i < sizeof branches / sizeof branches[0]; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_Z | test_it_bits(0x08u);
        c.r[4] = 0x300u;
        m_w32(NULL, 0x300u, 0x200u); /* aligned ARM target */
        m_w16(NULL, 0, branches[i].first); m_w16(NULL, 2, branches[i].second);
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == branches[i].target && !(c.cpsr & TEST_IT_MASK) &&
              ((c.cpsr & ARM_CPSR_T) != 0u) == (i != 2u),
              "last IT instruction did not branch/interwork with cleared IT");
    }
    for (unsigned taken = 0; taken < 2; taken++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | test_it_bits(0x08u) | (taken ? ARM_CPSR_Z : 0u);
        uint32_t before = c.cpsr;
        m_w16(NULL, 0, 0xf380u); m_w16(NULL, 2, 0x8000u); /* unsupported system operation */
        CHECK(arm_step(&c) == (taken ? ARM_UNDEFINED : ARM_OK) &&
              c.r[15] == (taken ? 0u : 4u) && c.cpsr == (taken ? before : before & ~TEST_IT_MASK),
              "condition-failed undefined instruction policy is inconsistent");
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T | test_it_bits(0x08u); /* EQ false still cannot suppress BKPT. */
    m_w16(NULL, 0, 0xbe00u);
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && (c.cpsr & TEST_IT_MASK),
          "false IT condition hid unsupported unconditional BKPT");
}

static void test_thumb_it_fetch_faults(void) {
    for (unsigned host = 0; host < 2; host++) {
     for (unsigned fault = 0; fault < 3; fault++) {
        arm_cpu_t c;
        arm_bus_t bus = g_bus;
        if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
        memset(g_ram, 0, sizeof g_ram);
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | test_it_bits(0x08u); /* EQ false */
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        c.cp15.dfar = 0x777u;
        c.r[15] = 0xffeu; c.r[4] = 0x3000u; /* unmapped data: must not be touched */
        uint32_t before = c.cpsr;
        m_w32(NULL, 0x4000u, 0x6001u); m_w32(NULL, 0x6000u, 0x803eu);
        m_w32(NULL, 0x6004u, fault == 1u ? 0u : fault == 2u ? 0xc03fu : 0xc03eu);
        m_w16(NULL, 0x8ffeu, 0xf8c4u); m_w16(NULL, 0xc000u, 0x2000u);
        CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u && c.cp15.dfar == 0x777u && c.r[4] == 0x3000u,
              "condition-failed store touched data or retired more than once");
        if (!fault) {
            CHECK(c.r[15] == 0x1002u && c.cpsr == (before & ~TEST_IT_MASK),
                  "condition-failed wide instruction did not consume both halfwords and advance IT");
        } else {
            CHECK(c.r[15] == ARM_VEC_PREFETCH && c.r[14] == 0x1002u &&
                  c.cp15.ifar == 0x1000u && c.spsr[ARM_BANK_ABT] == before &&
                  c.cp15.ifsr == (fault == 1u ? ARM_FSR_PAGE_TRANSLATION : ARM_FSR_PAGE_PERMISSION) &&
                  !(c.cpsr & TEST_IT_MASK),
                  "false condition hid a second-half fetch fault or lost retry IT state");
        }
     }
    }
}

static void test_thumb_it_svc_hooks(void) {
    const arm_svc_result_t results[] = {ARM_SVC_HANDLED, ARM_SVC_REDIRECTED, ARM_SVC_UNHANDLED, ARM_SVC_ERROR};
    for (unsigned i = 0; i < 4; i++) {
     for (unsigned taken = 0; taken < 2; taken++) {
        svc_probe_t probe = {0};
        probe.result = results[i]; probe.mutate = true;
        probe.redirect = i == 1u; probe.redirect_pc = 0x200u;
        arm_bus_t bus = g_bus;
        bus.privileged_svc_handler = probe_privileged_svc;
        bus.privileged_svc_ctx = &probe;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        probe.expected_cpu = &c;
        c.cpsr |= ARM_CPSR_T | (taken ? ARM_CPSR_Z : 0u) | test_it_bits(i == 1u ? 0x08u : 0x04u);
        uint32_t before = c.cpsr;
        uint32_t after_it = i == 1u ? 0u : test_it_bits(0x08u);
        m_w16(NULL, 0, 0xdf00u);
        arm_status_t status = arm_step(&c);
        CHECK(probe.calls == taken && (!taken || probe.seen_cpsr == before),
              "conditional SVC called host hook when skipped or exposed advanced IT too soon");
        if (!taken) {
            CHECK(status == ARM_OK && c.r[15] == 2u && c.cycles == 1u && c.r[0] == 0u &&
                  c.cpsr == ((before & ~TEST_IT_MASK) | after_it), "skipped host SVC did not retire normally");
        } else if (i == 3u) {
            CHECK(status == ARM_HALT && c.r[15] == 0u && c.cycles == 0u && c.r[0] == 0u &&
                  c.cpsr == before, "failed host SVC changed IT/CPU state or retired");
        } else if (i == 2u) {
            CHECK(status == ARM_OK && c.r[15] == ARM_VEC_SWI && c.r[0] == 0u &&
                  c.spsr[ARM_BANK_SVC] == ((before & ~TEST_IT_MASK) | after_it) &&
                  !(c.cpsr & TEST_IT_MASK), "unhandled host SVC did not roll back before IT exception entry");
        } else {
            uint32_t expected = ((before ^ ARM_CPSR_N) & ~TEST_IT_MASK) | after_it;
            if (i == 1u) expected &= ~ARM_CPSR_T;
            CHECK(status == ARM_OK && c.r[0] == 0xfeed0001u && c.cycles == 1u &&
                  c.r[15] == (i == 1u ? 0x200u : 2u) && c.cpsr == expected,
                  "consumed host SVC lost changes or advanced IT incorrectly");
        }
     }
    }
}

static void test_thumb2_logical_immediates(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    const unsigned ops[] = {0u,1u,2u,3u,4u,3u,0u,4u}; /* AND,BIC,ORR,ORN,EOR,MVN,TST,TEQ */
    static const struct { uint16_t imm; uint32_t input, result[8]; int carry; } cases[] = {
        {0x000u,0x01234567u,{0u,0x01234567u,0x01234567u,0xffffffffu,0x01234567u,0xffffffffu,0u,0x01234567u},-1},
        {0x1ffu,0xa5a55a5au,{0x00a5005au,0xa5005a00u,0xa5ff5affu,0xffa5ff5au,0xa55a5aa5u,0xff00ff00u,0x00a5005au,0xa55a5aa5u},-1},
        {0x400u,0x80000000u,{0x80000000u,0u,0x80000000u,0xffffffffu,0u,0x7fffffffu,0x80000000u,0u},1},
        {0x480u,0x80000000u,{0u,0x80000000u,0xc0000000u,0xbfffffffu,0xc0000000u,0xbfffffffu,0u,0xc0000000u},0},
    };
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned op = 0; op < 8; op++) {
      for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
       for (unsigned flags = 0; flags < 4; flags++) {
        bool set = op >= 6u || (flags & 1u), carry = (flags & 2u) != 0u;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
        /* One AL slot also verifies that a TST/TEQ alias does not branch,
         * and that explicit wide S behavior survives conditional dispatch. */
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_V | ARM_CPSR_Q |
                  (carry ? ARM_CPSR_C : 0u) | test_it_bits(0xe8u);
        c.r[4] = cases[i].input; c.r[3] = 0xdeadbeefu;
        uint32_t expected_flags = c.cpsr & ~TEST_IT_MASK;
        uint32_t expected = cases[i].result[op];
        if (set) {
            expected_flags &= ~(ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C);
            if (expected & 0x80000000u) expected_flags |= ARM_CPSR_N;
            if (!expected) expected_flags |= ARM_CPSR_Z;
            if (cases[i].carry < 0 ? carry : cases[i].carry != 0) expected_flags |= ARM_CPSR_C;
        }
        unsigned rn = op == 5u ? 15u : 4u, rd = op >= 6u ? 15u : 3u;
        uint16_t first = (uint16_t)(0xf000u | (ops[op] << 5) | (set ? 0x10u : 0u) |
                                   rn | ((cases[i].imm & 0x800u) >> 1));
        uint16_t second = (uint16_t)((rd << 8) | ((cases[i].imm & 0x700u) << 4) | (cases[i].imm & 0xffu));
        m_w16(NULL, 0, first); m_w16(NULL, 2, second);
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u && c.r[4] == cases[i].input &&
              c.r[3] == (op >= 6u ? 0xdeadbeefu : expected) && c.cpsr == expected_flags,
              "logical immediate op=%u case=%u flags=%u has wrong result, NZC or alias", op, i, flags);
       }
      }
     }
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T; c.r[0] = 0x43u;
    m_w16(NULL, 0, 0xf020u); m_w16(NULL, 2, 0x0003u); /* exact kernel BIC.W */
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x40u && c.r[15] == 4u, "BIC.W base/destination alias");

    static const uint16_t bad[][2] = {
        {0xf004u,0x0d01u}, {0xf004u,0x0f01u}, {0xf00du,0x0301u}, {0xf00fu,0x0301u},
        {0xf034u,0x0f01u}, {0xf02fu,0x0301u}, {0xf02du,0x0301u},
        {0xf04du,0x0301u}, {0xf054u,0x0f01u}, {0xf064u,0x0d01u}, {0xf06du,0x0301u},
        {0xf07fu,0x0f01u}, {0xf084u,0x0f01u}, {0xf09du,0x0f01u}, {0xf09fu,0x0f01u},
        {0xf034u,0x1300u}, {0xf06fu,0x2300u}, {0xf094u,0x3f00u}, /* zero replication */
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_C;
        c.r[3] = 0x12345678u; c.r[4] = 0x87654321u; c.r[13] = 0x100u;
        uint32_t before = c.cpsr;
        m_w16(NULL, 0, bad[i][0]); m_w16(NULL, 2, bad[i][1]);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == before &&
              c.r[3] == 0x12345678u && c.r[4] == 0x87654321u && c.r[13] == 0x100u,
              "invalid logical immediate %u changed state", i);
    }
}

static void test_thumb2_doubleword_transfers(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    const unsigned offsets[] = {0u, 4u, 1020u};
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned host = 0; host < 2; host++) {
      for (unsigned load = 0; load < 2; load++) {
       for (unsigned form = 0; form < 8; form++) {
        bool pre = (form & 4u) != 0u, add = (form & 2u) != 0u, wb = (form & 1u) != 0u;
        if (!pre && !wb) continue;
        for (unsigned i = 0; i < 3; i++) {
         for (unsigned big = 0; big < 2; big++) {
            arm_bus_t bus = g_bus;
            if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &bus, profiles[p]), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V | ARM_CPSR_Q |
                      (big ? ARM_CPSR_E : 0u) | test_it_bits(0xe8u);
            c.cp15.sctlr = 0u; /* ARMv7 requires word alignment even with raw U=0. */
            c.r[13] = 0x2004u; c.r[14] = 0x11223344u; c.r[3] = 0x55667788u;
            uint32_t adjusted = add ? 0x2004u + offsets[i] : 0x2004u - offsets[i];
            uint32_t address = pre ? adjusted : 0x2004u, cpsr = c.cpsr & ~TEST_IT_MASK;
            /* Reversed, nonadjacent data registers with LR first; SP base. */
            m_w16(NULL, 0, (uint16_t)(0xe84du | (pre ? 0x100u : 0u) | (add ? 0x80u : 0u) |
                                     (wb ? 0x20u : 0u) | (load ? 0x10u : 0u)));
            m_w16(NULL, 2, (uint16_t)(0xe300u | (offsets[i] / 4u)));
            m_w32(NULL, address, 0xa1b2c3d4u); m_w32(NULL, address + 4u, 0xe5f60718u);
            if (big) {
                CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u &&
                      c.cpsr == (cpsr | test_it_bits(0xe8u)) && c.r[13] == 0x2004u &&
                      c.r[14] == 0x11223344u && c.r[3] == 0x55667788u &&
                      m_r32(NULL, address) == 0xa1b2c3d4u && m_r32(NULL, address + 4u) == 0xe5f60718u,
                      "Unsupported big-endian dual transfer changed memory or CPU state");
                continue;
            }
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
                  c.cpsr == cpsr && c.r[13] == (wb ? adjusted : 0x2004u),
                  "Thumb dual transfer form=%u offset=%u lost writeback, flags or retirement", form, offsets[i]);
            if (load) {
                CHECK(c.r[14] == 0xa1b2c3d4u && c.r[3] == 0xe5f60718u,
                      "LDRD reversed its explicit registers, bytes or word order");
            } else {
                CHECK(m_r32(NULL, address) == 0x11223344u && m_r32(NULL, address + 4u) == 0x55667788u &&
                      c.r[14] == 0x11223344u && c.r[3] == 0x55667788u,
                      "STRD reversed its explicit registers, bytes or word order");
            }
         }
        }
       }
      }
     }
    }
    for (unsigned rn = 0; rn < 3; rn++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[rn] = 0x204u;
        m_w16(NULL, 0, (uint16_t)(0xe9d0u | rn)); m_w16(NULL, 2, 0x0200u);
        m_w32(NULL, 0x204u, 0x12345678u); m_w32(NULL, 0x208u, 0x9abcdef0u);
        CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x12345678u && c.r[2] == 0x9abcdef0u && c.r[15] == 4u,
              "LDRD without writeback lost an aliased base");
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T; c.r[3] = 0x204u;
    m_w16(NULL, 0, 0xe9c3u); m_w16(NULL, 2, 0x3300u); /* STRD permits identical sources. */
    CHECK(arm_step(&c) == ARM_OK && m_r32(NULL, 0x204u) == 0x204u &&
          m_r32(NULL, 0x208u) == 0x204u, "STRD rejected duplicate/base source");
    for (unsigned half = 0; half < 2; half++) {
     for (unsigned add = 0; add < 2; add++) {
      for (unsigned i = 0; i < 2; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[15] = 0x1000u + 2u * half;
        uint32_t offset = i ? 1020u : 4u, address = add ? 0x1004u + offset : 0x1004u - offset;
        m_w16(NULL, c.r[15], (uint16_t)(add ? 0xe9dfu : 0xe95fu));
        m_w16(NULL, c.r[15] + 2u, (uint16_t)(0x3500u | offset / 4u));
        /* Negative small offset would overlap the instruction: use the actual
         * code bytes there as data, without altering the fetched instruction. */
        if (address >= 0x1008u || address + 8u <= 0x1000u) {
            m_w32(NULL, address, 0x76543210u); m_w32(NULL, address + 4u, 0xfedcba98u);
        }
        uint32_t first = m_r32(NULL, address), second = m_r32(NULL, address + 4u);
        CHECK(arm_step(&c) == ARM_OK && c.r[3] == first && c.r[5] == second && c.r[15] == 0x1004u + 2u * half,
              "LDRD literal used wrong PC alignment, offset scale or sign");
      }
     }
    }
    static const uint16_t bad[][2] = {
        {0xe9d4u,0xd100u}, {0xe9d4u,0x0d00u}, {0xe9d4u,0xf100u}, {0xe9d4u,0x0f00u},
        {0xe9d4u,0x1100u}, {0xe9f0u,0x0100u}, {0xe9f1u,0x0100u},
        {0xe9c4u,0xd100u}, {0xe9c4u,0x0d00u}, {0xe9c4u,0xf100u}, {0xe9c4u,0x0f00u},
        {0xe9e0u,0x0100u}, {0xe9e1u,0x0100u}, {0xe9cfu,0x0100u},
        {0xe9ffu,0x0100u}, {0xe8ffu,0x0100u}, /* literal writeback */
        {0xe854u,0x0100u}, {0xe8c4u,0x0100u}, /* P=W=0 belongs to other instructions */
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_C;
        for (unsigned reg = 0; reg < 15; reg++) c.r[reg] = 0x204u;
        m_w16(NULL, 0, bad[i][0]); m_w16(NULL, 2, bad[i][1]);
        g_watch_addr = 0x204u; g_watch_reads32 = g_watch_writes32 = 0u;
        uint32_t before = c.cpsr;
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == before &&
              c.r[0] == 0x204u && c.r[1] == 0x204u && c.r[4] == 0x204u &&
              g_watch_reads32 == 0u && g_watch_writes32 == 0u,
              "Invalid dual transfer %u touched memory/state", i);
        g_watch_addr = 0xffffffffu;
    }
}

static void test_thumb2_doubleword_aborts(void) {
    for (unsigned host = 0; host < 2; host++) {
     for (unsigned load = 0; load < 2; load++) {
      for (unsigned fault = 0; fault < 7; fault++) {
        arm_bus_t bus = g_bus;
        if (host) { bus.host_ram = m_host_ram; bus.host_ram_write = m_host_ram_write; }
        arm_cpu_t c;
        memset(g_ram, 0xee, sizeof g_ram);
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | (fault == 6 ? ARM_SCTLR_A : 0u);
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N | test_it_bits(0x18u);
        c.r[4] = fault >= 5 ? 0x1ff9u : 0x1ff8u;
        c.r[2] = 0x44332211u; c.r[3] = 0x88776655u;
        uint32_t cpsr = c.cpsr, base = c.r[4];
        m_w32(NULL, 0x4000u, 0x6001u);
        m_w32(NULL, 0x6000u, 0x803eu);
        m_w32(NULL, 0x6004u, fault == 1 ? 0u : fault == 3 ? 0xa01eu : 0xa03eu);
        m_w32(NULL, 0x6008u, fault == 2 ? 0u : fault == 4 ? 0xc01eu : 0xc03eu);
        m_w16(NULL, 0x8000u, (uint16_t)(load ? 0xe9f4u : 0xe9e4u));
        m_w16(NULL, 0x8002u, 0x2301u); /* Pre-index +4 with writeback, crossing two mapped frames. */
        m_w32(NULL, 0xaffcu, 0x12345678u); m_w32(NULL, 0xc000u, 0x9abcdef0u);
        g_watch_addr = 0xc000u; g_watch_reads32 = g_watch_writes32 = 0u;
        CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u, "dual transfer did not retire");
        if (fault) {
            uint32_t fsr = fault >= 5 ? ARM_FSR_ALIGNMENT : fault >= 3 ? ARM_FSR_PAGE_PERMISSION : ARM_FSR_PAGE_TRANSLATION;
            uint32_t address = fault >= 5 ? 0x1ffdu : fault == 1 || fault == 3 ? 0x1ffcu : 0x2000u;
            CHECK(c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 8u && c.r[4] == base &&
                  c.r[2] == 0x44332211u && c.r[3] == 0x88776655u && c.spsr[ARM_BANK_ABT] == cpsr &&
                  c.cp15.dfar == address && c.cp15.dfsr == (fsr | (load ? 0u : (1u << 11))) &&
                  !(c.cpsr & TEST_IT_MASK) && g_watch_reads32 == 0u && g_watch_writes32 == 0u,
                  "dual transfer abort lost pair/base, DFAR/WnR, exception IT state or touched second frame");
        } else {
            CHECK(c.r[15] == 4u && c.r[4] == 0x1ffcu && c.cpsr == (cpsr & ~TEST_IT_MASK) &&
                  c.r[2] == (load ? 0x12345678u : 0x44332211u) &&
                  c.r[3] == (load ? 0x9abcdef0u : 0x88776655u),
                  "dual transfer lost noncontiguous frame data or writeback");
        }
        g_watch_addr = 0xffffffffu;
        bool first_written = !load && (fault == 0 || fault == 2 || fault == 4);
        CHECK(m_r32(NULL, 0xaffcu) == (first_written ? 0x44332211u : 0x12345678u) &&
              m_r32(NULL, 0xc000u) == (!load && !fault ? 0x88776655u : 0x9abcdef0u) &&
              m_r32(NULL, 0xb000u) == 0xeeeeeeeeu,
              "dual transfer did not preserve exactly the first completed store before a fault");
      }
     }
    }
}

static void test_thumb2_small_loads(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    static const struct { uint16_t first; uint32_t negative, positive; } kinds[] = {
        {0xf810u,0x80u,0x7fu}, {0xf830u,0x8080u,0x7f7fu},
        {0xf910u,0xffffff80u,0x7fu}, {0xf930u,0xffff8080u,0x7f7fu},
    };
    static const struct { uint16_t high, low; int access_delta, base_delta; } forms[] = {
        {0x80u,0x000u,0,0}, {0x80u,0xfffu,4095,0},
        {0u,0xcffu,-255,0}, {0u,0xfffu,255,255}, {0u,0xd01u,-1,-1},
        {0u,0xb01u,0,1}, {0u,0x9ffu,0,-255},
    };
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned host = 0; host < 2; host++) {
      for (unsigned kind = 0; kind < 4; kind++) {
       for (unsigned form = 0; form < sizeof forms / sizeof forms[0]; form++) {
        for (unsigned positive = 0; positive < 2; positive++) {
            arm_bus_t bus = g_bus;
            if (host) bus.host_ram = m_host_ram;
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &bus, profiles[p]), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V | ARM_CPSR_Q | test_it_bits(0xe8u);
            c.cp15.sctlr = 0u; c.r[13] = 0x10001u; c.r[14] = 0xdeadbeefu;
            uint32_t flags = c.cpsr & ~TEST_IT_MASK;
            m_w16(NULL, 0, (uint16_t)(kinds[kind].first | forms[form].high | 13u));
            m_w16(NULL, 2, (uint16_t)(0xe000u | forms[form].low));
            m_w16(NULL, 0x10001u + (uint32_t)forms[form].access_delta, positive ? 0x7f7fu : 0x8080u);
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u && c.cpsr == flags &&
                  c.r[14] == (positive ? kinds[kind].positive : kinds[kind].negative) &&
                  c.r[13] == 0x10001u + (uint32_t)forms[form].base_delta,
                  "small load kind=%u form=%u lost extension, byte offset, writeback or flags", kind, form);
        }
       }
       for (unsigned halfpc = 0; halfpc < 2; halfpc++) {
        for (unsigned add = 0; add < 2; add++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
            c.cpsr |= ARM_CPSR_T; c.r[15] = 0x2000u + 2u * halfpc;
            m_w16(NULL, c.r[15], (uint16_t)(kinds[kind].first | 15u | (add ? 0x80u : 0u)));
            m_w16(NULL, c.r[15] + 2u, 0x3fffu);
            m_w16(NULL, add ? 0x3003u : 0x1005u, 0x8080u);
            CHECK(arm_step(&c) == ARM_OK && c.r[3] == kinds[kind].negative &&
                  c.r[15] == 0x2004u + 2u * halfpc,
                  "small literal load used wrong PC alignment, sign, offset scale or data width");
        }
       }
       arm_cpu_t c;
       CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
       c.cpsr |= ARM_CPSR_T; c.r[4] = 0x200u;
       m_w16(NULL, 0, (uint16_t)(kinds[kind].first | 0x84u)); m_w16(NULL, 2, 0x4000u);
       m_w16(NULL, 0x200u, 0x8080u);
       CHECK(arm_step(&c) == ARM_OK && c.r[4] == kinds[kind].negative && c.r[15] == 4u,
             "small load without writeback rejected the destination/base alias");
      }
     }
    }
    for (unsigned kind = 0; kind < 4; kind++) {
     for (unsigned form = 0; form < 3; form++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_C; c.r[4] = 0x80010000u;
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP; c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        memset(g_ram + 0x4000u, 0, 0x4000u);
        m_w32(NULL, 0x4000u, 0x6001u); m_w32(NULL, 0x6000u, 0x803eu);
        /* PC destinations in these forms encode PLD/PLDW/PLI or unallocated
         * memory hints. They must not load, fault, branch or clear exclusives. */
        uint16_t first = (uint16_t)(kinds[kind].first | (form == 0 ? 0x84u : form == 1 ? 15u : 4u));
        m_w16(NULL, 0x8000u, first); m_w16(NULL, 0x8002u, form == 2 ? 0xfc20u : 0xffffu);
        c.excl_valid = true;
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cpsr == flags &&
              c.r[4] == 0x80010000u && c.excl_valid && !c.abort_pending,
              "small-load hint alias issued a load or changed architectural state");
     }
     static const uint16_t bad[] = {0xdf01u,0x4f01u,0xff01u,0x3e01u,0x3a01u,0x3801u};
     for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_C; c.r[4] = 0x200u; c.r[3] = 0x12345678u;
        m_w16(NULL, 0, (uint16_t)(kinds[kind].first | 4u)); m_w16(NULL, 2, bad[i]);
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == flags &&
              c.r[4] == 0x200u && c.r[3] == 0x12345678u,
              "small indexed load accepted bad registers/addressing or unsupported unprivileged alias");
     }
     for (unsigned literal = 0; literal < 2; literal++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[4] = 0x200u; c.r[13] = 0x12345678u;
        m_w16(NULL, 0, (uint16_t)(kinds[kind].first | 0x80u | (literal ? 15u : 4u)));
        m_w16(NULL, 2, 0xd020u);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.r[13] == 0x12345678u,
              "small literal/imm12 load accepted SP destination");
     }
     arm_cpu_t c;
     CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
     c.cpsr |= ARM_CPSR_T | ARM_CPSR_E; c.r[4] = 0x200u; c.r[3] = 0x12345678u;
     m_w16(NULL, 0, (uint16_t)(kinds[kind].first | 4u)); m_w16(NULL, 2, 0x3f01u);
     m_w16(NULL, 0x201u, 0x8080u);
     CHECK(arm_step(&c) == ((kind & 1u) ? ARM_UNDEFINED : ARM_OK) &&
           c.r[15] == ((kind & 1u) ? 0u : 4u) && c.r[4] == ((kind & 1u) ? 0x200u : 0x201u) &&
           c.r[3] == ((kind & 1u) ? 0x12345678u : kinds[kind].negative),
           "unsupported big-endian halfword load changed state or byte load depended on endianness");
    }
}

static void test_thumb2_small_load_aborts(void) {
    const uint16_t first[] = {0xf814u,0xf834u,0xf914u,0xf934u};
    const uint32_t expected[] = {0x80u,0xfe80u,0xffffff80u,0xfffffe80u};
    for (unsigned host = 0; host < 2; host++) {
     for (unsigned kind = 0; kind < 4; kind++) {
      for (unsigned fault = 0; fault < 5; fault++) {
        arm_bus_t bus = g_bus;
        if (host) bus.host_ram = m_host_ram;
        arm_cpu_t c;
        memset(g_ram, 0xee, sizeof g_ram);
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP | (fault == 4 ? ARM_SCTLR_A : 0u);
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N | test_it_bits(0x18u);
        c.r[4] = 0x1ffeu; c.r[2] = 0x12345678u;
        uint32_t flags = c.cpsr;
        m_w32(NULL, 0x4000u, 0x6001u); m_w32(NULL, 0x6000u, 0x803eu);
        m_w32(NULL, 0x6004u, fault == 1 ? 0u : 0xa03eu);
        m_w32(NULL, 0x6008u, fault == 2 ? 0u : fault == 3 ? 0xc01eu : 0xc03eu);
        m_w16(NULL, 0x8000u, first[kind]); m_w16(NULL, 0x8002u, 0x2f01u);
        m_w8(NULL, 0xafffu, 0x80u); m_w8(NULL, 0xc000u, 0xfeu);
        bool abort = fault == 1 || ((kind & 1u) && fault);
        CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u, "small indexed load did not retire");
        if (abort) {
            uint32_t fsr = fault == 4 ? ARM_FSR_ALIGNMENT : fault == 3 ? ARM_FSR_PAGE_PERMISSION : ARM_FSR_PAGE_TRANSLATION;
            CHECK(c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 8u && c.r[4] == 0x1ffeu &&
                  c.r[2] == 0x12345678u && c.spsr[ARM_BANK_ABT] == flags && !(c.cpsr & TEST_IT_MASK) &&
                  c.cp15.dfsr == fsr && c.cp15.dfar == (fault == 1 || fault == 4 ? 0x1fffu : 0x2000u),
                  "small load abort lost base/result, exact DFAR or saved IT state");
        } else {
            CHECK(c.r[15] == 4u && c.r[4] == 0x1fffu && c.r[2] == expected[kind] && c.cpsr == (flags & ~TEST_IT_MASK),
                  "small load crossed wrong physical frames or byte load accessed its neighbor");
        }
      }
     }
    }
}

static void test_thumb2_extend(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    /* Source80ff7f01, rotations0/8/16/24. Adding forms use basefffffff0;
     * paired-byte forms wrap each halfword independently. */
    static const uint32_t expected[6][2][4] = {
        {{0x7f01u,0xffffff7fu,0xffff80ffu,0x180u}, {0x7ef1u,0xffffff6fu,0xffff80efu,0x170u}},
        {{0x7f01u,0xff7fu,0x80ffu,0x180u}, {0x7ef1u,0xff6fu,0x80efu,0x170u}},
        {{0xffff0001u,0xff80007fu,0x0001ffffu,0x007fff80u}, {0xfffefff1u,0xff7f006fu,0x0000ffefu,0x007eff70u}},
        {{0x00ff0001u,0x0080007fu,0x000100ffu,0x007f0080u}, {0x00fefff1u,0x007f006fu,0x000000efu,0x007e0070u}},
        {{1u,0x7fu,0xffffffffu,0xffffff80u}, {0xfffffff1u,0x6fu,0xffffffefu,0xffffff70u}},
        {{1u,0x7fu,0xffu,0x80u}, {0xfffffff1u,0x6fu,0xefu,0x70u}},
    };
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned op = 0; op < 6; op++) {
      for (unsigned add = 0; add < 2; add++) {
       for (unsigned rot = 0; rot < 4; rot++) {
        for (unsigned alias = 0; alias < 3; alias++) {
         for (unsigned execute = 0; execute < 2; execute++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V | ARM_CPSR_Q |
                      (0xau << 16) | test_it_bits(execute ? 0x18u : 0x08u);
            uint32_t flags = c.cpsr & ~TEST_IT_MASK;
            unsigned rd = alias == 0 ? 12u : alias == 1 ? 8u : 14u;
            c.r[12] = 0x12345678u; c.r[8] = 0x80ff7f01u; c.r[14] = 0xfffffff0u;
            uint32_t unchanged = c.r[rd];
            m_w16(NULL, 0, (uint16_t)(0xfa00u | (op << 4) | (add ? 14u : 15u)));
            m_w16(NULL, 2, (uint16_t)(0xf080u | (rd << 8) | (rot << 4) | 8u));
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u && c.cpsr == flags &&
                  c.r[rd] == (execute ? expected[op][add][rot] : unchanged),
                  "wide extend op=%u add=%u rot=%u alias=%u lost result, lane isolation or flags/IT", op, add, rot, alias);
         }
        }
       }
      }
      static const unsigned bad_regs[][3] = {{13,0,1},{15,0,1},{0,13,1},{0,14,13},{0,15,15}};
      for (unsigned b = 0; b < sizeof bad_regs / sizeof bad_regs[0]; b++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_Q; c.r[0] = 0x87654321u; c.r[13] = 0x300u;
        m_w16(NULL, 0, (uint16_t)(0xfa00u | (op << 4) | bad_regs[b][1]));
        m_w16(NULL, 2, (uint16_t)(0xf080u | (bad_regs[b][0] << 8) | bad_regs[b][2]));
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.r[0] == 0x87654321u &&
              c.r[13] == 0x300u && c.cpsr == flags, "wide extend accepted forbidden SP/PC register");
      }
     }
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T; c.r[8] = 0xabcdef20u;
    m_w16(NULL, 0, 0xfa5fu); m_w16(NULL, 2, 0xf088u);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x20u && c.r[8] == 0xabcdef20u && c.r[15] == 4u,
          "real kernel UXTB.W blocker failed");
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T; c.r[9] = 0x80ff7f01u;
    m_w16(NULL, 0, 0xfa29u); m_w16(NULL, 2, 0xf989u);
    CHECK(arm_step(&c) == ARM_OK && c.r[9] == 0x80fe7f02u,
          "SXTAB16 with all registers aliased used a partially updated source");
    const uint16_t reserved[][2] = {{0xfa6fu,0xf088u},{0xfa7fu,0xf088u},{0xfa5fu,0xf0c8u},{0xfa5fu,0xe088u}};
    for (unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[0] = 0x12345678u;
        m_w16(NULL, 0, reserved[i][0]); m_w16(NULL, 2, reserved[i][1]);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.r[0] == 0x12345678u,
              "wide extend overmatched reserved bits or neighboring operation");
    }
    arm_reset(&c, &g_bus); c.cpsr |= ARM_CPSR_T; c.r[14] = 0x1000u; c.r[0] = 0x12345678u;
    m_w16(NULL, 0, 0xfa5fu); m_w16(NULL, 2, 0xf088u);
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x14beu && c.r[14] == 3u && c.r[0] == 0x12345678u,
          "ARM1176 no longer uses its legacy 16-bit BL suffix framing");
}

static void write_thumb2_shifted(unsigned op, bool set, unsigned rn, unsigned rd,
                                  unsigned rm, unsigned type, unsigned amount) {
    m_w16(NULL, 0, (uint16_t)(0xea00u | (op << 5) | (set ? 0x10u : 0u) | rn));
    m_w16(NULL, 2, (uint16_t)(((amount >> 2) << 12) | (rd << 8) |
                             ((amount & 3u) << 6) | (type << 4) | rm));
}

static void test_thumb2_shifted_data_processing(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    /* Rn=80000000, Rm=80000003 LSR#1. The shifter carry is1;
     * ADC/SBC must still use the original CPSR.C as their arithmetic input. */
    static const struct { unsigned op; bool move; uint32_t result[2], nzcv; } cases[] = {
        {0,false,{0u,0u},0x70000000u}, {1,false,{0x80000000u,0x80000000u},0xb0000000u},
        {2,false,{0xc0000001u,0xc0000001u},0xb0000000u},
        {3,false,{0xbffffffeu,0xbffffffeu},0xb0000000u},
        {4,false,{0xc0000001u,0xc0000001u},0xb0000000u},
        {8,false,{0xc0000001u,0xc0000001u},0x80000000u},
        {10,false,{0xc0000001u,0xc0000002u},0x80000000u},
        {11,false,{0x3ffffffeu,0x3fffffffu},0x30000000u},
        {13,false,{0x3fffffffu,0x3fffffffu},0x30000000u},
        {14,false,{0xc0000001u,0xc0000001u},0x90000000u},
        {2,true,{0x40000001u,0x40000001u},0x30000000u},
        {3,true,{0xbffffffeu,0xbffffffeu},0xb0000000u},
    };
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      for (unsigned cin = 0; cin < 2; cin++) {
       for (unsigned set = 0; set < 2; set++) {
        for (unsigned dest = 0; dest < 4; dest++) {
         for (unsigned execute = 0; execute < 2; execute++) {
            unsigned op = cases[i].op;
            if (dest == 3 && (!set || cases[i].move || (op != 0 && op != 4 && op != 8 && op != 13))) continue;
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_V | ARM_CPSR_Q |
                      (0x5u << 16) | (cin ? ARM_CPSR_C : 0u) | test_it_bits(execute ? 0x08u : 0x18u);
            uint32_t flags = c.cpsr & ~TEST_IT_MASK;
            unsigned rd = dest == 0 ? 0u : dest == 1 ? 8u : dest == 2 ? 14u : 15u;
            c.r[0] = 0x12345678u; c.r[8] = 0x80000000u; c.r[14] = 0x80000003u;
            uint32_t before[15]; memcpy(before, c.r, sizeof before);
            write_thumb2_shifted(op, set != 0, cases[i].move ? 15u : 8u, rd, 14u, 1u, 1u);
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
                  c.cpsr == (execute && set ? (flags & 0x0fffffffu) | cases[i].nzcv : flags),
                  "shifted ALU op=%u move=%u lost explicit flags, arithmetic carry or IT", op, cases[i].move);
            for (unsigned reg = 0; reg < 15; reg++)
                CHECK(c.r[reg] == (execute && reg == rd ? cases[i].result[cin] : before[reg]),
                      "shifted ALU op=%u wrote wrong result or an aliased source", op);
         }
        }
       }
      }
     }
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T | ARM_CPSR_C; c.r[8] = 0x80324db0u; c.r[4] = 0x12u;
    m_w16(NULL, 0, 0xeb08u); m_w16(NULL, 2, 0x0004u);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x80324dc2u && c.r[15] == 4u && (c.cpsr & ARM_CPSR_C),
          "real ADD.W firmware blocker failed");
    const uint32_t rrx_expected[2][2] = {{1u,0x80000002u},{0xfffffffeu,0x7fffffffu}};
    for (unsigned sub = 0; sub < 2; sub++) for (unsigned cin = 0; cin < 2; cin++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | (cin ? ARM_CPSR_C : 0u); c.r[2] = 2u;
        write_thumb2_shifted(sub ? 11u : 10u, true, 1u, 0u, 2u, 3u, 0u);
        CHECK(arm_step(&c) == ARM_OK && c.r[0] == rrx_expected[sub][cin] && !(c.cpsr & ARM_CPSR_C),
              "RRX consumed/overwrote carry before ADC/SBC arithmetic");
    }
}

static void test_thumb2_immediate_shift_aliases(void) {
    /* Fixed outputs for80000003. carry=2 denotes preservation. */
    static const struct { unsigned type, amount; uint32_t value; unsigned carry; } cases[] = {
        {0,0,0x80000003u,2}, {0,1,6u,1}, {0,4,0x30u,0}, {0,31,0x80000000u,1},
        {1,0,0u,1}, {1,1,0x40000001u,1}, {1,4,0x08000000u,0}, {1,31,1u,0},
        {2,0,0xffffffffu,1}, {2,1,0xc0000001u,1}, {2,4,0xf8000000u,0}, {2,31,0xffffffffu,0},
        {3,0,0x40000001u,1}, {3,1,0xc0000001u,1}, {3,4,0x38000000u,0}, {3,31,7u,0},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
     for (unsigned cin = 0; cin < 2; cin++) {
      for (unsigned invert = 0; invert < 2; invert++) {
       for (unsigned set = 0; set < 2; set++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_V | ARM_CPSR_Z | (cin ? ARM_CPSR_C : 0u);
        uint32_t flags = c.cpsr;
        c.r[14] = 0x80000003u;
        uint32_t expected = cases[i].value;
        if (cases[i].type == 3u && cases[i].amount == 0u) expected |= cin << 31;
        if (invert) expected = ~expected;
        bool carry = cases[i].carry == 2u ? cin != 0u : cases[i].carry != 0u;
        uint32_t nzcv = (expected & ARM_CPSR_N) | (!expected ? ARM_CPSR_Z : 0u) |
                        (carry ? ARM_CPSR_C : 0u) | ARM_CPSR_V;
        write_thumb2_shifted(invert ? 3u : 2u, set != 0u, 15u, 14u, 14u, cases[i].type, cases[i].amount);
        CHECK(arm_step(&c) == ARM_OK && c.r[14] == expected && c.r[15] == 4u &&
              c.cpsr == (set ? (flags & 0x0fffffffu) | nzcv : flags),
              "shift/MVN alias lost zero/32 rule, split shift amount, RRX input, sign or carry");
       }
      }
     }
    }
}

static void test_thumb2_shifted_register_constraints(void) {
    static const struct { unsigned op, set, rn, rd, rm, type, amount; bool valid; uint32_t result; } cases[] = {
        {2,0,15,13,0,0,0,true,4u}, {2,0,15,0,13,0,0,true,0x100u},
        {2,0,15,13,13,0,0,false,0u}, {2,1,15,13,0,0,0,false,0u}, {2,1,15,0,13,0,0,false,0u},
        {2,0,15,13,0,0,1,false,0u}, {2,0,15,0,13,3,0,false,0u}, {3,0,15,13,0,0,0,false,0u},
        {8,0,13,13,0,0,3,true,0x120u}, {8,0,13,13,0,0,4,false,0u}, {8,0,13,13,0,1,1,false,0u},
        {8,1,13,15,0,3,0,true,0u}, {13,0,13,13,0,0,3,true,0xe0u}, {13,1,13,13,0,0,3,true,0xe0u},
        {8,0,13,12,0,1,0,true,0x100u}, {13,0,13,12,0,2,0,true,0x100u},
        {8,0,8,13,0,0,0,false,0u}, {8,0,13,0,13,0,0,false,0u},
        {8,0,15,0,1,0,0,false,0u}, {8,0,1,15,0,0,0,false,0u},
        {0,1,13,15,0,0,0,false,0u}, {4,1,15,15,0,0,0,false,0u},
        {10,0,13,0,2,0,0,false,0u}, {11,0,1,0,15,0,0,false,0u}, {14,1,1,15,0,0,0,false,0u},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_Q | ARM_CPSR_C;
        c.r[0] = 4u; c.r[13] = 0x100u;
        uint32_t flags = c.cpsr, before[15]; memcpy(before, c.r, sizeof before);
        write_thumb2_shifted(cases[i].op, cases[i].set != 0, cases[i].rn, cases[i].rd,
                            cases[i].rm, cases[i].type, cases[i].amount);
        CHECK(arm_step(&c) == (cases[i].valid ? ARM_OK : ARM_UNDEFINED) &&
              c.r[15] == (cases[i].valid ? 4u : 0u), "shifted SP/PC constraints wrong for case%u", i);
        if (cases[i].valid) {
            if (cases[i].rd != 15u) CHECK(c.r[cases[i].rd] == cases[i].result, "SP form result wrong");
            if (!cases[i].set) CHECK(c.cpsr == flags, "non-S SP form changed flags");
        } else CHECK(!memcmp(before, c.r, sizeof before) && c.cpsr == flags, "invalid shifted form changed state");
    }
    for (unsigned op = 0; op < 16; op++) {
        if (op != 5u && op != 6u && op != 7u && op != 9u && op != 12u && op != 15u) continue;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset"); c.cpsr |= ARM_CPSR_T;
        write_thumb2_shifted(op, false, 1u, 0u, 2u, 0u, 0u);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u, "shifted ALU overmatched PKH or unallocated op");
    }
    arm_cpu_t c;
    arm_reset(&c, &g_bus); c.cpsr |= ARM_CPSR_T; c.r[14] = 0x1000u; c.r[0] = 0x12345678u;
    m_w16(NULL, 0, 0xeb08u); m_w16(NULL, 2, 0x0004u);
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x1610u && c.r[14] == 3u &&
          !(c.cpsr & ARM_CPSR_T) && c.r[0] == 0x12345678u, "ARM1176 BLX suffix framing changed");
}

static void test_thumb2_bitfields(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    const uint16_t first[] = {0xf340u,0xf3c0u,0xf360u,0xf36fu}; /* SBFX, UBFX, BFI, BFC */
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned op = 0; op < 4; op++) {
      for (unsigned lsb = 0; lsb < 32; lsb++) {
       for (unsigned encoded = 0; encoded < 32; encoded++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V | ARM_CPSR_Q |
                  (0x9u << 16) | test_it_bits(0x18u);
        uint32_t flags = c.cpsr;
        c.r[14] = 0x80ff7f01u; c.r[9] = 0x5aa55aa5u;
        unsigned rd = ((lsb ^ encoded) & 1u) ? 14u : 9u;
        unsigned rn = op == 3u ? 15u : 14u;
        uint32_t before = c.r[rd], expected = 0u;
        bool valid = op < 2u ? lsb + encoded < 32u : encoded >= lsb;
        unsigned width = op < 2u ? encoded + 1u : encoded - lsb + 1u;
        /* Independent bit-by-bit oracle, including untouched destination
         * bits and signed upper bits. No mask/shift-by-width arithmetic. */
        for (unsigned bit = 0; valid && bit < 32u; bit++) {
            unsigned value;
            if (op < 2u) {
                if (bit < width) value = (c.r[rn] >> (lsb + bit)) & 1u;
                else value = op == 0u ? (c.r[rn] >> (lsb + width - 1u)) & 1u : 0u;
            } else if (bit < lsb || bit > encoded) value = (before >> bit) & 1u;
            else value = op == 3u ? 0u : (c.r[rn] >> (bit - lsb)) & 1u;
            expected |= (uint32_t)value << bit;
        }
        m_w16(NULL, 0, (uint16_t)(first[op] | rn));
        m_w16(NULL, 2, (uint16_t)(((lsb >> 2) << 12) | (rd << 8) | ((lsb & 3u) << 6) | encoded));
        CHECK(arm_step(&c) == (valid ? ARM_OK : ARM_UNDEFINED) && c.r[15] == (valid ? 4u : 0u) &&
              c.r[rd] == (valid ? expected : before) && c.cycles == 1u &&
              c.cpsr == (valid ? flags & ~TEST_IT_MASK : flags),
              "bitfield op=%u lsb=%u encoded=%u alias=%u lost range, field/sign bits or flags", op, lsb, encoded, rd == rn);
        if (rd != 14u) CHECK(c.r[14] == 0x80ff7f01u, "bitfield changed separate source register");
       }
      }
      const unsigned bad[][2] = {{13,0},{15,0},{0,13},{0,15}};
      for (unsigned b = 0; b < sizeof bad / sizeof bad[0]; b++) {
        if (op == 3u && b >= 2u) continue; /* BFC has no source operand. */
        if (op == 2u && b == 3u) continue; /* Rn=PC is the valid BFC alias. */
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_C; c.r[0] = 0x12345678u; c.r[13] = 0x100u;
        m_w16(NULL, 0, (uint16_t)(first[op] | (op == 3u ? 15u : bad[b][1])));
        m_w16(NULL, 2, (uint16_t)((bad[b][0] << 8) | 3u));
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == flags &&
              c.r[0] == 0x12345678u && c.r[13] == 0x100u, "bitfield accepted forbidden SP/PC register");
      }
     }
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T; c.r[0] = 2u;
    m_w16(NULL, 0, 0xf3c0u); m_w16(NULL, 2, 0x0040u);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 1u && c.r[15] == 4u, "real UBFX blocker failed");
    const uint16_t malformed[][2] = {{0xf7c0u,0x0000u},{0xf3c0u,0x0020u},{0xf3c0u,0x8000u}};
    for (unsigned i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[0] = 0x12345678u;
        m_w16(NULL, 0, malformed[i][0]); m_w16(NULL, 2, malformed[i][1]);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[0] == 0x12345678u && c.r[15] == 0u,
              "bitfield accepted nonzero reserved encoding bits");
    }
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T | ARM_CPSR_Z | test_it_bits(0x18u); c.r[0] = 0x12345678u;
    m_w16(NULL, 0, 0xf3c0u); m_w16(NULL, 2, 0x0040u);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x12345678u && c.r[15] == 4u && !(c.cpsr & TEST_IT_MASK),
          "condition-failed UBFX changed its result or did not advance IT");
    arm_reset(&c, &g_bus); c.cpsr |= ARM_CPSR_T; c.r[0] = 2u;
    m_w16(NULL, 0, 0xf3c0u); m_w16(NULL, 2, 0x0040u);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 2u && c.r[15] == 2u && c.r[14] == 0x3c0004u,
          "ARM1176 lost its legacy BL-prefix interpretation");
}

/* Independent shift/add oracle for the multiply decoder tests. Signed high
 * halves are corrected modulo 2^64, without signed casts or host multiply. */
static uint64_t test_wide_product(uint32_t a, uint32_t b, bool is_signed) {
    uint64_t result = 0;
    for (unsigned bit = 0; bit < 32u; bit++)
        if ((b >> bit) & 1u) result += (uint64_t)a << bit;
    if (is_signed && (a & 0x80000000u)) result -= (uint64_t)b << 32;
    if (is_signed && (b & 0x80000000u)) result -= (uint64_t)a << 32;
    return result;
}

static void test_thumb2_multiply(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    const uint32_t values[][2] = {
        {0,UINT32_MAX},{1,UINT32_MAX},{UINT32_MAX,UINT32_MAX},
        {0x80000000u,2},{0x80000000u,0x80000000u},{0x80000000u,UINT32_MAX},
        {0x7fffffffu,0x7fffffffu},{0x12345678u,0x9abcdef0u}
    };
    /* Rd, Rn, Rm, Ra. Include destination/source/accumulator overlaps, LR,
     * equal multiplicands and a completely aliased word operation. */
    const unsigned regs[][4] = {{9,6,14,1},{6,6,14,1},{14,6,14,1},
                               {1,6,14,1},{9,6,6,6},{6,6,6,6}};
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned op = 0; op < 3; op++) { /* MUL, MLA, MLS */
      for (unsigned v = 0; v < sizeof values / sizeof values[0]; v++) {
       for (unsigned layout = 0; layout < sizeof regs / sizeof regs[0]; layout++) {
        for (unsigned execute = 0; execute < 2; execute++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V | ARM_CPSR_Q |
                      (0xau << 16) | test_it_bits(execute ? 0x1cu : 0x0cu);
            uint32_t flags = c.cpsr;
            unsigned rd = regs[layout][0], rn = regs[layout][1], rm = regs[layout][2];
            unsigned ra = op == 0u ? 15u : regs[layout][3];
            c.r[1] = 0xffffffffu; c.r[9] = 0x12345678u;
            c.r[6] = values[v][0]; c.r[14] = values[v][1];
            uint32_t before[16]; memcpy(before, c.r, sizeof before);
            uint32_t product = (uint32_t)test_wide_product(c.r[rn], c.r[rm], false);
            uint32_t expected = op == 0u ? product : op == 1u ? product + c.r[ra] : c.r[ra] - product;
            m_w16(NULL, 0, (uint16_t)(0xfb00u | rn));
            m_w16(NULL, 2, (uint16_t)((ra << 12) | (rd << 8) | (op == 2u ? 0x10u : 0u) | rm));
            CHECK(arm_step(&c) == ARM_OK && c.r[rd] == (execute ? expected : before[rd]) &&
                  c.r[15] == 4u && c.cycles == 1u &&
                  c.cpsr == ((flags & ~TEST_IT_MASK) | test_it_bits(0x18u)),
                  "word multiply op=%u vector=%u layout=%u execute=%u lost result or flags/IT", op, v, layout, execute);
            for (unsigned r = 0; r < 15u; r++)
                if (r != rd) CHECK(c.r[r] == before[r], "word multiply changed another register");
        }
       }
      }
     }
    }
}

static void test_thumb2_multiply_long(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    const uint16_t opcodes[] = {0xfb80u,0xfba0u,0xfbc0u,0xfbe0u,0xfbe0u};
    const uint32_t values[][4] = { /* R6, LR, R9, R1 */
        {0,UINT32_MAX,0,0},{1,UINT32_MAX,1,UINT32_MAX},
        {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},
        {0x80000000u,2,0,1},{0x80000000u,0x80000000u,0,0xc0000000u},
        {0x80000000u,UINT32_MAX,0x80000000u,UINT32_MAX},
        {0x7fffffffu,0x7fffffffu,UINT32_MAX,0x80000000u},
        {0x12345678u,0x9abcdef0u,0x87654321u,0x13579bdfu}
    };
    /* RdLo, RdHi, Rn, Rm: independent, reversed/nonadjacent pairs and all
     * source/destination alias positions. */
    const unsigned regs[][4] = {{9,1,6,14},{6,1,6,14},{9,6,6,14},
                               {14,1,6,14},{9,14,6,14},{6,14,6,14},
                               {14,6,6,14},{9,1,6,6}};
    CHECK(test_wide_product(UINT32_MAX, UINT32_MAX, false) == UINT64_C(0xfffffffe00000001) &&
          test_wide_product(UINT32_MAX, UINT32_MAX, true) == 1u &&
          test_wide_product(0x80000000u, 2u, true) == UINT64_C(0xffffffff00000000),
          "multiply reference sign/high-word sanity check");
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned op = 0; op < 5; op++) {
      for (unsigned v = 0; v < sizeof values / sizeof values[0]; v++) {
       for (unsigned layout = 0; layout < sizeof regs / sizeof regs[0]; layout++) {
        for (unsigned execute = 0; execute < 2; execute++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_Z | ARM_CPSR_V | (0x5u << 16) |
                      test_it_bits(execute ? 0x08u : 0x18u);
            uint32_t flags = c.cpsr;
            unsigned lo = regs[layout][0], hi = regs[layout][1], rn = regs[layout][2], rm = regs[layout][3];
            c.r[6] = values[v][0]; c.r[14] = values[v][1];
            c.r[9] = values[v][2]; c.r[1] = values[v][3];
            uint32_t before[16]; memcpy(before, c.r, sizeof before);
            uint64_t expected = test_wide_product(c.r[rn], c.r[rm], op == 0u || op == 2u);
            if (op == 2u || op == 3u) expected += ((uint64_t)c.r[hi] << 32) | c.r[lo];
            if (op == 4u) expected += (uint64_t)c.r[hi] + c.r[lo];
            m_w16(NULL, 0, (uint16_t)(opcodes[op] | rn));
            m_w16(NULL, 2, (uint16_t)((lo << 12) | (hi << 8) | (op == 4u ? 0x60u : 0u) | rm));
            CHECK(arm_step(&c) == ARM_OK && c.r[lo] == (execute ? (uint32_t)expected : before[lo]) &&
                  c.r[hi] == (execute ? (uint32_t)(expected >> 32) : before[hi]) &&
                  c.r[15] == 4u && c.cycles == 1u && c.cpsr == (flags & ~TEST_IT_MASK),
                  "long multiply op=%u vector=%u layout=%u execute=%u lost sign/carry/alias or flags", op, v, layout, execute);
            for (unsigned r = 0; r < 15u; r++)
                if (r != lo && r != hi) CHECK(c.r[r] == before[r], "long multiply changed another register");
        }
       }
      }
     }
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c.cpsr |= ARM_CPSR_T; c.r[6] = 0xaaaaaaabu; c.r[1] = 0x12345678u;
    m_w16(NULL, 0, 0xfba6u); m_w16(NULL, 2, 0x0101u);
    CHECK(arm_step(&c) == ARM_OK && c.r[0] == 0x06117228u && c.r[1] == 0x0c22e450u,
          "real UMULL blocker failed");
}

static void test_thumb2_multiply_constraints(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    const uint16_t opcodes[][2] = {{0xfb06,0xf90e},{0xfb06,0x190e},{0xfb06,0x191e},
                                 {0xfb86,0x910e},{0xfba6,0x910e},{0xfbc6,0x910e},
                                 {0xfbe6,0x910e},{0xfbe6,0x916e}};
    for (unsigned p = 0; p < 2; p++) {
     for (unsigned op = 0; op < 8; op++) {
      for (unsigned field = 0; field < 4; field++) {
       if (op == 0u && field == 3u) continue; /* MUL has no accumulator. */
       for (unsigned bad = 13; bad <= 15; bad += 2) {
        if (op == 1u && field == 3u && bad == 15u) continue; /* MUL alias. */
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | test_it_bits(0x18u);
        for (unsigned r = 0; r < 15u; r++) c.r[r] = 0x12345678u + r;
        uint32_t before[16]; memcpy(before, c.r, sizeof before);
        uint32_t flags = c.cpsr;
        uint16_t first = opcodes[op][0], second = opcodes[op][1];
        if (field == 0u) first = (uint16_t)((first & 0xfff0u) | bad);
        else {
            unsigned shift = field == 1u ? 0u : field == 2u ? 8u : 12u;
            second = (uint16_t)((second & ~(15u << shift)) | (bad << shift));
        }
        m_w16(NULL, 0, first); m_w16(NULL, 2, second);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.cpsr == flags &&
              memcmp(c.r, before, sizeof before) == 0, "multiply op=%u accepted SP/PC field=%u", op, field);
       }
      }
     }
     for (unsigned op = 3; op < 8; op++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[9] = 0x12345678u;
        m_w16(NULL, 0, opcodes[op][0]); m_w16(NULL, 2, (uint16_t)((opcodes[op][1] & 0xf0ffu) | 0x900u));
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[9] == 0x12345678u && c.r[15] == 0u,
              "long multiply accepted equal destinations");
     }
     const uint16_t neighbors[][2] = {{0xfb06,0x192e},{0xfb16,0xf90e},{0xfb86,0x911e},
                                    {0xfba6,0x911e},{0xfbc6,0x916e},{0xfbe6,0x915e},
                                    {0xfb96,0xf9fe},{0xfbb6,0xf9fe}};
     for (unsigned i = 0; i < sizeof neighbors / sizeof neighbors[0]; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
        c.cpsr |= ARM_CPSR_T; c.r[9] = 0x12345678u;
        m_w16(NULL, 0, neighbors[i][0]); m_w16(NULL, 2, neighbors[i][1]);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[9] == 0x12345678u && c.r[15] == 0u,
              "multiply consumed a separate DSP/divide or reserved encoding");
     }
    }
    arm_cpu_t c;
    arm_reset(&c, &g_bus); c.cpsr |= ARM_CPSR_T; c.r[1] = 0x12345678u;
    m_w16(NULL, 0, 0xfba6u); m_w16(NULL, 2, 0x0101u);
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0x74cu && c.r[14] == 3u && c.r[1] == 0x12345678u,
          "ARM1176 lost its legacy BL-suffix interpretation");
}

static void test_thumb_compare_and_branch(void) {
    const arm_arch_t profiles[] = {ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT};
    const uint32_t offsets[] = {0u, 62u, 64u, 126u};
    for (unsigned p = 0; p < 3; p++) {
      for (unsigned nz = 0; nz < 2; nz++) {
       for (unsigned value = 0; value < 2; value++) {
        for (unsigned rn = 0; rn < 8; rn++) {
         for (unsigned i = 0; i < 4; i++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, profiles[p]), "reset");
            /* Z deliberately disagrees with the register's zero status. */
            c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V | (value ? ARM_CPSR_Z : 0u);
            c.r[15] = 0x100u; c.r[14] = 0x777u; c.r[rn] = value ? 0x80000000u : 0u;
            uint32_t cpsr = c.cpsr;
            uint16_t insn = (uint16_t)(0xb100u | (nz << 11) | ((offsets[i] & 64u) << 3) |
                                      ((offsets[i] & 62u) << 2) | rn);
            m_w16(NULL, 0x100u, insn);
            bool supported = p != 0u, taken = value == nz;
            CHECK(arm_step(&c) == (supported ? ARM_OK : ARM_UNDEFINED) && c.cycles == 1u &&
                  c.r[15] == (supported ? (taken ? 0x104u + offsets[i] : 0x102u) : 0x100u) &&
                  c.r[rn] == (value ? 0x80000000u : 0u) && c.r[14] == 0x777u && c.cpsr == cpsr,
                  "CB%sZ profile %u Rn %u offset %u used flags or changed state", nz ? "N" : "", p, rn, offsets[i]);
         }
        }
       }
      }
    }
    /* CBZ/CBNZ are forbidden in every active IT state, even when not taken. */
    const uint32_t it_bits[] = {1u << 25, 1u << 26, 1u << 10, 1u << 11};
    for (unsigned i = 0; i < 4; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | it_bits[i];
        c.r[0] = 0u;
        uint32_t cpsr = c.cpsr;
        m_w16(NULL, 0, 0xbb18u); /* actual CBNZ r0,PC+70, not taken */
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == cpsr,
              "CBNZ accepted an active IT state");
    }
}

static void test_cortex_a8_thumb_cp15_transfers(void) {
    /* MCR/MRC T1 fields match A1, with Thumb's additional SP restriction.
     * Independent register values and permissions cover real storage, L2's
     * absent-ECC readback, context-ID/TTBR flushes, NZCV and IT advancement. */
    static const struct {
        uint16_t first, second;
        uint32_t initial, written, readback;
        bool user_read, user_write, flush;
    } cases[] = {
        {0xee0d,0x0f30,0x12345678,0xabcdef80,0xabcdef80,false,false,true},
        {0xee0d,0x0f50,0x12345678,0xabcdef80,0xabcdef80,true,true,false},
        {0xee0d,0x0f70,0x12345678,0xabcdef80,0xabcdef80,true,false,false},
        {0xee0d,0x0f90,0x12345678,0xabcdef80,0xabcdef80,false,false,false},
        {0xee02,0x0f10,0x12344000,0xabcdef80,0xabcdef80,false,false,true},
        {0xee29,0x0f50,0x00000042,0x13200042,0x13000042,false,false,false}
    };
    const unsigned states[] = {0u,0x18u,0x0cu,0x1cu};
    const unsigned advanced[] = {0u,0u,0x18u,0x18u};
    for (unsigned op = 0; op < sizeof cases / sizeof cases[0]; op++) {
     for (unsigned load = 0; load < 2u; load++) {
      for (unsigned user = 0; user < 2u; user++) {
       for (unsigned rd = 0; rd < 16u; rd++) {
        for (unsigned it = 0; it < sizeof states / sizeof states[0]; it++) {
            arm_cpu_t c;
            CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
            uint32_t *stored[] = {&c.cp15.context_id,&c.cp15.tpidrurw,&c.cp15.tpidruro,
                                 &c.cp15.tpidrprw,&c.cp15.ttbr0,&c.a8_l2actlr};
            *stored[op] = cases[op].initial;
            c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_T | ARM_CPSR_I |
                     ARM_CPSR_F | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_V | ARM_CPSR_Q |
                     (5u << 16) | test_it_bits(states[it]);
            if (rd != 15u) c.r[rd] = cases[op].written;
            uint32_t before[16]; memcpy(before, c.r, sizeof before);
            uint32_t flags = c.cpsr;
            uint64_t flushes = c.tlb_flushes;
            bool passed = it != 2u; /* EQ fails with Z clear. */
            bool allowed = rd != 13u && rd != 15u &&
                           (!user || (load ? cases[op].user_read : cases[op].user_write));
            m_w16(NULL, 0, (uint16_t)(cases[op].first | (load << 4)));
            m_w16(NULL, 2, (uint16_t)(cases[op].second | (rd << 12)));
            bool ok = !passed || allowed;
            CHECK(arm_step(&c) == (ok ? ARM_OK : ARM_UNDEFINED) &&
                  c.r[15] == (ok ? 4u : 0u) && c.cycles == 1u,
                  "Thumb CP15 framing/permission op=%u load=%u user=%u rd=%u it=%u", op, load, user, rd, it);
            uint32_t expected = passed && allowed && !load ? cases[op].readback : cases[op].initial;
            CHECK(*stored[op] == expected, "Thumb CP15 storage changed too early or lost the operand");
            if (ok) flags = (flags & ~TEST_IT_MASK) | test_it_bits(advanced[it]);
            CHECK(c.cpsr == flags && c.tlb_flushes == flushes +
                  (passed && allowed && !load && cases[op].flush ? 1u : 0u),
                  "Thumb CP15 flags/IT/TLB op=%u load=%u user=%u rd=%u it=%u cpsr=%08x expected=%08x",
                  op, load, user, rd, it, c.cpsr, flags);
            for (unsigned r = 0; r < 15u; r++)
                CHECK(c.r[r] == (passed && allowed && load && r == rd ? cases[op].initial : before[r]),
                      "Thumb CP15 wrote an unintended core register");
        }
       }
      }
     }
    }
    /* The system-register rules override generic MRC's APSR transfer:
     * CP15 Rt=15 is unpredictable. Refuse without advancing even a middle
     * IT slot, independently of the register's possible flag pattern. */
    for (unsigned nzcv = 0; nzcv < 16u; nzcv++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_Q | (10u << 16) | test_it_bits(0x1cu);
        c.cp15.tpidrurw = (nzcv << 28) | 0x07ffffffu;
        uint32_t flags = c.cpsr;
        m_w16(NULL, 0, 0xee1du); m_w16(NULL, 2, 0xff50u);
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.cpsr == flags &&
              c.cp15.tpidrurw == ((nzcv << 28) | 0x07ffffffu),
              "Thumb CP15 MRC APSR changed flags/IT instead of refusing unpredictable Rt15");
    }
    /* Other coprocessors and the MRC2/MCR2/CDP/LDC/MRRC spaces remain
     * independent unsupported instructions, with no legacy VFP/debug leak. */
    static const uint16_t neighbors[][2] = {
        {0xfe1d,0x4f50},{0xfe0d,0x4f50},{0xee1d,0x4f40},{0xed9d,0x4f50},
        {0xec5d,0x4f50},{0xee10,0x4f10}, /* Unknown MIDR is still refused. */
        {0xee01,0x4f11},{0xee0d,0x4f51},{0xee3d,0x4f50},{0xee1d,0x4ff0}
    };
    for (unsigned i = 0; i < sizeof neighbors / sizeof neighbors[0] + 15u; i++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr |= ARM_CPSR_T | ARM_CPSR_N | ARM_CPSR_C;
        c.r[4] = 0xabcdef01u; c.cp15.tpidrurw = 0x12345678u;
        uint32_t flags = c.cpsr;
        unsigned n = sizeof neighbors / sizeof neighbors[0];
        m_w16(NULL, 0, i < n ? neighbors[i][0] : 0xee1du);
        m_w16(NULL, 2, i < n ? neighbors[i][1] : (uint16_t)(0x4050u | ((i - n) << 8)));
        CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u && c.r[4] == 0xabcdef01u &&
              c.cp15.tpidrurw == 0x12345678u && c.cpsr == flags,
              "Thumb CP15 leaked into unsupported instruction/profile space index=%u", i);
    }
    arm_cpu_t c;
    CHECK(arm_reset_profile(&c, &g_bus, ARM_ARCH_V7_SWIFT), "reset");
    c.cpsr |= ARM_CPSR_T;
    m_w16(NULL, 0, 0xee1du); m_w16(NULL, 2, 0x4f50u);
    CHECK(arm_step(&c) == ARM_UNDEFINED && c.r[15] == 0u, "Cortex-A8 Thumb CP15 enabled Swift's unaudited CP15 path");
    arm_reset(&c, &g_bus); c.cpsr |= ARM_CPSR_T; c.r[14] = 0x100u;
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == 0xd38u && c.r[14] == 3u && !(c.cpsr & ARM_CPSR_T),
          "Cortex-A8 Thumb CP15 changed ARM1176 legacy BLX suffix");
}

static void test_cortex_a8_thumb_cp15_fetch_and_maintenance(void) {
    for (unsigned host = 0; host < 2u; host++) {
     for (unsigned load = 0; load < 2u; load++) {
      for (unsigned fault = 0; fault < 4u; fault++) {
        memset(g_ram, 0, sizeof g_ram);
        arm_bus_t bus = g_bus;
        if (host) bus.host_ram = m_host_ram;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
        c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u; c.cp15.tpidrurw = 0x11223344u;
        c.cpsr = ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_N;
        c.r[15] = 0xffeu; c.r[4] = 0xa5a55a5au;
        m_w32(NULL, 0x4000u, 0x6001u); m_w32(NULL, 0x6000u, 0x8032u);
        m_w32(NULL, 0x6004u, fault == 1u ? 0u : fault == 2u ? 0xa033u : fault == 3u ? 0xa012u : 0xa032u);
        m_w16(NULL, 0x8ffeu, (uint16_t)(0xee0du | (load << 4)));
        m_w16(NULL, 0xa000u, 0x4f50u); m_w16(NULL, 0x9000u, 0x4f70u);
        CHECK(arm_step(&c) == ARM_OK && c.cycles == 1u, "Thumb CP15 fetch did not retire/vector once");
        if (!fault) {
            CHECK(c.r[15] == 0x1002u && c.r[4] == (load ? 0x11223344u : 0xa5a55a5au) &&
                  c.cp15.tpidrurw == (load ? 0x11223344u : 0xa5a55a5au),
                  "Thumb CP15 failed noncontiguous second-half fetch");
        } else {
            CHECK(c.r[15] == ARM_VEC_PREFETCH && c.cp15.ifar == 0x1000u &&
                  c.cp15.tpidrurw == 0x11223344u && c.r[4] == 0xa5a55a5au &&
                  (c.cp15.ifsr & 15u) == (fault == 1u ? ARM_FSR_PAGE_TRANSLATION : ARM_FSR_PAGE_PERMISSION),
                  "Thumb CP15 changed state before complete instruction fetch");
        }
      }
     }
    }
    static const uint16_t operations[] = {0x0f95u,0x0f9au,0x0fbau,0x0f15u,0x0f90u};
    for (unsigned op = 0; op < sizeof operations / sizeof operations[0]; op++) {
     for (unsigned user = 0; user < 2u; user++) {
        unsigned calls = 0;
        arm_bus_t bus = g_bus; bus.ctx = &calls; bus.wait_for_interrupt = count_wfi;
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &bus, ARM_ARCH_V7_CORTEX_A8), "reset");
        c.cpsr = (user ? ARM_MODE_USR : ARM_MODE_SVC) | ARM_CPSR_T;
        uint32_t flags = c.cpsr, generation = c.tlb_gen;
        c.r[0] = UINT32_MAX; c.excl_valid = true; c.excl_addr = 0x1234u;
        m_w16(NULL, 0, op == 3u ? 0xee08u : 0xee07u); m_w16(NULL, 2, operations[op]);
        bool allowed = !user || op < 3u;
        CHECK(arm_step(&c) == (allowed ? ARM_OK : ARM_UNDEFINED) && c.r[15] == (allowed ? 4u : 0u) &&
              c.cpsr == flags && c.r[0] == UINT32_MAX && calls == 0u && c.excl_valid && c.excl_addr == 0x1234u,
              "Thumb CP15 maintenance lost User/barrier/legacy-WFI-NOP semantics");
        CHECK(allowed && op == 3u ? c.tlb_gen != generation : c.tlb_gen == generation,
              "Thumb CP15 maintenance flushed only on an accepted TLB operation");
     }
    }
}

int main(void) {
    test_cortex_a8_vfp_system_access();
    test_cortex_a8_vfp_control_fields();
    test_cortex_a8_vfp_core_registers();
    test_cortex_a8_vfp_undefined_retry();
    test_cortex_a8_vfp_bitwise_fetch_and_retry();
    test_cortex_a8_vfp_single_memory_aborts();
    test_cortex_a8_vfp_multiple_memory_aborts();
    test_cortex_a8_vfp_fetch_and_refusals();
    test_cortex_a8_thumb_cp15_transfers();
    test_cortex_a8_thumb_cp15_fetch_and_maintenance();
    test_reset_initializes_the_default_profile();
    test_explicit_profile_reset_and_invalid_configuration();
    test_profile_instruction_boundaries();
    test_a32_barrier_profile_boundaries();
    test_a32_barriers_observe_completed_stores();
    test_thumb2_movw_movt_and_unknown_width();
    test_thumb2_fetch_across_page_boundary();
    test_thumb2_modified_immediate_moves();
    test_armv7_has_no_legacy_unaligned_mode();
    test_thumb2_str_immediate();
    test_thumb2_str_page_crossing_and_aborts();
    test_armv7_multiword_and_sync_alignment();
    test_thumb2_modified_immediate_arithmetic();
    test_thumb2_wide_branches();
    test_thumb2_multiple_transfers();
    test_thumb2_multiple_transfer_aborts();
    test_thumb2_word_loads();
    test_thumb2_word_load_aborts();
    test_thumb2_small_stores();
    test_thumb2_small_store_aborts();
    test_thumb2_indexed_transfers();
    test_thumb2_indexed_transfer_aborts();
    test_thumb_it_encodings_and_sequences();
    test_cortex_a8_wfi();
    test_cortex_a8_actlr_guest_wait_control();
    test_cortex_a8_wfi_fetch_boundary();
    test_thumb_it_narrow_flags();
    test_thumb_it_exceptions();
    test_thumb_it_placement_and_skips();
    test_thumb_it_fetch_faults();
    test_thumb_it_svc_hooks();
    test_thumb2_logical_immediates();
    test_thumb2_doubleword_transfers();
    test_thumb2_doubleword_aborts();
    test_thumb2_small_loads();
    test_thumb2_small_load_aborts();
    test_thumb2_extend();
    test_thumb2_shifted_data_processing();
    test_thumb2_immediate_shift_aliases();
    test_thumb2_shifted_register_constraints();
    test_thumb2_bitfields();
    test_thumb2_multiply();
    test_thumb2_multiply_long();
    test_thumb2_multiply_constraints();
    test_thumb_compare_and_branch();
    printf("S5LBox ARMv6 interpreter tests\n");
    test_mov_imm();
    test_add_reg();
    test_sub_flags();
    test_subs_negative();
    test_adds_overflow();
    test_nzcv_updates_preserve_the_rest_of_cpsr();
    test_barrel_lsl();
    test_register_shifted_pc_operands_are_unpredictable();
    test_branch();
    test_bl_sets_lr();
    test_ldr_str();
    test_direct_write_cache_requires_explicit_consent();
    test_str_and_stm_store_pc_plus_12();
    test_ldrb();
    test_cond_not_taken();
    test_mul();
    test_dsp_smul_halfword_selectors_and_real_alias();
    test_dsp_smla_wraps_and_sets_sticky_q();
    test_dsp_smlal_sign_carry_wrap_and_alias();
    test_dsp_word_halfword_extract_and_accumulate();
    test_dsp_multiply_unpredictable_and_reserved_forms_trap();
    test_orr_bic_mvn();
    test_bx_branches();
    test_exception_return_to_thumb_keeps_halfword();
    test_exception_return_to_arm_stays_word_aligned();
    test_ldm_exception_return_takes_state_from_spsr();
    test_rfe_aligns_for_the_restored_state();
    test_srs_and_rfe_reject_unprivileged_execution();
    test_exception_returns_reject_invalid_modes_before_mutation();
    test_setend_le_runs_be_traps();
    test_umull_and_smull();
    test_umlal_accumulates();
    test_clz();
    test_qadd_saturates_and_sets_q();
    test_swp_exchanges();
    test_swp_legacy_unaligned_word_semantics();
    test_ldrexd_strexd_roundtrip();
    test_clrex_makes_strex_fail();
    test_failed_strex_still_checks_write_permission();
    test_exception_clears_exclusive_monitor();
    test_imm_dp_not_trapped();
    test_strh_ldrh();
    test_ldrsb_sign_extends();
    test_ldrsh_sign_extends();
    test_stmia_ldmia();
    test_push_pop();
    test_ldm_to_pc_branches();
    test_plain_loads_to_pc_reject_arm_halfword_targets();
    test_unaligned_ldr_to_pc_never_reads_memory();
    test_banked_sp_per_mode();
    test_fiq_banks_r8_r12();
    test_mrs_reads_cpsr();
    test_msr_switches_mode();
    test_swi_enters_svc();
    test_exception_return_restores_mode();
    test_cp15_reads_midr();
    test_cp15_cpuid_feature_bank();
    test_cp15_cpuid_scheme_grades_as_armv6();
    test_cp15_id_dfr0_matches_absent_debug_unit();
    test_cp15_sctlr_roundtrip();
    test_cp15_c1_is_gated_on_crm_zero();
    test_cp15_ttbr0_roundtrip();
    test_cp15_ttbcr_masks_only_reserved_bits();
    test_cp15_cache_op_is_accepted();
    test_cp15_wfi_uses_only_the_exact_privileged_hook();
    test_cortex_a8_cp15_selector_boundary();
    test_cortex_a8_cp15_maintenance_boundary();
    test_cortex_a8_l2_auxiliary_control();
    test_cortex_a8_system_control_register();
    test_cortex_a8_auxiliary_control_register();
    test_cortex_a8_coprocessor_access_control();
    test_high_vectors();
    test_mmu_disabled_is_identity();
    test_mmu_section_translation();
    test_mmu_unmapped_faults();
    test_mmu_user_write_permission();
    test_mmu_small_page_translation();
    test_data_abort_taken();
    test_pld_is_a_nop_not_a_branch();
    test_unconditional_space_traps();
    test_clz_does_not_corrupt_cpsr();
    test_msr_still_works();
    test_arm_media_extend_and_reverse();
    test_arm_media_pair_extend();
    test_arm_media_reverse_edges();
    test_arm_media_saturate();
    test_apx_makes_mapping_read_only();
    test_xp0_ignores_extended_apx_bits();
    test_xp0_large_and_small_ap_subpages();
    test_sctlr_sr_legacy_permissions();
    test_arm1176_rejects_fine_page_tables();
    test_page_translation_fault_precedes_page_domain_fault();
    test_force_access_flag_faults_precede_domain_permissions();
    test_cortex_a8_access_flag_retry_without_tlbi();
    test_fetch_cache_refill_requires_an_exact_live_witness();
    test_data_cache_refill_requires_an_exact_live_witness();
    test_abort_restores_base_register();
    test_sctlr_a_faults_ordinary_unaligned_accesses();
    test_sctlr_u_selects_legacy_or_armv6_unaligned_data();
    test_multiword_alignment_depends_on_sctlr_u_a();
    test_arm_multiword_base_writeback_restrictions();
    test_single_transfer_zero_post_same_base_load();
    test_single_transfer_unpredictable_forms_trap_before_bus();
    test_ldrd_strd_transfer_the_register_pair();
    test_ldrd_strd_alignment_is_the_armv6_word_rule();
    test_ldrd_strd_operand_restrictions_trap_before_bus();
    test_exclusive_alignment_is_never_silently_fixed();
    test_atomic_operand_aliases_are_rejected_before_bus();
    test_thumb_mov_add();
    test_thumb_lsl_flags();
    test_thumb_push_pop();
    test_thumb_multiword_alignment_uses_strict_rules();
    test_thumb_conditional_branch();
    test_arm_to_thumb_and_back();
    test_bx_blx_register_reject_invalid_targets_without_side_effects();
    test_thumb_bl_pair();
    test_thumb_extend_and_reverse();
    test_thumb_rev();
    test_thumb_cps();
    test_mmu_supersection();
    test_thumb_blx_suffix_is_not_a_branch();
    test_user_bank_stm();
    test_mmu_ttbcr_n0_ignores_ttbr1();
    test_mmu_ttbcr_n2_low_va_uses_shrunk_ttbr0();
    test_mmu_ttbcr_n2_kernel_va_uses_ttbr1();
    test_mmu_ttbcr_n2_split_boundary();
    test_mmu_kernel_mapping_survives_ttbr0_pmap_switch();
    test_mmu_ttbcr_n1_table_geometry();
    test_mmu_ttbcr_n3_table_geometry();
    test_mmu_ttbcr_pd_bits_suppress_selected_walk();
    test_dfsr_wnr_write_vs_read();
    test_data_abort_write_sets_dfsr_wnr();
    test_prefetch_abort_never_sets_wnr();
    test_dfar_is_the_faulting_word_of_a_block_transfer();
    test_cps_is_a_nop_in_user_mode();
    test_vfp_disabled_vectors_the_guest();
    test_vfp_enabled_still_halts();
    test_non_vfp_undefined_still_halts();
    test_vfp_undef_entry_state();
    test_vfp_sysreg_access_follows_fpexc();
    test_vfp_cpacr_denial_vectors();
    test_vfp_lazy_trap_cannot_loop();
    test_privileged_svc_hook_handles_a32();
    test_privileged_svc_hook_redirects_a32_to_thumb();
    test_privileged_svc_hook_nonhandled_is_transactional();
    test_privileged_svc_hook_a32_error_halts_transactionally();
    test_privileged_svc_hook_obeys_a32_guards();
    test_privileged_svc_hook_handles_thumb();
    test_privileged_svc_hook_redirects_thumb_to_a32();
    test_privileged_svc_hook_thumb_error_and_user_guard();
    test_swi_entry_state_from_user_mode();
    test_thumb_swi_lr_is_the_next_halfword();
    test_irq_sets_a_but_swi_does_not();
    test_exception_entry_takes_cpsr_e_from_sctlr_ee();
    test_xnu_syscall_frame_round_trip();
    test_syscall_error_flag_reaches_user_through_the_spsr();
    test_user_bank_ldm_writes_the_user_bank();
    test_user_bank_stm_from_fiq_uses_the_user_r8_r12();
    test_user_bank_transfer_with_writeback_traps();
    test_user_bank_transfers_reject_user_and_system_modes();
    test_srs_stores_lr_and_spsr_of_the_current_mode();
    test_subs_pc_lr_4_returns_from_fiq();
    test_ldrt_from_svc_translates_as_unprivileged();
    test_strt_from_svc_translates_as_unprivileged();
    test_user_mode_load_uses_user_permissions();
    test_cp15_is_privileged_from_user_mode();
    test_cp15_thread_id_registers_stay_user_accessible();
    test_unaligned_access_spanning_two_pages();
    test_unaligned_access_faulting_on_the_second_page();
    test_parallel_add_sub_family();
    test_pack_halfword_and_select();
    test_signed_dual_multiply_family();
    test_integer_divide_requires_swift();
    test_movw_movt_are_armv7_only();
    test_srs_and_rfe_stop_after_the_first_fault();
    test_ldrd_strd_stop_after_the_first_faulting_word();
    test_xn_blocks_fetch_from_a_small_page();
    test_xn_on_a_section_and_the_xp_gate();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
