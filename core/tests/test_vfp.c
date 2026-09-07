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

#include <fenv.h>
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

/* A8.8.280/339/340/355: op 0=copy, 1=absolute, 2=negate, 3=immediate.
 * The immediate case takes the encoded imm8 as its source argument. */
static uint32_t a8_fp_bits(unsigned op, unsigned dbl, unsigned dst, unsigned src) {
    uint32_t insn = 0xeeb00a00u | (dbl << 8) |
        (dbl ? ((dst & 15u) << 12) | ((dst >> 4) << 22) :
               ((dst >> 1) << 12) | ((dst & 1u) << 22));
    if (op == 3u) return insn | ((src >> 4) << 16) | (src & 15u);
    return insn | (op == 2u ? 0x10040u : op == 1u ? 0xc0u : 0x40u) |
        (dbl ? (src & 15u) | ((src >> 4) << 5) : (src >> 1) | ((src & 1u) << 5));
}

static uint64_t a8_fp_value(const arm_cpu_t *c, unsigned dbl, unsigned reg) {
    return dbl ? vfp_get_d(c, reg) : vfp_get_s(c, reg);
}

static void a8_fp_value_set(arm_cpu_t *c, unsigned dbl, unsigned reg, uint64_t value) {
    if (dbl) vfp_set_d(c, reg, value);
    else vfp_set_s(c, reg, (uint32_t)value);
}

static uint64_t a8_fp_sign_result(unsigned op, unsigned dbl, uint64_t value) {
    uint64_t sign = dbl ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000);
    return op == 1u ? value & ~sign : op == 2u ? value ^ sign : value;
}

/* Numeric definition from Table A7-18, independent of the production bit
 * expansion. All values are exactly representable in both IEEE formats. */
static uint64_t a8_fp_constant(unsigned dbl, unsigned imm) {
    int exponent = (int)(((imm >> 4) & 7u) ^ 4u) - 3;
    double value = (double)(16u + (imm & 15u)) / 16.0;
    if (exponent < 0) value /= (double)(1u << (unsigned)-exponent);
    else value *= (double)(1u << (unsigned)exponent);
    if (imm & 128u) value = -value;
    return dbl ? d2u(value) : f2u((float)value);
}

static void test_a8_vfp_bitwise_scalar_registers(void) {
    static const uint64_t patterns[] = {
        0u, UINT64_C(0x8000000080000000), UINT64_C(0x0000000000000001),
        UINT64_C(0x8000000000000001), UINT64_C(0x7ff000007f800000),
        UINT64_C(0xfff00000ff800000), UINT64_C(0x7ff000017f800001),
        UINT64_C(0xfff80001ffc00001), UINT64_C(0x0010000000800000),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0x1234567887654321)
    };
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned op = 0; op < 3u; op++)
       for (unsigned dst = 0; dst < 32u; dst++)
        for (unsigned src = 0; src < 32u; src++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d, UINT64_C(0xdead1234beef0000) + d);
            unsigned pattern = (dst + src * 3u) % (sizeof patterns / sizeof patterns[0]);
            a8_fp_value_set(&c, dbl, src, patterns[pattern]);
            uint64_t before[32], value = a8_fp_sign_result(op, dbl, a8_fp_value(&c, dbl, src));
            for (unsigned d = 0; d < 32u; d++) before[d] = vfp_get_d(&c, d);
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, a8_fp_bits(op, dbl, dst, src)) == ARM_OK &&
                  c.r[15] == 0x104u && c.cpsr == flags && c.vfp_fpscr == fpscr && c.cycles == 1u,
                  "A8 raw FP disposition T=%u D=%u op=%u dst=%u src=%u", thumb, dbl, op, dst, src);
            if (dbl) before[dst] = value;
            else {
                unsigned shift = (dst & 1u) * 32u;
                before[dst / 2u] = (before[dst / 2u] & ~(UINT64_C(0xffffffff) << shift)) | (value << shift);
            }
            for (unsigned d = 0; d < 32u; d++)
                CHECK(vfp_get_d(&c, d) == before[d], "A8 raw FP bits/alias D%u T=%u D=%u op=%u dst=%u src=%u",
                      d, thumb, dbl, op, dst, src);
        }
}

static void test_a8_vfp_all_immediate_constants(void) {
    CHECK(a8_fp_bits(3u, 0u, 0u, 0x70u) == 0xeeb70a00u &&
          a8_fp_bits(3u, 1u, 31u, 0xf0u) == 0xeefffb00u,
          "independent VFP immediate encoding anchors");
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned imm = 0; imm < 256u; imm++) {
            unsigned dst = imm % 32u;
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d, UINT64_C(0x123456789abcdef0) + d);
            uint64_t expected = a8_fp_constant(dbl, imm);
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, a8_fp_bits(3u, dbl, dst, imm)) == ARM_OK &&
                  c.r[15] == 0x104u && c.cpsr == flags && c.vfp_fpscr == fpscr &&
                  a8_fp_value(&c, dbl, dst) == expected,
                  "A8 VFP immediate T=%u D=%u dst=%u imm=%02x", thumb, dbl, dst, imm);
            for (unsigned d = 0; d < 32u; d++) {
                uint64_t want = UINT64_C(0x123456789abcdef0) + d;
                if (dbl && d == dst) want = expected;
                if (!dbl && d == dst / 2u) {
                    unsigned shift = (dst & 1u) * 32u;
                    want = (want & ~(UINT64_C(0xffffffff) << shift)) | (expected << shift);
                }
                CHECK(vfp_get_d(&c, d) == want, "VFP immediate modified wrong register D%u", d);
            }
       }
}

static void test_a8_vfp_bitwise_vectors(void) {
    /* Table K-1 is global even with a scalar-bank destination. D16-D19 is
     * a second scalar bank. Source/destination vector banks are kept distinct. */
    static const unsigned destinations[] = {0u,3u,7u,11u,15u,16u,19u,23u,27u,31u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned op = 0; op < 4u; op++)
       for (unsigned destination = 0; destination < sizeof destinations / sizeof destinations[0]; destination++)
        for (unsigned src_bank = 0; src_bank < (dbl ? 8u : 4u); src_bank++)
         for (unsigned len = 0; len < 8u; len++)
          for (unsigned stride = 0; stride < 4u; stride++) {
            unsigned dst = destinations[destination];
            unsigned bank = dbl ? 4u : 8u, src = src_bank * bank + bank - 1u;
            if (src / bank == dst / bank) continue;
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.vfp_fpscr |= (len << 16) | (stride << 20);
            for (unsigned r = 0; r < 32u; r++) a8_fp_value_set(&c, dbl, r, UINT64_C(0x8000000080000010) + r);
            uint64_t expected[32];
            for (unsigned r = 0; r < 32u; r++) expected[r] = a8_fp_value(&c, dbl, r);
            bool scalar_dst = dbl ? dst < 4u || (dst >= 16u && dst < 20u) : dst < 8u;
            bool scalar_src = dbl ? src < 4u || (src >= 16u && src < 20u) : src < 8u;
            unsigned count = scalar_dst ? 1u : len + 1u, step = stride == 3u ? 2u : 1u;
            bool valid = stride == 0u || (stride == 3u && len >= 1u && len <= 3u);
            valid = valid && (scalar_dst || count * step <= bank);
            if (valid) for (unsigned i = 0; i < count; i++) {
                unsigned dr = dst / bank * bank + (dst % bank + i * step) % bank;
                unsigned sr = scalar_dst || scalar_src ? src : src / bank * bank + (src % bank + i * step) % bank;
                expected[dr] = op == 3u ? a8_fp_constant(dbl, 0xabu) :
                    a8_fp_sign_result(op, dbl, a8_fp_value(&c, dbl, sr));
            }
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, a8_fp_bits(op, dbl, dst, op == 3u ? 0xabu : src)) ==
                  (valid ? ARM_OK : ARM_UNDEFINED) && c.r[15] == (valid ? 0x104u : 0x100u) &&
                  c.cpsr == flags && c.vfp_fpscr == fpscr,
                  "A8 vector shape T=%u D=%u op=%u dst=%u src=%u len=%u stride=%u",
                  thumb, dbl, op, dst, src, len, stride);
            for (unsigned r = 0; r < 32u; r++) CHECK(a8_fp_value(&c, dbl, r) == expected[r],
                "A8 vector value r=%u T=%u D=%u op=%u dst=%u src=%u len=%u stride=%u",
                r, thumb, dbl, op, dst, src, len, stride);
          }
}

static void test_a8_vfp_bitwise_access_and_host_state(void) {
    static const unsigned permissions[] = {0u, 1u, 3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned op = 0; op < 4u; op++)
       for (unsigned user = 0; user < 2u; user++)
        for (unsigned enabled = 0; enabled < 2u; enabled++)
         for (unsigned access = 0; access < 3u; access++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[access] * 0x00500000u;
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            a8_fp_value_set(&c, dbl, 17u, UINT64_C(0xfff00001ff800001));
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            uint64_t want = op == 3u ? a8_fp_constant(dbl, 0x70u) :
                a8_fp_sign_result(op, dbl, a8_fp_value(&c, dbl, 17u));
            bool allowed = enabled && (permissions[access] == 3u || (permissions[access] == 1u && !user));
            CHECK(a8_move_step(&c, thumb, a8_fp_bits(op, dbl, 31u, op == 3u ? 0x70u : 17u)) == ARM_OK &&
                  c.vfp_fpscr == fpscr && a8_fp_value(&c, dbl, 31u) == (allowed ? want : 0u),
                  "A8 bitwise access T=%u D=%u op=%u U=%u EN=%u permission=%u", thumb, dbl, op, user, enabled, access);
            CHECK(allowed ? c.r[15] == 0x104u && c.cpsr == flags :
                  c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == (thumb ? 0x102u : 0x104u) &&
                  c.spsr[ARM_BANK_UND] == flags && (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND,
                  "A8 bitwise access exception state");
         }
    fenv_t saved;
    CHECK(fegetenv(&saved) == 0, "save host FP environment");
    static const int rounds[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    for (unsigned host = 0; host < 4u; host++)
     for (unsigned guest = 0; guest < 4u; guest++)
      for (unsigned op = 0; op < 4u; op++) {
        arm_cpu_t c;
        a8_move_reset(&c, 1u);
        c.vfp_fpscr = (c.vfp_fpscr & ~ARM_FPSCR_RMODE) | (guest << 22);
        vfp_set_d(&c, 31u, UINT64_C(0x7ff0000000000001));
        CHECK(fesetround(rounds[host]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
              feraiseexcept(FE_INVALID | FE_DIVBYZERO) == 0, "prepare host FP environment");
        int exceptions = fetestexcept(FE_ALL_EXCEPT);
        CHECK(a8_move_step(&c, 1u, a8_fp_bits(op, 1u, 16u, op == 3u ? 0x80u : 31u)) == ARM_OK,
              "raw operation refused a host rounding mode");
        CHECK(fegetround() == rounds[host] && fetestexcept(FE_ALL_EXCEPT) == exceptions,
              "raw operation touched host rounding/exception flags");
      }
    CHECK(fesetenv(&saved) == 0, "restore host FP environment");
}

static void test_a8_vfp_bitwise_invalid_and_conditional(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned enabled = 0; enabled < 2u; enabled++)
      for (unsigned skip = 0; skip < 2u; skip++)
       for (unsigned kind = 0; kind < 7u; kind++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d, UINT64_C(0x7ff01234dead0000) + d);
        c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
        if (skip) c.cp15.cpacr = 0u;
        if (thumb) { m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); CHECK(arm_step(&c) == ARM_OK, "FP bits IT setup"); }
        uint32_t insn = a8_fp_bits(kind < 4u ? kind : 3u, 1u, 31u, kind == 3u ? 0x70u : 16u);
        bool valid = kind < 4u;
        if (!valid) insn |= kind == 4u ? 0x20u : kind == 5u ? 0x80u : 0xa0u;
        if (!thumb && skip) insn &= 0x0fffffffu;
        uint32_t pc = c.r[15], flags = c.cpsr, fpscr = c.vfp_fpscr;
        arm_status_t status = a8_move_step(&c, thumb, insn);
        CHECK(status == (skip || valid ? ARM_OK : ARM_UNDEFINED), "FP bits conditional disposition");
        if (skip || !valid) {
            CHECK(c.r[15] == (skip ? pc + 4u : pc) && c.vfp_fpscr == fpscr &&
                  c.cpsr == (thumb && skip ? flags & ~0x0600fc00u : flags), "skipped/refused FP bits effects");
            for (unsigned d = 0; d < 32u; d++) CHECK(vfp_get_d(&c, d) == UINT64_C(0x7ff01234dead0000) + d,
                "skipped/refused FP bits modified D%u", d);
        }
       }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++)
     for (unsigned op = 0; op < 4u; op++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "legacy reset");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        c.vfp_s[0] = 0xff800001u;
        CHECK(a8_move_step(&c, 0u, a8_fp_bits(op, 1u, 16u, 0u)) == ARM_UNDEFINED && c.r[15] == 0u &&
              c.vfp_s[0] == 0xff800001u, "A8 raw FP enabled on a legacy profile");
     }
    /* Neither instruction-set prefix nor the neighboring sqrt/conversion
     * encodings may become a raw move. These upper-bank operations remain
     * unsupported with access enabled. CDP2 stays refused even with EN=0. */
    static const uint32_t neighbors[] = {0xfef7fb00u,0xeef1fbe0u,0xeef7fbe0u,0xeef8fb60u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned n = 0; n < sizeof neighbors / sizeof neighbors[0]; n++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        vfp_set_d(&c, 31u, UINT64_C(0xabcdef1234567890));
        if (n == 0u) c.vfp_fpexc = 0u;
        CHECK(a8_move_step(&c, thumb, neighbors[n]) == ARM_UNDEFINED && c.r[15] == 0x100u &&
              vfp_get_d(&c, 31u) == UINT64_C(0xabcdef1234567890), "raw FP swallowed neighboring encoding");
     }
    /* CPSR.E changes memory byte order, not register bit positions. */
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned op = 0; op < 4u; op++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.cpsr |= ARM_CPSR_E;
        vfp_set_d(&c, 31u, UINT64_C(0xfff0000000000001));
        uint32_t flags = c.cpsr;
        uint64_t expected = op == 3u ? UINT64_C(0x3ff0000000000000) :
            a8_fp_sign_result(op, 1u, UINT64_C(0xfff0000000000001));
        CHECK(a8_move_step(&c, thumb, a8_fp_bits(op, 1u, 16u, op == 3u ? 0x70u : 31u)) == ARM_OK &&
              c.cpsr == flags && vfp_get_d(&c, 16u) == expected, "CPSR.E changed raw FP register bits");
     }
}

/* VCMP/VCMPE A1/T1 and A2/T2, DDI0406C.b A8.8.303. */
static uint32_t a8_fp_compare(unsigned dbl, unsigned signaling, unsigned zero,
                               unsigned left, unsigned right) {
    return 0xeeb40a40u | (dbl << 8) | (signaling << 7) | (zero << 16) |
        (dbl ? ((left & 15u) << 12) | ((left >> 4) << 22) |
               (right & 15u) | ((right >> 4) << 5) :
               ((left >> 1) << 12) | ((left & 1u) << 22) |
               (right >> 1) | ((right & 1u) << 5));
}

/* Explicit numeric ordering and classes are the oracle, independent of the
 * production bit ordering. Zero has rank 8; NaNs have no numeric rank. */
static const struct {
    uint32_t single;
    uint64_t dual;
    int rank;
    bool denormal, nan, signaling;
} a8_compare_values[] = {
    {0xff800000u, UINT64_C(0xfff0000000000000), 0, false, false, false},
    {0xff7fffffu, UINT64_C(0xffefffffffffffff), 1, false, false, false},
    {0xbf800001u, UINT64_C(0xbff0000000000001), 2, false, false, false},
    {0xbf800000u, UINT64_C(0xbff0000000000000), 3, false, false, false},
    {0xbf7fffffu, UINT64_C(0xbfefffffffffffff), 4, false, false, false},
    {0x80800000u, UINT64_C(0x8010000000000000), 5, false, false, false},
    {0x807fffffu, UINT64_C(0x800fffffffffffff), 6, true,  false, false},
    {0x80000001u, UINT64_C(0x8000000000000001), 7, true,  false, false},
    {0x80000000u, UINT64_C(0x8000000000000000), 8, false, false, false},
    {0x00000000u, UINT64_C(0x0000000000000000), 8, false, false, false},
    {0x00000001u, UINT64_C(0x0000000000000001), 9, true,  false, false},
    {0x007fffffu, UINT64_C(0x000fffffffffffff),10, true,  false, false},
    {0x00800000u, UINT64_C(0x0010000000000000),11, false, false, false},
    {0x3f7fffffu, UINT64_C(0x3fefffffffffffff),12, false, false, false},
    {0x3f800000u, UINT64_C(0x3ff0000000000000),13, false, false, false},
    {0x3f800001u, UINT64_C(0x3ff0000000000001),14, false, false, false},
    {0x7f7fffffu, UINT64_C(0x7fefffffffffffff),15, false, false, false},
    {0x7f800000u, UINT64_C(0x7ff0000000000000),16, false, false, false},
    {0x7fc12345u, UINT64_C(0x7ff8123456789abc), 0, false, true,  false},
    {0xffc00000u, UINT64_C(0xfff8000000000000), 0, false, true,  false},
    {0x7f800001u, UINT64_C(0x7ff0000000000001), 0, false, true,  true},
    {0xffbfffffu, UINT64_C(0xfff7ffffffffffff), 0, false, true,  true}
};

static uint32_t a8_compare_flags(unsigned x, unsigned y, unsigned signaling, bool fz) {
    bool nan = a8_compare_values[x].nan || a8_compare_values[y].nan;
    int a = fz && a8_compare_values[x].denormal ? 8 : a8_compare_values[x].rank;
    int b = fz && a8_compare_values[y].denormal ? 8 : a8_compare_values[y].rank;
    uint32_t flags = nan ? 0x30000000u : a == b ? 0x60000000u : a < b ? 0x80000000u : 0x20000000u;
    if (a8_compare_values[x].signaling || a8_compare_values[y].signaling || (signaling && nan)) flags |= 1u;
    if (fz && (a8_compare_values[x].denormal || a8_compare_values[y].denormal)) flags |= 0x80u;
    return flags;
}

static void a8_compare_value_set(arm_cpu_t *c, unsigned dbl, unsigned reg, unsigned value) {
    a8_fp_value_set(c, dbl, reg, dbl ? a8_compare_values[value].dual : a8_compare_values[value].single);
}

static void test_a8_vfp_compare_registers_and_values(void) {
    CHECK(a8_fp_compare(0u, 0u, 0u, 0u, 0u) == 0xeeb40a40u &&
          a8_fp_compare(1u, 0u, 0u, 31u, 16u) == 0xeef4fb60u &&
          a8_fp_compare(1u, 1u, 1u, 31u, 0u) == 0xeef5fbc0u, "VCMP encoding anchors");
    unsigned count = sizeof a8_compare_values / sizeof a8_compare_values[0];
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned signaling = 0; signaling < 2u; signaling++)
       for (unsigned zero = 0; zero < 2u; zero++)
        for (unsigned left = 0; left < 32u; left++)
         for (unsigned right = 0; right < (zero ? 1u : 32u); right++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d, UINT64_C(0xdead1234beef0000) + d);
            unsigned x = (left * 3u + right) % count, y = zero ? 9u : (left + right * 5u) % count;
            if (!zero && left == right) x = y;
            a8_compare_value_set(&c, dbl, left, x);
            if (!zero) a8_compare_value_set(&c, dbl, right, y);
            uint64_t before[32];
            for (unsigned d = 0; d < 32u; d++) before[d] = vfp_get_d(&c, d);
            c.excl_valid = true; c.excl_addr = 0x12340u;
            c.vfp_fpscr |= ((left & 7u) << 16) | ((right & 3u) << 20) | ARM_FPSCR_N | ARM_FPSCR_DZC;
            uint32_t flags = c.cpsr, fpscr = (c.vfp_fpscr & ~ARM_FPSCR_NZCV) |
                a8_compare_flags(x, y, signaling, true);
            CHECK(a8_move_step(&c, thumb, a8_fp_compare(dbl, signaling, zero, left, right)) == ARM_OK &&
                  c.r[15] == 0x104u && c.cycles == 1u && c.cpsr == flags && c.vfp_fpscr == fpscr &&
                  c.excl_valid && c.excl_addr == 0x12340u,
                  "VCMP register/state T=%u D=%u E=%u Z=%u left=%u right=%u", thumb, dbl, signaling, zero, left, right);
            bool unchanged = true;
            for (unsigned d = 0; d < 32u; d++) unchanged &= vfp_get_d(&c, d) == before[d];
            for (unsigned r = 0; r < 15u; r++) unchanged &= c.r[r] == 0u;
            CHECK(unchanged, "VCMP changed a core/extension register");
         }
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned signaling = 0; signaling < 2u; signaling++)
       for (unsigned controls = 0; controls < 4u; controls++)
        for (unsigned x = 0; x < count; x++)
         for (unsigned y = 0; y < count; y++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.vfp_fpscr = ARM_FPSCR_QC | ARM_FPSCR_RMODE | ARM_FPSCR_LEN | ARM_FPSCR_STRIDE |
                ARM_FPSCR_NZCV | ARM_FPSCR_DZC | (controls & 1u ? ARM_FPSCR_FZ : 0u) |
                (controls & 2u ? ARM_FPSCR_DN : 0u);
            c.cpsr |= ARM_CPSR_E;
            a8_compare_value_set(&c, dbl, 31u, x); a8_compare_value_set(&c, dbl, 16u, y);
            uint32_t flags = c.cpsr, fpscr = (c.vfp_fpscr & ~ARM_FPSCR_NZCV) |
                a8_compare_flags(x, y, signaling, (controls & 1u) != 0u);
            CHECK(a8_move_step(&c, thumb, a8_fp_compare(dbl, signaling, 0u, 31u, 16u)) == ARM_OK &&
                  c.vfp_fpscr == fpscr && c.cpsr == flags,
                  "VCMP classes T=%u D=%u E=%u controls=%u x=%u y=%u", thumb, dbl, signaling, controls, x, y);
         }
}

static void test_a8_vfp_compare_access_and_host_state(void) {
    static const unsigned permissions[] = {0u, 1u, 3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned signaling = 0; signaling < 2u; signaling++)
       for (unsigned zero = 0; zero < 2u; zero++)
        for (unsigned user = 0; user < 2u; user++)
         for (unsigned enabled = 0; enabled < 2u; enabled++)
          for (unsigned access = 0; access < 3u; access++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[access] * 0x00500000u;
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            a8_compare_value_set(&c, dbl, 31u, 18u); a8_compare_value_set(&c, dbl, 16u, 20u);
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            bool allowed = enabled && (permissions[access] == 3u || (permissions[access] == 1u && !user));
            uint32_t want = allowed ? (fpscr & ~ARM_FPSCR_NZCV) |
                a8_compare_flags(18u, zero ? 9u : 20u, signaling, true) : fpscr;
            CHECK(a8_move_step(&c, thumb, a8_fp_compare(dbl, signaling, zero, 31u, zero ? 0u : 16u)) == ARM_OK &&
                  c.vfp_fpscr == want, "VCMP access/exception flags");
            CHECK(allowed ? c.r[15] == 0x104u && c.cpsr == flags :
                  c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == (thumb ? 0x102u : 0x104u) &&
                  c.spsr[ARM_BANK_UND] == flags && (c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND,
                  "VCMP access exception state");
          }
    fenv_t saved;
    CHECK(fegetenv(&saved) == 0, "save comparison host FP environment");
    static const int rounds[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned host = 0; host < 4u; host++)
       for (unsigned guest = 0; guest < 4u; guest++)
        for (unsigned signaling = 0; signaling < 2u; signaling++)
         for (unsigned pending = 0; pending < 2u; pending++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.vfp_fpscr = (c.vfp_fpscr & ~ARM_FPSCR_RMODE) | (guest << 22);
            a8_compare_value_set(&c, dbl, 31u, 20u); a8_compare_value_set(&c, dbl, 16u, 7u);
            CHECK(fesetround(rounds[host]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  (!pending || feraiseexcept(FE_INVALID | FE_DIVBYZERO) == 0), "prepare comparison host FP state");
            int exceptions = fetestexcept(FE_ALL_EXCEPT);
            CHECK(a8_move_step(&c, thumb, a8_fp_compare(dbl, signaling, 0u, 31u, 16u)) == ARM_OK &&
                  fegetround() == rounds[host] && fetestexcept(FE_ALL_EXCEPT) == exceptions,
                  "VCMP changed host FP environment T=%u D=%u host=%u guest=%u E=%u pending=%u",
                  thumb, dbl, host, guest, signaling, pending);
         }
    CHECK(fesetenv(&saved) == 0, "restore comparison host FP environment");
}

static void test_a8_vfp_compare_status_sequence(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.vfp_fpscr = ARM_FPSCR_QC | ARM_FPSCR_FZ | ARM_FPSCR_DZC;
        uint32_t flags = c.cpsr, controls = c.vfp_fpscr;
        a8_compare_value_set(&c, dbl, 31u, 20u); a8_compare_value_set(&c, dbl, 16u, 7u);
        CHECK(a8_move_step(&c, thumb, a8_fp_compare(dbl, 0u, 0u, 31u, 16u)) == ARM_OK &&
              c.vfp_fpscr == (controls | 0x30000081u) && c.cpsr == flags, "VCMP accumulated IOC and IDC");
        a8_compare_value_set(&c, dbl, 31u, 14u); a8_compare_value_set(&c, dbl, 16u, 14u);
        CHECK(a8_move_step(&c, thumb, a8_fp_compare(dbl, 0u, 0u, 31u, 16u)) == ARM_OK &&
              c.vfp_fpscr == (controls | 0x60000081u) && c.cpsr == flags,
              "finite VCMP did not replace NZCV while retaining cumulative exceptions");
        CHECK(a8_move_step(&c, thumb, VMRS(15u, 1u)) == ARM_OK && c.r[15] == 0x10cu && c.cycles == 3u &&
              c.vfp_fpscr == (controls | 0x60000081u) && c.cpsr == ((flags & ~ARM_FPSCR_NZCV) | 0x60000000u),
              "VMRS did not publish comparison flags without changing controls");
     }
}

static void test_a8_vfp_compare_invalid_and_conditional(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned signaling = 0; signaling < 2u; signaling++)
       for (unsigned enabled = 0; enabled < 2u; enabled++)
        for (unsigned skip = 0; skip < 2u; skip++)
         for (unsigned invalid = 0; invalid < 64u; invalid++) {
            /* 0 is a valid comparison. 1..31 cover every forbidden Vm/M
             * combination in #0.0; 32..63 cover unrepresentable FPSCR bits. */
            if (invalid >= 32u && (ARM_FPSCR_A8_WMASK & (1u << (invalid - 32u)))) continue;
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            a8_compare_value_set(&c, dbl, 31u, 20u);
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            if (skip) c.cp15.cpacr = 0u;
            if (invalid >= 32u) c.vfp_fpscr |= 1u << (invalid - 32u);
            if (thumb) { m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); CHECK(arm_step(&c) == ARM_OK, "VCMP IT setup"); }
            uint32_t insn = a8_fp_compare(dbl, signaling, 1u, 31u, invalid < 32u ? invalid : 0u);
            if (!thumb && skip) insn &= 0x0fffffffu;
            uint32_t pc = c.r[15], flags = c.cpsr, fpscr = c.vfp_fpscr;
            bool valid = invalid == 0u;
            CHECK(a8_move_step(&c, thumb, insn) == (skip || valid ? ARM_OK : ARM_UNDEFINED),
                  "VCMP conditional/invalid disposition T=%u D=%u E=%u EN=%u skip=%u invalid=%u",
                  thumb, dbl, signaling, enabled, skip, invalid);
            if (skip || !valid) CHECK(c.vfp_fpscr == fpscr && c.r[15] == (skip ? pc + 4u : pc) &&
                c.cpsr == (thumb && skip ? flags & ~0x0600fc00u : flags), "VCMP invalid/skip changed state");
            else if (!enabled) CHECK(c.vfp_fpscr == fpscr && c.r[15] == ARM_VEC_UNDEFINED &&
                c.spsr[ARM_BANK_UND] == flags, "VCMP denied valid encoding was not a guest fault");
            else CHECK(c.vfp_fpscr == ((fpscr & ~ARM_FPSCR_NZCV) | 0x30000001u) &&
                c.cpsr == (thumb ? flags & ~0x0600fc00u : flags) && c.r[15] == pc + 4u,
                "VCMP valid conditional execution");
         }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176, ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++)
     for (unsigned signaling = 0; signaling < 2u; signaling++)
      for (unsigned zero = 0; zero < 2u; zero++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "legacy compare reset");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        CHECK(a8_move_step(&c, 0u, a8_fp_compare(1u, signaling, zero, 31u, zero ? 0u : 16u)) == ARM_UNDEFINED &&
              c.r[15] == 0u && c.vfp_fpscr == 0u, "upper-bank comparison leaked into legacy profile");
      }
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.vfp_fpexc = 0u;
        CHECK(a8_move_step(&c, thumb, 0xfef4fb60u) == ARM_UNDEFINED && c.r[15] == 0x100u,
              "VCMP admitted an unconditional coprocessor encoding");
    }
}

static uint32_t a8_neon_memory(unsigned thumb, unsigned load, unsigned type, unsigned size,
                                 unsigned align, unsigned first, unsigned rn, unsigned rm) {
    return (thumb ? 0xf9000000u : 0xf4000000u) | (load << 21) | ((first >> 4) << 22) |
        (rn << 16) | ((first & 15u) << 12) | (type << 8) | (size << 6) | (align << 4) | rm;
}

static void test_a8_neon_memory_registers(void) {
    static const unsigned types[] = {7u,10u,6u,2u}, offsets[] = {15u,13u,6u,5u};
    CHECK(a8_neon_memory(1u, 0u, 10u, 3u, 2u, 8u, 4u, 15u) == 0xf9048aefu &&
          a8_neon_memory(0u, 1u, 10u, 3u, 2u, 8u, 4u, 15u) == 0xf4248aefu,
          "NEON memory encoding anchors from matching fmodf");
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned load_ = 0; load_ < 2u; load_++)
      for (unsigned size = 2u; size <= 3u; size++)
       for (unsigned count = 1u; count <= 4u; count++)
        for (unsigned first = 0; first < 32u; first++)
         for (unsigned align = 0; align < 4u; align++)
          for (unsigned post = 0; post < 4u; post++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.r[5] = 0x2040u; c.r[6] = 0xfffffff0u;
            unsigned rm = offsets[post];
            bool valid = first + count <= 32u && (count == 4u ||
                (count == 2u ? align != 3u : align < 2u));
            uint64_t before[32];
            for (unsigned d = 0; d < 32u; d++) {
                before[d] = UINT64_C(0xff01234567890000) + d * UINT64_C(0x1234567);
                vfp_set_d(&c, d, before[d]);
            }
            uint8_t expected[64];
            memset(g_ram + 0x2030u, 0xa5, sizeof expected);
            for (unsigned word = 0; word < 8u; word++) m_w32(NULL, 0x2040u + word * 4u, 0xabcdef01u + word * 0x87654321u);
            memcpy(expected, g_ram + 0x2030u, sizeof expected);
            if (valid) for (unsigned d = 0; d < count; d++) {
                if (load_) memcpy(&before[first + d], expected + 16u + d * 8u, 8u);
                else memcpy(expected + 16u + d * 8u, &before[first + d], 8u);
            }
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            uint32_t updated = rm == 15u ? 0x2040u : rm == 13u ? 0x2040u + count * 8u : rm == 5u ? 0x4080u : 0x2030u;
            CHECK(a8_move_step(&c, thumb, a8_neon_memory(thumb, load_, types[count - 1u], size, align, first, 5u, rm)) ==
                  (valid ? ARM_OK : ARM_UNDEFINED) && c.r[15] == (valid ? 0x104u : 0x100u) &&
                  c.r[5] == (valid ? updated : 0x2040u) && c.cpsr == flags && c.vfp_fpscr == fpscr,
                  "NEON memory disposition T=%u L=%u size=%u count=%u first=%u align=%u rm=%u",
                  thumb, load_, size, count, first, align, rm);
            bool same = !memcmp(g_ram + 0x2030u, expected, sizeof expected);
            for (unsigned d = 0; d < 32u; d++) same &= vfp_get_d(&c, d) == before[d];
            CHECK(same, "NEON memory register/byte order or guard region changed");
          }
}

static void test_a8_neon_memory_alignment_and_access(void) {
    static const unsigned types[] = {7u,10u,6u,2u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned load_ = 0; load_ < 2u; load_++)
      for (unsigned size = 2u; size <= 3u; size++)
       for (unsigned count = 1u; count <= 4u; count++)
        for (unsigned align = 0; align < 4u; align++)
         for (unsigned a = 0; a < 2u; a++)
          for (unsigned offset = 0; offset < 32u; offset++) {
            if (count != 4u && (count == 2u ? align == 3u : align >= 2u)) continue;
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | (a ? ARM_SCTLR_A : 0u);
            c.r[13] = 0x2040u + offset;
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            unsigned alignment = align ? 4u << align : 1u << size;
            bool misaligned = (offset % alignment) != 0u;
            bool abort = misaligned && (align || a), unsupported = misaligned && !abort;
            uint32_t insn = a8_neon_memory(thumb, load_, types[count - 1u], size, align, 28u, 13u, 13u);
            CHECK(a8_move_step(&c, thumb, insn) == (unsupported ? ARM_UNDEFINED : ARM_OK) && c.vfp_fpscr == fpscr,
                  "NEON alignment disposition");
            if (abort) CHECK(c.r[15] == ARM_VEC_DATA_ABORT && c.r[14] == 0x108u &&
                c.spsr[ARM_BANK_ABT] == flags && c.bank_r13[ARM_BANK_USR] == 0x2040u + offset &&
                c.cp15.dfar == 0x2040u + offset && c.cp15.dfsr == (1u | (load_ ? 0u : 0x800u)),
                "NEON alignment abort state");
            else CHECK(c.r[15] == (unsupported ? 0x100u : 0x104u) && c.cpsr == flags &&
                c.r[13] == 0x2040u + offset + (unsupported ? 0u : count * 8u), "NEON SP/writeback alignment state");
          }
    static const unsigned permissions[] = {0u,1u,3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned load_ = 0; load_ < 2u; load_++)
      for (unsigned user = 0; user < 2u; user++)
       for (unsigned enabled = 0; enabled < 2u; enabled++)
        for (unsigned access = 0; access < 3u; access++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[access] * 0x00500000u;
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            c.r[5] = 0x2040u;
            bool allowed = enabled && (permissions[access] == 3u || (permissions[access] == 1u && !user));
            uint32_t flags = c.cpsr;
            CHECK(a8_move_step(&c, thumb, a8_neon_memory(thumb, load_, 10u, 3u, 2u, 16u, 5u, 13u)) == ARM_OK &&
                  c.r[5] == (allowed ? 0x2050u : 0x2040u), "NEON access disposition");
            CHECK(allowed ? c.r[15] == 0x104u && c.cpsr == flags :
                  c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == (thumb ? 0x102u : 0x104u) &&
                  c.spsr[ARM_BANK_UND] == flags, "NEON access exception state");
        }
}

static void test_a8_neon_memory_invalid_and_it(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned enabled = 0; enabled < 2u; enabled++)
      for (unsigned skip = 0; skip < (thumb ? 2u : 1u); skip++)
       for (unsigned kind = 0; kind < 13u; kind++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.r[5] = 0x2040u;
        c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
        if (skip) c.cp15.cpacr = 0u;
        if (kind == 7u) c.cpsr |= ARM_CPSR_E;
        uint32_t insn = a8_neon_memory(thumb, 0u, 10u, kind == 0u ? 2u : 3u, 2u, 31u, 5u, 13u);
        /* Keep D31 in unsupported neighboring forms to catch the former
         * broad A32 preload-hint alias. Valid cases use D28-D29. */
        if (kind < 3u || kind == 7u) insn = a8_neon_memory(thumb, 0u, 10u, kind == 0u ? 2u : 3u, 2u, 28u,
            kind == 2u ? 15u : 5u, 13u);
        if (kind == 4u) insn = a8_neon_memory(thumb, 0u, 10u, 3u, 3u, 28u, 5u, 13u);
        if (kind == 5u || kind == 6u) insn = a8_neon_memory(thumb, 0u, 7u, kind - 5u, 0u, 31u, 5u, 13u);
        if (kind == 8u) insn |= 1u << 23; /* single-lane/replicate space */
        if (kind >= 9u) insn = a8_neon_memory(thumb, 0u, kind == 12u ? 15u : kind - 9u, 3u, 0u, 31u, 5u, 13u);
        if (thumb) { m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); CHECK(arm_step(&c) == ARM_OK, "NEON IT setup"); }
        memset(g_ram + 0x2040u, 0xa5, 32u);
        uint32_t pc = c.r[15], flags = c.cpsr, fpscr = c.vfp_fpscr;
        bool denied = !enabled && (kind < 2u || kind == 7u), valid = kind < 2u;
        CHECK(a8_move_step(&c, thumb, insn) == (skip || valid || denied ? ARM_OK : ARM_UNDEFINED),
              "NEON invalid/IT disposition T=%u EN=%u skip=%u kind=%u", thumb, enabled, skip, kind);
        if (skip || (!valid && !denied)) {
            CHECK(c.r[5] == 0x2040u && c.r[15] == (skip ? pc + 4u : pc) && c.vfp_fpscr == fpscr &&
                  c.cpsr == (skip ? flags & ~0x0600fc00u : flags), "NEON refused/skipped state changed");
            bool same = true;
            for (unsigned i = 0; i < 32u; i++) same &= g_ram[0x2040u + i] == 0xa5u;
            CHECK(same, "NEON refused/skipped store wrote memory");
        } else if (denied) CHECK(c.r[15] == ARM_VEC_UNDEFINED && c.spsr[ARM_BANK_UND] == flags &&
            c.r[5] == 0x2040u, "NEON disabled valid transfer did not enter guest Undefined");
        else CHECK(c.r[15] == pc + 4u && c.r[5] == 0x2050u && c.cpsr == (flags & ~0x0600fc00u),
            "NEON valid IT slot did not retire");
       }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "legacy NEON reset");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        CHECK(a8_move_step(&c, 0u, a8_neon_memory(0u, 0u, 10u, 3u, 2u, 8u, 5u, 15u)) == ARM_UNDEFINED &&
              c.r[15] == 0u, "NEON memory leaked into a legacy profile");
    }
}

/* Raw Advanced SIMD Boolean register operations, in encoding order:
 * VAND, VBIC, VORR/VMOV, VORN, VEOR, VBSL, VBIT, VBIF. Q operands here
 * are spelled as their first D register to test every alignment bit. */
static uint32_t a8_neon_bits(unsigned thumb, unsigned op, unsigned quad,
                              unsigned dst, unsigned left, unsigned right) {
    return (thumb ? 0xef000110u : 0xf2000110u) | ((op & 3u) << 20) |
        ((op >> 2) << (thumb ? 28u : 24u)) | (quad << 6) |
        ((dst & 15u) << 12) | ((dst >> 4) << 22) |
        ((left & 15u) << 16) | ((left >> 4) << 7) | (right & 15u) | ((right >> 4) << 5);
}

static uint64_t a8_neon_bits_expected(unsigned op, uint64_t old, uint64_t left, uint64_t right) {
    /* Truth tables indexed by (old destination, left, right), independent
     * of the production word expressions. Covers all eight Boolean inputs. */
    static const unsigned truth[] = {0x88u,0x44u,0xeeu,0xddu,0x66u,0xcau,0xd8u,0xe4u};
    uint64_t result = 0;
    for (unsigned bit = 0; bit < 64u; bit++) {
        unsigned index = (unsigned)(((old >> bit) & 1u) * 4u + ((left >> bit) & 1u) * 2u + ((right >> bit) & 1u));
        result |= (uint64_t)((truth[op] >> index) & 1u) << bit;
    }
    return result;
}

static void test_a8_neon_bitwise_registers(void) {
    static const uint64_t patterns[] = {
        0u, UINT64_MAX, UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0xcccccccccccccccc),
        UINT64_C(0xf0f0f0f0f0f0f0f0), UINT64_C(0xfff0000000000001),
        UINT64_C(0x7ff8123487654321), UINT64_C(0x8000000000000000)
    };
    CHECK(a8_neon_bits(1u, 2u, 0u, 0u, 8u, 8u) == 0xef280118u &&
          a8_neon_bits(0u, 2u, 0u, 0u, 8u, 8u) == 0xf2280118u &&
          a8_neon_bits(1u, 7u, 0u, 31u, 16u, 16u) == 0xff70f1b0u, "NEON Boolean encoding anchors");
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned op = 0; op < 8u; op++)
       for (unsigned dst = 0; dst < 32u; dst += quad + 1u)
        for (unsigned left = 0; left < 32u; left += quad + 1u)
         for (unsigned variant = 0; variant < 6u; variant++) {
            unsigned right = variant == 0u ? dst : variant == 1u ? left :
                (dst + left * 3u + variant * 7u) & (quad ? 30u : 31u);
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr |= ARM_CPSR_E;
            c.vfp_fpscr |= ARM_FPSCR_LEN | ARM_FPSCR_STRIDE | ARM_FPSCR_NZCV;
            c.excl_valid = true; c.excl_addr = 0x2468u;
            uint64_t expected[32];
            for (unsigned d = 0; d < 32u; d++) {
                expected[d] = patterns[(d + variant) % 8u] ^ (UINT64_C(1) << ((d * 3u) % 64u));
                vfp_set_d(&c, d, expected[d]);
            }
            uint64_t result[2];
            for (unsigned lane = 0; lane <= quad; lane++) result[lane] =
                a8_neon_bits_expected(op, expected[dst + lane], expected[left + lane], expected[right + lane]);
            for (unsigned lane = 0; lane <= quad; lane++) expected[dst + lane] = result[lane];
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, a8_neon_bits(thumb, op, quad, dst, left, right)) == ARM_OK &&
                  c.r[15] == 0x104u && c.cycles == 1u && c.cpsr == flags && c.vfp_fpscr == fpscr &&
                  c.excl_valid && c.excl_addr == 0x2468u, "NEON Boolean disposition T=%u Q=%u op=%u d=%u n=%u m=%u",
                  thumb, quad, op, dst, left, right);
            bool same = true;
            for (unsigned d = 0; d < 32u; d++) same &= vfp_get_d(&c, d) == expected[d];
            for (unsigned r = 0; r < 15u; r++) same &= c.r[r] == 0u;
            CHECK(same, "NEON Boolean result, alias or untouched register mismatch");
         }
}

static void test_a8_neon_bitwise_access_and_host_state(void) {
    static const unsigned permissions[] = {0u,1u,3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned op = 0; op < 8u; op++)
       for (unsigned user = 0; user < 2u; user++)
        for (unsigned enabled = 0; enabled < 2u; enabled++)
         for (unsigned access = 0; access < 3u; access++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[access] * 0x00500000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            uint64_t expected[2];
            bool allowed = enabled && (permissions[access] == 3u || (permissions[access] == 1u && !user));
            for (unsigned lane = 0; lane <= quad; lane++) {
                vfp_set_d(&c, 30u + lane, UINT64_C(0xcccccccccccccccc) ^ lane);
                vfp_set_d(&c, 16u + lane, UINT64_C(0xaaaaaaaaaaaaaaaa) ^ lane);
                vfp_set_d(&c, 0u + lane, UINT64_C(0xf0f0f0f0f0f0f0f0) ^ lane);
                expected[lane] = allowed ? a8_neon_bits_expected(op, vfp_get_d(&c, 30u + lane),
                    vfp_get_d(&c, 16u + lane), vfp_get_d(&c, lane)) : vfp_get_d(&c, 30u + lane);
            }
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, a8_neon_bits(thumb, op, quad, 30u, 16u, 0u)) == ARM_OK &&
                  c.vfp_fpscr == fpscr, "NEON Boolean access disposition");
            CHECK(allowed ? c.r[15] == 0x104u && c.cpsr == flags :
                  c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == (thumb ? 0x102u : 0x104u) &&
                  c.spsr[ARM_BANK_UND] == flags, "NEON Boolean access exception state");
            for (unsigned lane = 0; lane <= quad; lane++) CHECK(vfp_get_d(&c, 30u + lane) == expected[lane],
                "NEON Boolean denied access changed destination");
         }
    fenv_t saved;
    CHECK(fegetenv(&saved) == 0, "save NEON host FP environment");
    static const int rounds[] = {FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned op = 0; op < 8u; op++)
       for (unsigned host = 0; host < 4u; host++)
        for (unsigned guest = 0; guest < 4u; guest++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.vfp_fpscr = (c.vfp_fpscr & ~ARM_FPSCR_RMODE) | (guest << 22);
            vfp_set_d(&c, 16u, UINT64_C(0xfff0000000000001));
            vfp_set_d(&c, 17u, UINT64_C(0x8000000000000001));
            CHECK(fesetround(rounds[host]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0, "prepare NEON host FP environment");
            int exceptions = fetestexcept(FE_ALL_EXCEPT);
            CHECK(a8_move_step(&c, thumb, a8_neon_bits(thumb, op, quad, 30u, 16u, 16u)) == ARM_OK &&
                  fegetround() == rounds[host] && fetestexcept(FE_ALL_EXCEPT) == exceptions,
                  "NEON Boolean operation touched host rounding or exception flags");
        }
    CHECK(fesetenv(&saved) == 0, "restore NEON host FP environment");
}

static void test_a8_neon_bitwise_invalid_and_it(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned op = 0; op < 8u; op++)
      for (unsigned enabled = 0; enabled < 2u; enabled++)
       for (unsigned skip = 0; skip < (thumb ? 2u : 1u); skip++)
        for (unsigned odd = 0; odd < 8u; odd++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            if (skip) c.cp15.cpacr = 0u;
            if (thumb) { m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); CHECK(arm_step(&c) == ARM_OK, "NEON Boolean IT setup"); }
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d, UINT64_C(0xf0f0f0f0f0f00000) + d);
            uint32_t pc = c.r[15], flags = c.cpsr, fpscr = c.vfp_fpscr;
            uint32_t insn = a8_neon_bits(thumb, op, 1u, 30u + (odd & 1u), 16u + ((odd >> 1) & 1u), (odd >> 2) & 1u);
            CHECK(a8_move_step(&c, thumb, insn) == (skip || !odd ? ARM_OK : ARM_UNDEFINED),
                  "NEON Boolean odd-Q/IT disposition T=%u op=%u EN=%u skip=%u odd=%u", thumb, op, enabled, skip, odd);
            if (skip || odd) {
                CHECK(c.r[15] == (skip ? pc + 4u : pc) && c.cpsr == (skip ? flags & ~0x0600fc00u : flags) &&
                      c.vfp_fpscr == fpscr, "NEON odd Q mutated flags or retired");
                bool same = true;
                for (unsigned d = 0; d < 32u; d++) same &= vfp_get_d(&c, d) == UINT64_C(0xf0f0f0f0f0f00000) + d;
                CHECK(same, "NEON odd Q or skipped instruction modified registers");
            } else if (!enabled) CHECK(c.r[15] == ARM_VEC_UNDEFINED && c.spsr[ARM_BANK_UND] == flags,
                "NEON valid Q access did not enter guest Undefined");
            else CHECK(c.r[15] == pc + 4u && c.cpsr == (flags & ~0x0600fc00u), "NEON valid Q IT did not retire");
        }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++)
     for (unsigned op = 0; op < 8u; op++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "legacy NEON Boolean reset");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        CHECK(a8_move_step(&c, 0u, a8_neon_bits(0u, op, 0u, 31u, 16u, 0u)) == ARM_UNDEFINED && c.r[15] == 0u,
              "NEON Boolean operation leaked into a legacy profile");
     }
    /* Adjacent fixed fields select other SIMD arithmetic or immediates,
     * which cannot become Boolean copies. Keep access enabled here. */
    static const uint32_t toggles[] = {0x00800000u,0x00000200u,0x00000400u,0x00000800u,0x00000010u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned field = 0; field < sizeof toggles / sizeof toggles[0]; field++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        uint32_t insn = a8_neon_bits(thumb, 2u, 0u, 31u, 16u, 16u) ^ toggles[field];
        CHECK(a8_move_step(&c, thumb, insn) == ARM_UNDEFINED && c.r[15] == 0x100u && vfp_get_d(&c, 31u) == 0u,
              "NEON Boolean decoder swallowed a neighboring allocation");
     }
}

static uint32_t a8_neon_imm(unsigned thumb, unsigned op, unsigned mode, unsigned quad,
                            unsigned dst, unsigned imm) {
    return (thumb ? 0xef800010u : 0xf2800010u) | ((imm >> 7) << (thumb ? 28u : 24u)) |
        ((imm & 0x70u) << 12) | (imm & 15u) | (op << 5) | (quad << 6) | (mode << 8) |
        ((dst & 15u) << 12) | ((dst >> 4) << 22);
}

static bool a8_neon_imm_valid(unsigned op, unsigned mode, unsigned imm) {
    /* Table A7-15 marks zero as UNPREDICTABLE except cmode 0/1/8/9/14/15. */
    return !(op && mode == 15u) && (imm || ((0xc303u >> mode) & 1u));
}

static uint64_t a8_neon_imm_expected(unsigned op, unsigned mode, unsigned imm, uint64_t old) {
    /* Build the manual's byte columns, separately from production shifts
     * and replication. F32 base values are 2/4/8/16 and 1/8,1/4,1/2,1. */
    uint8_t bytes[8] = {0};
    if (mode < 8u) bytes[mode / 2u] = bytes[mode / 2u + 4u] = (uint8_t)imm;
    else if (mode < 12u) for (unsigned b = 0; b < 8u; b++) {
        if ((b & 1u) == (mode / 2u - 4u)) bytes[b] = (uint8_t)imm;
    } else if (mode < 14u) for (unsigned b = 0; b < 8u; b++) {
        unsigned position = b % 4u, filled = mode - 11u;
        bytes[b] = position < filled ? 255u : position == filled ? (uint8_t)imm : 0u;
    } else if (mode == 14u) for (unsigned b = 0; b < 8u; b++)
        bytes[b] = op ? (imm & (1u << b) ? 255u : 0u) : (uint8_t)imm;
    else {
        static const uint32_t bases[] = {0x40000000u,0x40800000u,0x41000000u,0x41800000u,
            0x3e000000u,0x3e800000u,0x3f000000u,0x3f800000u};
        uint32_t bits = bases[(imm >> 4) & 7u] + (imm % 16u) * 0x80000u + (imm >= 128u ? 0x80000000u : 0u);
        for (unsigned b = 0; b < 8u; b++) bytes[b] = (uint8_t)(bits >> ((b % 4u) * 8u));
    }
    uint64_t expanded = 0;
    for (unsigned b = 0; b < 8u; b++) expanded += (uint64_t)bytes[b] << (b * 8u);
    if (mode < 12u && (mode & 1u)) return op ? old & ~expanded : old | expanded;
    return op && mode != 14u ? ~expanded : expanded;
}

/* A8.8.316: the immediate is a byte offset in the concatenated operands. */
static uint32_t a8_neon_extract(unsigned thumb, unsigned quad, unsigned dst,
                                  unsigned left, unsigned right, unsigned offset) {
    return (thumb ? 0xefb00000u : 0xf2b00000u) | (quad << 6) | (offset << 8) |
        ((dst & 15u) << 12) | ((dst >> 4) << 22) |
        ((left & 15u) << 16) | ((left >> 4) << 7) | (right & 15u) | ((right >> 4) << 5);
}

static void a8_extract_check(arm_cpu_t *c, unsigned thumb, unsigned quad, unsigned dst,
                             unsigned left, unsigned right, unsigned offset) {
    uint64_t before[32], expected[2] = {0u,0u};
    uint8_t joined[32];
    for (unsigned d = 0; d < 32u; d++) before[d] = vfp_get_d(c, d);
    for (unsigned byte = 0; byte < 8u * (1u + quad); byte++) {
        joined[byte] = (uint8_t)(before[left + byte / 8u] >> (8u * (byte % 8u)));
        joined[8u * (1u + quad) + byte] = (uint8_t)(before[right + byte / 8u] >> (8u * (byte % 8u)));
    }
    for (unsigned byte = 0; byte < 8u * (1u + quad); byte++)
        expected[byte / 8u] |= (uint64_t)joined[offset + byte] << (8u * (byte % 8u));
    uint32_t flags = c->cpsr, fpscr = c->vfp_fpscr, core[15]; memcpy(core, c->r, sizeof core);
    c->excl_valid = true; c->excl_addr = 0x12340u;
    CHECK(a8_move_step(c, thumb, a8_neon_extract(thumb, quad, dst, left, right, offset)) == ARM_OK &&
          c->r[15] == 0x104u && c->cycles == 1u && c->cpsr == flags && c->vfp_fpscr == fpscr &&
          c->vfp_fpexc == ARM_FPEXC_EN && c->excl_valid && c->excl_addr == 0x12340u && !memcmp(core, c->r, sizeof core),
          "VEXT state T=%u Q=%u D=%u N=%u M=%u offset=%u", thumb, quad, dst, left, right, offset);
    for (unsigned d = 0; d < 32u; d++) CHECK(vfp_get_d(c, d) ==
        (d >= dst && d <= dst + quad ? expected[d - dst] : before[d]), "VEXT destination D%u", d);
}

static void test_a8_neon_extract_registers(void) {
    CHECK(a8_neon_extract(1u, 0u, 0u, 2u, 3u, 1u) == 0xefb20103u &&
          a8_neon_extract(0u, 0u, 31u, 16u, 0u, 3u) == 0xf2f0f380u &&
          a8_neon_extract(1u, 1u, 30u, 0u, 30u, 15u) == 0xeff0ef6eu, "VEXT encoding anchors");
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned left = 0; left < 32u; left += 1u + quad)
       for (unsigned right = 0; right < 32u; right += 1u + quad)
        for (unsigned alias = 0; alias < 4u; alias++) {
            arm_cpu_t c; a8_move_reset(&c, thumb);
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d,
                UINT64_C(0x0011223344556677) ^ (UINT64_C(0x0101010101010101) * d));
            unsigned dst = alias == 0u ? left : alias == 1u ? right : alias == 2u ? 30u :
                ((left + right + 4u) & (quad ? 30u : 31u));
            unsigned offset = (left + right + alias) % (8u * (1u + quad));
            c.vfp_fpscr |= ((left & 7u) << 16) | ((right & 3u) << 20) | ARM_FPSCR_N | ARM_FPSCR_DZC;
            if (alias & 1u) c.cpsr |= ARM_CPSR_E;
            a8_extract_check(&c, thumb, quad, dst, left, right, offset);
        }
    /* Every byte boundary, including zero, the Q word boundary and the last
     * byte, with both operand orders and both aliased destinations. */
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned offset = 0; offset < 8u * (1u + quad); offset++)
       for (unsigned variant = 0; variant < 4u; variant++) {
            arm_cpu_t c; a8_move_reset(&c, thumb);
            vfp_set_d(&c, 0u, UINT64_C(0x0123456789abcdef)); vfp_set_d(&c, 1u, UINT64_C(0xff80000180000000));
            vfp_set_d(&c, 30u, UINT64_C(0x0011223344556677)); vfp_set_d(&c, 31u, UINT64_C(0x8899aabbccddeeff));
            a8_extract_check(&c, thumb, quad, (variant & 1u) ? 30u : 0u,
                (variant & 2u) ? 30u : 0u, (variant & 2u) ? 0u : 30u, offset);
       }
}

static void test_a8_neon_extract_access_and_invalid(void) {
    static const unsigned permissions[] = {0u,1u,3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned user = 0; user < 2u; user++)
       for (unsigned enabled = 0; enabled < 2u; enabled++)
        for (unsigned access = 0; access < 3u; access++) {
            arm_cpu_t c; a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[access] * 0x00500000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            vfp_set_d(&c, 31u, UINT64_C(0x123456789abcdef0));
            vfp_set_d(&c, quad ? 1u : 0u, UINT64_C(0xabcdef0123456789));
            bool allowed = enabled && (permissions[access] == 3u || (permissions[access] == 1u && !user));
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, a8_neon_extract(thumb, quad, 30u, 0u, 16u, quad ? 15u : 7u)) == ARM_OK &&
                  c.vfp_fpscr == fpscr, "VEXT access disposition");
            CHECK(allowed ? c.r[15] == 0x104u && c.cpsr == flags : c.r[15] == ARM_VEC_UNDEFINED &&
                  c.r[14] == (thumb ? 0x102u : 0x104u) && c.spsr[ARM_BANK_UND] == flags, "VEXT access exception state");
            CHECK(vfp_get_d(&c, 30u) == (allowed ? 0xabu : 0u) &&
                  vfp_get_d(&c, 31u) == (allowed && quad ? 0u : UINT64_C(0x123456789abcdef0)), "VEXT access destination");
        }
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned enabled = 0; enabled < 2u; enabled++)
      for (unsigned skip = 0; skip < (thumb ? 2u : 1u); skip++)
       for (unsigned kind = 0; kind < 6u; kind++) {
            arm_cpu_t c; a8_move_reset(&c, thumb);
            vfp_set_d(&c, 30u, UINT64_C(0x7ff0000000000001));
            vfp_set_d(&c, 31u, UINT64_C(0x8000000000000000));
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            if (skip) c.cp15.cpacr = 0u;
            if (thumb) { m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); CHECK(arm_step(&c) == ARM_OK, "VEXT IT setup"); }
            uint32_t pc = c.r[15], flags = c.cpsr, fpscr = c.vfp_fpscr;
            uint32_t insn = a8_neon_extract(thumb, kind >= 3u && kind < 5u ? 0u : 1u,
                kind == 0u ? 31u : 30u, kind == 1u ? 17u : 16u, kind == 2u ? 1u : 0u, kind == 3u ? 8u : 15u);
            bool valid = kind == 5u;
            CHECK(a8_move_step(&c, thumb, insn) == (skip || valid ? ARM_OK : ARM_UNDEFINED), "VEXT invalid/IT disposition");
            if (skip || !valid) CHECK(c.r[15] == (skip ? pc + 4u : pc) &&
                c.cpsr == (skip ? flags & ~0x0600fc00u : flags) && c.vfp_fpscr == fpscr, "VEXT invalid/skipped state changed");
            else if (!enabled) CHECK(c.r[15] == ARM_VEC_UNDEFINED && c.spsr[ARM_BANK_UND] == flags, "VEXT lazy enable");
            else CHECK(c.r[15] == pc + 4u && c.cpsr == (flags & ~0x0600fc00u), "VEXT valid IT retirement");
            for (unsigned d = 0; d < 32u; d++) CHECK(vfp_get_d(&c, d) ==
                (valid && enabled && !skip ? 0u : d == 30u ? UINT64_C(0x7ff0000000000001) :
                 d == 31u ? UINT64_C(0x8000000000000000) : 0u), "VEXT invalid/zero operand mutation");
       }
    static const uint32_t toggles[] = {1u << 24,1u << 23,1u << 21,1u << 20,1u << 4};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned field = 0; field < sizeof toggles / sizeof toggles[0]; field++) {
        arm_cpu_t c; a8_move_reset(&c, thumb);
        CHECK(a8_move_step(&c, thumb, a8_neon_extract(thumb, 0u, 31u, 16u, 0u, 3u) ^ toggles[field]) == ARM_UNDEFINED &&
              c.r[15] == 0x100u, "VEXT swallowed neighboring allocation");
     }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++) {
        arm_cpu_t c; CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "legacy VEXT reset");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        CHECK(a8_move_step(&c, 0u, 0xf2b20103u) == ARM_UNDEFINED && c.r[15] == 0u, "VEXT leaked to legacy profile");
    }
    fenv_t saved; CHECK(fegetenv(&saved) == 0, "save extract host FP state");
    static const int rounds[] = {FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned host = 0; host < 4u; host++) {
        arm_cpu_t c; a8_move_reset(&c, thumb); vfp_set_d(&c, 0u, UINT64_C(0x7ff0000000000001));
        CHECK(fesetround(rounds[host]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 && feraiseexcept(FE_DIVBYZERO) == 0,
              "prepare extract host FP state");
        int exceptions = fetestexcept(FE_ALL_EXCEPT);
        CHECK(a8_move_step(&c, thumb, a8_neon_extract(thumb, quad, 30u, 0u, 0u, 3u)) == ARM_OK &&
              fegetround() == rounds[host] && fetestexcept(FE_ALL_EXCEPT) == exceptions, "VEXT touched host FP state");
      }
    CHECK(fesetenv(&saved) == 0, "restore extract host FP state");
}

static void test_a8_neon_immediate_constants(void) {
    CHECK(a8_neon_imm(1u, 0u, 4u, 0u, 16u, 0x80u) == 0xffc00410u &&
          a8_neon_imm(0u, 0u, 4u, 0u, 16u, 0x80u) == 0xf3c00410u, "firmware NEON immediate encoding anchors");
    CHECK(a8_neon_imm_expected(0u, 15u, 0x70u, 0u) == UINT64_C(0x3f8000003f800000) &&
          a8_neon_imm_expected(1u, 14u, 0x80u, 0u) == UINT64_C(0xff00000000000000) &&
          a8_neon_imm_expected(0u, 13u, 1u, 0u) == UINT64_C(0x0001ffff0001ffff), "NEON immediate value anchors");
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned op = 0; op < 2u; op++)
       for (unsigned mode = 0; mode < 16u; mode++)
        for (unsigned imm = 0; imm < 256u; imm++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr |= ARM_CPSR_E;
            c.vfp_fpscr |= ARM_FPSCR_LEN | ARM_FPSCR_STRIDE | ARM_FPSCR_NZCV;
            c.excl_valid = true; c.excl_addr = 0x2468u;
            unsigned dst = imm & (quad ? 30u : 31u);
            uint64_t expected[32];
            for (unsigned d = 0; d < 32u; d++) {
                expected[d] = UINT64_C(0x0123456789abcdef) ^ (d * UINT64_C(0x0102040810204080));
                vfp_set_d(&c, d, expected[d]);
            }
            bool valid = a8_neon_imm_valid(op, mode, imm);
            if (valid) for (unsigned r = 0; r <= quad; r++)
                expected[dst + r] = a8_neon_imm_expected(op, mode, imm, expected[dst + r]);
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, a8_neon_imm(thumb, op, mode, quad, dst, imm)) == (valid ? ARM_OK : ARM_UNDEFINED) &&
                  c.r[15] == (valid ? 0x104u : 0x100u) && c.cpsr == flags && c.vfp_fpscr == fpscr &&
                  c.excl_valid && c.excl_addr == 0x2468u, "NEON immediate disposition T=%u Q=%u op=%u cmode=%u imm=%u",
                  thumb, quad, op, mode, imm);
            bool same = true;
            for (unsigned d = 0; d < 32u; d++) same &= vfp_get_d(&c, d) == expected[d];
            for (unsigned r = 0; r < 15u; r++) same &= c.r[r] == 0u;
            CHECK(same, "NEON immediate value or untouched register mismatch");
        }
}

static void test_a8_neon_immediate_access(void) {
    static const unsigned permissions[] = {0u,1u,3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned op = 0; op < 2u; op++)
       for (unsigned mode = 0; mode < 16u; mode++)
        for (unsigned user = 0; user < 2u; user++)
         for (unsigned enabled = 0; enabled < 2u; enabled++)
          for (unsigned access = 0; access < 3u; access++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[access] * 0x00500000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            bool valid = a8_neon_imm_valid(op, mode, 0x81u);
            bool allowed = valid && enabled && (permissions[access] == 3u || (permissions[access] == 1u && !user));
            uint64_t expected[2];
            for (unsigned r = 0; r <= quad; r++) {
                uint64_t old = UINT64_C(0x7ff00000a5a50000) + r;
                vfp_set_d(&c, 30u + r, old);
                expected[r] = allowed ? a8_neon_imm_expected(op, mode, 0x81u, old) : old;
            }
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, a8_neon_imm(thumb, op, mode, quad, 30u, 0x81u)) == (valid ? ARM_OK : ARM_UNDEFINED) &&
                  c.vfp_fpscr == fpscr, "NEON immediate access disposition");
            CHECK(!valid ? c.r[15] == 0x100u && c.cpsr == flags : allowed ? c.r[15] == 0x104u && c.cpsr == flags :
                  c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == (thumb ? 0x102u : 0x104u) && c.spsr[ARM_BANK_UND] == flags,
                  "NEON immediate validation/access priority");
            for (unsigned r = 0; r <= quad; r++) CHECK(vfp_get_d(&c, 30u + r) == expected[r], "NEON immediate access destination");
          }
    fenv_t saved;
    CHECK(fegetenv(&saved) == 0, "save immediate host FP state");
    static const int rounds[] = {FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned mode = 0; mode < 16u; mode++)
       for (unsigned host = 0; host < 4u; host++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            CHECK(fesetround(rounds[host]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 && feraiseexcept(FE_DIVBYZERO) == 0,
                  "prepare immediate host FP state");
            int exceptions = fetestexcept(FE_ALL_EXCEPT);
            CHECK(a8_move_step(&c, thumb, a8_neon_imm(thumb, 0u, mode, quad, 30u, 0x81u)) == ARM_OK &&
                  fegetround() == rounds[host] && fetestexcept(FE_ALL_EXCEPT) == exceptions, "immediate touched host FP state");
       }
    CHECK(fesetenv(&saved) == 0, "restore immediate host FP state");
}

static void test_a8_neon_immediate_invalid_and_it(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned op = 0; op < 2u; op++)
      for (unsigned mode = 0; mode < 16u; mode++)
       for (unsigned enabled = 0; enabled < 2u; enabled++)
        for (unsigned skip = 0; skip < (thumb ? 2u : 1u); skip++)
         for (unsigned variant = 0; variant < 3u; variant++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            if (skip) c.cp15.cpacr = 0u;
            if (thumb) { m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); CHECK(arm_step(&c) == ARM_OK, "immediate IT setup"); }
            unsigned imm = variant == 1u ? 0u : 0x81u, dst = variant == 2u ? 31u : 30u;
            bool valid = variant != 2u && a8_neon_imm_valid(op, mode, imm);
            uint32_t pc = c.r[15], flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, a8_neon_imm(thumb, op, mode, 1u, dst, imm)) == (skip || valid ? ARM_OK : ARM_UNDEFINED),
                  "NEON immediate invalid/IT disposition");
            if (skip || !valid) {
                CHECK(c.r[15] == (skip ? pc + 4u : pc) && c.cpsr == (skip ? flags & ~0x0600fc00u : flags) &&
                      c.vfp_fpscr == fpscr, "NEON invalid immediate mutated state or retired");
                bool same = true;
                for (unsigned d = 0; d < 32u; d++) same &= vfp_get_d(&c, d) == 0u;
                CHECK(same, "NEON skipped/invalid immediate changed FP registers");
            } else if (!enabled) CHECK(c.r[15] == ARM_VEC_UNDEFINED && c.spsr[ARM_BANK_UND] == flags, "immediate lazy enable");
            else CHECK(c.r[15] == pc + 4u && c.cpsr == (flags & ~0x0600fc00u), "immediate IT retirement");
         }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "legacy immediate reset");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        CHECK(a8_move_step(&c, 0u, 0xf3c00410u) == ARM_UNDEFINED && c.r[15] == 0u, "NEON immediate leaked to legacy profile");
    }
    static const uint32_t toggles[] = {0x00080000u,0x00100000u,0x00200000u,0x00000080u,0x00000010u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned field = 0; field < sizeof toggles / sizeof toggles[0]; field++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        CHECK(a8_move_step(&c, thumb, a8_neon_imm(thumb, 0u, 4u, 0u, 31u, 0x80u) ^ toggles[field]) == ARM_UNDEFINED &&
              c.r[15] == 0x100u && vfp_get_d(&c, 31u) == 0u, "NEON immediate swallowed neighboring allocation");
     }
}

static uint32_t a8_neon_mul(unsigned thumb, unsigned quad, unsigned dst, unsigned left, unsigned right) {
    return (thumb ? 0xff000d10u : 0xf3000d10u) | (quad << 6) |
        ((dst & 15u) << 12) | ((dst >> 4) << 22) |
        ((left & 15u) << 16) | ((left >> 4) << 7) | (right & 15u) | ((right >> 4) << 5);
}

static uint32_t a8_neon_mul_expected(uint32_t a, uint32_t b, uint32_t *flags) {
    /* Binary64 holds a binary32 product exactly (at most 48 significant
     * bits). Use numeric conversion as an oracle for the integer datapath,
     * with explicit ARM standard input/NaN/flush rules from A2.7. */
    uint32_t aa = a & 0x7fffffffu, bb = b & 0x7fffffffu, sign = (a ^ b) & 0x80000000u;
    if (aa && aa < 0x00800000u) { *flags |= ARM_FPSCR_IDC; aa = 0u; }
    if (bb && bb < 0x00800000u) { *flags |= ARM_FPSCR_IDC; bb = 0u; }
    if (aa > 0x7f800000u || bb > 0x7f800000u) {
        if ((aa > 0x7f800000u && !(aa & 0x00400000u)) || (bb > 0x7f800000u && !(bb & 0x00400000u))) *flags |= ARM_FPSCR_IOC;
        return 0x7fc00000u;
    }
    if (aa == 0x7f800000u || bb == 0x7f800000u) {
        if (!aa || !bb) { *flags |= ARM_FPSCR_IOC; return 0x7fc00000u; }
        return sign | 0x7f800000u;
    }
    if (!aa || !bb) return sign;
    double exact = (double)u2f(a) * (double)u2f(b);
    double magnitude = exact < 0.0 ? -exact : exact;
    if (magnitude < 0x1p-126) { *flags |= ARM_FPSCR_UFC; return sign; }
    volatile float rounded = (float)exact;
    uint32_t bits = f2u(rounded);
    if ((bits & 0x7fffffffu) == 0x7f800000u) *flags |= ARM_FPSCR_OFC | ARM_FPSCR_IXC;
    else if ((double)rounded != exact) *flags |= ARM_FPSCR_IXC;
    return bits;
}

static uint32_t a8_neon_add(unsigned thumb, unsigned subtract, unsigned quad,
                            unsigned dst, unsigned left, unsigned right) {
    return (thumb ? 0xef000d00u : 0xf2000d00u) | (subtract << 21) | (quad << 6) |
        ((dst & 15u) << 12) | ((dst >> 4) << 22) |
        ((left & 15u) << 16) | ((left >> 4) << 7) | (right & 15u) | ((right >> 4) << 5);
}

static uint32_t a8_neon_add_expected(uint32_t a, uint32_t b, bool subtract, uint32_t *flags) {
    if (subtract) b ^= 0x80000000u;
    uint32_t aa = a & 0x7fffffffu, bb = b & 0x7fffffffu;
    if (aa && aa < 0x00800000u) { *flags |= ARM_FPSCR_IDC; aa = 0u; }
    if (bb && bb < 0x00800000u) { *flags |= ARM_FPSCR_IDC; bb = 0u; }
    if (aa > 0x7f800000u || bb > 0x7f800000u) {
        if ((aa > 0x7f800000u && !(aa & 0x00400000u)) || (bb > 0x7f800000u && !(bb & 0x00400000u))) *flags |= ARM_FPSCR_IOC;
        return 0x7fc00000u;
    }
    if (aa == 0x7f800000u || bb == 0x7f800000u) {
        if (aa == bb && ((a ^ b) & 0x80000000u)) { *flags |= ARM_FPSCR_IOC; return 0x7fc00000u; }
        return aa == 0x7f800000u ? a : b;
    }
    if (!aa && !bb) return a & b & 0x80000000u;
    if (!aa) return b;
    if (!bb) return a;
    /* TwoSum retains the residual lost from the binary64 sum. A plain
     * double-sum equality check loses IXC when one input is much smaller.
     * Near a binary32 rounding boundary, both binary32 inputs fit exactly
     * in binary64; use a numeric cast, independent of production bit shifts. */
    double x = (double)u2f(a), y = (double)u2f(b);
    volatile double high = x + y, z = high - x;
    double residual = (x - (high - z)) + (y - z);
    if (high == 0.0) return 0u;
    uint32_t sign = high < 0.0 ? 0x80000000u : 0u;
    double magnitude = high < 0.0 ? -high : high;
    if (magnitude < 0x1p-126) { *flags |= ARM_FPSCR_UFC; return sign; }
    volatile float rounded = (float)high;
    uint32_t bits = f2u(rounded);
    if ((bits & 0x7fffffffu) == 0x7f800000u) *flags |= ARM_FPSCR_OFC | ARM_FPSCR_IXC;
    else if ((double)rounded != high || residual != 0.0) *flags |= ARM_FPSCR_IXC;
    return bits;
}

static uint32_t a8_neon_arithmetic_expected(unsigned kind, uint32_t a, uint32_t b, uint32_t *flags) {
    return kind ? a8_neon_add_expected(a, b, kind == 2u, flags) : a8_neon_mul_expected(a, b, flags);
}

static void a8_neon_arithmetic_check(arm_cpu_t *c, unsigned thumb, unsigned quad, unsigned dst,
                                     unsigned left, unsigned right, unsigned host, unsigned kind) {
    static const int rounds[] = {FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
    CHECK(fesetround(FE_TONEAREST) == 0, "prepare arithmetic oracle");
    uint64_t expected[32], results[2];
    for (unsigned d = 0; d < 32u; d++) expected[d] = vfp_get_d(c, d);
    uint32_t exceptions = 0;
    for (unsigned r = 0; r <= quad; r++) {
        uint32_t lo = a8_neon_arithmetic_expected(kind, (uint32_t)expected[left + r], (uint32_t)expected[right + r], &exceptions);
        uint32_t hi = a8_neon_arithmetic_expected(kind, (uint32_t)(expected[left + r] >> 32), (uint32_t)(expected[right + r] >> 32), &exceptions);
        results[r] = (uint64_t)hi << 32 | lo;
    }
    for (unsigned r = 0; r <= quad; r++) expected[dst + r] = results[r];
    uint32_t flags = c->cpsr, fpscr = c->vfp_fpscr;
    c->excl_valid = true; c->excl_addr = 0x2468u;
    CHECK(fesetround(rounds[host]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 && feraiseexcept(FE_DIVBYZERO) == 0,
          "prepare arithmetic host FP state");
    int host_flags = fetestexcept(FE_ALL_EXCEPT);
    uint32_t insn = kind ? a8_neon_add(thumb, kind == 2u, quad, dst, left, right) : a8_neon_mul(thumb, quad, dst, left, right);
    CHECK(a8_move_step(c, thumb, insn) == ARM_OK && c->r[15] == 0x104u &&
          c->cpsr == flags && c->vfp_fpscr == (fpscr | exceptions) && c->excl_valid && c->excl_addr == 0x2468u,
          "NEON arithmetic disposition/flags kind=%u T=%u Q=%u d=%u n=%u m=%u", kind, thumb, quad, dst, left, right);
    bool same = true;
    for (unsigned d = 0; d < 32u; d++) same &= vfp_get_d(c, d) == expected[d];
    for (unsigned r = 0; r < 15u; r++) same &= c->r[r] == 0u;
    CHECK(same, "NEON arithmetic result or register preservation");
    CHECK(fegetround() == rounds[host] && fetestexcept(FE_ALL_EXCEPT) == host_flags, "NEON arithmetic changed host FP environment");
}

static void a8_neon_mul_check(arm_cpu_t *c, unsigned thumb, unsigned quad, unsigned dst,
                              unsigned left, unsigned right, unsigned host) {
    a8_neon_arithmetic_check(c, thumb, quad, dst, left, right, host, 0u);
}

static void test_a8_neon_multiply_results(void) {
    /* Raw expected anchors include both tie directions, normalization carry,
     * and a tiny value that would round UP to min-normal without ARM's FZ. */
    static const uint32_t cases[][4] = {
        {0u,0x80000000u,0x80000000u,0u},{0x80000000u,0x80000000u,0u,0u},
        {0x3f800000u,0x7f800000u,0x7f800000u,0u},{0xff800000u,0xbf800000u,0x7f800000u,0u},
        {1u,0x3f800000u,0u,0x80u},{0x80000001u,0xbf800000u,0u,0x80u},
        {0x7f800000u,1u,0x7fc00000u,0x81u},{0x7fc12345u,0x80000001u,0x7fc00000u,0x80u},
        {0xff800001u,0x7fc00000u,0x7fc00000u,1u},{0x00800000u,0x3f000000u,0u,8u},
        {0x00800000u,0x3f7fffffu,0u,8u},{0x00800000u,0x3f800000u,0x00800000u,0u},
        {0x80800000u,0x3f000000u,0x80000000u,8u},{0x7f7fffffu,0x3f800000u,0x7f7fffffu,0u},
        {0x7f7fffffu,0x40000000u,0x7f800000u,0x14u},{0x3f800001u,0x3fc00000u,0x3fc00002u,0x10u},
        {0x3f800003u,0x3fc00000u,0x3fc00004u,0x10u},{0x3f800001u,0x3ffffffeu,0x40000000u,0x10u},
        {0x7f7ffffeu,0x3f800001u,0x7f800000u,0x14u},{0x3f800001u,0x3f800001u,0x3f800002u,0x10u},
        {0x3f800000u,0x3f800000u,0x3f800000u,0u},{0x3fc00000u,0x3fc00000u,0x40100000u,0u},
        {0x7fc12345u,0xffc12345u,0x7fc00000u,0u},{1u,0xff800001u,0x7fc00000u,0x81u},
        {0x00800000u,0x7f000000u,0x40000000u,0u}
    };
    const unsigned count = sizeof cases / sizeof cases[0];
    fenv_t saved;
    CHECK(fegetenv(&saved) == 0 && fesetround(FE_TONEAREST) == 0, "save multiply FP environment");
    CHECK(a8_neon_mul(1u, 0u, 0u, 17u, 16u) == 0xff010db0u &&
          a8_neon_mul(0u, 0u, 0u, 17u, 16u) == 0xf3010db0u, "firmware multiply encoding anchors");
    for (unsigned i = 0; i < count; i++) {
        uint32_t exceptions = 0;
        CHECK(a8_neon_mul_expected(cases[i][0], cases[i][1], &exceptions) == cases[i][2] && exceptions == cases[i][3],
              "multiply numeric oracle disagrees with raw anchor %u", i);
    }
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned control = 0; control < 16u; control++)
       for (unsigned i = 0; i < count; i++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr |= ARM_CPSR_E;
            c.vfp_fpscr = ARM_FPSCR_NZCV | ARM_FPSCR_QC | ARM_FPSCR_DZC | ARM_FPSCR_LEN | ARM_FPSCR_STRIDE |
                ((control & 3u) << 22) | ((control & 4u) ? ARM_FPSCR_FZ : 0u) | ((control & 8u) ? ARM_FPSCR_DN : 0u);
            for (unsigned r = 0; r <= quad; r++) {
                vfp_set_d(&c, 16u + r, (uint64_t)cases[(i + r) % count][0] << 32 | cases[i][0]);
                vfp_set_d(&c, r, (uint64_t)cases[(i + r) % count][1] << 32 | cases[i][1]);
            }
            a8_neon_mul_check(&c, thumb, quad, 30u, 16u, 0u, control & 3u);
       }
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned left = 0; left < 32u; left += quad + 1u)
       for (unsigned right = 0; right < 32u; right += quad + 1u)
        for (unsigned alias = 0; alias < 4u; alias++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.vfp_fpscr = ARM_FPSCR_QC | ARM_FPSCR_DZC;
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d,
                (uint64_t)cases[(d * 3u + alias) % count][1] << 32 | cases[(d + alias) % count][0]);
            unsigned dst = alias == 0u ? left : alias == 1u ? right : alias == 2u ?
                ((left + right + 10u) & (quad ? 30u : 31u)) : (quad ? 30u : 31u);
            a8_neon_mul_check(&c, thumb, quad, dst, left, right, (left + right + alias) % 4u);
        }
    uint32_t random = 0x89abcdefu;
    for (unsigned sample = 0; sample < 2048u; sample++) {
        unsigned thumb = sample & 1u, quad = (sample >> 1) & 1u;
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.vfp_fpscr = ARM_FPSCR_NZCV | ARM_FPSCR_QC | ARM_FPSCR_DZC;
        for (unsigned r = 0; r <= quad; r++) for (unsigned which = 0; which < 2u; which++) {
            uint64_t value = 0;
            for (unsigned e = 0; e < 2u; e++) {
                random = random * 1664525u + 1013904223u;
                uint32_t bits = (random & 0x807fffffu) | (((random >> 16) % 254u + 1u) << 23);
                value |= (uint64_t)bits << (e * 32u);
            }
            vfp_set_d(&c, (which ? 16u : 0u) + r, value);
        }
        a8_neon_mul_check(&c, thumb, quad, 30u, 16u, 0u, (sample >> 2) & 3u);
    }
    CHECK(fesetenv(&saved) == 0, "restore multiply FP environment");
}

static void test_a8_neon_multiply_access_and_invalid(void) {
    const unsigned permissions[] = {0u,1u,3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned user = 0; user < 2u; user++)
      for (unsigned access = 0; access < 3u; access++)
       for (unsigned enabled = 0; enabled < 2u; enabled++)
        for (unsigned variant = 0; variant < 9u; variant++)
         for (unsigned skip = 0; skip < (thumb ? 2u : 1u); skip++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[access] * 0x00500000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            if (thumb) { m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); CHECK(arm_step(&c) == ARM_OK, "multiply IT setup"); }
            unsigned odd = variant < 8u ? variant : 0u;
            uint32_t insn = a8_neon_mul(thumb, 1u, 30u + (odd & 1u), 16u + ((odd >> 1) & 1u), (odd >> 2) & 1u);
            if (variant == 8u) insn |= 1u << 20;
            uint32_t pc = c.r[15], flags = c.cpsr, fpscr = c.vfp_fpscr;
            bool valid = variant == 0u, allowed = enabled && (permissions[access] == 3u || (permissions[access] == 1u && !user));
            CHECK(a8_move_step(&c, thumb, insn) == (skip || valid ? ARM_OK : ARM_UNDEFINED) && c.vfp_fpscr == fpscr,
                  "multiply access/validation disposition");
            CHECK(skip ? c.r[15] == pc + 4u && c.cpsr == (flags & ~0x0600fc00u) :
                  !valid ? c.r[15] == pc && c.cpsr == flags : allowed ? c.r[15] == pc + 4u && c.cpsr == (flags & ~0x0600fc00u) :
                  c.r[15] == ARM_VEC_UNDEFINED && c.spsr[ARM_BANK_UND] == flags && c.r[14] == pc + (thumb ? 2u : 4u),
                  "multiply access priority/IT retirement");
            bool same = true;
            for (unsigned d = 0; d < 32u; d++) same &= vfp_get_d(&c, d) == 0u;
            CHECK(same, "multiply invalid/denied/zero inputs changed FP registers");
         }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "legacy multiply reset");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        CHECK(a8_move_step(&c, 0u, 0xf3010db0u) == ARM_UNDEFINED && c.r[15] == 0u, "NEON multiply leaked to legacy profile");
    }
    static const uint32_t toggles[] = {0x00800000u,0x00200000u,0x00000100u,0x00000200u,0x00000400u,0x00000010u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned field = 0; field < sizeof toggles / sizeof toggles[0]; field++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        CHECK(a8_move_step(&c, thumb, a8_neon_mul(thumb, 0u, 31u, 16u, 16u) ^ toggles[field]) == ARM_UNDEFINED &&
              c.r[15] == 0x100u && vfp_get_d(&c, 31u) == 0u, "NEON multiply swallowed neighboring allocation");
     }
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.vfp_fpscr = ARM_FPSCR_QC | 0x50000000u;
        uint32_t flags = c.cpsr;
        vfp_set_d(&c, 16u, UINT64_C(0x7f8000007f800000));
        CHECK(a8_move_step(&c, thumb, a8_neon_mul(thumb, 0u, 31u, 16u, 0u)) == ARM_OK &&
              c.vfp_fpscr == (ARM_FPSCR_QC | 0x50000001u) && c.cpsr == flags, "multiply invalid-op sticky sequence");
        vfp_set_d(&c, 16u, UINT64_C(0x3fc000003fc00000)); vfp_set_d(&c, 0u, UINT64_C(0x4000000040000000));
        CHECK(a8_move_step(&c, thumb, a8_neon_mul(thumb, 0u, 31u, 16u, 0u)) == ARM_OK &&
              vfp_get_d(&c, 31u) == UINT64_C(0x4040000040400000) && c.vfp_fpscr == (ARM_FPSCR_QC | 0x50000001u) &&
              c.cpsr == flags, "multiply exact result cleared sticky status or changed ARM flags");
        CHECK(a8_move_step(&c, thumb, 0xeef1fa10u) == ARM_OK && c.r[15] == 0x10cu &&
              c.cpsr == ((flags & ~0xf0000000u) | 0x50000000u) && c.vfp_fpscr == (ARM_FPSCR_QC | 0x50000001u),
              "VMRS after multiply did not preserve the existing FPSCR comparison flags");
    }
}

static void test_a8_neon_add_results(void) {
    /* a,b,add result/flags,subtract result/flags. Includes cancellation,
     * signed zero, both tie directions, binade edges and large exponent gaps. */
    static const uint32_t cases[][6] = {
        {0x3f800000u,0x3f800000u,0x40000000u,0u,0u,0u},
        {0x3f800000u,0xbf800000u,0u,0u,0x40000000u,0u},
        {0x80000000u,0x80000000u,0x80000000u,0u,0u,0u},
        {0u,0x80000000u,0u,0u,0u,0u},{0x80000000u,0u,0u,0u,0x80000000u,0u},
        {0x7f800000u,0x7f800000u,0x7f800000u,0u,0x7fc00000u,1u},
        {0x7f800000u,0xff800000u,0x7fc00000u,1u,0x7f800000u,0u},
        {0x7fc12345u,0x3f800000u,0x7fc00000u,0u,0x7fc00000u,0u},
        {0xff800001u,1u,0x7fc00000u,0x81u,0x7fc00000u,0x81u},
        {1u,0x3f800000u,0x3f800000u,0x80u,0xbf800000u,0x80u},
        {0x00800000u,0x00800001u,0x01000000u,0x10u,0x80000000u,8u},
        {0x00800001u,0x00800000u,0x01000000u,0x10u,0u,8u},
        {0x01000000u,0x00800000u,0x01400000u,0u,0x00800000u,0u},
        {0x7f7fffffu,0x7f7fffffu,0x7f800000u,0x14u,0u,0u},
        {0x7f7fffffu,0x73000000u,0x7f800000u,0x14u,0x7f7ffffeu,0x10u},
        {0x7f7fffffu,0x72800000u,0x7f7fffffu,0x10u,0x7f7fffffu,0x10u},
        {0x3f800000u,0x33800000u,0x3f800000u,0x10u,0x3f7fffffu,0u},
        {0x3f800001u,0x33800000u,0x3f800002u,0x10u,0x3f800000u,0x10u},
        {0x3f800000u,0x33000000u,0x3f800000u,0x10u,0x3f800000u,0x10u},
        {0x3f800000u,0x32ffffffu,0x3f800000u,0x10u,0x3f800000u,0x10u},
        {0x3f800000u,0x33000001u,0x3f800000u,0x10u,0x3f7fffffu,0x10u},
        {0x3f800000u,0x00800000u,0x3f800000u,0x10u,0x3f800000u,0x10u},
        {0x3f800000u,0x80000001u,0x3f800000u,0x80u,0x3f800000u,0x80u},
        {1u,0x80000001u,0u,0x80u,0u,0x80u},{0x80000001u,1u,0u,0x80u,0x80000000u,0x80u},
        {0x3f800000u,0x3f7fffffu,0x40000000u,0x10u,0x33800000u,0u},
        {0x3f800001u,0x3f800000u,0x40000000u,0x10u,0x34000000u,0u},
        {0x3f800002u,0x3f800000u,0x40000001u,0u,0x34800000u,0u}
    };
    const unsigned count = sizeof cases / sizeof cases[0];
    fenv_t saved;
    CHECK(fegetenv(&saved) == 0 && fesetround(FE_TONEAREST) == 0, "save addition FP environment");
    CHECK(a8_neon_add(1u, 0u, 0u, 0u, 8u, 9u) == 0xef080d09u &&
          a8_neon_add(0u, 0u, 0u, 0u, 8u, 9u) == 0xf2080d09u, "firmware addition encoding anchors");
    for (unsigned sub = 0; sub < 2u; sub++) for (unsigned i = 0; i < count; i++) {
        uint32_t exceptions = 0;
        CHECK(a8_neon_add_expected(cases[i][0], cases[i][1], sub != 0u, &exceptions) == cases[i][2u + 2u * sub] &&
              exceptions == cases[i][3u + 2u * sub], "addition oracle disagrees with raw anchor sub=%u row=%u", sub, i);
    }
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned sub = 0; sub < 2u; sub++)
       for (unsigned control = 0; control < 16u; control++)
        for (unsigned i = 0; i < count; i++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr |= ARM_CPSR_E;
            c.vfp_fpscr = ARM_FPSCR_NZCV | ARM_FPSCR_QC | ARM_FPSCR_DZC | ARM_FPSCR_LEN | ARM_FPSCR_STRIDE |
                ((control & 3u) << 22) | ((control & 4u) ? ARM_FPSCR_FZ : 0u) | ((control & 8u) ? ARM_FPSCR_DN : 0u);
            for (unsigned r = 0; r <= quad; r++) {
                vfp_set_d(&c, 16u + r, (uint64_t)cases[(i + r) % count][0] << 32 | cases[i][0]);
                vfp_set_d(&c, r, (uint64_t)cases[(i + r) % count][1] << 32 | cases[i][1]);
            }
            a8_neon_arithmetic_check(&c, thumb, quad, 30u, 16u, 0u, control & 3u, sub + 1u);
        }
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned sub = 0; sub < 2u; sub++)
       for (unsigned left = 0; left < 32u; left += quad + 1u)
        for (unsigned right = 0; right < 32u; right += quad + 1u)
         for (unsigned alias = 0; alias < 4u; alias++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.vfp_fpscr = ARM_FPSCR_QC | ARM_FPSCR_DZC;
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d,
                (uint64_t)cases[(d * 3u + alias) % count][1] << 32 | cases[(d + alias) % count][0]);
            unsigned dst = alias == 0u ? left : alias == 1u ? right : alias == 2u ?
                ((left + right + 10u) & (quad ? 30u : 31u)) : (quad ? 30u : 31u);
            a8_neon_arithmetic_check(&c, thumb, quad, dst, left, right, (left + right + alias) % 4u, sub + 1u);
         }
    uint32_t random = 0x2468ace1u;
    for (unsigned sample = 0; sample < 4096u; sample++) {
        unsigned thumb = sample & 1u, quad = (sample >> 1) & 1u, sub = (sample >> 2) & 1u;
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.vfp_fpscr = ARM_FPSCR_NZCV | ARM_FPSCR_QC | ARM_FPSCR_DZC;
        for (unsigned r = 0; r <= quad; r++) for (unsigned which = 0; which < 2u; which++) {
            uint64_t value = 0;
            for (unsigned e = 0; e < 2u; e++) {
                random = random * 1664525u + 1013904223u;
                uint32_t bits = (random & 0x807fffffu) | (((random >> 16) % 254u + 1u) << 23);
                value |= (uint64_t)bits << (e * 32u);
            }
            vfp_set_d(&c, (which ? 16u : 0u) + r, value);
        }
        a8_neon_arithmetic_check(&c, thumb, quad, 30u, 16u, 0u, (sample >> 3) & 3u, sub + 1u);
    }
    /* Exercise alignment boundaries explicitly, including machine-word and
     * binary64-precision gaps where discarded bits must still set IXC. */
    static const unsigned gaps[] = {0u,1u,2u,3u,4u,23u,24u,25u,26u,31u,32u,53u,54u,253u};
    static const uint32_t fractions[] = {0u,1u,0x007ffffeu,0x007fffffu};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned sub = 0; sub < 2u; sub++)
      for (unsigned gap = 0; gap < sizeof gaps / sizeof gaps[0]; gap++)
       for (unsigned variant = 0; variant < 4u; variant++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.vfp_fpscr = ARM_FPSCR_NZCV | ARM_FPSCR_QC | ARM_FPSCR_DZC;
        for (unsigned r = 0; r < 2u; r++) {
            uint32_t a = 0x7f000000u | fractions[(variant + r) % 4u];
            uint32_t b = ((254u - gaps[gap]) << 23) | fractions[(variant + r + 1u) % 4u] |
                ((variant & 1u) ? 0x80000000u : 0u);
            vfp_set_d(&c, 16u + r, (uint64_t)(a ^ 0x80000000u) << 32 | a);
            vfp_set_d(&c, r, (uint64_t)b << 32 | b);
        }
        a8_neon_arithmetic_check(&c, thumb, 1u, 30u, 16u, 0u, variant, sub + 1u);
       }
    CHECK(fesetenv(&saved) == 0, "restore addition FP environment");
}

static void test_a8_neon_add_access_and_invalid(void) {
    const unsigned permissions[] = {0u,1u,3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned sub = 0; sub < 2u; sub++)
      for (unsigned user = 0; user < 2u; user++)
       for (unsigned access = 0; access < 3u; access++)
        for (unsigned enabled = 0; enabled < 2u; enabled++)
         for (unsigned variant = 0; variant < 9u; variant++)
          for (unsigned skip = 0; skip < (thumb ? 2u : 1u); skip++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[access] * 0x00500000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            if (thumb) { m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); CHECK(arm_step(&c) == ARM_OK, "addition IT setup"); }
            unsigned odd = variant < 8u ? variant : 0u;
            uint32_t insn = a8_neon_add(thumb, sub, 1u, 30u + (odd & 1u), 16u + ((odd >> 1) & 1u), (odd >> 2) & 1u);
            if (variant == 8u) insn |= 1u << 20;
            uint32_t pc = c.r[15], flags = c.cpsr, fpscr = c.vfp_fpscr;
            bool valid = variant == 0u, allowed = enabled && (permissions[access] == 3u || (permissions[access] == 1u && !user));
            CHECK(a8_move_step(&c, thumb, insn) == (skip || valid ? ARM_OK : ARM_UNDEFINED) && c.vfp_fpscr == fpscr,
                  "addition access/validation disposition");
            CHECK(skip ? c.r[15] == pc + 4u && c.cpsr == (flags & ~0x0600fc00u) :
                  !valid ? c.r[15] == pc && c.cpsr == flags : allowed ? c.r[15] == pc + 4u && c.cpsr == (flags & ~0x0600fc00u) :
                  c.r[15] == ARM_VEC_UNDEFINED && c.spsr[ARM_BANK_UND] == flags && c.r[14] == pc + (thumb ? 2u : 4u),
                  "addition access priority/IT retirement");
            bool same = true;
            for (unsigned d = 0; d < 32u; d++) same &= vfp_get_d(&c, d) == 0u;
            CHECK(same, "addition invalid/denied/zero inputs changed FP registers");
          }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++) for (unsigned sub = 0; sub < 2u; sub++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "legacy addition reset");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        CHECK(a8_move_step(&c, 0u, a8_neon_add(0u, sub, 0u, 0u, 8u, 9u)) == ARM_UNDEFINED && c.r[15] == 0u,
              "NEON addition leaked to legacy profile");
    }
    static const uint32_t toggles[] = {0x00800000u,0x00000100u,0x00000200u,0x00000400u,0x00000010u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned sub = 0; sub < 2u; sub++)
      for (unsigned field = 0; field < sizeof toggles / sizeof toggles[0]; field++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        CHECK(a8_move_step(&c, thumb, a8_neon_add(thumb, sub, 0u, 31u, 16u, 16u) ^ toggles[field]) == ARM_UNDEFINED &&
              c.r[15] == 0x100u && vfp_get_d(&c, 31u) == 0u, "NEON addition swallowed neighboring allocation");
      }
    for (unsigned thumb = 0; thumb < 2u; thumb++) for (unsigned sub = 0; sub < 2u; sub++) {
        arm_cpu_t c;
        a8_move_reset(&c, thumb);
        c.vfp_fpscr = ARM_FPSCR_QC | 0x50000000u;
        uint32_t flags = c.cpsr;
        vfp_set_d(&c, 16u, UINT64_C(0x7f8000007f800000));
        vfp_set_d(&c, 0u, sub ? UINT64_C(0x7f8000007f800000) : UINT64_C(0xff800000ff800000));
        CHECK(a8_move_step(&c, thumb, a8_neon_add(thumb, sub, 0u, 31u, 16u, 0u)) == ARM_OK &&
              c.vfp_fpscr == (ARM_FPSCR_QC | 0x50000001u) && c.cpsr == flags, "addition invalid-op sticky sequence");
        vfp_set_d(&c, 16u, UINT64_C(0x3f8000003f800000)); vfp_set_d(&c, 0u, UINT64_C(0x4000000040000000));
        CHECK(a8_move_step(&c, thumb, a8_neon_add(thumb, sub, 0u, 31u, 16u, 0u)) == ARM_OK &&
              vfp_get_d(&c, 31u) == (sub ? UINT64_C(0xbf800000bf800000) : UINT64_C(0x4040000040400000)) &&
              c.vfp_fpscr == (ARM_FPSCR_QC | 0x50000001u) && c.cpsr == flags, "addition exact result cleared sticky flags");
        CHECK(a8_move_step(&c, thumb, 0xeef1fa10u) == ARM_OK && c.r[15] == 0x10cu &&
              c.cpsr == ((flags & ~0xf0000000u) | 0x50000000u) && c.vfp_fpscr == (ARM_FPSCR_QC | 0x50000001u),
              "VMRS after addition changed existing comparison flags");
    }
}

/* A8.8.314: VDUP from a core register has D in bit7, Q in bit21,
 * and its destination in bits19:16, unlike the SIMD data-processing forms. */
static uint32_t a8_core_duplicate(unsigned size, unsigned quad, unsigned dst, unsigned rt) {
    return 0xee800b10u | (size == 8u ? 1u << 22 : size == 16u ? 1u << 5 : 0u) |
        (quad << 21) | ((dst & 15u) << 16) | ((dst >> 4) << 7) | (rt << 12);
}

static uint64_t a8_duplicate_expected(uint32_t source, unsigned size) {
    uint64_t result = 0;
    for (unsigned bit = 0; bit < 64u; bit++)
        if ((source >> (bit % size)) & 1u) result |= UINT64_C(1) << bit;
    return result;
}

static void test_a8_neon_core_duplicate_registers(void) {
    CHECK(a8_core_duplicate(8u, 1u, 0u, 1u) == 0xeee01b10u &&
          a8_core_duplicate(16u, 0u, 31u, 2u) == 0xee8f2bb0u &&
          a8_core_duplicate(32u, 1u, 30u, 14u) == 0xeeaeeb90u, "VDUP core encoding anchors");
    static const uint32_t patterns[] = {0u,UINT32_MAX,1u,0x80000000u,0xdead1280u,0x12345678u,0x7f800001u,0x00ff00ffu};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned size = 8u; size <= 32u; size *= 2u)
       for (unsigned dst = 0; dst < 32u; dst += 1u + quad)
        for (unsigned rt = 0; rt < 15u; rt++) {
            if (thumb && rt == 13u) continue;
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d, UINT64_C(0x7ff01234dead0000) + d);
            for (unsigned r = 0; r < 15u; r++) c.r[r] = 0xa5100000u + r;
            c.r[rt] = patterns[(dst + rt + size / 8u) % 8u];
            uint32_t before[15]; memcpy(before, c.r, sizeof before);
            c.vfp_fpscr |= ((dst & 7u) << 16) | ((rt & 3u) << 20) | ARM_FPSCR_N | ARM_FPSCR_DZC;
            if (rt & 1u) c.cpsr |= ARM_CPSR_E;
            c.excl_valid = true; c.excl_addr = 0x12340u;
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            uint64_t expected = a8_duplicate_expected(c.r[rt], size);
            CHECK(a8_move_step(&c, thumb, a8_core_duplicate(size, quad, dst, rt)) == ARM_OK &&
                  c.r[15] == 0x104u && c.cycles == 1u && c.cpsr == flags && c.vfp_fpscr == fpscr &&
                  c.vfp_fpexc == ARM_FPEXC_EN && c.excl_valid && c.excl_addr == 0x12340u &&
                  !memcmp(before, c.r, sizeof before), "VDUP core register/state T=%u Q=%u size=%u D=%u Rt=%u", thumb, quad, size, dst, rt);
            for (unsigned d = 0; d < 32u; d++) CHECK(vfp_get_d(&c, d) ==
                (d >= dst && d <= dst + quad ? expected : UINT64_C(0x7ff01234dead0000) + d),
                "VDUP core destination D%u", d);
        }
}

static void test_a8_neon_core_duplicate_access(void) {
    static const unsigned permissions[] = {0u,1u,3u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned size = 8u; size <= 32u; size *= 2u)
       for (unsigned user = 0; user < 2u; user++)
        for (unsigned enabled = 0; enabled < 2u; enabled++)
         for (unsigned access = 0; access < 3u; access++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | (user ? ARM_MODE_USR : ARM_MODE_SVC);
            c.cp15.cpacr = permissions[access] * 0x00500000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            c.r[2] = 0xabcdef01u;
            vfp_set_d(&c, 30u, UINT64_C(0x7ff0000000000001));
            vfp_set_d(&c, 31u, UINT64_C(0x8000000000000000));
            bool allowed = enabled && (permissions[access] == 3u || (permissions[access] == 1u && !user));
            uint32_t flags = c.cpsr, fpscr = c.vfp_fpscr;
            uint64_t expected = a8_duplicate_expected(c.r[2], size);
            CHECK(a8_move_step(&c, thumb, a8_core_duplicate(size, quad, 30u, 2u)) == ARM_OK &&
                  c.r[2] == 0xabcdef01u && c.vfp_fpscr == fpscr, "VDUP core access disposition");
            CHECK(allowed ? c.r[15] == 0x104u && c.cpsr == flags :
                  c.r[15] == ARM_VEC_UNDEFINED && c.r[14] == (thumb ? 0x102u : 0x104u) && c.spsr[ARM_BANK_UND] == flags,
                  "VDUP core access exception state");
            CHECK(vfp_get_d(&c, 30u) == (allowed ? expected : UINT64_C(0x7ff0000000000001)) &&
                  vfp_get_d(&c, 31u) == (allowed && quad ? expected : UINT64_C(0x8000000000000000)),
                  "VDUP core access destination");
         }
    fenv_t saved;
    CHECK(fegetenv(&saved) == 0, "save duplicate host FP state");
    static const int rounds[] = {FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned quad = 0; quad < 2u; quad++)
      for (unsigned size = 8u; size <= 32u; size *= 2u)
       for (unsigned host = 0; host < 4u; host++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb); c.r[2] = 0xff800001u;
            CHECK(fesetround(rounds[host]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 && feraiseexcept(FE_DIVBYZERO) == 0,
                  "prepare duplicate host FP state");
            int exceptions = fetestexcept(FE_ALL_EXCEPT);
            CHECK(a8_move_step(&c, thumb, a8_core_duplicate(size, quad, 30u, 2u)) == ARM_OK &&
                  fegetround() == rounds[host] && fetestexcept(FE_ALL_EXCEPT) == exceptions, "duplicate touched host FP state");
       }
    CHECK(fesetenv(&saved) == 0, "restore duplicate host FP state");
}

static void test_a8_neon_core_duplicate_invalid_and_it(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned enabled = 0; enabled < 2u; enabled++)
      for (unsigned skip = 0; skip < 2u; skip++)
       for (unsigned kind = 0; kind < 22u; kind++) {
            arm_cpu_t c;
            a8_move_reset(&c, thumb);
            c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
            if (skip) c.cp15.cpacr = 0u;
            c.r[2] = 0xabcdef01u; c.r[13] = 0x12345678u;
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c, d, UINT64_C(0x7ff01234dead0000) + d);
            uint32_t insn = a8_core_duplicate(32u, 1u, kind == 1u ? 31u : 30u,
                kind == 2u ? 15u : kind == 3u ? 13u : 2u);
            if (kind == 0u) insn |= 0x00400020u; /* B:E=11 */
            if (kind >= 4u && kind < 19u) insn |= kind - 3u; /* every nonzero reserved low nibble */
            if (kind == 19u) insn |= 0x40u; /* neighboring allocation */
            if (kind == 20u) insn |= 1u << 20; /* read direction, not duplicate */
            if (kind == 21u) insn ^= 0x100u; /* CP10, not SIMD */
            bool valid = kind == 3u && !thumb;
            if (thumb) { m_w16(NULL, 0x100u, skip ? 0xbf08u : 0xbf18u); CHECK(arm_step(&c) == ARM_OK, "duplicate IT setup"); }
            else if (skip) insn &= 0x0fffffffu;
            uint32_t pc = c.r[15], flags = c.cpsr, fpscr = c.vfp_fpscr;
            CHECK(a8_move_step(&c, thumb, insn) == (skip || valid ? ARM_OK : ARM_UNDEFINED),
                  "VDUP core invalid/IT disposition T=%u EN=%u skip=%u kind=%u", thumb, enabled, skip, kind);
            if (skip || !valid) CHECK(c.r[15] == (skip ? pc + 4u : pc) &&
                c.cpsr == (thumb && skip ? flags & ~0x0600fc00u : flags) && c.vfp_fpscr == fpscr,
                "VDUP core invalid/skipped state changed");
            else if (!enabled) CHECK(c.r[15] == ARM_VEC_UNDEFINED && c.spsr[ARM_BANK_UND] == flags, "duplicate lazy enable");
            else CHECK(c.r[15] == pc + 4u && c.cpsr == flags, "ARM VDUP from SP did not retire");
            uint64_t expected = a8_duplicate_expected(0x12345678u, 32u);
            for (unsigned d = 0; d < 32u; d++) CHECK(vfp_get_d(&c, d) ==
                (valid && enabled && !skip && d >= 30u ? expected : UINT64_C(0x7ff01234dead0000) + d),
                "VDUP core invalid/IT destination");
       }
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned enabled = 0; enabled < 2u; enabled++) {
        arm_cpu_t c; a8_move_reset(&c, thumb); c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
        CHECK(a8_move_step(&c, thumb, 0xfea02b10u) == ARM_UNDEFINED && c.r[15] == 0x100u,
              "VDUP core swallowed MCR2 instruction-set prefix");
     }
    const arm_arch_t legacy[] = {ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT};
    for (unsigned profile = 0; profile < 2u; profile++) {
        arm_cpu_t c;
        CHECK(arm_reset_profile(&c, &g_bus, legacy[profile]), "legacy duplicate reset");
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN;
        CHECK(a8_move_step(&c, 0u, 0xeee01b10u) == ARM_UNDEFINED && c.r[15] == 0u,
              "NEON duplicate leaked to legacy profile");
    }
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
    test_a8_neon_bitwise_registers();
    test_a8_neon_bitwise_access_and_host_state();
    test_a8_neon_bitwise_invalid_and_it();
    test_a8_neon_extract_registers();
    test_a8_neon_extract_access_and_invalid();
    test_a8_neon_immediate_constants();
    test_a8_neon_immediate_access();
    test_a8_neon_immediate_invalid_and_it();
    test_a8_neon_multiply_results();
    test_a8_neon_multiply_access_and_invalid();
    test_a8_neon_add_results();
    test_a8_neon_add_access_and_invalid();
    test_a8_neon_memory_registers();
    test_a8_neon_memory_alignment_and_access();
    test_a8_neon_memory_invalid_and_it();
    test_a8_vfp_compare_registers_and_values();
    test_a8_vfp_compare_access_and_host_state();
    test_a8_vfp_compare_status_sequence();
    test_a8_vfp_compare_invalid_and_conditional();
    test_a8_vfp_bitwise_scalar_registers();
    test_a8_vfp_all_immediate_constants();
    test_a8_vfp_bitwise_vectors();
    test_a8_vfp_bitwise_access_and_host_state();
    test_a8_vfp_bitwise_invalid_and_conditional();
    test_a8_vfp_multiple_memory_register_lists();
    test_a8_vfp_multiple_memory_rejections_and_pc();
    test_a8_vfp_multiple_odd_word_gap();
    test_a8_vfp_multiple_access_and_skip();
    test_a8_vfp_single_memory_bank_and_offsets();
    test_a8_vfp_single_memory_bases_and_conditions();
    test_a8_vfp_single_memory_permissions();
    test_a8_vfp_single_memory_decode_boundaries();
    test_a8_vfp_core_move_refusals_and_it();
    test_a8_neon_core_duplicate_registers();
    test_a8_neon_core_duplicate_access();
    test_a8_neon_core_duplicate_invalid_and_it();
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
