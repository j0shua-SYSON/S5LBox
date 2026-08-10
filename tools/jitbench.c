/*
 * S5LBox -- Apple-arm64 native-semantics ceiling benchmark.
 *
 * This is deliberately NOT a product-performance claim and not a JIT
 * dispatcher. It translates one small synthetic block once, then compares
 * repeated interpreter execution with both that already-built block and a
 * firmware-independent static-threaded proof. The proof's 26,508 generic
 * ISA/register handlers are compiled and signed with the executable; runtime
 * decoding creates data records only. The table includes product-only guarded
 * read/write-cache, exact VFP register/system transfer, guarded scalar VFPv2
 * arithmetic and terminal A32 immediate plus A32/Thumb indirect branches.
 *
 * The answer is only a feasibility bound. The native and direct product-entry
 * rows have no device tick, MMIO, interrupt sampling, cache lookup,
 * translation, chaining, framebuffer publication, UIKit, or real iOS
 * instruction mix. A separate SoC-entry curve now adds the real machine run
 * API, signed cache/raw witness, dynamic gates, timer boundaries and device
 * ticks, with complete serialized-machine equality. It still uses tiny
 * synthetic MMU-off loops and omits firmware, framebuffer publication and UI.
 * In particular, a positive result cannot be reported as phone FPS or as proof
 * that a complete no-JIT interpreter will have the same speed. Separate
 * exactness cases cover every A32 data-processing opcode, all conditions,
 * immediate and register barrel-shifter edge cases, r8-r14 and the
 * architecturally valid PC reads. Those omissions are why this remains an
 * architecture gate, not an emulator speed claim.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "jit.h"
#include "a64_static.h"
#include "snapshot.h"
#include "soc.h"
#include "vfp.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define RAM_SIZE       (1u << 20)
#define DATA_BASE      0x8000u
#define STACK_BASE     0x9000u
#define CODE_WORDS     4096u
#define DEFAULT_INSNS  20000000ull
#define DEFAULT_REPS   3u
#define FETCH_REFILL_MIX_BLOCKS 20u
#define FETCH_REFILL_MIX_SINGLE_BLOCKS 3u
#define FETCH_REFILL_MIX_LONG_BLOCKS 17u
#define FETCH_REFILL_MIX_LONG_INSNS 10u
#define FETCH_REFILL_MIX_MAX_LONG_INSNS 64u
#define FETCH_REFILL_MIX_LOOP_INSNS 173u

_Static_assert(FETCH_REFILL_MIX_SINGLE_BLOCKS +
                   FETCH_REFILL_MIX_LONG_BLOCKS ==
                   FETCH_REFILL_MIX_BLOCKS,
               "fetch-refill mix block counts disagree");
_Static_assert(FETCH_REFILL_MIX_SINGLE_BLOCKS +
                   FETCH_REFILL_MIX_LONG_BLOCKS *
                       FETCH_REFILL_MIX_LONG_INSNS ==
                   FETCH_REFILL_MIX_LOOP_INSNS,
               "fetch-refill mix instruction count disagrees");

typedef struct {
    const char *name;
    const void *program;
    unsigned insns;
    bool thumb;
    bool touches_memory;
} bench_case_t;

typedef struct {
    uint32_t r[16];
    uint32_t cpsr;
    uint64_t cycles;
    uint64_t ram_hash;
    arm_status_t status;
    int jit_exit;
} final_state_t;

static uint8_t g_ram[RAM_SIZE];

static uint32_t mem_r32(void *ctx, uint32_t addr) {
    uint32_t value;
    (void)ctx;
    memcpy(&value, &g_ram[addr & (RAM_SIZE - 1u)], sizeof value);
    return value;
}

static uint16_t mem_r16(void *ctx, uint32_t addr) {
    uint16_t value;
    (void)ctx;
    memcpy(&value, &g_ram[addr & (RAM_SIZE - 1u)], sizeof value);
    return value;
}

static uint8_t mem_r8(void *ctx, uint32_t addr) {
    (void)ctx;
    return g_ram[addr & (RAM_SIZE - 1u)];
}

static void mem_w32(void *ctx, uint32_t addr, uint32_t value) {
    (void)ctx;
    memcpy(&g_ram[addr & (RAM_SIZE - 1u)], &value, sizeof value);
}

static void mem_w16(void *ctx, uint32_t addr, uint16_t value) {
    (void)ctx;
    memcpy(&g_ram[addr & (RAM_SIZE - 1u)], &value, sizeof value);
}

static void mem_w8(void *ctx, uint32_t addr, uint8_t value) {
    (void)ctx;
    g_ram[addr & (RAM_SIZE - 1u)] = value;
}

static uint8_t *mem_host_ram(void *ctx, uint32_t addr, uint32_t len) {
    (void)ctx;
    if ((uint64_t)addr + len > RAM_SIZE) return NULL;
    return &g_ram[addr];
}

static const arm_bus_t g_bus = {
    .ctx = NULL,
    .read32 = mem_r32, .read16 = mem_r16, .read8 = mem_r8,
    .write32 = mem_w32, .write16 = mem_w16, .write8 = mem_w8,
    .host_ram = mem_host_ram,
};

/* Fifteen ordinary operations plus a branch back to address zero. The mixed
 * rows contain four memory operations out of sixteen instructions (25%),
 * close to the historical 22.6% guest share and enough to expose helper-call
 * cost, but still not presented as a replay of the measured guest mix. */
static const uint32_t A32_ALU[] = {
    0xe2800001u, 0xe2811003u, 0xe2422001u, 0xe0233000u,
    0xe0844001u, 0xe2455002u, 0xe1866002u, 0xe0200004u,
    0xe0811005u, 0xe2422003u, 0xe0833006u, 0xe2244007u,
    0xe2855001u, 0xe0466001u, 0xe0877002u, 0xeaffffefu,
};

static const uint32_t A32_MIXED[] = {
    0xe2800001u, 0xe5870000u, 0xe5971000u, 0xe0822001u,
    0xe2833001u, 0xe0244003u, 0xe2855001u, 0xe0466005u,
    0xe5874008u, 0xe5975008u, 0xe0866005u, 0xe0200006u,
    0xe2811001u, 0xe2422001u, 0xe0833002u, 0xeaffffefu,
};

/* Four ordinary stores in a sixteen-instruction loop. This is a deliberately
 * synthetic same-binary A/B for the app-facing SoC path: it is store-heavier
 * than the restored workload and is never presented as firmware or phone FPS.
 * r7 is seeded to DATA_BASE by the store benchmark before either warmup. */
static const uint32_t A32_SOC_STORES[] = {
    0xe2800001u, 0xe5870000u, 0xe2811003u, 0xe5871004u,
    0xe0222001u, 0xe5973000u, 0xe2844001u, 0xe5874008u,
    0xe0855003u, 0xe5976004u, 0xe2466001u, 0xe587600cu,
    0xe0800006u, 0xe0211005u, 0xe2422001u, 0xeaffffefu,
};

/* Eight signed ALU instructions, one deliberately unsupported but ordinary
 * interpreter MUL, and a signed branch. In steady state the branch chains into
 * the ALU head and stops on the already-cached negative MUL descriptor. This
 * matches the restored trace's common native-to-ineligible boundary shape
 * without pretending to replay firmware or report phone FPS. */
static const uint32_t A32_SOC_NEGATIVE_BOUNDARY[] = {
    UINT32_C(0xe2833001), UINT32_C(0xe2844003),
    UINT32_C(0xe2455001), UINT32_C(0xe0266003),
    UINT32_C(0xe0877004), UINT32_C(0xe2888001),
    UINT32_C(0xe2899001), UINT32_C(0xe24aa001),
    UINT32_C(0xe0000090), /* MUL r0,r0,r0: literal interpreter */
    UINT32_C(0xeafffff5), /* branch at 0x24 -> 0x00 */
};

/* Four two-instruction heads cross ARM/Thumb state through every register
 * branch family. Distinct graph slots remove alias churn from the timing:
 * ARM 0x00 -> Thumb 0x10 -> ARM 0x40 -> Thumb 0x60 -> ARM 0x00. */
static const uint32_t A32_SOC_INDIRECT_ZERO[] = {
    UINT32_C(0xe2844001), /* ADD r4,r4,#1 */
    UINT32_C(0xe12fff38), /* BLX r8 */
};
static const uint16_t THUMB_SOC_INDIRECT_TEN[] = {
    UINT16_C(0x3501), /* ADDS r5,#1 */
    UINT16_C(0x4748), /* BX r9 */
};
static const uint32_t A32_SOC_INDIRECT_FORTY[] = {
    UINT32_C(0xe2866001), /* ADD r6,r6,#1 */
    UINT32_C(0xe12fff1a), /* BX r10 */
};
static const uint16_t THUMB_SOC_INDIRECT_SIXTY[] = {
    UINT16_C(0x3701), /* ADDS r7,#1 */
    UINT16_C(0x47d8), /* BLX r11 */
};

/* Four terminal conditional branches in a sixteen-instruction Thumb loop.
 * Each branch's taken target equals its natural fallthrough (imm8=-1), so
 * taken and failed conditions execute the same fixed-size loop while still
 * exercising the live NZCV selector and terminal graph exit. The paired flag
 * setters guarantee both outcomes before long-run register wraparound. */
static const uint16_t THUMB_SOC_CONDITIONAL[] = {
    UINT16_C(0x2000), /* MOVS r0,#0: Z=1 */
    UINT16_C(0xd0ff), /* BEQ next: taken */
    UINT16_C(0x3101), /* ADDS r1,#1: normally Z=0 */
    UINT16_C(0xd0ff), /* BEQ next: fallthrough */
    UINT16_C(0x2200), /* MOVS r2,#0: Z=1 */
    UINT16_C(0xd1ff), /* BNE next: fallthrough */
    UINT16_C(0x3301), /* ADDS r3,#1: normally Z=0 */
    UINT16_C(0xd1ff), /* BNE next: taken */
    UINT16_C(0x3401), UINT16_C(0x3503),
    UINT16_C(0x3e01), UINT16_C(0x3705),
    UINT16_C(0x3002), UINT16_C(0x3901),
    UINT16_C(0x3204), UINT16_C(0xe7ef),
};

static const uint16_t THUMB_ALU[] = {
    0x3001u, 0x3103u, 0x3a01u, 0x3305u,
    0x3c02u, 0x3507u, 0x3e03u, 0x3709u,
    0x3002u, 0x3901u, 0x3204u, 0x3b03u,
    0x3401u, 0x3d02u, 0x3605u, 0xe7efu,
};

static const uint16_t THUMB_MIXED[] = {
    0x3001u, 0x9001u, 0x9901u, 0x3203u,
    0x3301u, 0x4043u, 0x3401u, 0x3501u,
    0x9502u, 0x9e02u, 0x3601u, 0x3701u,
    0x3002u, 0x3901u, 0x3201u, 0xe7efu,
};

/* Short blocks exercise the product-facing block contract independently of
 * the 16-instruction benchmark loops: variable length, natural fallthrough,
 * and a terminal branch whose destination is not the block start. */
static const uint32_t A32_SHORT_FALL[] = {
    0xe2800001u, 0xe2811003u, 0xe2422001u,
};

static const uint32_t A32_SHORT_BRANCH[] = {
    0xe2800001u, 0xea00000du, /* branch at 0x1004 -> 0x1040 */
};

static const uint16_t THUMB_SHORT_FALL[] = {
    0x3001u, 0x3103u, 0x3a01u,
};

static const uint16_t THUMB_SHORT_BRANCH[] = {
    0x3001u, 0xe00du, /* branch at 0x0202 -> 0x0220 */
};

#define THUMB_ALU_REG(opcode, rd, rm)                                      \
    ((uint16_t)(UINT16_C(0x4000) | ((uint16_t)(opcode) << 6) |             \
                ((uint16_t)(rm) << 3) | (uint16_t)(rd)))

/* Every 0x4000 Thumb register-ALU opcode. The static path deliberately reuses
 * the full A32 barrel-shifter/ALU records for the shared semantics; EOR and
 * MUL retain compact signed handlers. */
static const uint16_t THUMB_REGISTER_ALU_ALL[] = {
    THUMB_ALU_REG( 0, 0, 1), /* AND  r0,r1 */
    THUMB_ALU_REG( 1, 2, 3), /* EOR  r2,r3 */
    THUMB_ALU_REG( 2, 4, 5), /* LSL  r4,r5 */
    THUMB_ALU_REG( 3, 6, 1), /* LSR  r6,r1 */
    THUMB_ALU_REG( 4, 7, 2), /* ASR  r7,r2 */
    THUMB_ALU_REG( 5, 0, 3), /* ADC  r0,r3 */
    THUMB_ALU_REG( 6, 1, 4), /* SBC  r1,r4 */
    THUMB_ALU_REG( 7, 2, 5), /* ROR  r2,r5 */
    THUMB_ALU_REG( 8, 3, 6), /* TST  r3,r6 */
    THUMB_ALU_REG( 9, 4, 7), /* NEG  r4,r7 */
    THUMB_ALU_REG(10, 5, 0), /* CMP  r5,r0 */
    THUMB_ALU_REG(11, 6, 1), /* CMN  r6,r1 */
    THUMB_ALU_REG(12, 7, 2), /* ORR  r7,r2 */
    THUMB_ALU_REG(13, 0, 3), /* MUL  r0,r3 */
    THUMB_ALU_REG(14, 1, 4), /* BIC  r1,r4 */
    THUMB_ALU_REG(15, 2, 5), /* MVN  r2,r5 */
};

/* Broad non-control Thumb integer forms, including immediate/register shifts,
 * three-bit ADD/SUB, all four immediate ALU families, high-register PC reads,
 * PC/SP address formation and SP adjustment. Writes to PC remain refused. */
static const uint16_t THUMB_INTEGER_MISC[] = {
    0x0008u, /* LSLS r0,r1,#0  */
    0x081au, /* LSRS r2,r3,#32 */
    0x17ecu, /* ASRS r4,r5,#31 */
    0x188eu, /* ADDS r6,r1,r2  */
    0x1fdfu, /* SUBS r7,r3,#7  */
    0x2080u, /* MOVS r0,#0x80  */
    0x29ffu, /* CMP  r1,#0xff  */
    0x3201u, /* ADDS r2,#1     */
    0x3b02u, /* SUBS r3,#2     */
    0x4480u, /* ADD  r8,r0     */
    0x45c7u, /* CMP  pc,r8     */
    0x46fau, /* MOV  r10,pc    */
    0xa403u, /* ADD  r4,pc,#12 */
    0xad05u, /* ADD  r5,sp,#20 */
    0xb008u, /* ADD  sp,#32    */
    0xb087u, /* SUB  sp,#28    */
};

#define A32_DP_IMM(cond, opcode, set, rn, rd, rotate, imm8)                 \
    (((uint32_t)(cond) << 28) | UINT32_C(0x02000000) |                     \
     ((uint32_t)(opcode) << 21) | ((uint32_t)(set) << 20) |                \
     ((uint32_t)(rn) << 16) | ((uint32_t)(rd) << 12) |                     \
     ((uint32_t)(rotate) << 8) | (uint32_t)(imm8))

#define A32_DP_REG_IMM(cond, opcode, set, rn, rd, rm, type, amount)         \
    (((uint32_t)(cond) << 28) | ((uint32_t)(opcode) << 21) |               \
     ((uint32_t)(set) << 20) | ((uint32_t)(rn) << 16) |                    \
     ((uint32_t)(rd) << 12) | ((uint32_t)(amount) << 7) |                  \
     ((uint32_t)(type) << 5) | (uint32_t)(rm))

#define A32_DP_REG_REG(cond, opcode, set, rn, rd, rm, type, rs)             \
    (((uint32_t)(cond) << 28) | ((uint32_t)(opcode) << 21) |               \
     ((uint32_t)(set) << 20) | ((uint32_t)(rn) << 16) |                    \
     ((uint32_t)(rd) << 12) | ((uint32_t)(rs) << 8) |                      \
     ((uint32_t)(type) << 5) | UINT32_C(0x10) | (uint32_t)(rm))

#define A32_SINGLE(cond, indexed, up, byte, load, rn, rd, offset)           \
    (((uint32_t)(cond) << 28) | UINT32_C(0x04000000) |                     \
     ((uint32_t)(indexed) << 25) | UINT32_C(0x01000000) |                  \
     ((uint32_t)(up) << 23) | ((uint32_t)(byte) << 22) |                   \
     ((uint32_t)(load) << 20) | ((uint32_t)(rn) << 16) |                   \
     ((uint32_t)(rd) << 12) | (uint32_t)(offset))

#define A32_SINGLE_MODE2(cond, indexed, pre, up, byte, writeback, load,     \
                         rn, rd, offset)                                    \
    (((uint32_t)(cond) << 28) | UINT32_C(0x04000000) |                     \
     ((uint32_t)(indexed) << 25) | ((uint32_t)(pre) << 24) |               \
     ((uint32_t)(up) << 23) | ((uint32_t)(byte) << 22) |                   \
     ((uint32_t)(writeback) << 21) | ((uint32_t)(load) << 20) |           \
     ((uint32_t)(rn) << 16) | ((uint32_t)(rd) << 12) |                    \
     (uint32_t)(offset))

#define A32_BLOCK(cond, pre, up, user_bank, writeback, load, rn, list)      \
    (((uint32_t)(cond) << 28) | UINT32_C(0x08000000) |                    \
     ((uint32_t)(pre) << 24) | ((uint32_t)(up) << 23) |                   \
     ((uint32_t)(user_bank) << 22) | ((uint32_t)(writeback) << 21) |      \
     ((uint32_t)(load) << 20) | ((uint32_t)(rn) << 16) |                  \
     (uint32_t)(list))

#define A32_COPROC_TRANSFER(cond, opc1, load, crn, rd, cp, opc2, crm)       \
    (((uint32_t)(cond) << 28) | UINT32_C(0x0e000010) |                     \
     ((uint32_t)(opc1) << 21) | ((uint32_t)(load) << 20) |                 \
     ((uint32_t)(crn) << 16) | ((uint32_t)(rd) << 12) |                    \
     ((uint32_t)(cp) << 8) | ((uint32_t)(opc2) << 5) | (uint32_t)(crm))

#define VFP_SV(n) ((uint32_t)(n) >> 1)
#define VFP_SB(n) ((uint32_t)(n) & 1u)
#define VFP_DP(p,q,r,D,vn,vd,sz,N,s,M,vm)                                  \
    (UINT32_C(0xee000a00) | ((uint32_t)(p) << 23) |                        \
     ((uint32_t)(D) << 22) | ((uint32_t)(q) << 21) |                      \
     ((uint32_t)(r) << 20) | ((uint32_t)(vn) << 16) |                     \
     ((uint32_t)(vd) << 12) | ((uint32_t)(sz) << 8) |                     \
     ((uint32_t)(N) << 7) | ((uint32_t)(s) << 6) |                        \
     ((uint32_t)(M) << 5) | (uint32_t)(vm))
#define VFP_UN_S(opc2, top, sd, sm)                                        \
    VFP_DP(1, 1, 1, VFP_SB(sd), (opc2), VFP_SV(sd), 0, (top), 1,          \
           VFP_SB(sm), VFP_SV(sm))
#define VFP_UN_D(opc2, top, dd, dm)                                        \
    VFP_DP(1, 1, 1, 0, (opc2), (dd), 1, (top), 1, 0, (dm))
#define VFP_ARITH_S(op, alt, sd, sn, sm)                                   \
    VFP_DP(((op) >> 2) & 1u, ((op) >> 1) & 1u, (op) & 1u,                 \
           VFP_SB(sd), VFP_SV(sn), VFP_SV(sd), 0, VFP_SB(sn), (alt),      \
           VFP_SB(sm), VFP_SV(sm))
#define VFP_ARITH_D(op, alt, dd, dn, dm)                                   \
    VFP_DP(((op) >> 2) & 1u, ((op) >> 1) & 1u, (op) & 1u,                 \
           0, (dn), (dd), 1, 0, (alt), 0, (dm))
#define VFP_WIDEN(dd, sm)                                                  \
    VFP_DP(1, 1, 1, 0, 7, (dd), 0, 1, 1, VFP_SB(sm), VFP_SV(sm))
#define VFP_NARROW(sd, dm)                                                 \
    VFP_DP(1, 1, 1, VFP_SB(sd), 7, VFP_SV(sd), 1, 1, 1, 0, (dm))
#define VFP_VMRS(rt, crn)                                                  \
    (UINT32_C(0xeef00a10) | ((uint32_t)(crn) << 16) |                     \
     ((uint32_t)(rt) << 12))
#define VFP_VMSR(crn, rt)                                                  \
    (UINT32_C(0xeee00a10) | ((uint32_t)(crn) << 16) |                     \
     ((uint32_t)(rt) << 12))
#define VFP_VMOV_S_R(sn, rt)                                               \
    (UINT32_C(0xee000a10) | (VFP_SV(sn) << 16) |                          \
     ((uint32_t)(rt) << 12) | (VFP_SB(sn) << 7))
#define VFP_VMOV_R_S(rt, sn)                                               \
    (UINT32_C(0xee100a10) | (VFP_SV(sn) << 16) |                          \
     ((uint32_t)(rt) << 12) | (VFP_SB(sn) << 7))
#define VFP_LDST(cond, P, U, D, W, L, rn, vd, sz, imm8)                    \
    (((uint32_t)(cond) << 28) | UINT32_C(0x0c000a00) |                    \
     ((uint32_t)(P) << 24) | ((uint32_t)(U) << 23) |                      \
     ((uint32_t)(D) << 22) | ((uint32_t)(W) << 21) |                      \
     ((uint32_t)(L) << 20) | ((uint32_t)(rn) << 16) |                     \
     ((uint32_t)(vd) << 12) | ((uint32_t)(sz) << 8) |                     \
     (uint32_t)(imm8))

/* Four VSTR instructions in a sixteen-instruction loop, matching the 25%
 * memory-operation density of A32_SOC_STORES while covering both S and D
 * forms and the extreme architectural register numbers. This is deliberately
 * synthetic; the restored firmware observer, not this loop, owns instruction-
 * mix claims. */
static const uint32_t A32_SOC_VSTR[] = {
    UINT32_C(0xe2800001),
    VFP_LDST(14, 1, 1, 0, 0, 0, 7,  0, 0, 0), /* VSTR s0,[r7] */
    UINT32_C(0xe2811003),
    VFP_LDST(14, 1, 1, 0, 0, 0, 7,  1, 1, 2), /* VSTR d1,[r7,#8] */
    UINT32_C(0xe0222001),
    UINT32_C(0xe2844001),
    VFP_LDST(14, 1, 1, 1, 0, 0, 7, 15, 0, 4), /* VSTR s31,[r7,#16] */
    UINT32_C(0xe0855003),
    UINT32_C(0xe5976000),
    UINT32_C(0xe2466001),
    VFP_LDST(14, 1, 1, 0, 0, 0, 7, 15, 1, 6), /* VSTR d15,[r7,#24] */
    UINT32_C(0xe0800006),
    UINT32_C(0xe0211005),
    UINT32_C(0xe2422001),
    UINT32_C(0xe2833001),
    UINT32_C(0xeaffffef),
};

/* Four ordinary STM instructions in a sixteen-instruction loop. Their twelve
 * write32 calls cover IA/IB/DA/DB and PC+12 while every run stays in one 1 KiB
 * DWRITE block. This deliberately dense 25% block-store mix isolates the
 * tranche; it is not a restored-firmware mix or phone-FPS claim. */
static const uint32_t A32_SOC_STM[] = {
    UINT32_C(0xe2800001),
    A32_BLOCK(14, 0, 1, 0, 0, 0, 7, UINT32_C(0x0003)), /* IA, 2 words */
    UINT32_C(0xe2811003),
    A32_BLOCK(14, 1, 1, 0, 0, 0, 7, UINT32_C(0x001c)), /* IB, 3 words */
    UINT32_C(0xe0222001),
    UINT32_C(0xe2833001),
    A32_BLOCK(14, 0, 0, 0, 0, 0, 7, UINT32_C(0x0060)), /* DA, 2 words */
    UINT32_C(0xe0244003),
    UINT32_C(0xe2855001),
    A32_BLOCK(14, 1, 0, 0, 0, 0, 7, UINT32_C(0xc700)), /* DB, 5 words */
    UINT32_C(0xe0466005),
    UINT32_C(0xe0800006),
    UINT32_C(0xe0211005),
    UINT32_C(0xe2422001),
    UINT32_C(0xe0833002),
    UINT32_C(0xeaffffef),
};

/* Four no-PC LDM instructions in a sixteen-instruction loop. Twelve read32
 * calls cover IA/IB/DA/DB while all transfers stay in one DREAD block. The
 * fixed base is excluded from every list, making repeated loops deterministic.
 * This dense 25% block-load mix isolates the tranche; it is not firmware mix
 * or phone-FPS evidence. */
static const uint32_t A32_SOC_LDM[] = {
    UINT32_C(0xe2800001),
    A32_BLOCK(14, 0, 1, 0, 0, 1, 7, UINT32_C(0x0003)), /* IA, 2 words */
    UINT32_C(0xe2811003),
    A32_BLOCK(14, 1, 1, 0, 0, 1, 7, UINT32_C(0x001c)), /* IB, 3 words */
    UINT32_C(0xe0222001),
    UINT32_C(0xe2833001),
    A32_BLOCK(14, 0, 0, 0, 0, 1, 7, UINT32_C(0x0060)), /* DA, 2 words */
    UINT32_C(0xe0244003),
    UINT32_C(0xe2855001),
    A32_BLOCK(14, 1, 0, 0, 0, 1, 7, UINT32_C(0x1f00)), /* DB, 5 words */
    UINT32_C(0xe0466005),
    UINT32_C(0xe0800006),
    UINT32_C(0xe0211005),
    UINT32_C(0xe2422001),
    UINT32_C(0xe0833002),
    UINT32_C(0xeaffffef),
};

/* Four architectural VSTM instructions in a sixteen-instruction loop. The
 * sixteen written words cover IA without writeback, IA with writeback, DB
 * with writeback/VPUSH addressing, single and double lists, and the highest
 * VFPv2 double register. Reset instructions keep every transfer in the same
 * proved 1 KiB DWRITE block. This 25% VSTM mix is deliberately synthetic. */
static const uint32_t A32_SOC_VSTM[] = {
    UINT32_C(0xe2800001),
    VFP_LDST(14, 0, 1, 0, 0, 0, 7,  0, 0, 2), /* IA {s0-s1}, 2 words */
    UINT32_C(0xe2811003),
    VFP_LDST(14, 0, 1, 0, 1, 0, 7,  1, 0, 4), /* IA! {s2-s5}, 4 words */
    UINT32_C(0xe2477010),                        /* SUB r7,r7,#16 */
    UINT32_C(0xe0222001),
    VFP_LDST(14, 1, 0, 0, 1, 0, 7,  3, 1, 6), /* DB! {d3-d5}, 6 words */
    UINT32_C(0xe2877018),                        /* ADD r7,r7,#24 */
    UINT32_C(0xe2844001),
    VFP_LDST(14, 0, 1, 0, 0, 0, 7, 14, 1, 4), /* IA {d14-d15}, 4 words */
    UINT32_C(0xe0855003),
    UINT32_C(0xe2466001),
    UINT32_C(0xe0800006),
    UINT32_C(0xe0211005),
    UINT32_C(0xe2422001),
    UINT32_C(0xeaffffef),
};

/* Fifteen VFPv2 arithmetic instructions plus the loop branch. All operands
 * remain signed zero or finite normal across repetitions. This deliberately
 * extreme 93.75% arithmetic mix isolates handler overhead and is neither a
 * restored-firmware instruction mix nor phone-FPS evidence. */
static const uint32_t A32_SOC_VFP_ARITH[] = {
    VFP_ARITH_S(0,0, 0, 8, 9),  /* VMLA  s0,s8,s9 */
    VFP_ARITH_S(0,1, 1, 8, 9),  /* VMLS  s1,s8,s9 */
    VFP_ARITH_S(1,0, 2, 8, 9),  /* VNMLS s2,s8,s9 */
    VFP_ARITH_S(1,1, 3, 8, 9),  /* VNMLA s3,s8,s9 */
    VFP_ARITH_S(2,0, 4, 8, 9),  /* VMUL  s4,s8,s9 */
    VFP_ARITH_S(2,1, 5, 8, 9),  /* VNMUL s5,s8,s9 */
    VFP_ARITH_S(3,0, 6, 8, 9),  /* VADD  s6,s8,s9 */
    VFP_ARITH_S(3,1, 7, 8, 9),  /* VSUB  s7,s8,s9 */
    VFP_ARITH_S(4,0,10,30,31),  /* VDIV  s10,s30,s31 */
    VFP_ARITH_D(0,0, 0, 7, 8),  /* VMLA  d0,d7,d8 */
    VFP_ARITH_D(0,1, 1, 7, 8),  /* VMLS  d1,d7,d8 */
    VFP_ARITH_D(2,0, 2, 7, 8),  /* VMUL  d2,d7,d8 */
    VFP_ARITH_D(3,0, 3, 7, 8),  /* VADD  d3,d7,d8 */
    VFP_ARITH_D(3,1, 4, 7, 8),  /* VSUB  d4,d7,d8 */
    VFP_ARITH_D(4,0, 5,13,14),  /* VDIV  d5,d13,d14 */
    UINT32_C(0xeaffffef),       /* B 0 */
};

/* All sixteen A32 immediate data-processing opcodes. This deliberately uses
 * r8-r14, PC as an input, arithmetic and logical flag writes, both rotated and
 * unrotated immediates, and carry-consuming operations. */
static const uint32_t A32_IMM_ALL_OPS[] = {
    A32_DP_IMM(14,  0, 1,  0,  8, 0, 0xff), /* ANDS r8,r0,#ff       */
    A32_DP_IMM(14,  1, 1,  1,  9, 1, 0x02), /* EORS r9,r1,#80000000 */
    A32_DP_IMM(14,  2, 1,  2, 10, 0, 0x01), /* SUBS r10,r2,#1       */
    A32_DP_IMM(14,  3, 1,  3, 11, 0, 0x02), /* RSBS r11,r3,#2       */
    A32_DP_IMM(14,  4, 0, 15, 12, 0, 0x04), /* ADD r12,pc,#4        */
    A32_DP_IMM(14,  5, 1,  4, 13, 0, 0x03), /* ADCS sp,r4,#3        */
    A32_DP_IMM(14,  6, 1,  5, 14, 0, 0x04), /* SBCS lr,r5,#4        */
    A32_DP_IMM(14,  7, 0,  6,  8, 0, 0x05), /* RSC r8,r6,#5         */
    A32_DP_IMM(14,  8, 1,  8,  0, 0, 0x80), /* TST r8,#80           */
    A32_DP_IMM(14,  9, 1,  9,  0, 0, 0xff), /* TEQ r9,#ff           */
    A32_DP_IMM(14, 10, 1, 10,  0, 0, 0x80), /* CMP r10,#80          */
    A32_DP_IMM(14, 11, 1, 11,  0, 0, 0x7f), /* CMN r11,#7f          */
    A32_DP_IMM(14, 12, 1, 12, 12, 0, 0x10), /* ORRS r12,r12,#10     */
    A32_DP_IMM(14, 13, 1,  0, 13, 0, 0x00), /* MOVS sp,#0           */
    A32_DP_IMM(14, 14, 1, 14, 14, 0, 0xff), /* BICS lr,lr,#ff       */
    A32_DP_IMM(14, 15, 1,  0,  8, 1, 0x02), /* MVNS r8,#80000000    */
};

/* With N=0,Z=0,C=1,V=0, these fourteen guards exercise seven passing and
 * seven failing ARM conditions without changing the flags. */
static const uint32_t A32_IMM_CONDITIONS[] = {
    A32_DP_IMM(14, 10, 1, 0, 0, 0, 0),
    A32_DP_IMM( 0,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM( 1,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM( 2,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM( 3,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM( 4,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM( 5,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM( 6,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM( 7,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM( 8,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM( 9,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM(10,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM(11,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM(12,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM(13,  4, 0, 8, 8, 0, 1),
    A32_DP_IMM(14, 13, 0, 0, 14, 0, 0x5a),
};

/* All sixteen register-form data-processing opcodes. The first four cover
 * the architecturally special immediate-shift encodings LSL #0, LSR #32,
 * ASR #32 and RRX. The rest cover ordinary amounts, high registers, SP and
 * both valid PC input positions. */
static const uint32_t A32_REG_IMM_ALL_OPS[] = {
    A32_DP_REG_IMM(14,  0, 1,  0,  8,  1, 0,  0), /* ANDS r8,r0,r1,LSL #0  */
    A32_DP_REG_IMM(14,  1, 1,  1,  9,  2, 1,  0), /* EORS r9,r1,r2,LSR #32 */
    A32_DP_REG_IMM(14,  2, 1,  2, 10,  3, 2,  0), /* SUBS r10,r2,r3,ASR #32*/
    A32_DP_REG_IMM(14,  3, 1,  3, 11,  4, 3,  0), /* RSBS r11,r3,r4,RRX    */
    A32_DP_REG_IMM(14,  4, 0, 15, 12,  5, 0,  1), /* ADD r12,pc,r5,LSL #1  */
    A32_DP_REG_IMM(14,  5, 1,  4, 13,  6, 1,  1), /* ADCS sp,r4,r6,LSR #1  */
    A32_DP_REG_IMM(14,  6, 1,  5, 14,  7, 2, 31), /* SBCS lr,r5,r7,ASR #31 */
    A32_DP_REG_IMM(14,  7, 0,  6,  8, 15, 3,  7), /* RSC r8,r6,pc,ROR #7   */
    A32_DP_REG_IMM(14,  8, 1,  8,  0,  9, 0, 31), /* TST r8,r9,LSL #31     */
    A32_DP_REG_IMM(14,  9, 1,  9,  0, 10, 1, 31), /* TEQ r9,r10,LSR #31    */
    A32_DP_REG_IMM(14, 10, 1, 10,  0, 11, 2,  1), /* CMP r10,r11,ASR #1    */
    A32_DP_REG_IMM(14, 11, 1, 11,  0, 12, 3, 16), /* CMN r11,r12,ROR #16   */
    A32_DP_REG_IMM(14, 12, 1, 12, 12, 13, 0,  2), /* ORRS r12,r12,sp,LSL #2*/
    A32_DP_REG_IMM(14, 13, 1,  0, 13, 14, 1,  4), /* MOVS sp,lr,LSR #4     */
    A32_DP_REG_IMM(14, 14, 1, 14, 14,  0, 2,  4), /* BICS lr,lr,r0,ASR #4  */
    A32_DP_REG_IMM(14, 15, 1,  0,  8,  1, 3,  8), /* MVNS r8,r1,ROR #8     */
};

/* Register-specified shifts use only Rs[7:0], and ARM's answers at 0, 32 and
 * above 32 deliberately differ from AArch64's modulo-32 variable shifts.
 * r8/r9/r10/r11/r12 and r14[7:0] are seeded to 0/1/31/32/33/255. */
static const uint32_t A32_REG_REG_ALL_OPS[] = {
    A32_DP_REG_REG(14,  0, 0,  1,  0,  2, 0,  8), /* AND r0,r1,r2,LSL r8   */
    A32_DP_REG_REG(14,  1, 0,  2,  1,  3, 1,  9), /* EOR r1,r2,r3,LSR r9   */
    A32_DP_REG_REG(14,  2, 1,  3,  2,  4, 2, 10), /* SUBS r2,r3,r4,ASR r10 */
    A32_DP_REG_REG(14,  3, 1,  4,  3,  5, 3, 11), /* RSBS r3,r4,r5,ROR r11 */
    A32_DP_REG_REG(14,  4, 0,  5,  4,  6, 0, 12), /* ADD r4,r5,r6,LSL r12  */
    A32_DP_REG_REG(14,  5, 1,  6,  5,  7, 1, 14), /* ADCS r5,r6,r7,LSR lr  */
    A32_DP_REG_REG(14,  6, 1,  7,  6, 13, 2,  8), /* SBCS r6,r7,sp,ASR r8  */
    A32_DP_REG_REG(14,  7, 0, 13,  7,  0, 3,  9), /* RSC r7,sp,r0,ROR r9   */
    A32_DP_REG_REG(14,  8, 1,  0,  0,  1, 0, 10), /* TST r0,r1,LSL r10     */
    A32_DP_REG_REG(14,  9, 1,  1,  0,  2, 1, 11), /* TEQ r1,r2,LSR r11     */
    A32_DP_REG_REG(14, 10, 1,  2,  0,  3, 2, 12), /* CMP r2,r3,ASR r12     */
    A32_DP_REG_REG(14, 11, 1,  3,  0,  4, 3, 14), /* CMN r3,r4,ROR lr      */
    A32_DP_REG_REG(14, 12, 1, 13, 13,  5, 0,  8), /* ORRS sp,sp,r5,LSL r8  */
    A32_DP_REG_REG(14, 13, 1,  0,  0,  6, 1,  9), /* MOVS r0,r6,LSR r9     */
    A32_DP_REG_REG(14, 14, 1,  1,  1,  7, 2, 11), /* BICS r1,r1,r7,ASR r11 */
    A32_DP_REG_REG(14, 15, 1,  0,  2,  8, 3, 12), /* MVNS r2,r8,ROR r12    */
};

/* The same N=0,Z=0,C=1,V=0 condition matrix as the immediate case, but each
 * guarded operation occupies two semantic records. This proves all fourteen
 * guard handlers skip exactly two records on failure and none on success. */
static const uint32_t A32_REG_CONDITIONS[] = {
    A32_DP_IMM(14, 10, 1, 0, 0, 0, 0),
    A32_DP_REG_IMM( 0, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM( 1, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM( 2, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM( 3, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM( 4, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM( 5, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM( 6, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM( 7, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM( 8, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM( 9, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM(10, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM(11, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM(12, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM(13, 4, 0, 8, 8, 9, 0, 1),
    A32_DP_REG_IMM(14, 13, 0, 0, 14, 8, 3, 0),
};

/* Product-only cache-hit loads: immediate/register offsets, add/subtract,
 * byte/word, PC base, high destinations and both condition outcomes. Every
 * dynamic address is inside one of two explicitly warmed 1 KiB blocks. */
static const uint32_t A32_READ_HITS[] = {
    A32_SINGLE(14, 0, 1, 0, 1,  0,  8, 0x020), /* LDR r8,[r0,#0x20]       */
    A32_SINGLE(14, 0, 0, 1, 1,  2,  9, 0x001), /* LDRB r9,[r2,#-1]        */
    A32_SINGLE(14, 1, 1, 0, 1,  0, 10, 0x101), /* LDR r10,[r0,r1,LSL #2] */
    A32_SINGLE(14, 1, 0, 0, 1,  2, 11, 0x101), /* LDR r11,[r2,-r1,LSL #2]*/
    A32_SINGLE(14, 0, 1, 0, 1, 15, 12, 0x0e8), /* LDR r12,[pc,#0xe8]      */
    A32_SINGLE(14, 0, 1, 0, 1,  0, 13, 0x024), /* LDR sp,[r0,#0x24]       */
    A32_SINGLE(14, 0, 1, 0, 1,  0, 14, 0x028), /* LDR lr,[r0,#0x28]       */
    A32_SINGLE( 1, 0, 1, 0, 1,  0,  3, 0x02c), /* LDRNE r3,[r0,#0x2c]     */
    A32_SINGLE( 0, 0, 1, 0, 1,  0,  4, 0x030), /* LDREQ r4,... (skipped)   */
};

static const uint32_t A32_READ_PARTIAL[] = {
    A32_DP_IMM(14, 4, 0, 0, 0, 0, 1),
    A32_SINGLE(14, 0, 1, 0, 1, 7, 8, 0),
    A32_DP_IMM(14, 4, 0, 1, 1, 0, 3),
};

static const uint32_t A32_READ_ZERO_PREFIX[] = {
    A32_SINGLE(14, 0, 1, 0, 1, 7, 8, 0),
};

/* Product-only Thumb DREAD loads. The ordering keeps r0/r1/r2 intact until
 * every register-offset form has consumed them, then deliberately permits
 * load destinations to replace those base values. */
static const uint16_t THUMB_READ_HITS[] = {
    0x4f40u, /* LDR   r7,[pc,#0x100] */
    0x5843u, /* LDR   r3,[r0,r1]     */
    0x5c44u, /* LDRB  r4,[r0,r1]     */
    0x5645u, /* LDRSB r5,[r0,r1]     */
    0x5a46u, /* LDRH  r6,[r0,r1]     */
    0x5e47u, /* LDRSH r7,[r0,r1]     */
    0x6850u, /* LDR   r0,[r2,#4]     */
    0x7891u, /* LDRB  r1,[r2,#2]     */
    0x88d2u, /* LDRH  r2,[r2,#6]     */
    0x9b02u, /* LDR   r3,[sp,#8]     */
};

static const uint16_t THUMB_READ_ZERO_PREFIX[] = {
    0x6800u, /* LDR r0,[r0,#0] */
};

/* Exact raw VFPv2 state operations. These span pinned and memory-backed ARM
 * registers, S/D aliasing, both two-core transfer forms, FPSCR flag transfer
 * and every non-arithmetic unary bit operation. */
static const uint32_t VFP_REGISTER_OPS[] = {
    VFP_VMOV_S_R(5, 0),                  /* VMOV s5,r0 */
    VFP_VMOV_R_S(1, 5),                  /* VMOV r1,s5 */
    UINT32_C(0xee272b10),                /* VMOV d7[63:32],r2 */
    UINT32_C(0xee373b10),                /* VMOV r3,d7[63:32] */
    UINT32_C(0xec454b10),                /* VMOV d0,r4,r5 */
    UINT32_C(0xec576b10),                /* VMOV r6,r7,d0 */
    UINT32_C(0xec498a11),                /* VMOV s2,s3,r8,r9 */
    UINT32_C(0xec5baa11),                /* VMOV r10,r11,s2,s3 */
    VFP_VMSR(1, 12),                     /* VMSR FPSCR,r12 */
    VFP_VMRS(14, 1),                     /* VMRS lr,FPSCR */
    VFP_VMRS(15, 1),                     /* VMRS APSR_nzcv,FPSCR */
    VFP_UN_S(0, 0, 13, 12),              /* VMOV.F32 s13,s12 */
    VFP_UN_S(0, 1, 14, 12),              /* VABS.F32 s14,s12 */
    VFP_UN_S(1, 0, 15, 12),              /* VNEG.F32 s15,s12 */
    VFP_UN_D(0, 1, 5, 4),                /* VABS.F64 d5,d4 */
    VFP_UN_D(1, 0, 6, 4),                /* VNEG.F64 d6,d4 */
};

static const uint32_t VFP_COMPARE_OPS[] = {
    VFP_UN_S(4, 0,  0,  2),              /* VCMP.F32 s0,s2 */
    VFP_UN_S(4, 1,  3,  4),              /* VCMPE.F32 s3,s4 */
    VFP_UN_S(5, 0,  5,  0),              /* VCMP.F32 s5,#0 */
    VFP_UN_S(5, 1, 31,  0),              /* VCMPE.F32 s31,#0 */
    VFP_UN_D(4, 0,  0,  1),              /* VCMP.F64 d0,d1 */
    VFP_UN_D(4, 1,  2,  3),              /* VCMPE.F64 d2,d3 */
    VFP_UN_D(5, 0,  4,  0),              /* VCMP.F64 d4,#0 */
    VFP_UN_D(5, 1, 15,  0),              /* VCMPE.F64 d15,#0 */
};

static const uint32_t VFP_WIDEN_OPS[] = {
    VFP_WIDEN( 0,  0),                    /* VCVT.F64.F32 d0,s0 */
    VFP_WIDEN( 1,  1),                    /* VCVT.F64.F32 d1,s1 */
    VFP_WIDEN( 2,  2),                    /* VCVT.F64.F32 d2,s2 */
    VFP_WIDEN( 3,  7),                    /* VCVT.F64.F32 d3,s7 */
    VFP_WIDEN( 4, 15),                    /* VCVT.F64.F32 d4,s15 */
    VFP_WIDEN( 8, 16),                    /* VCVT.F64.F32 d8,s16 */
    VFP_WIDEN(14, 30),                    /* VCVT.F64.F32 d14,s30 */
    VFP_WIDEN(15, 31),                    /* VCVT.F64.F32 d15,s31 */
};

static const uint32_t VFP_NARROW_OPS[] = {
    VFP_NARROW( 0,  0),                   /* VCVT.F32.F64 s0,d0  */
    VFP_NARROW( 1,  0),                   /* destination aliases d0 high */
    VFP_NARROW( 2,  1),
    VFP_NARROW( 3,  7),
    VFP_NARROW(15,  7),                   /* witnessed hot encoding */
    VFP_NARROW(16,  8),
    VFP_NARROW(30, 14),
    VFP_NARROW(31, 15),
};

/* All nine VFPv2 scalar arithmetic operations in both architectural widths.
 * Register choices cover low/high S words and D-register word aliasing; the
 * native semantic oracle below owns value and fallback correctness. */
static const uint32_t VFP_ARITH32_OPS[] = {
    VFP_ARITH_S(0, 0,  0,  1,  2), /* VMLA  */
    VFP_ARITH_S(0, 1, 31, 30, 29), /* VMLS  */
    VFP_ARITH_S(1, 0,  3,  5,  7), /* VNMLS */
    VFP_ARITH_S(1, 1, 28, 26, 24), /* VNMLA */
    VFP_ARITH_S(2, 0,  8,  9, 10), /* VMUL  */
    VFP_ARITH_S(2, 1, 23, 21, 19), /* VNMUL */
    VFP_ARITH_S(3, 0, 12, 13, 14), /* VADD  */
    VFP_ARITH_S(3, 1, 18, 17, 16), /* VSUB  */
    VFP_ARITH_S(4, 0, 22, 20, 15), /* VDIV  */
};

static const uint32_t VFP_ARITH64_OPS[] = {
    VFP_ARITH_D(0, 0,  0,  1,  2), /* VMLA  */
    VFP_ARITH_D(0, 1, 15, 14, 13), /* VMLS  */
    VFP_ARITH_D(1, 0,  3,  5,  7), /* VNMLS */
    VFP_ARITH_D(1, 1, 12, 11, 10), /* VNMLA */
    VFP_ARITH_D(2, 0,  4,  6,  8), /* VMUL  */
    VFP_ARITH_D(2, 1, 13,  9,  5), /* VNMUL */
    VFP_ARITH_D(3, 0,  2,  3,  4), /* VADD  */
    VFP_ARITH_D(3, 1, 10,  8,  6), /* VSUB  */
    VFP_ARITH_D(4, 0, 11,  7,  1), /* VDIV  */
};

static const uint32_t VFP_READ_HITS[] = {
    VFP_LDST(14, 1, 1, 0, 0, 1,  0, 0, 0, 0), /* VLDR s0,[r0] */
    VFP_LDST(14, 1, 1, 1, 0, 1,  0,15, 0, 1), /* VLDR s31,[r0,#4] */
    VFP_LDST(14, 1, 0, 0, 0, 1,  1, 1, 0, 1), /* VLDR s2,[r1,#-4] */
    VFP_LDST(14, 1, 1, 0, 0, 1,  0, 2, 1, 2), /* VLDR d2,[r0,#8] */
    VFP_LDST(14, 1, 0, 0, 0, 1,  1, 3, 1, 2), /* VLDR d3,[r1,#-8] */
    VFP_LDST(14, 1, 1, 0, 0, 1, 15, 2, 0,32), /* VLDR s4,[pc,#128] */
    VFP_LDST(14, 1, 1, 0, 0, 1, 15, 4, 1,33), /* VLDR d4,[pc,#132] */
    VFP_LDST( 0, 1, 1, 0, 0, 1,  0, 5, 0,63), /* LDREQ skipped */
    VFP_VMOV_R_S(10, 0),                       /* VMOV r10,s0 */
};

static const uint32_t VFP_WRITE_HITS[] = {
    VFP_LDST(14, 1, 1, 0, 0, 0,  0, 0, 0, 0), /* VSTR s0,[r0] */
    VFP_LDST(14, 1, 1, 1, 0, 0,  0,15, 0, 1), /* VSTR s31,[r0,#4] */
    VFP_LDST(14, 1, 0, 0, 0, 0,  1, 1, 0, 1), /* VSTR s2,[r1,#-4] */
    VFP_LDST(14, 1, 1, 0, 0, 0,  0, 2, 1, 2), /* VSTR d2,[r0,#8] */
    VFP_LDST(14, 1, 0, 0, 0, 0,  1,15, 1, 2), /* VSTR d15,[r1,#-8] */
    VFP_LDST(14, 1, 1, 0, 0, 0, 15, 2, 0,32), /* VSTR s4,[pc,#128] */
    VFP_LDST( 0, 1, 1, 0, 0, 0,  0, 5, 0,63), /* VSTREQ skipped */
};

static const uint32_t VSTM_WRITE_HITS[] = {
    VFP_LDST(14, 0, 1, 0, 0, 0,  0, 0, 0, 1), /* VSTMIA r0,{s0} */
    VFP_LDST(14, 0, 1, 1, 1, 0,  1,15, 0, 1), /* VSTMIA r1!,{s31} */
    VFP_LDST(14, 1, 0, 0, 1, 0, 13, 0, 1,32), /* VSTMDB sp!,{d0-d15} */
    VFP_LDST(14, 0, 1, 0, 0, 0,  2,14, 1, 4), /* VSTMIA r2,{d14-d15} */
    VFP_LDST( 0, 0, 1, 0, 1, 0,  3, 0, 0, 2), /* VSTMEQIA r3!,{s0-s1} */
};

typedef struct {
    const char *name;
    const void *program;
    unsigned insns;
    bool thumb;
    uint32_t pc;
    uint32_t exit_pc;
    unsigned uops;
} static_case_t;

static const static_case_t STATIC_CASES[] = {
    { "a32-fallthrough", A32_SHORT_FALL, 3u, false,
      0x1000u, 0x100cu, 4u },
    { "a32-branch-target", A32_SHORT_BRANCH, 2u, false,
      0x1000u, 0x1040u, 2u },
    { "thumb-fallthrough", THUMB_SHORT_FALL, 3u, true,
      0x0200u, 0x0206u, 4u },
    { "thumb-branch-target", THUMB_SHORT_BRANCH, 2u, true,
      0x0200u, 0x0220u, 2u },
    { "a32-imm-all-ops", A32_IMM_ALL_OPS, 16u, false,
      0x4000u, 0x4040u, 17u },
    { "a32-imm-conditions", A32_IMM_CONDITIONS, 16u, false,
      0x5000u, 0x5040u, 31u },
    { "a32-reg-imm-all-ops", A32_REG_IMM_ALL_OPS, 16u, false,
      0x6000u, 0x6040u, 33u },
    { "a32-reg-reg-all-ops", A32_REG_REG_ALL_OPS, 16u, false,
      0x7000u, 0x7040u, 33u },
    { "a32-reg-conditions", A32_REG_CONDITIONS, 16u, false,
      0xa000u, 0xa040u, 46u },
    { "thumb-register-alu-all", THUMB_REGISTER_ALU_ALL, 16u, true,
      0xe000u, 0xe020u, 30u },
    { "thumb-integer-misc", THUMB_INTEGER_MISC, 16u, true,
      0xe100u, 0xe120u, 24u },
};

static const bench_case_t CASES[] = {
    { "a32-alu", A32_ALU, 16u, false, false },
    { "a32-mixed", A32_MIXED, 16u, false, true },
    { "thumb-alu", THUMB_ALU, 16u, true, false },
    { "thumb-mixed", THUMB_MIXED, 16u, true, true },
};

static const unsigned PRODUCT_ENTRY_LENGTHS[] = {1u, 2u, 3u, 4u, 8u, 16u};
static const unsigned SOC_ENTRY_LENGTHS[] = {
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
    9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u,
};

static double now_seconds(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) || frequency.QuadPart == 0)
        return -1.0;
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static uint64_t hash_ram(void) {
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t i;
    for (i = 0; i < sizeof g_ram; i++)
        hash = (hash ^ g_ram[i]) * UINT64_C(1099511628211);
    return hash;
}

static void seed_cpu_at(arm_cpu_t *cpu, const void *program, unsigned insns,
                        bool thumb, uint32_t pc) {
    unsigned i;
    memset(g_ram, 0, sizeof g_ram);
    if (thumb) {
        const uint16_t *code = (const uint16_t *)program;
        for (i = 0; i < insns; i++) mem_w16(NULL, pc + i * 2u, code[i]);
    } else {
        const uint32_t *code = (const uint32_t *)program;
        for (i = 0; i < insns; i++) mem_w32(NULL, pc + i * 4u, code[i]);
    }

    arm_reset(cpu, &g_bus);
    cpu->cpsr = (cpu->cpsr & ~(ARM_CPSR_MODE_MASK | ARM_CPSR_T)) |
                ARM_MODE_SYS | (thumb ? ARM_CPSR_T : 0u) | ARM_CPSR_C;
    for (i = 0; i < 13u; i++) cpu->r[i] = 0x10203040u + i * 0x01010101u;
    cpu->r[7] = DATA_BASE;
    cpu->r[13] = STACK_BASE;
    cpu->r[14] = 0xdead00ffu;
    cpu->r[8] = 0u;
    cpu->r[9] = 1u;
    cpu->r[10] = 31u;
    cpu->r[11] = 32u;
    cpu->r[12] = 33u;
    cpu->r[15] = pc;
}

static void seed_cpu(arm_cpu_t *cpu, const bench_case_t *bc) {
    seed_cpu_at(cpu, bc->program, bc->insns, bc->thumb, 0u);
}

static void capture_state(final_state_t *out, const arm_cpu_t *cpu,
                          arm_status_t status, int jit_exit) {
    memcpy(out->r, cpu->r, sizeof out->r);
    out->cpsr = cpu->cpsr;
    out->cycles = cpu->cycles;
    out->ram_hash = hash_ram();
    out->status = status;
    out->jit_exit = jit_exit;
}

static bool architectural_states_equal(const final_state_t *a,
                                       const final_state_t *b) {
    return memcmp(a->r, b->r, sizeof a->r) == 0 &&
           a->cpsr == b->cpsr && a->cycles == b->cycles &&
           a->ram_hash == b->ram_hash && a->status == b->status;
}

static bool states_equal(const final_state_t *a, const final_state_t *b) {
    return architectural_states_equal(a, b) &&
           a->status == ARM_OK && b->jit_exit == JIT_EXIT_NEXT;
}

static bool prepare_product_entry(unsigned length,
                                  uint32_t program[A64_STATIC_MAX_INSNS],
                                  a64_static_block_t *block) {
    uint8_t bytes[A64_STATIC_MAX_INSNS * sizeof(uint32_t)];

    if (!length || length > A64_STATIC_MAX_INSNS || !program || !block)
        return false;
    for (unsigned i = 0u; i + 1u < length; i++) program[i] = A32_ALU[i];
    program[length - 1u] = UINT32_C(0xea000000) |
        ((uint32_t)(-(int32_t)(length + 1u)) & UINT32_C(0x00ffffff));
    for (unsigned i = 0u; i < length; i++) {
        uint32_t value = program[i];
        bytes[i * 4u + 0u] = (uint8_t)value;
        bytes[i * 4u + 1u] = (uint8_t)(value >> 8);
        bytes[i * 4u + 2u] = (uint8_t)(value >> 16);
        bytes[i * 4u + 3u] = (uint8_t)(value >> 24);
    }
    memset(block, 0, sizeof *block);
    return a64_static_decode_read_hits_bytes_at(bytes, length, false, 0u,
                                                block) &&
           block->insn_count == length && block->start_pc == 0u &&
           block->exit_pc == 0u && !block->touches_memory &&
           !block->direct_reads && !block->runtime_guards && !block->vfp;
}

static bool validate_static_shapes(void) {
    static const uint32_t MID_BLOCK_BRANCH[] = {
        0xea000000u, 0xe2800001u,
    };
    static const uint32_t MID_BLOCK_CONDITIONAL_BRANCH[] = {
        0x1a000000u, 0xe2800001u,
    };
    static const uint32_t MID_BLOCK_LINK[] = {
        0xeb000000u, 0xe2800001u,
    };
    static const uint32_t MID_BLOCK_INDIRECT[] = {
        0xe12fff10u, 0xe2800001u,
    };
    static const uint16_t MID_BLOCK_THUMB_INDIRECT[] = {
        UINT16_C(0x4700), UINT16_C(0x3001),
    };
    static const uint32_t BLX_IMMEDIATE[] = { 0xfa000000u };
    static const uint32_t BLX_PC[] = { UINT32_C(0xe12fff3f) };
    static const uint16_t THUMB_BLX_PC[] = { UINT16_C(0x47f8) };
    static const uint32_t PC_WRITE[] = { 0xe3a0f000u };
    static const uint32_t MULTIPLY[] = { 0xe0000090u };
    static const uint32_t CONDITIONAL_DP[] = {
        A32_DP_IMM(1, 4, 0, 8, 8, 0, 1),
    };
    static const uint32_t REG_SHIFT_PC[] = {
        A32_DP_REG_REG(14, 4, 0, 0, 15, 1, 0, 2),
        A32_DP_REG_REG(14, 4, 0, 15, 0, 1, 0, 2),
        A32_DP_REG_REG(14, 4, 0, 0, 0, 15, 0, 2),
        A32_DP_REG_REG(14, 4, 0, 0, 0, 1, 0, 15),
    };
    static const uint32_t INVALID_READ_HITS[] = {
        A32_SINGLE(14, 0, 1, 0, 0, 0, 1, 0), /* store */
        UINT32_C(0xe4901000),                 /* post-index load */
        UINT32_C(0xe5b01000),                 /* pre-index writeback */
        A32_SINGLE(14, 0, 1, 0, 1, 0, 15, 0),/* load to PC */
        A32_SINGLE(14, 1, 1, 0, 1, 0, 1, 15),/* register Rm=PC */
        A32_SINGLE(14, 1, 1, 0, 1, 0, 1, 0x11),/* reserved bit 4 */
    };
    static const uint32_t VALID_A32_STORES[] = {
        A32_SINGLE_MODE2(14, 0, 1, 1, 0, 0, 0, 4,  3, 0x00c),
        A32_SINGLE_MODE2( 1, 0, 1, 0, 1, 1, 0, 4,  3, 0x001),
        A32_SINGLE_MODE2(14, 1, 0, 1, 0, 0, 0, 4,  3, 0x102),
        A32_SINGLE_MODE2(14, 0, 0, 1, 0, 1, 0, 4,  3, 0x004),
        A32_SINGLE_MODE2(14, 0, 1, 1, 0, 0, 0, 4, 15, 0x010),
        A32_SINGLE_MODE2( 0, 0, 1, 1, 0, 0, 0, 4,  3, 0x014),
    };
    static const unsigned VALID_A32_STORE_UOPS[] = {3u, 4u, 4u, 3u, 3u, 4u};
    static const uint32_t VALID_A32_STM[] = {
        A32_BLOCK(14, 0, 1, 0, 0, 0,  4, UINT32_C(0x8109)), /* IA */
        A32_BLOCK(14, 1, 1, 0, 1, 0, 12, UINT32_C(0x4106)), /* IB! */
        A32_BLOCK(14, 0, 0, 0, 0, 0, 13, UINT32_C(0x4210)), /* DA */
        A32_BLOCK(14, 1, 0, 0, 1, 0, 11, UINT32_C(0x8481)), /* DB! */
        A32_BLOCK( 0, 0, 1, 0, 0, 0,  4, UINT32_C(0x4004)), /* EQ */
        A32_BLOCK(14, 0, 1, 0, 0, 0,  7, UINT32_C(0xffff)), /* all */
    };
    static const unsigned VALID_A32_STM_WORDS[] = {4u, 4u, 3u, 4u, 2u, 16u};
    static const uint32_t VALID_A32_LDM[] = {
        A32_BLOCK(14, 0, 1, 0, 0, 1,  4, UINT32_C(0x4109)), /* IA */
        A32_BLOCK(14, 1, 1, 0, 1, 1, 12, UINT32_C(0x4106)), /* IB! */
        A32_BLOCK(14, 0, 0, 0, 0, 1, 13, UINT32_C(0x4210)), /* DA */
        A32_BLOCK(14, 1, 0, 0, 1, 1, 11, UINT32_C(0x4481)), /* DB! */
        A32_BLOCK( 0, 0, 1, 0, 0, 1,  4, UINT32_C(0x4004)), /* EQ */
        A32_BLOCK(14, 0, 1, 0, 0, 1,  7, UINT32_C(0x7fff)), /* r0-r14 */
    };
    static const unsigned VALID_A32_LDM_WORDS[] = {4u, 4u, 3u, 4u, 2u, 15u};
    static const uint32_t INVALID_A32_STORES[] = {
        /* Every invalid writeback alias and operand is rejected before a
         * direct record can exist, matching exec_single_transfer(). */
        A32_SINGLE_MODE2(14, 0, 1, 1, 0, 1, 0, 4, 4, 0x004),
        A32_SINGLE_MODE2(14, 0, 0, 1, 0, 0, 0, 4, 4, 0x004),
        A32_SINGLE_MODE2(14, 0, 1, 1, 0, 1, 0, 15, 3, 0x004),
        A32_SINGLE_MODE2(14, 0, 1, 1, 1, 0, 0, 4, 15, 0x004),
        A32_SINGLE_MODE2(14, 1, 1, 1, 0, 0, 0, 4, 3, 0x00f),
        A32_SINGLE_MODE2(14, 1, 1, 1, 0, 0, 0, 4, 3, 0x012),
    };
    static const uint32_t INVALID_A32_STM[] = {
        A32_BLOCK(14, 0, 1, 1, 0, 0, 4, UINT32_C(0x0003)), /* user bank */
        A32_BLOCK(14, 0, 1, 0, 0, 0,15, UINT32_C(0x0003)), /* PC base */
        A32_BLOCK(14, 0, 1, 0, 0, 0, 4, UINT32_C(0x0000)), /* empty */
        A32_BLOCK(14, 0, 1, 0, 1, 0, 4, UINT32_C(0x0010)), /* wb alias */
        A32_BLOCK(15, 0, 1, 0, 0, 0, 4, UINT32_C(0x0003)), /* cond=1111 */
    };
    static const uint32_t INVALID_A32_LDM[] = {
        A32_BLOCK(14, 0, 1, 1, 0, 1, 4, UINT32_C(0x0003)), /* user bank */
        A32_BLOCK(14, 0, 1, 0, 0, 1,15, UINT32_C(0x0003)), /* PC base */
        A32_BLOCK(14, 0, 1, 0, 0, 1, 4, UINT32_C(0x0000)), /* empty */
        A32_BLOCK(14, 0, 1, 0, 1, 1, 4, UINT32_C(0x0010)), /* wb alias */
        A32_BLOCK(14, 0, 1, 0, 0, 1, 4, UINT32_C(0x8003)), /* PC list */
        A32_BLOCK(15, 0, 1, 0, 0, 1, 4, UINT32_C(0x0003)), /* cond=1111 */
    };
    static const uint16_t VALID_THUMB_STORES[] = {
        UINT16_C(0x508b), /* STR  r3,[r1,r2] */
        UINT16_C(0x528b), /* STRH r3,[r1,r2] */
        UINT16_C(0x548b), /* STRB r3,[r1,r2] */
        UINT16_C(0x604b), /* STR  r3,[r1,#4] */
        UINT16_C(0x704b), /* STRB r3,[r1,#1] */
        UINT16_C(0x804b), /* STRH r3,[r1,#2] */
        UINT16_C(0x9301), /* STR  r3,[sp,#4] */
    };
    static const unsigned VALID_THUMB_STORE_UOPS[] = {
        4u, 4u, 4u, 3u, 3u, 3u, 3u
    };
    static const uint16_t INVALID_PRODUCT_THUMB[] = {
        0x4487u, /* ADD pc,r0 */
        0x4687u, /* MOV pc,r0 */
        0x47f8u, /* BLX pc */
        0x6000u, /* STR r0,[r0] */
        0x8000u, /* STRH r0,[r0] */
        0x9000u, /* STR r0,[sp] */
        0xb401u, /* PUSH {r0} */
        0xde00u, /* UDF */
        0xdf00u, /* SVC */
        0xf000u, /* BL first half */
    };
    static const uint32_t INVALID_PRODUCT_VFP[] = {
        VFP_UN_S(1, 1, 0, 1), /* VSQRT.F32 */
        UINT32_C(0xeeb00b00), /* VMOV.F64 immediate: VFPv3 */
        UINT32_C(0xee000a11), /* VMOV core transfer reserved bit */
        VFP_VMRS(15, 0),      /* PC is valid only with FPSCR */
        VFP_VMSR(0, 0),       /* FPSID is read-only */
        VFP_VMRS(0, 7),       /* unimplemented MVFR0 */
        UINT32_C(0xec4f0b10), /* PC in a two-core transfer */
        VFP_LDST(14, 1, 1, 0, 0, 0, 0, 0, 0, 0), /* VSTR */
        VFP_LDST(14, 0, 1, 0, 0, 1, 0, 0, 0, 1), /* VLDM */
        VFP_LDST(14, 1, 1, 0, 1, 1, 0, 0, 0, 1), /* writeback */
        VFP_LDST(14, 1, 1, 1, 0, 1, 0, 0, 1, 0), /* d16 absent */
        VFP_UN_S(5, 0, 0, 1), /* VCMP #0 with non-zero Vm field */
        VFP_UN_D(5, 0, 0, 1), /* double VCMP #0 with non-zero Vm */
        VFP_DP(1, 1, 1, 1, 4, 0, 1, 0, 1, 0, 0), /* compare d16 */
        VFP_DP(1, 1, 1, 0, 7, 0, 1, 1, 1, 1, 0), /* narrow from d16 */
        VFP_DP(1, 1, 1, 1, 7, 0, 0, 1, 1, 0, 0), /* widen to d16 */
        VFP_DP(0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0), /* arithmetic d16 D */
        VFP_DP(0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0), /* arithmetic d16 N */
        VFP_DP(0, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0), /* arithmetic d16 M */
        VFP_ARITH_S(4, 1, 0, 1, 2), /* undefined VDIV alternate */
        VFP_ARITH_S(5, 0, 0, 1, 2), /* VFPv4 fused family */
        VFP_ARITH_S(6, 0, 0, 1, 2), /* VFPv4 fused family */
    };
    static const uint32_t INVALID_PRODUCT_VFP_STORES[] = {
        VFP_LDST(14, 1, 1, 1, 0, 0, 0, 0, 1, 0), /* d16 absent */
        VFP_LDST(14, 0, 1, 0, 0, 1, 0, 0, 0, 1), /* VLDM, not VSTM */
        VFP_LDST(14, 0, 0, 0, 1, 0, 0, 0, 0, 1), /* invalid DA form */
        VFP_LDST(14, 1, 1, 0, 1, 0, 0, 0, 0, 1), /* invalid pre/up/wb */
        VFP_LDST(14, 0, 1, 0, 0, 0,15, 0, 0, 1), /* VSTM PC base */
        VFP_LDST(14, 0, 1, 0, 0, 0, 0, 0, 0, 0), /* empty VSTM */
        VFP_LDST(14, 0, 1, 1, 0, 0, 0, 0, 1, 2), /* VSTM d16 */
        VFP_LDST(14, 0, 1, 0, 0, 0, 0, 0, 1, 3), /* deprecated FSTMX */
        VFP_LDST(14, 0, 1, 1, 0, 0, 0,15, 0, 2), /* S list overflow */
        VFP_LDST(14, 0, 1, 0, 0, 0, 0,15, 1, 4), /* D list overflow */
    };
    /* Deliberately unaligned guest byte stream: ADD r0,r0,#1. */
    static const uint8_t UNALIGNED_A32[] = {
        0xffu, 0x01u, 0x00u, 0x80u, 0xe2u,
    };
    a64_static_block_t block;
    uint8_t read_bytes[sizeof A32_READ_HITS];
    uint8_t thumb_read_bytes[sizeof THUMB_READ_HITS];
    uint8_t vfp_bytes[sizeof VFP_REGISTER_OPS];
    uint8_t vfp_compare_bytes[sizeof VFP_COMPARE_OPS];
    uint8_t vfp_widen_bytes[sizeof VFP_WIDEN_OPS];
    uint8_t vfp_narrow_bytes[sizeof VFP_NARROW_OPS];
    uint8_t vfp_read_bytes[sizeof VFP_READ_HITS];
    uint32_t branch_handlers[29];
    uint32_t indirect_handlers[62];
    uint32_t vfp_write_handlers[2] = {0u, 0u};
    uint32_t vfp_arith_handlers[2][9] = {{0u}};
    uint32_t vstm_write_handlers[3][15] = {{0u}};
    unsigned branch_handler_count = 0u;
    unsigned indirect_handler_count = 0u;
    unsigned i;

    for (i = 0u; i < sizeof A32_READ_HITS / sizeof A32_READ_HITS[0]; i++) {
        uint32_t value = A32_READ_HITS[i];
        read_bytes[i * 4u + 0u] = (uint8_t)value;
        read_bytes[i * 4u + 1u] = (uint8_t)(value >> 8);
        read_bytes[i * 4u + 2u] = (uint8_t)(value >> 16);
        read_bytes[i * 4u + 3u] = (uint8_t)(value >> 24);
    }
    for (i = 0u; i < sizeof THUMB_READ_HITS / sizeof THUMB_READ_HITS[0]; i++) {
        uint16_t value = THUMB_READ_HITS[i];
        thumb_read_bytes[i * 2u + 0u] = (uint8_t)value;
        thumb_read_bytes[i * 2u + 1u] = (uint8_t)(value >> 8);
    }
    for (i = 0u; i < sizeof VFP_REGISTER_OPS / sizeof VFP_REGISTER_OPS[0];
         i++) {
        uint32_t value = VFP_REGISTER_OPS[i];
        vfp_bytes[i * 4u + 0u] = (uint8_t)value;
        vfp_bytes[i * 4u + 1u] = (uint8_t)(value >> 8);
        vfp_bytes[i * 4u + 2u] = (uint8_t)(value >> 16);
        vfp_bytes[i * 4u + 3u] = (uint8_t)(value >> 24);
    }
    for (i = 0u; i < sizeof VFP_READ_HITS / sizeof VFP_READ_HITS[0]; i++) {
        uint32_t value = VFP_READ_HITS[i];
        vfp_read_bytes[i * 4u + 0u] = (uint8_t)value;
        vfp_read_bytes[i * 4u + 1u] = (uint8_t)(value >> 8);
        vfp_read_bytes[i * 4u + 2u] = (uint8_t)(value >> 16);
        vfp_read_bytes[i * 4u + 3u] = (uint8_t)(value >> 24);
    }
    for (i = 0u; i < sizeof VFP_COMPARE_OPS / sizeof VFP_COMPARE_OPS[0];
         i++) {
        uint32_t value = VFP_COMPARE_OPS[i];
        vfp_compare_bytes[i * 4u + 0u] = (uint8_t)value;
        vfp_compare_bytes[i * 4u + 1u] = (uint8_t)(value >> 8);
        vfp_compare_bytes[i * 4u + 2u] = (uint8_t)(value >> 16);
        vfp_compare_bytes[i * 4u + 3u] = (uint8_t)(value >> 24);
    }
    for (i = 0u; i < sizeof VFP_WIDEN_OPS / sizeof VFP_WIDEN_OPS[0]; i++) {
        uint32_t value = VFP_WIDEN_OPS[i];
        vfp_widen_bytes[i * 4u + 0u] = (uint8_t)value;
        vfp_widen_bytes[i * 4u + 1u] = (uint8_t)(value >> 8);
        vfp_widen_bytes[i * 4u + 2u] = (uint8_t)(value >> 16);
        vfp_widen_bytes[i * 4u + 3u] = (uint8_t)(value >> 24);
    }
    for (i = 0u; i < sizeof VFP_NARROW_OPS / sizeof VFP_NARROW_OPS[0]; i++) {
        uint32_t value = VFP_NARROW_OPS[i];
        vfp_narrow_bytes[i * 4u + 0u] = (uint8_t)value;
        vfp_narrow_bytes[i * 4u + 1u] = (uint8_t)(value >> 8);
        vfp_narrow_bytes[i * 4u + 2u] = (uint8_t)(value >> 16);
        vfp_narrow_bytes[i * 4u + 3u] = (uint8_t)(value >> 24);
    }

    for (i = 0u; i < sizeof STATIC_CASES / sizeof STATIC_CASES[0]; i++) {
        const static_case_t *sc = &STATIC_CASES[i];
        memset(&block, 0, sizeof block);
        if (!a64_static_decode_at(sc->program, sc->insns, sc->thumb,
                                  sc->pc, &block) ||
            block.insn_count != sc->insns || block.uop_count != sc->uops ||
            block.start_pc != sc->pc || block.exit_pc != sc->exit_pc ||
            block.thumb != sc->thumb || block.dynamic_exit ||
            block.indirect_exit ||
            block.touches_memory ||
            block.uops[block.uop_count - 1u].handler != 0u ||
            block.uops[block.uop_count - 1u].immediate != sc->exit_pc) {
            fprintf(stderr, "jitbench: static shape failed for %s\n", sc->name);
            return false;
        }
        printf("STATIC-BLOCK-SHAPE case=%s pc=%08" PRIx32
               " exit=%08" PRIx32 " insns=%u uops=%u\n",
               sc->name, block.start_pc, block.exit_pc,
               block.insn_count, block.uop_count);
    }

    for (i = 0u; i < sizeof REG_SHIFT_PC / sizeof REG_SHIFT_PC[0]; i++) {
        if (a64_static_decode_at(&REG_SHIFT_PC[i], 1u, false, 0u, &block)) {
            fprintf(stderr,
                    "jitbench: static decoder accepted register-shift PC "
                    "case %u\n", i);
            return false;
        }
    }

    for (unsigned link = 0u; link < 2u; link++) {
        for (unsigned condition = 0u; condition < 15u; condition++) {
            const uint32_t pc = UINT32_C(0x1100);
            const uint32_t imm24 = ((condition + link) & 1u) != 0u
                ? UINT32_C(0x000002)
                : UINT32_C(0xfffffc);
            const uint32_t insn = (condition << 28) |
                UINT32_C(0x0a000000) | (link << 24) | imm24;
            const int32_t displacement = (int32_t)(insn << 8) >> 6;
            const uint32_t target = pc + 8u + (uint32_t)displacement;
            const bool dynamic = link != 0u || condition != 14u;
            const unsigned expected_uops = dynamic ? 2u : 1u;
            uint8_t bytes[4] = {
                (uint8_t)insn, (uint8_t)(insn >> 8),
                (uint8_t)(insn >> 16), (uint8_t)(insn >> 24)
            };
            a64_static_block_t product_block;

            if (!a64_static_decode_at(&insn, 1u, false, pc, &block) ||
                !a64_static_decode_read_hits_bytes_at(
                    bytes, 1u, false, pc, &product_block) ||
                memcmp(&block, &product_block, sizeof block) != 0 ||
                block.insn_count != 1u ||
                block.uop_count != expected_uops ||
                block.start_pc != pc || block.thumb ||
                block.dynamic_exit != dynamic || block.indirect_exit ||
                block.touches_memory ||
                block.direct_reads || block.runtime_guards || block.vfp ||
                block.uops[block.uop_count - 1u].handler != 0u ||
                block.exit_pc != (dynamic ? pc + 4u : target) ||
                block.uops[block.uop_count - 1u].immediate !=
                    block.exit_pc) {
                fprintf(stderr,
                        "jitbench: A32 branch shape failed link=%u cond=%u\n",
                        link, condition);
                return false;
            }
            if (dynamic) {
                const a64_static_uop_t *branch =
                    &block.uops[block.uop_count - 2u];
                if (branch->handler == 0u || branch->immediate != target ||
                    branch->pc_value != pc + 4u || branch->metadata != 0u) {
                    fprintf(stderr,
                            "jitbench: A32 dynamic exit record failed "
                            "link=%u cond=%u\n", link, condition);
                    return false;
                }
                for (i = 0u; i < branch_handler_count; i++) {
                    if (branch_handlers[i] == branch->handler) {
                        fprintf(stderr,
                                "jitbench: duplicate A32 branch handler "
                                "link=%u cond=%u\n", link, condition);
                        return false;
                    }
                }
                branch_handlers[branch_handler_count++] = branch->handler;
            }
        }
    }
    if (branch_handler_count != 29u) {
        fprintf(stderr, "jitbench: incomplete A32 branch handler family\n");
        return false;
    }
    printf("STATIC-BRANCH-SHAPE exact=yes conditional=14 link=15 "
           "handlers=29 forward=yes backward=yes\n");

    for (unsigned condition = 0u; condition < 14u; condition++) {
        const uint32_t pc = UINT32_C(0x1160);
        const uint16_t insn = (uint16_t)(UINT16_C(0xd000) |
            (condition << 8) | (((condition & 1u) != 0u) ? 2u : 0xfcu));
        const int32_t displacement =
            (int32_t)((uint32_t)(insn & 0xffu) << 24) >> 23;
        const uint32_t target = pc + 4u + (uint32_t)displacement;
        const uint8_t bytes[2] = {(uint8_t)insn, (uint8_t)(insn >> 8)};
        a64_static_block_t product_block;
        const a64_static_uop_t *branch;

        if (!a64_static_decode_at(&insn, 1u, true, pc, &block) ||
            !a64_static_decode_read_hits_bytes_at(
                bytes, 1u, true, pc, &product_block) ||
            memcmp(&block, &product_block, sizeof block) != 0 ||
            block.insn_count != 1u || block.uop_count != 2u ||
            block.start_pc != pc || block.exit_pc != pc + 2u ||
            !block.thumb || !block.dynamic_exit || block.indirect_exit ||
            !block.thumb_conditional_exit || block.touches_memory ||
            block.direct_reads || block.direct_writes ||
            block.runtime_guards || block.vfp ||
            block.uops[1].handler != 0u ||
            block.uops[1].immediate != pc + 2u) {
            fprintf(stderr,
                    "jitbench: Thumb conditional branch shape failed "
                    "cond=%u\n", condition);
            return false;
        }
        branch = &block.uops[0];
        if (branch->handler != branch_handlers[condition] ||
            branch->immediate != target || branch->pc_value != pc + 2u ||
            branch->metadata != 0u) {
            fprintf(stderr,
                    "jitbench: Thumb conditional branch record failed "
                    "cond=%u\n", condition);
            return false;
        }
    }
    {
        const uint16_t mid_block[] = {
            UINT16_C(0xd0ff), UINT16_C(0x3001)
        };
        if (a64_static_decode_at(mid_block, 2u, true,
                                 UINT32_C(0x1160), &block)) {
            fprintf(stderr,
                    "jitbench: Thumb conditional branch accepted mid-block\n");
            return false;
        }
    }
    printf("STATIC-THUMB-COND-BRANCH-SHAPE exact=yes conditions=14 "
           "handlers=reused forward=yes backward=yes terminal=yes "
           "mid-block-refused=yes rollout=yes\n");

    for (unsigned link = 0u; link < 2u; link++) {
        const unsigned registers = link ? 15u : 16u;
        for (unsigned condition = 0u; condition < 15u; condition++) {
            for (unsigned rm = 0u; rm < registers; rm++) {
                const uint32_t pc = UINT32_C(0x1180);
                const uint32_t insn = (condition << 28) |
                    UINT32_C(0x012fff10) | (link << 5) | rm;
                const unsigned expected_uops = condition < 14u ? 3u : 2u;
                uint8_t bytes[4] = {
                    (uint8_t)insn, (uint8_t)(insn >> 8),
                    (uint8_t)(insn >> 16), (uint8_t)(insn >> 24)
                };
                a64_static_block_t product_block;
                const a64_static_uop_t *branch;

                if (!a64_static_decode_at(&insn, 1u, false, pc, &block) ||
                    !a64_static_decode_read_hits_bytes_at(
                        bytes, 1u, false, pc, &product_block) ||
                    memcmp(&block, &product_block, sizeof block) != 0 ||
                    block.insn_count != 1u ||
                    block.uop_count != expected_uops ||
                    block.start_pc != pc || block.exit_pc != pc + 4u ||
                    block.thumb || !block.dynamic_exit ||
                    !block.indirect_exit ||
                    block.touches_memory || block.direct_reads ||
                    block.direct_writes || !block.runtime_guards || block.vfp ||
                    block.uops[block.uop_count - 1u].handler != 0u ||
                    block.uops[block.uop_count - 1u].immediate != pc + 4u ||
                    (condition < 14u &&
                     (block.uops[0].handler == 0u ||
                      block.uops[0].metadata != 1u))) {
                    fprintf(stderr,
                            "jitbench: A32 indirect shape failed "
                            "link=%u cond=%u rm=%u\n",
                            link, condition, rm);
                    return false;
                }
                branch = &block.uops[block.uop_count - 2u];
                if (branch->handler == 0u || branch->immediate != 0u ||
                    branch->pc_value != pc ||
                    branch->metadata != UINT32_C(0x101)) {
                    fprintf(stderr,
                            "jitbench: A32 indirect record failed "
                            "link=%u cond=%u rm=%u\n",
                            link, condition, rm);
                    return false;
                }
                if (condition == 14u) {
                    for (i = 0u; i < branch_handler_count; i++) {
                        if (branch_handlers[i] == branch->handler) {
                            fprintf(stderr,
                                    "jitbench: indirect handler collided with "
                                    "immediate branch link=%u rm=%u\n",
                                    link, rm);
                            return false;
                        }
                    }
                    for (i = 0u; i < indirect_handler_count; i++) {
                        if (indirect_handlers[i] == branch->handler) {
                            fprintf(stderr,
                                    "jitbench: duplicate A32 indirect handler "
                                    "link=%u rm=%u\n", link, rm);
                            return false;
                        }
                    }
                    indirect_handlers[indirect_handler_count++] =
                        branch->handler;
                }
            }
        }
    }

    for (unsigned link = 0u; link < 2u; link++) {
        const unsigned registers = link ? 15u : 16u;
        for (unsigned rm = 0u; rm < registers; rm++) {
            const uint32_t pc = UINT32_C(0x11c0);
            const uint16_t insn = (uint16_t)(UINT16_C(0x4700) |
                                            (link << 7) | (rm << 3));
            uint8_t bytes[2] = {(uint8_t)insn, (uint8_t)(insn >> 8)};
            a64_static_block_t product_block;
            const a64_static_uop_t *branch;

            if (!a64_static_decode_at(&insn, 1u, true, pc, &block) ||
                !a64_static_decode_read_hits_bytes_at(
                    bytes, 1u, true, pc, &product_block) ||
                memcmp(&block, &product_block, sizeof block) != 0 ||
                block.insn_count != 1u || block.uop_count != 2u ||
                block.start_pc != pc || block.exit_pc != pc + 2u ||
                !block.thumb || !block.dynamic_exit ||
                !block.indirect_exit ||
                block.touches_memory || block.direct_reads ||
                block.direct_writes || !block.runtime_guards || block.vfp ||
                block.uops[1].handler != 0u ||
                block.uops[1].immediate != pc + 2u) {
                fprintf(stderr,
                        "jitbench: Thumb indirect shape failed "
                        "link=%u rm=%u\n", link, rm);
                return false;
            }
            branch = &block.uops[0];
            if (branch->handler == 0u || branch->immediate != 0u ||
                branch->pc_value != pc ||
                branch->metadata != UINT32_C(0x101)) {
                fprintf(stderr,
                        "jitbench: Thumb indirect record failed "
                        "link=%u rm=%u\n", link, rm);
                return false;
            }
            for (i = 0u; i < branch_handler_count; i++) {
                if (branch_handlers[i] == branch->handler) {
                    fprintf(stderr,
                            "jitbench: Thumb indirect handler collided with "
                            "immediate branch link=%u rm=%u\n", link, rm);
                    return false;
                }
            }
            for (i = 0u; i < indirect_handler_count; i++) {
                if (indirect_handlers[i] == branch->handler) {
                    fprintf(stderr,
                            "jitbench: duplicate Thumb indirect handler "
                            "link=%u rm=%u\n", link, rm);
                    return false;
                }
            }
            indirect_handlers[indirect_handler_count++] = branch->handler;
        }
    }
    if (indirect_handler_count != 62u) {
        fprintf(stderr, "jitbench: incomplete indirect branch family\n");
        return false;
    }
    printf("STATIC-INDIRECT-BRANCH-SHAPE exact=yes a32-bx=16 a32-blx=15 "
           "thumb-bx=16 thumb-blx=15 handlers=62 conditional=yes "
           "terminal=yes guarded=yes\n");

    for (i = 0u; i < sizeof VALID_A32_STORES /
                         sizeof VALID_A32_STORES[0]; i++) {
        uint32_t value = VALID_A32_STORES[i];
        const uint32_t pc = UINT32_C(0x1200) + i * 4u;
        uint8_t bytes[4] = {
            (uint8_t)value, (uint8_t)(value >> 8),
            (uint8_t)(value >> 16), (uint8_t)(value >> 24)
        };
        if (a64_static_decode_read_hits_bytes_at(
                bytes, 1u, false, pc, &block) ||
            !a64_static_decode_memory_hits_bytes_at(
                bytes, 1u, false, pc, &block) ||
            block.insn_count != 1u ||
            block.uop_count != VALID_A32_STORE_UOPS[i] ||
            block.start_pc != pc || block.exit_pc != pc + 4u ||
            block.thumb || !block.touches_memory || block.direct_reads ||
            !block.direct_writes || !block.runtime_guards || block.vfp ||
            block.uops[block.uop_count - 2u].handler == 0u ||
            block.uops[block.uop_count - 2u].pc_value != pc ||
            block.uops[block.uop_count - 2u].metadata != UINT32_C(0x101)) {
            fprintf(stderr,
                    "jitbench: product A32 store shape failed at %u\n", i);
            return false;
        }
    }
    for (i = 0u; i < sizeof INVALID_A32_STORES /
                         sizeof INVALID_A32_STORES[0]; i++) {
        uint32_t value = INVALID_A32_STORES[i];
        uint8_t bytes[4] = {
            (uint8_t)value, (uint8_t)(value >> 8),
            (uint8_t)(value >> 16), (uint8_t)(value >> 24)
        };
        if (a64_static_decode_memory_hits_bytes_at(
                bytes, 1u, false, 0x1300u, &block)) {
            fprintf(stderr,
                    "jitbench: product decoder accepted invalid A32 store %u\n",
                    i);
            return false;
        }
    }
    {
        uint32_t values[2] = {
            VALID_A32_STORES[0], A32_DP_IMM(14, 4, 0, 0, 0, 0, 1)
        };
        uint8_t bytes[8];
        for (i = 0u; i < 2u; i++) {
            bytes[i * 4u + 0u] = (uint8_t)values[i];
            bytes[i * 4u + 1u] = (uint8_t)(values[i] >> 8);
            bytes[i * 4u + 2u] = (uint8_t)(values[i] >> 16);
            bytes[i * 4u + 3u] = (uint8_t)(values[i] >> 24);
        }
        if (a64_static_decode_memory_hits_bytes_at(
                bytes, 2u, false, 0x1400u, &block)) {
            fprintf(stderr,
                    "jitbench: product decoder accepted a mid-block store\n");
            return false;
        }
    }

    for (i = 0u; i < sizeof VALID_THUMB_STORES /
                         sizeof VALID_THUMB_STORES[0]; i++) {
        uint16_t value = VALID_THUMB_STORES[i];
        const uint32_t pc = UINT32_C(0x1500) + i * 2u;
        uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
        if (a64_static_decode_read_hits_bytes_at(
                bytes, 1u, true, pc, &block) ||
            !a64_static_decode_memory_hits_bytes_at(
                bytes, 1u, true, pc, &block) ||
            block.insn_count != 1u ||
            block.uop_count != VALID_THUMB_STORE_UOPS[i] ||
            block.start_pc != pc || block.exit_pc != pc + 2u ||
            !block.thumb || !block.touches_memory || block.direct_reads ||
            !block.direct_writes || !block.runtime_guards || block.vfp ||
            block.uops[block.uop_count - 2u].handler == 0u ||
            block.uops[block.uop_count - 2u].pc_value != pc ||
            block.uops[block.uop_count - 2u].metadata != UINT32_C(0x101)) {
            fprintf(stderr,
                    "jitbench: product Thumb store shape failed at %u\n", i);
            return false;
        }
    }
    {
        uint16_t values[2] = {
            VALID_THUMB_STORES[0], UINT16_C(0x3001)
        };
        uint8_t bytes[4] = {
            (uint8_t)values[0], (uint8_t)(values[0] >> 8),
            (uint8_t)values[1], (uint8_t)(values[1] >> 8)
        };
        if (a64_static_decode_memory_hits_bytes_at(
                bytes, 2u, true, 0x1600u, &block)) {
            fprintf(stderr,
                    "jitbench: product decoder accepted a mid-block Thumb store\n");
            return false;
        }
    }
    printf("STATIC-STORE-SHAPE exact=yes a32=6 thumb=7 terminal=yes "
           "read-contract=preserved handlers=%u\n",
           A64_STATIC_HANDLER_COUNT);

    {
        uint32_t preflight_handlers[60];
        uint32_t writeback_handlers[15];
        uint32_t no_writeback_handler = 0u;
        unsigned preflight_count = 0u;

        for (i = 0u; i < sizeof VALID_A32_STM /
                             sizeof VALID_A32_STM[0]; i++) {
            const uint32_t value = VALID_A32_STM[i];
            const uint32_t pc = UINT32_C(0x1700) + i * 4u;
            const unsigned words = VALID_A32_STM_WORDS[i];
            const bool conditional = (value >> 28) < 14u;
            const unsigned preflight = conditional ? 1u : 0u;
            const unsigned expected_uops = words + 3u +
                                            (conditional ? 1u : 0u);
            uint8_t bytes[4] = {
                (uint8_t)value, (uint8_t)(value >> 8),
                (uint8_t)(value >> 16), (uint8_t)(value >> 24)
            };
            if (a64_static_decode_read_hits_bytes_at(
                    bytes, 1u, false, pc, &block) ||
                !a64_static_decode_memory_hits_bytes_at(
                    bytes, 1u, false, pc, &block) ||
                block.insn_count != 1u ||
                block.uop_count != expected_uops || block.thumb ||
                !block.touches_memory || block.direct_reads ||
                !block.direct_writes || !block.runtime_guards || block.vfp ||
                block.vfp_direct_writes || !block.stm_direct_writes ||
                block.uops[preflight].handler == 0u ||
                block.uops[preflight].immediate != words ||
                block.uops[preflight].pc_value != pc ||
                block.uops[preflight].metadata != UINT32_C(0x101) ||
                block.uops[block.uop_count - 2u].handler == 0u ||
                block.uops[block.uop_count - 2u].immediate != words ||
                block.uops[block.uop_count - 2u].pc_value != 0u ||
                block.uops[block.uop_count - 2u].metadata != 0u ||
                (conditional && block.uops[0].metadata != words + 2u)) {
                fprintf(stderr, "jitbench: product STM shape failed at %u\n",
                        i);
                return false;
            }
            for (unsigned j = 1u; j < words; j++) {
                if (block.uops[preflight + j].handler >=
                    block.uops[preflight + j + 1u].handler) {
                    fprintf(stderr,
                            "jitbench: product STM source order failed at %u\n",
                            i);
                    return false;
                }
            }
        }

        /* Prove that every P/U/base dimension reaches distinct signed text. */
        for (unsigned pre = 0u; pre < 2u; pre++) {
            for (unsigned up = 0u; up < 2u; up++) {
                for (unsigned rn = 0u; rn < 15u; rn++) {
                    const uint32_t value = A32_BLOCK(
                        14, pre, up, 0, 0, 0, rn, UINT32_C(1));
                    uint8_t bytes[4] = {
                        (uint8_t)value, (uint8_t)(value >> 8),
                        (uint8_t)(value >> 16), (uint8_t)(value >> 24)
                    };
                    if (!a64_static_decode_memory_hits_bytes_at(
                            bytes, 1u, false, UINT32_C(0x1800), &block) ||
                        block.uop_count != 4u) {
                        fprintf(stderr, "jitbench: STM preflight family gap\n");
                        return false;
                    }
                    for (unsigned j = 0u; j < preflight_count; j++) {
                        if (preflight_handlers[j] == block.uops[0].handler) {
                            fprintf(stderr,
                                    "jitbench: duplicate STM preflight handler\n");
                            return false;
                        }
                    }
                    preflight_handlers[preflight_count++] =
                        block.uops[0].handler;
                    if (!no_writeback_handler)
                        no_writeback_handler = block.uops[2].handler;
                    else if (no_writeback_handler != block.uops[2].handler) {
                        fprintf(stderr,
                                "jitbench: STM no-writeback handler drifted\n");
                        return false;
                    }
                }
            }
        }
        for (unsigned rn = 0u; rn < 15u; rn++) {
            const unsigned source = (rn + 1u) % 15u;
            const uint32_t value = A32_BLOCK(
                14, 0, 1, 0, 1, 0, rn, UINT32_C(1) << source);
            uint8_t bytes[4] = {
                (uint8_t)value, (uint8_t)(value >> 8),
                (uint8_t)(value >> 16), (uint8_t)(value >> 24)
            };
            if (!a64_static_decode_memory_hits_bytes_at(
                    bytes, 1u, false, UINT32_C(0x1900), &block) ||
                block.uop_count != 4u ||
                block.uops[2].handler == no_writeback_handler) {
                fprintf(stderr, "jitbench: STM writeback family gap\n");
                return false;
            }
            for (unsigned j = 0u; j < rn; j++) {
                if (writeback_handlers[j] == block.uops[2].handler) {
                    fprintf(stderr,
                            "jitbench: duplicate STM writeback handler\n");
                    return false;
                }
            }
            writeback_handlers[rn] = block.uops[2].handler;
        }
        if (preflight_count != 60u) return false;

        for (i = 0u; i < sizeof INVALID_A32_STM /
                             sizeof INVALID_A32_STM[0]; i++) {
            const uint32_t value = INVALID_A32_STM[i];
            uint8_t bytes[4] = {
                (uint8_t)value, (uint8_t)(value >> 8),
                (uint8_t)(value >> 16), (uint8_t)(value >> 24)
            };
            if (a64_static_decode_memory_hits_bytes_at(
                    bytes, 1u, false, UINT32_C(0x1a00), &block)) {
                fprintf(stderr,
                        "jitbench: product decoder accepted invalid STM %u\n",
                        i);
                return false;
            }
        }
        {
            const uint32_t values[2] = {
                VALID_A32_STM[0], A32_DP_IMM(14, 4, 0, 0, 0, 0, 1)
            };
            uint8_t bytes[8];
            for (i = 0u; i < 2u; i++) {
                bytes[i * 4u + 0u] = (uint8_t)values[i];
                bytes[i * 4u + 1u] = (uint8_t)(values[i] >> 8);
                bytes[i * 4u + 2u] = (uint8_t)(values[i] >> 16);
                bytes[i * 4u + 3u] = (uint8_t)(values[i] >> 24);
            }
            if (a64_static_decode_memory_hits_bytes_at(
                    bytes, 2u, false, UINT32_C(0x1b00), &block)) {
                fprintf(stderr,
                        "jitbench: product decoder accepted a mid-block STM\n");
                return false;
            }
        }
        printf("STATIC-STM-SHAPE exact=yes modes=4 bases=15 sources=16 "
               "writeback=15 conditional=yes pc-source=yes max-words=16 "
               "transactional=yes terminal=yes read-contract=preserved "
               "handlers=%u\n", A64_STATIC_HANDLER_COUNT);
    }

    {
        uint32_t preflight_handlers[60];
        uint32_t writeback_handlers[15];
        uint32_t no_writeback_handler = 0u;
        unsigned preflight_count = 0u;

        for (i = 0u; i < sizeof VALID_A32_LDM /
                             sizeof VALID_A32_LDM[0]; i++) {
            const uint32_t value = VALID_A32_LDM[i];
            const uint32_t pc = UINT32_C(0x1c00) + i * 4u;
            const unsigned words = VALID_A32_LDM_WORDS[i];
            const bool conditional = (value >> 28) < 14u;
            const unsigned preflight = conditional ? 1u : 0u;
            const unsigned expected_uops = words + 3u +
                                            (conditional ? 1u : 0u);
            uint8_t bytes[4] = {
                (uint8_t)value, (uint8_t)(value >> 8),
                (uint8_t)(value >> 16), (uint8_t)(value >> 24)
            };
            if (!a64_static_decode_read_hits_bytes_at(
                    bytes, 1u, false, pc, &block) ||
                block.insn_count != 1u || block.uop_count != expected_uops ||
                block.thumb || !block.touches_memory || !block.direct_reads ||
                block.direct_writes || !block.runtime_guards || block.vfp ||
                block.vfp_direct_writes || block.stm_direct_writes ||
                !block.ldm_direct_reads || block.vstm_direct_writes ||
                block.uops[preflight].handler == 0u ||
                block.uops[preflight].immediate != words ||
                block.uops[preflight].pc_value != pc ||
                block.uops[preflight].metadata != UINT32_C(0x101) ||
                block.uops[block.uop_count - 2u].handler == 0u ||
                block.uops[block.uop_count - 2u].immediate != words ||
                block.uops[block.uop_count - 2u].pc_value != 0u ||
                block.uops[block.uop_count - 2u].metadata != 0u ||
                (conditional && block.uops[0].metadata != words + 2u)) {
                fprintf(stderr, "jitbench: product LDM shape failed at %u\n",
                        i);
                return false;
            }
            for (unsigned j = 1u; j < words; j++) {
                if (block.uops[preflight + j].handler >=
                    block.uops[preflight + j + 1u].handler) {
                    fprintf(stderr,
                            "jitbench: product LDM destination order failed "
                            "at %u\n", i);
                    return false;
                }
            }
        }

        for (unsigned pre = 0u; pre < 2u; pre++) {
            for (unsigned up = 0u; up < 2u; up++) {
                for (unsigned rn = 0u; rn < 15u; rn++) {
                    const uint32_t value = A32_BLOCK(
                        14, pre, up, 0, 0, 1, rn, UINT32_C(1));
                    uint8_t bytes[4] = {
                        (uint8_t)value, (uint8_t)(value >> 8),
                        (uint8_t)(value >> 16), (uint8_t)(value >> 24)
                    };
                    if (!a64_static_decode_read_hits_bytes_at(
                            bytes, 1u, false, UINT32_C(0x1d00), &block) ||
                        block.uop_count != 4u || !block.ldm_direct_reads) {
                        fprintf(stderr, "jitbench: LDM preflight family gap\n");
                        return false;
                    }
                    for (unsigned j = 0u; j < preflight_count; j++) {
                        if (preflight_handlers[j] == block.uops[0].handler) {
                            fprintf(stderr,
                                    "jitbench: duplicate LDM preflight handler\n");
                            return false;
                        }
                    }
                    preflight_handlers[preflight_count++] =
                        block.uops[0].handler;
                    if (!no_writeback_handler)
                        no_writeback_handler = block.uops[2].handler;
                    else if (no_writeback_handler != block.uops[2].handler) {
                        fprintf(stderr,
                                "jitbench: LDM no-writeback handler drifted\n");
                        return false;
                    }
                }
            }
        }
        for (unsigned rn = 0u; rn < 15u; rn++) {
            const unsigned destination = (rn + 1u) % 15u;
            const uint32_t value = A32_BLOCK(
                14, 0, 1, 0, 1, 1, rn, UINT32_C(1) << destination);
            uint8_t bytes[4] = {
                (uint8_t)value, (uint8_t)(value >> 8),
                (uint8_t)(value >> 16), (uint8_t)(value >> 24)
            };
            if (!a64_static_decode_read_hits_bytes_at(
                    bytes, 1u, false, UINT32_C(0x1e00), &block) ||
                block.uop_count != 4u ||
                block.uops[2].handler == no_writeback_handler) {
                fprintf(stderr, "jitbench: LDM writeback family gap\n");
                return false;
            }
            for (unsigned j = 0u; j < rn; j++) {
                if (writeback_handlers[j] == block.uops[2].handler) {
                    fprintf(stderr,
                            "jitbench: duplicate LDM writeback handler\n");
                    return false;
                }
            }
            writeback_handlers[rn] = block.uops[2].handler;
        }
        if (preflight_count != 60u) return false;

        for (i = 0u; i < sizeof INVALID_A32_LDM /
                             sizeof INVALID_A32_LDM[0]; i++) {
            const uint32_t value = INVALID_A32_LDM[i];
            uint8_t bytes[4] = {
                (uint8_t)value, (uint8_t)(value >> 8),
                (uint8_t)(value >> 16), (uint8_t)(value >> 24)
            };
            if (a64_static_decode_read_hits_bytes_at(
                    bytes, 1u, false, UINT32_C(0x1f00), &block)) {
                fprintf(stderr,
                        "jitbench: product decoder accepted invalid LDM %u\n",
                        i);
                return false;
            }
        }
        {
            const uint32_t values[2] = {
                VALID_A32_LDM[0], A32_DP_IMM(14, 4, 0, 0, 0, 0, 1)
            };
            uint8_t bytes[8];
            for (i = 0u; i < 2u; i++) {
                bytes[i * 4u + 0u] = (uint8_t)values[i];
                bytes[i * 4u + 1u] = (uint8_t)(values[i] >> 8);
                bytes[i * 4u + 2u] = (uint8_t)(values[i] >> 16);
                bytes[i * 4u + 3u] = (uint8_t)(values[i] >> 24);
            }
            if (!a64_static_decode_read_hits_bytes_at(
                    bytes, 2u, false, UINT32_C(0x2000), &block) ||
                block.insn_count != 2u || block.uop_count != 8u ||
                !block.ldm_direct_reads || block.direct_writes) {
                fprintf(stderr,
                        "jitbench: product decoder did not continue after LDM\n");
                return false;
            }
        }
        printf("STATIC-LDM-SHAPE exact=yes modes=4 bases=15 "
               "destinations=15 writeback=15 conditional=yes max-words=15 "
               "transactional=yes nonterminal=yes pc=no user-bank=no "
               "handlers=%u\n", A64_STATIC_HANDLER_COUNT);
    }

    if (!a64_static_decode_read_hits_bytes_at(
            read_bytes, (unsigned)(sizeof A32_READ_HITS /
                                   sizeof A32_READ_HITS[0]),
            false, 0xb000u, &block) ||
        block.insn_count != sizeof A32_READ_HITS / sizeof A32_READ_HITS[0] ||
        block.uop_count != 23u || block.start_pc != 0xb000u ||
        block.exit_pc != 0xb024u || !block.touches_memory ||
        !block.direct_reads) {
        fprintf(stderr, "jitbench: product read-hit shape failed\n");
        return false;
    }
    printf("STATIC-READ-SHAPE exact=yes insns=%u uops=%u handlers=%u\n",
           block.insn_count, block.uop_count, A64_STATIC_HANDLER_COUNT);

    if (!a64_static_decode_read_hits_bytes_at(
            thumb_read_bytes, (unsigned)(sizeof THUMB_READ_HITS /
                                         sizeof THUMB_READ_HITS[0]),
            true, 0xe200u, &block) ||
        block.insn_count != sizeof THUMB_READ_HITS /
                            sizeof THUMB_READ_HITS[0] ||
        block.uop_count != 26u || block.start_pc != 0xe200u ||
        block.exit_pc != 0xe214u || !block.touches_memory ||
        !block.direct_reads) {
        fprintf(stderr, "jitbench: product Thumb read-hit shape failed\n");
        return false;
    }
    printf("STATIC-THUMB-READ-SHAPE exact=yes insns=%u uops=%u "
           "handlers=%u\n", block.insn_count, block.uop_count,
           A64_STATIC_HANDLER_COUNT);

    if (!a64_static_decode_read_hits_bytes_at(
            vfp_bytes, (unsigned)(sizeof VFP_REGISTER_OPS /
                                  sizeof VFP_REGISTER_OPS[0]),
            false, 0xe400u, &block) ||
        block.insn_count != sizeof VFP_REGISTER_OPS /
                            sizeof VFP_REGISTER_OPS[0] ||
        block.uop_count != 17u || block.start_pc != 0xe400u ||
        block.exit_pc != 0xe440u || block.touches_memory ||
        block.direct_reads || !block.runtime_guards || !block.vfp) {
        fprintf(stderr, "jitbench: product VFP register shape failed\n");
        return false;
    }
    printf("STATIC-VFP-REGISTER-SHAPE exact=yes insns=%u uops=%u "
           "handlers=%u\n", block.insn_count, block.uop_count,
           A64_STATIC_HANDLER_COUNT);

    if (!a64_static_decode_read_hits_bytes_at(
            vfp_compare_bytes,
            (unsigned)(sizeof VFP_COMPARE_OPS / sizeof VFP_COMPARE_OPS[0]),
            false, 0xe500u, &block) ||
        block.insn_count != sizeof VFP_COMPARE_OPS /
                            sizeof VFP_COMPARE_OPS[0] ||
        block.uop_count != 9u || block.start_pc != 0xe500u ||
        block.exit_pc != 0xe520u || block.touches_memory ||
        block.direct_reads || !block.runtime_guards || !block.vfp) {
        fprintf(stderr, "jitbench: product VFP compare shape failed\n");
        return false;
    }
    printf("STATIC-VFP-COMPARE-SHAPE exact=yes insns=%u uops=%u "
           "handlers=%u\n", block.insn_count, block.uop_count,
           A64_STATIC_HANDLER_COUNT);

    if (!a64_static_decode_read_hits_bytes_at(
            vfp_widen_bytes,
            (unsigned)(sizeof VFP_WIDEN_OPS / sizeof VFP_WIDEN_OPS[0]),
            false, 0xe600u, &block) ||
        block.insn_count != sizeof VFP_WIDEN_OPS /
                            sizeof VFP_WIDEN_OPS[0] ||
        block.uop_count != 9u || block.start_pc != 0xe600u ||
        block.exit_pc != 0xe620u || block.touches_memory ||
        block.direct_reads || !block.runtime_guards || !block.vfp) {
        fprintf(stderr, "jitbench: product VFP widen shape failed\n");
        return false;
    }
    printf("STATIC-VFP-WIDEN-SHAPE exact=yes insns=%u uops=%u "
           "handlers=%u\n", block.insn_count, block.uop_count,
           A64_STATIC_HANDLER_COUNT);

    if (!a64_static_decode_read_hits_bytes_at(
            vfp_narrow_bytes,
            (unsigned)(sizeof VFP_NARROW_OPS / sizeof VFP_NARROW_OPS[0]),
            false, 0xe680u, &block) ||
        block.insn_count != sizeof VFP_NARROW_OPS /
                            sizeof VFP_NARROW_OPS[0] ||
        block.uop_count != 9u || block.start_pc != 0xe680u ||
        block.exit_pc != 0xe6a0u || block.touches_memory ||
        block.direct_reads || !block.runtime_guards || !block.vfp) {
        fprintf(stderr, "jitbench: product VFP narrow shape failed\n");
        return false;
    }
    printf("STATIC-VFP-NARROW-SHAPE exact=yes insns=%u uops=%u "
           "handlers=%u\n", block.insn_count, block.uop_count,
           A64_STATIC_HANDLER_COUNT);

    {
        static const unsigned S_RD[] = {0u,31u,3u,28u,8u,23u,12u,18u,22u};
        static const unsigned S_RN[] = {1u,30u,5u,26u,9u,21u,13u,17u,20u};
        static const unsigned S_RM[] = {2u,29u,7u,24u,10u,19u,14u,16u,15u};
        static const unsigned D_RD[] = {0u,15u,3u,12u,4u,13u,2u,10u,11u};
        static const unsigned D_RN[] = {1u,14u,5u,11u,6u,9u,3u,8u,7u};
        static const unsigned D_RM[] = {2u,13u,7u,10u,8u,5u,4u,6u,1u};
        const uint32_t *programs[2] = {VFP_ARITH32_OPS, VFP_ARITH64_OPS};
        const unsigned *rds[2] = {S_RD, D_RD};
        const unsigned *rns[2] = {S_RN, D_RN};
        const unsigned *rms[2] = {S_RM, D_RM};
        for (unsigned width = 0u; width < 2u; width++) {
            for (unsigned operation = 0u; operation < 9u; operation++) {
                uint32_t value = programs[width][operation];
                uint32_t pc = UINT32_C(0xe700) +
                              (width * 9u + operation) * 4u;
                unsigned scale = width ? 2u : 1u;
                uint8_t bytes[4] = {
                    (uint8_t)value, (uint8_t)(value >> 8),
                    (uint8_t)(value >> 16), (uint8_t)(value >> 24)
                };
                uint32_t immediate = rds[width][operation] * scale |
                    (rns[width][operation] * scale << 8) |
                    (rms[width][operation] * scale << 16);
                if (!a64_static_decode_read_hits_bytes_at(
                        bytes, 1u, false, pc, &block) ||
                    block.insn_count != 1u || block.uop_count != 2u ||
                    block.start_pc != pc || block.exit_pc != pc + 4u ||
                    block.touches_memory || block.direct_reads ||
                    !block.runtime_guards || !block.vfp ||
                    !block.vfp_arithmetic || block.vfp_direct_writes ||
                    block.uops[0].handler == 0u ||
                    block.uops[0].immediate != immediate ||
                    block.uops[0].pc_value != pc ||
                    block.uops[0].metadata != UINT32_C(0x101)) {
                    fprintf(stderr,
                            "jitbench: VFP arithmetic shape failed "
                            "width=%u operation=%u\n", width, operation);
                    return false;
                }
                for (unsigned old_width = 0u; old_width <= width;
                     old_width++) {
                    unsigned limit = old_width == width ? operation : 9u;
                    for (unsigned old_operation = 0u;
                         old_operation < limit; old_operation++) {
                        if (vfp_arith_handlers[old_width][old_operation] ==
                                block.uops[0].handler) {
                            fprintf(stderr,
                                    "jitbench: VFP arithmetic handler alias "
                                    "width=%u operation=%u\n",
                                    width, operation);
                            return false;
                        }
                    }
                }
                vfp_arith_handlers[width][operation] =
                    block.uops[0].handler;
            }
        }
    }
    printf("STATIC-VFP-ARITH-SHAPE exact=yes operations=9 widths=2 "
           "handlers=18 conditional=yes runtime-guard=yes total=%u\n",
           A64_STATIC_HANDLER_COUNT);

    if (!a64_static_decode_read_hits_bytes_at(
            vfp_read_bytes,
            (unsigned)(sizeof VFP_READ_HITS / sizeof VFP_READ_HITS[0]),
            false, 0xf000u, &block) ||
        block.insn_count != sizeof VFP_READ_HITS /
                            sizeof VFP_READ_HITS[0] ||
        block.uop_count != 19u || block.start_pc != 0xf000u ||
        block.exit_pc != 0xf024u || !block.touches_memory ||
        !block.direct_reads || !block.runtime_guards || !block.vfp) {
        fprintf(stderr, "jitbench: product VFP read shape failed\n");
        return false;
    }
    printf("STATIC-VFP-READ-SHAPE exact=yes insns=%u uops=%u handlers=%u\n",
           block.insn_count, block.uop_count, A64_STATIC_HANDLER_COUNT);

    {
        static const unsigned source_registers[] = {
            0u, 31u, 2u, 4u, 30u, 4u, 10u
        };
        for (i = 0u; i < sizeof VFP_WRITE_HITS /
                             sizeof VFP_WRITE_HITS[0]; i++) {
            const uint32_t value = VFP_WRITE_HITS[i];
            const uint32_t pc = UINT32_C(0x10200) + i * 4u;
            const bool conditional = (value >> 28) < 14u;
            const bool dbl = (value & (1u << 8)) != 0u;
            uint8_t bytes[4] = {
                (uint8_t)value, (uint8_t)(value >> 8),
                (uint8_t)(value >> 16), (uint8_t)(value >> 24)
            };
            const a64_static_uop_t *store;

            if (a64_static_decode_read_hits_bytes_at(
                    bytes, 1u, false, pc, &block) ||
                !a64_static_decode_memory_hits_bytes_at(
                    bytes, 1u, false, pc, &block) ||
                block.insn_count != 1u ||
                block.uop_count != (conditional ? 4u : 3u) ||
                block.start_pc != pc || block.exit_pc != pc + 4u ||
                block.thumb || !block.touches_memory || block.direct_reads ||
                !block.direct_writes || !block.runtime_guards || !block.vfp ||
                !block.vfp_direct_writes) {
                fprintf(stderr,
                        "jitbench: product VFP write shape failed at %u\n",
                        i);
                return false;
            }
            store = &block.uops[block.uop_count - 2u];
            if (store->handler == 0u ||
                store->immediate != source_registers[i] ||
                store->pc_value != pc ||
                store->metadata != UINT32_C(0x101)) {
                fprintf(stderr,
                        "jitbench: product VFP write record failed at %u\n",
                        i);
                return false;
            }
            if (vfp_write_handlers[dbl ? 1u : 0u] == 0u)
                vfp_write_handlers[dbl ? 1u : 0u] = store->handler;
            else if (vfp_write_handlers[dbl ? 1u : 0u] != store->handler) {
                fprintf(stderr,
                        "jitbench: VFP write width handler was not stable\n");
                return false;
            }
        }
        if (vfp_write_handlers[0] == vfp_write_handlers[1]) {
            fprintf(stderr, "jitbench: VFP write widths share a handler\n");
            return false;
        }
    }

    /* Every architectural VSTM address/base pair has a distinct signed-text
     * handler, while the data record carries only the contiguous VFP word
     * slice. This is a structural proof; runtime one-block rollback is covered
     * by the native semantic oracle below. */
    for (unsigned mode = 0u; mode < 3u; mode++) {
        const bool pre = mode == 2u;
        const bool up = mode != 2u;
        const bool writeback = mode != 0u;
        for (unsigned rn = 0u; rn < 15u; rn++) {
            uint32_t value = VFP_LDST(
                14, pre, up, 0, writeback, 0, rn, 0, 0, 1);
            uint32_t pc = UINT32_C(0x10500) +
                          (mode * 15u + rn) * 4u;
            uint8_t bytes[4] = {
                (uint8_t)value, (uint8_t)(value >> 8),
                (uint8_t)(value >> 16), (uint8_t)(value >> 24)
            };
            const a64_static_uop_t *store;

            if (a64_static_decode_read_hits_bytes_at(
                    bytes, 1u, false, pc, &block) ||
                !a64_static_decode_memory_hits_bytes_at(
                    bytes, 1u, false, pc, &block) ||
                block.insn_count != 1u || block.uop_count != 2u ||
                block.start_pc != pc || block.exit_pc != pc + 4u ||
                block.thumb || !block.touches_memory || block.direct_reads ||
                !block.direct_writes || !block.runtime_guards || !block.vfp ||
                block.vfp_direct_writes || block.stm_direct_writes ||
                !block.vstm_direct_writes) {
                fprintf(stderr,
                        "jitbench: product VSTM handler shape failed "
                        "mode=%u rn=%u\n", mode, rn);
                return false;
            }
            store = &block.uops[block.uop_count - 2u];
            if (store->handler == 0u || store->immediate != UINT32_C(0x100) ||
                store->pc_value != pc ||
                store->metadata != UINT32_C(0x101)) {
                fprintf(stderr,
                        "jitbench: product VSTM record failed mode=%u rn=%u\n",
                        mode, rn);
                return false;
            }
            for (unsigned old_mode = 0u; old_mode <= mode; old_mode++) {
                unsigned old_limit = old_mode == mode ? rn : 15u;
                for (unsigned old_rn = 0u; old_rn < old_limit; old_rn++) {
                    if (vstm_write_handlers[old_mode][old_rn] ==
                            store->handler) {
                        fprintf(stderr,
                                "jitbench: VSTM handler alias mode=%u rn=%u\n",
                                mode, rn);
                        return false;
                    }
                }
            }
            vstm_write_handlers[mode][rn] = store->handler;
        }
    }
    {
        static const unsigned FIRST[] = {0u, 31u, 0u, 28u, 0u};
        static const unsigned WORDS[] = {1u, 1u, 32u, 4u, 2u};
        static const unsigned MODES[] = {0u, 1u, 2u, 0u, 1u};
        static const unsigned BASES[] = {0u, 1u, 13u, 2u, 3u};
        for (i = 0u; i < sizeof VSTM_WRITE_HITS /
                             sizeof VSTM_WRITE_HITS[0]; i++) {
            uint32_t value = VSTM_WRITE_HITS[i];
            uint32_t pc = UINT32_C(0x10700) + i * 4u;
            bool conditional = (value >> 28) < 14u;
            uint8_t bytes[4] = {
                (uint8_t)value, (uint8_t)(value >> 8),
                (uint8_t)(value >> 16), (uint8_t)(value >> 24)
            };
            const a64_static_uop_t *store;
            if (a64_static_decode_read_hits_bytes_at(
                    bytes, 1u, false, pc, &block) ||
                !a64_static_decode_memory_hits_bytes_at(
                    bytes, 1u, false, pc, &block) ||
                block.uop_count != (conditional ? 3u : 2u) ||
                !block.direct_writes || !block.runtime_guards || !block.vfp ||
                block.vfp_direct_writes || block.stm_direct_writes ||
                !block.vstm_direct_writes) {
                fprintf(stderr,
                        "jitbench: VSTM list shape failed at %u\n", i);
                return false;
            }
            store = &block.uops[block.uop_count - 2u];
            if (store->handler !=
                    vstm_write_handlers[MODES[i]][BASES[i]] ||
                store->immediate !=
                    (FIRST[i] | (WORDS[i] << 8)) ||
                store->pc_value != pc ||
                store->metadata != UINT32_C(0x101)) {
                fprintf(stderr,
                        "jitbench: VSTM list record failed at %u\n", i);
                return false;
            }
        }
    }
    for (i = 0u; i < sizeof INVALID_PRODUCT_VFP_STORES /
                         sizeof INVALID_PRODUCT_VFP_STORES[0]; i++) {
        uint32_t value = INVALID_PRODUCT_VFP_STORES[i];
        uint8_t bytes[4] = {
            (uint8_t)value, (uint8_t)(value >> 8),
            (uint8_t)(value >> 16), (uint8_t)(value >> 24)
        };
        if (a64_static_decode_memory_hits_bytes_at(
                bytes, 1u, false, 0x10300u, &block)) {
            fprintf(stderr,
                    "jitbench: product decoder accepted invalid VFP store %u\n",
                    i);
            return false;
        }
    }
    {
        uint32_t values[2] = {
            VFP_WRITE_HITS[0], A32_DP_IMM(14, 4, 0, 0, 0, 0, 1)
        };
        uint8_t bytes[8];
        for (i = 0u; i < 2u; i++) {
            bytes[i * 4u + 0u] = (uint8_t)values[i];
            bytes[i * 4u + 1u] = (uint8_t)(values[i] >> 8);
            bytes[i * 4u + 2u] = (uint8_t)(values[i] >> 16);
            bytes[i * 4u + 3u] = (uint8_t)(values[i] >> 24);
        }
        if (a64_static_decode_memory_hits_bytes_at(
                bytes, 2u, false, 0x10400u, &block)) {
            fprintf(stderr,
                    "jitbench: product decoder accepted a mid-block VSTR\n");
            return false;
        }
    }
    {
        uint32_t values[2] = {
            VSTM_WRITE_HITS[0], A32_DP_IMM(14, 4, 0, 0, 0, 0, 1)
        };
        uint8_t bytes[8];
        for (i = 0u; i < 2u; i++) {
            bytes[i * 4u + 0u] = (uint8_t)values[i];
            bytes[i * 4u + 1u] = (uint8_t)(values[i] >> 8);
            bytes[i * 4u + 2u] = (uint8_t)(values[i] >> 16);
            bytes[i * 4u + 3u] = (uint8_t)(values[i] >> 24);
        }
        if (a64_static_decode_memory_hits_bytes_at(
                bytes, 2u, false, 0x10800u, &block)) {
            fprintf(stderr,
                    "jitbench: product decoder accepted a mid-block VSTM\n");
            return false;
        }
    }
    printf("STATIC-VFP-WRITE-SHAPE exact=yes cases=7 single=yes double=yes "
           "pc-relative=yes conditional=yes terminal=yes read-contract="
           "preserved handlers=%u\n", A64_STATIC_HANDLER_COUNT);
    printf("STATIC-VSTM-WRITE-SHAPE exact=yes modes=3 bases=15 "
           "single-words=1-32 double-words=even-2-32 FSTMX=no terminal=yes "
           "read-contract=preserved handlers=%u\n",
           A64_STATIC_HANDLER_COUNT);

    for (i = 0u; i < sizeof INVALID_READ_HITS /
                         sizeof INVALID_READ_HITS[0]; i++) {
        uint32_t value = INVALID_READ_HITS[i];
        uint8_t bytes[4] = {
            (uint8_t)value, (uint8_t)(value >> 8),
            (uint8_t)(value >> 16), (uint8_t)(value >> 24)
        };
        if (a64_static_decode_read_hits_bytes_at(bytes, 1u, false, 0u,
                                                 &block)) {
            fprintf(stderr,
                    "jitbench: product decoder accepted invalid read %u\n",
                    i);
            return false;
        }
    }

    for (i = 0u; i < sizeof INVALID_PRODUCT_THUMB /
                         sizeof INVALID_PRODUCT_THUMB[0]; i++) {
        uint16_t value = INVALID_PRODUCT_THUMB[i];
        uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
        if (a64_static_decode_read_hits_bytes_at(bytes, 1u, true, 0x200u,
                                                 &block)) {
            fprintf(stderr,
                    "jitbench: product decoder accepted invalid Thumb %u\n",
                    i);
            return false;
        }
    }

    for (i = 0u; i < sizeof INVALID_PRODUCT_VFP /
                         sizeof INVALID_PRODUCT_VFP[0]; i++) {
        uint32_t value = INVALID_PRODUCT_VFP[i];
        uint8_t bytes[4] = {
            (uint8_t)value, (uint8_t)(value >> 8),
            (uint8_t)(value >> 16), (uint8_t)(value >> 24)
        };
        if (a64_static_decode_read_hits_bytes_at(bytes, 1u, false, 0xe800u,
                                                 &block)) {
            fprintf(stderr,
                    "jitbench: product decoder accepted invalid VFP %u\n",
                    i);
            return false;
        }
    }

    if (a64_static_decode_at(A32_SHORT_FALL, 0u, false, 0u, &block) ||
        a64_static_decode_at(A32_SHORT_FALL, A64_STATIC_MAX_INSNS + 1u,
                             false, 0u, &block) ||
        a64_static_decode_at(A32_SHORT_FALL, 1u, false, 2u, &block) ||
        a64_static_decode_at(MID_BLOCK_BRANCH, 2u, false, 0u, &block) ||
        a64_static_decode_at(MID_BLOCK_CONDITIONAL_BRANCH, 2u, false, 0u,
                             &block) ||
        a64_static_decode_at(MID_BLOCK_LINK, 2u, false, 0u, &block) ||
        a64_static_decode_at(MID_BLOCK_INDIRECT, 2u, false, 0u, &block) ||
        a64_static_decode_at(MID_BLOCK_THUMB_INDIRECT, 2u, true, 0u,
                             &block) ||
        a64_static_decode_at(BLX_IMMEDIATE, 1u, false, 0u, &block) ||
        a64_static_decode_at(BLX_PC, 1u, false, 0u, &block) ||
        a64_static_decode_at(THUMB_BLX_PC, 1u, true, 0u, &block) ||
        a64_static_decode_at(PC_WRITE, 1u, false, 0u, &block) ||
        a64_static_decode_at(MULTIPLY, 1u, false, 0u, &block) ||
        !a64_static_decode_at(CONDITIONAL_DP, 1u, false, 0x3000u,
                             &block) ||
        block.insn_count != 1u || block.uop_count != 3u ||
        block.exit_pc != 0x3004u || block.touches_memory ||
        !a64_static_decode_bytes_at(&UNALIGNED_A32[1], 1u, false, 0x1000u,
                                    &block) ||
        block.start_pc != 0x1000u || block.exit_pc != 0x1004u ||
        block.insn_count != 1u || block.touches_memory) {
        fprintf(stderr, "jitbench: static decoder accepted an invalid shape\n");
        return false;
    }
    for (i = 0u; i < sizeof PRODUCT_ENTRY_LENGTHS /
                         sizeof PRODUCT_ENTRY_LENGTHS[0]; i++) {
        uint32_t program[A64_STATIC_MAX_INSNS];
        unsigned length = PRODUCT_ENTRY_LENGTHS[i];
        if (!prepare_product_entry(length, program, &block) ||
            block.uop_count != length) {
            fprintf(stderr,
                    "jitbench: product-entry shape failed at length %u\n",
                    length);
            return false;
        }
        printf("PRODUCT-ENTRY-SHAPE exact=yes length=%u uops=%u\n",
               length, block.uop_count);
    }
    return true;
}

static unsigned oracle_dread_slot(uint32_t va, bool priv) {
    return (unsigned)(((va >> 10) + (priv ? ARM_DREAD_ENTRIES / 2u : 0u)) &
                      (ARM_DREAD_ENTRIES - 1u));
}

static void oracle_warm_dread_as(arm_cpu_t *cpu, uint32_t va, bool priv) {
    const uint32_t block = va & ~UINT32_C(0x3ff);
    const unsigned slot = oracle_dread_slot(va, priv);
    cpu->dread[slot].host = &g_ram[block];
    cpu->dread[slot].tag = block | (priv ? 1u : 0u);
    cpu->dread[slot].gen = cpu->tlb_gen;
}

static void oracle_warm_dread(arm_cpu_t *cpu, uint32_t va) {
    const bool priv = (cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    oracle_warm_dread_as(cpu, va, priv);
}

static void oracle_warm_dwrite(arm_cpu_t *cpu, uint32_t va, bool priv) {
    const uint32_t block = va & ~UINT32_C(0x3ff);
    const unsigned slot = oracle_dread_slot(va, priv);
    cpu->dwrite[slot].host = &g_ram[block];
    cpu->dwrite[slot].tag = block | (priv ? 1u : 0u);
    cpu->dwrite[slot].gen = cpu->tlb_gen;
}

static void seed_read_oracle(arm_cpu_t *cpu, const uint32_t *program,
                             unsigned insns, uint32_t pc, bool warm) {
    seed_cpu_at(cpu, program, insns, false, pc);
    cpu->r[0] = DATA_BASE;
    cpu->r[1] = 4u;
    cpu->r[2] = DATA_BASE + 0x20u;
    mem_w8(NULL, DATA_BASE + 0x1fu, 0xabu);
    mem_w32(NULL, DATA_BASE + 0x10u, UINT32_C(0x10203040));
    mem_w32(NULL, DATA_BASE + 0x20u, UINT32_C(0x11223344));
    mem_w32(NULL, DATA_BASE + 0x24u, UINT32_C(0x55667788));
    mem_w32(NULL, DATA_BASE + 0x28u, UINT32_C(0x99aabbcc));
    mem_w32(NULL, DATA_BASE + 0x2cu, UINT32_C(0xddeeff00));
    mem_w32(NULL, DATA_BASE + 0x30u, UINT32_C(0x13579bdf));
    if (pc == 0xb000u)
        mem_w32(NULL, 0xb100u, UINT32_C(0xcafef00d));
    if (warm) {
        oracle_warm_dread(cpu, DATA_BASE);
        if (pc == 0xb000u) oracle_warm_dread(cpu, 0xb100u);
    }
}

static void seed_thumb_read_oracle(arm_cpu_t *cpu, bool warm) {
    seed_cpu_at(cpu, THUMB_READ_HITS,
                (unsigned)(sizeof THUMB_READ_HITS /
                           sizeof THUMB_READ_HITS[0]),
                true, 0xe200u);
    cpu->r[0] = DATA_BASE;
    cpu->r[1] = 4u;
    cpu->r[2] = DATA_BASE + 0x20u;
    cpu->r[13] = STACK_BASE;
    mem_w32(NULL, DATA_BASE + 4u, UINT32_C(0x1234ff80));
    mem_w8(NULL, DATA_BASE + 0x22u, 0x5au);
    mem_w32(NULL, DATA_BASE + 0x24u, UINT32_C(0x11223344));
    mem_w16(NULL, DATA_BASE + 0x26u, UINT16_C(0x7abc));
    mem_w32(NULL, STACK_BASE + 8u, UINT32_C(0xdeadbeef));
    mem_w32(NULL, 0xe304u, UINT32_C(0xcafef00d));
    if (warm) {
        oracle_warm_dread(cpu, DATA_BASE);
        oracle_warm_dread(cpu, STACK_BASE);
        oracle_warm_dread(cpu, 0xe304u);
    }
}

static bool validate_static_read_oracles(void) {
    a64_static_block_t block;
    arm_cpu_t reference, statik, before;
    final_state_t reference_state, static_state;
    arm_status_t status = ARM_OK;
    unsigned completed = 0u;

    seed_read_oracle(&reference, A32_READ_HITS,
                     (unsigned)(sizeof A32_READ_HITS /
                                sizeof A32_READ_HITS[0]),
                     0xb000u, true);
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(
            &g_ram[0xb000u],
            (unsigned)(sizeof A32_READ_HITS / sizeof A32_READ_HITS[0]),
            false, 0xb000u, &block))
        return false;
    for (unsigned i = 0u; i < block.insn_count; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    if (!a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed)) {
        fprintf(stderr, "jitbench: product read-hit execution refused\n");
        return false;
    }
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);
    capture_state(&static_state, &statik, ARM_OK, JIT_EXIT_NEXT);
    if (status != ARM_OK || completed != block.insn_count ||
        !architectural_states_equal(&reference_state, &static_state) ||
        reference.dread_hits != statik.dread_hits ||
        reference.dread_misses != statik.dread_misses ||
        statik.dread_hits != 8u || statik.dread_misses != 0u) {
        fprintf(stderr, "jitbench: product read-hit oracle mismatch\n");
        return false;
    }

    seed_read_oracle(&statik, A32_READ_ZERO_PREFIX, 1u, 0xd000u, false);
    before = statik;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xd000u], 1u, false,
                                              0xd000u, &block) ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) ||
        completed != 0u || memcmp(statik.r, before.r, sizeof statik.r) != 0 ||
        statik.cpsr != before.cpsr || statik.cycles != before.cycles ||
        statik.dread_hits != before.dread_hits ||
        statik.dread_misses != before.dread_misses) {
        fprintf(stderr, "jitbench: zero-prefix read miss changed state\n");
        return false;
    }

    seed_read_oracle(&reference, A32_READ_PARTIAL, 3u, 0xc000u, false);
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xc000u], 3u, false,
                                              0xc000u, &block) ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) ||
        completed != 1u || arm_step(&reference) != ARM_OK ||
        memcmp(statik.r, reference.r, sizeof statik.r) != 0 ||
        statik.cpsr != reference.cpsr || statik.cycles != reference.cycles ||
        statik.dread_hits != 0u || statik.dread_misses != 0u) {
        fprintf(stderr, "jitbench: partial-prefix read miss mismatch\n");
        return false;
    }
    if (arm_step(&statik) != ARM_OK || arm_step(&reference) != ARM_OK ||
        memcmp(statik.r, reference.r, sizeof statik.r) != 0 ||
        statik.cpsr != reference.cpsr || statik.cycles != reference.cycles ||
        statik.dread_hits != reference.dread_hits ||
        statik.dread_misses != reference.dread_misses ||
        statik.dread_misses != 1u) {
        fprintf(stderr, "jitbench: literal fallback after partial miss differs\n");
        return false;
    }

    seed_thumb_read_oracle(&reference, true);
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(
            &g_ram[0xe200u],
            (unsigned)(sizeof THUMB_READ_HITS /
                       sizeof THUMB_READ_HITS[0]),
            true, 0xe200u, &block))
        return false;
    for (unsigned i = 0u; i < block.insn_count; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    if (!a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed)) {
        fprintf(stderr, "jitbench: product Thumb read-hit execution refused\n");
        return false;
    }
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);
    capture_state(&static_state, &statik, ARM_OK, JIT_EXIT_NEXT);
    if (status != ARM_OK || completed != block.insn_count ||
        !architectural_states_equal(&reference_state, &static_state) ||
        reference.dread_hits != statik.dread_hits ||
        reference.dread_misses != statik.dread_misses ||
        statik.dread_hits != 10u || statik.dread_misses != 0u) {
        fprintf(stderr, "jitbench: product Thumb read-hit oracle mismatch\n");
        return false;
    }

    seed_cpu_at(&statik, THUMB_READ_ZERO_PREFIX, 1u, true, 0xd200u);
    statik.r[0] = DATA_BASE;
    before = statik;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xd200u], 1u, true,
                                              0xd200u, &block) ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) ||
        completed != 0u || memcmp(statik.r, before.r, sizeof statik.r) != 0 ||
        statik.cpsr != before.cpsr || statik.cycles != before.cycles ||
        statik.dread_hits != before.dread_hits ||
        statik.dread_misses != before.dread_misses) {
        fprintf(stderr, "jitbench: zero-prefix Thumb read miss changed state\n");
        return false;
    }

    printf("STATIC-READ-ORACLE exact=yes hits=18 thumb=yes zero-prefix=yes "
           "partial-prefix=yes\n");
    return true;
}

static bool validate_static_store_oracles(void) {
    typedef struct {
        const char *name;
        uint32_t insn;
        bool thumb;
        unsigned rn;
        uint32_t base;
        uint32_t va;
        bool access_priv;
        bool executes;
    } store_case_t;
    const uint32_t base = DATA_BASE + UINT32_C(0x400);
    const uint32_t stack = base + UINT32_C(0x100);
    const store_case_t cases[] = {
        {"a32-imm", A32_SINGLE_MODE2(14,0,1,1,0,0,0,4,3,12),
         false, 4u, base, base + 12u, true, true},
        {"a32-byte-pre-wb", A32_SINGLE_MODE2(1,0,1,0,1,1,0,4,3,1),
         false, 4u, base, base - 1u, true, true},
        {"a32-reg-post", A32_SINGLE_MODE2(14,1,0,1,0,0,0,4,3,0x102),
         false, 4u, base, base, true, true},
        {"a32-strt", A32_SINGLE_MODE2(14,0,0,1,0,1,0,4,3,4),
         false, 4u, base, base, false, true},
        {"a32-pc-source", A32_SINGLE_MODE2(14,0,1,1,0,0,0,4,15,16),
         false, 4u, base, base + 16u, true, true},
        {"a32-failed-condition", A32_SINGLE_MODE2(0,0,1,1,0,0,0,4,3,20),
         false, 4u, base, base + 20u, true, false},
        {"a32-high-wb", A32_SINGLE_MODE2(14,0,1,1,0,1,0,12,14,8),
         false, 12u, base, base + 8u, true, true},
        {"a32-sp-wb", A32_SINGLE_MODE2(14,0,1,1,0,1,0,13,8,4),
         false, 13u, stack, stack + 4u, true, true},
        {"thumb-reg-word", UINT32_C(0x508b),
         true, 1u, base, base + 4u, true, true},
        {"thumb-reg-half", UINT32_C(0x528b),
         true, 1u, base, base + 4u, true, true},
        {"thumb-reg-byte", UINT32_C(0x548b),
         true, 1u, base, base + 4u, true, true},
        {"thumb-imm-word", UINT32_C(0x604b),
         true, 1u, base, base + 4u, true, true},
        {"thumb-imm-byte", UINT32_C(0x704b),
         true, 1u, base, base + 1u, true, true},
        {"thumb-imm-half", UINT32_C(0x804b),
         true, 1u, base, base + 2u, true, true},
        {"thumb-sp-word", UINT32_C(0x9301),
         true, 13u, stack, stack + 4u, true, true},
    };
    arm_bus_t write_bus = g_bus;
    uint8_t *baseline = (uint8_t *)malloc(sizeof g_ram);
    unsigned hit_cases = 0u;

    if (!baseline) {
        fprintf(stderr, "jitbench: store-oracle allocation failed\n");
        return false;
    }
    write_bus.host_ram_write = mem_host_ram;

    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        const store_case_t *sc = &cases[i];
        const uint32_t pc = UINT32_C(0x10000);
        arm_cpu_t reference, statik;
        final_state_t reference_state, static_state;
        a64_static_block_t block;
        arm_status_t status;
        unsigned completed = 0u;
        uint8_t bytes[4];

        if (sc->thumb) {
            uint16_t insn = (uint16_t)sc->insn;
            seed_cpu_at(&reference, &insn, 1u, true, pc);
            bytes[0] = (uint8_t)insn;
            bytes[1] = (uint8_t)(insn >> 8);
        } else {
            seed_cpu_at(&reference, &sc->insn, 1u, false, pc);
            bytes[0] = (uint8_t)sc->insn;
            bytes[1] = (uint8_t)(sc->insn >> 8);
            bytes[2] = (uint8_t)(sc->insn >> 16);
            bytes[3] = (uint8_t)(sc->insn >> 24);
        }
        reference.bus = &write_bus;
        reference.r[1] = base;
        reference.r[2] = 4u;
        reference.r[3] = UINT32_C(0x11223344);
        reference.r[8] = UINT32_C(0x89abcdef);
        reference.r[14] = UINT32_C(0x55667788);
        reference.r[sc->rn] = sc->base;
        oracle_warm_dwrite(&reference, sc->va, sc->access_priv);
        statik = reference;
        memcpy(baseline, g_ram, sizeof g_ram);

        if (!a64_static_decode_memory_hits_bytes_at(
                bytes, 1u, sc->thumb, pc, &block) ||
            !block.direct_writes ||
            a64_static_decode_read_hits_bytes_at(
                bytes, 1u, sc->thumb, pc, &block)) {
            fprintf(stderr, "jitbench: store oracle decode failed for %s\n",
                    sc->name);
            free(baseline);
            return false;
        }
        /* The read-only refusal zeroes its output, so decode the executable
         * memory contract again before running it. */
        if (!a64_static_decode_memory_hits_bytes_at(
                bytes, 1u, sc->thumb, pc, &block)) {
            free(baseline);
            return false;
        }

        status = arm_step(&reference);
        capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);
        memcpy(g_ram, baseline, sizeof g_ram);
        if (!a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                        &completed)) {
            fprintf(stderr, "jitbench: signed store refused for %s\n",
                    sc->name);
            free(baseline);
            return false;
        }
        capture_state(&static_state, &statik, ARM_OK, JIT_EXIT_NEXT);
        if (status != ARM_OK || completed != 1u ||
            !architectural_states_equal(&reference_state, &static_state) ||
            reference.dwrite_hits != statik.dwrite_hits ||
            reference.dwrite_misses != statik.dwrite_misses ||
            statik.dwrite_hits != (sc->executes ? 1u : 0u) ||
            statik.dwrite_misses != 0u) {
            fprintf(stderr, "jitbench: signed store mismatch for %s\n",
                    sc->name);
            free(baseline);
            return false;
        }
        if (sc->executes) hit_cases++;
    }

    /* A guarded miss after one preceding instruction must retire exactly that
     * prefix, without counting or performing the store. arm_step then owns the
     * single slow miss/fill and both CPUs converge exactly. */
    {
        const uint32_t program[] = {
            A32_DP_IMM(14, 4, 0, 0, 0, 0, 1),
            A32_SINGLE_MODE2(14, 0, 1, 1, 0, 0, 0, 4, 3, 0),
        };
        const uint32_t pc = UINT32_C(0x11000);
        arm_cpu_t reference, statik;
        a64_static_block_t block;
        unsigned completed = 0u;

        seed_cpu_at(&reference, program, 2u, false, pc);
        reference.bus = &write_bus;
        reference.r[3] = UINT32_C(0xa5a55a5a);
        reference.r[4] = base;
        statik = reference;
        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[pc], 2u, false, pc, &block) ||
            arm_step(&reference) != ARM_OK ||
            !a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                        &completed) ||
            completed != 1u ||
            memcmp(reference.r, statik.r, sizeof statik.r) != 0 ||
            reference.cpsr != statik.cpsr ||
            reference.cycles != statik.cycles ||
            statik.dwrite_hits != 0u || statik.dwrite_misses != 0u ||
            arm_step(&reference) != ARM_OK || arm_step(&statik) != ARM_OK ||
            memcmp(reference.r, statik.r, sizeof statik.r) != 0 ||
            reference.cpsr != statik.cpsr ||
            reference.cycles != statik.cycles ||
            reference.dwrite_hits != statik.dwrite_hits ||
            reference.dwrite_misses != statik.dwrite_misses ||
            statik.dwrite_hits != 0u || statik.dwrite_misses != 1u) {
            fprintf(stderr, "jitbench: partial-prefix store miss mismatch\n");
            free(baseline);
            return false;
        }
    }

    /* Alignment-dependent ARMv6 behavior is never guessed in signed text. */
    {
        const uint32_t insn =
            A32_SINGLE_MODE2(14, 0, 1, 1, 0, 0, 0, 4, 3, 0);
        const uint32_t pc = UINT32_C(0x12000);
        arm_cpu_t statik, before;
        a64_static_block_t block;
        unsigned completed = 99u;
        uint64_t ram_before;

        seed_cpu_at(&statik, &insn, 1u, false, pc);
        statik.bus = &write_bus;
        statik.r[3] = UINT32_C(0x12345678);
        statik.r[4] = base + 1u;
        oracle_warm_dwrite(&statik, statik.r[4], true);
        before = statik;
        ram_before = hash_ram();
        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block) ||
            !a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                        &completed) ||
            completed != 0u ||
            memcmp(statik.r, before.r, sizeof statik.r) != 0 ||
            statik.cpsr != before.cpsr || statik.cycles != before.cycles ||
            statik.dwrite_hits != before.dwrite_hits ||
            statik.dwrite_misses != before.dwrite_misses ||
            hash_ram() != ram_before) {
            fprintf(stderr, "jitbench: unaligned store refusal changed state\n");
            free(baseline);
            return false;
        }
    }

    free(baseline);
    printf("STATIC-STORE-ORACLE exact=yes cases=%zu hits=%u "
           "pc-source=yes writeback=yes unprivileged=yes thumb=yes "
           "zero-prefix=yes partial-prefix=yes\n",
           sizeof cases / sizeof cases[0], hit_cases);
    return true;
}


static bool validate_static_stm_oracles(void) {
    typedef struct {
        const char *name;
        uint32_t insn;
        uint32_t pc;
        unsigned rn;
        uint32_t base;
        uint32_t start;
        unsigned words;
        bool executes;
    } stm_case_t;
    const uint32_t ordinary = DATA_BASE + UINT32_C(0x800);
    const uint32_t exact_boundary = DATA_BASE + UINT32_C(0xfc0);
    const stm_case_t cases[] = {
        {"ia", A32_BLOCK(14,0,1,0,0,0, 4,UINT32_C(0x8109)),
         UINT32_C(0x12400), 4u, ordinary, ordinary, 4u, true},
        {"ib-writeback", A32_BLOCK(14,1,1,0,1,0,12,UINT32_C(0x4106)),
         UINT32_C(0x12500), 12u, ordinary, ordinary + 4u, 4u, true},
        {"da-sp-base", A32_BLOCK(14,0,0,0,0,0,13,UINT32_C(0x4210)),
         UINT32_C(0x12600), 13u, ordinary + 0x40u,
         ordinary + 0x38u, 3u, true},
        {"db-writeback", A32_BLOCK(14,1,0,0,1,0,11,UINT32_C(0x8481)),
         UINT32_C(0x12700), 11u, ordinary, ordinary - 16u, 4u, true},
        {"failed-condition", A32_BLOCK(0,0,1,0,0,0,4,UINT32_C(0x4004)),
         UINT32_C(0x12800), 4u, ordinary, ordinary, 2u, false},
        {"sixteen-word-boundary",
         A32_BLOCK(14,0,1,0,0,0,7,UINT32_C(0xffff)),
         UINT32_C(0x12900), 7u, exact_boundary, exact_boundary, 16u, true},
    };
    arm_bus_t write_bus = g_bus;
    uint8_t *baseline = (uint8_t *)malloc(sizeof g_ram);
    uint8_t *expected = (uint8_t *)malloc(sizeof g_ram);
    unsigned hit_words = 0u;

    if (!a64_static_host_available()) {
        printf("STATIC-STM-ORACLE SKIP: no signed AArch64 handlers\n");
        free(baseline);
        free(expected);
        return true;
    }
    if (!baseline || !expected) {
        fprintf(stderr, "jitbench: STM oracle allocation failed\n");
        free(baseline);
        free(expected);
        return false;
    }
    write_bus.host_ram_write = mem_host_ram;

    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        const stm_case_t *sc = &cases[i];
        arm_cpu_t reference, statik;
        final_state_t reference_state, static_state;
        a64_static_block_t block;
        unsigned completed = 99u;
        arm_status_t status;

        seed_cpu_at(&reference, &sc->insn, 1u, false, sc->pc);
        reference.bus = &write_bus;
        for (unsigned reg = 0u; reg < 15u; reg++)
            reference.r[reg] = UINT32_C(0x11000000) |
                               (reg * UINT32_C(0x010101));
        reference.r[sc->rn] = sc->base;
        if (sc->executes) oracle_warm_dwrite(&reference, sc->start, true);
        statik = reference;
        memcpy(baseline, g_ram, sizeof g_ram);

        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[sc->pc], 1u, false, sc->pc, &block) ||
            !block.stm_direct_writes) {
            fprintf(stderr, "jitbench: STM oracle decode failed for %s\n",
                    sc->name);
            free(baseline);
            free(expected);
            return false;
        }
        status = arm_step(&reference);
        capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);
        memcpy(expected, g_ram, sizeof g_ram);
        memcpy(g_ram, baseline, sizeof g_ram);
        if (!a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                        &completed)) {
            fprintf(stderr, "jitbench: signed STM refused for %s\n", sc->name);
            free(baseline);
            free(expected);
            return false;
        }
        capture_state(&static_state, &statik, ARM_OK, JIT_EXIT_NEXT);
        if (status != ARM_OK || completed != 1u ||
            !architectural_states_equal(&reference_state, &static_state) ||
            memcmp(expected, g_ram, sizeof g_ram) != 0 ||
            reference.dwrite_hits != statik.dwrite_hits ||
            reference.dwrite_misses != statik.dwrite_misses ||
            statik.dwrite_hits != (sc->executes ? sc->words : 0u) ||
            statik.dwrite_misses != 0u) {
            fprintf(stderr, "jitbench: signed STM mismatch for %s\n", sc->name);
            free(baseline);
            free(expected);
            return false;
        }
        if (sc->executes) hit_words += sc->words;
    }

    /* A two-block run is refused before its first word. Literal fallback then
     * owns the complete instruction and must converge with a pure reference,
     * including the first-block hits and second-block fill. */
    {
        const uint32_t insn = A32_BLOCK(
            14, 0, 1, 0, 0, 0, 4, UINT32_C(0x8109));
        const uint32_t pc = UINT32_C(0x12a00);
        const uint32_t start = DATA_BASE + UINT32_C(0x13f8);
        arm_cpu_t reference, statik, before;
        final_state_t reference_state, static_state;
        a64_static_block_t block;
        unsigned completed = 99u;
        uint64_t ram_before;

        seed_cpu_at(&reference, &insn, 1u, false, pc);
        reference.bus = &write_bus;
        reference.r[0] = UINT32_C(0x01020304);
        reference.r[3] = UINT32_C(0x11223344);
        reference.r[4] = start;
        reference.r[8] = UINT32_C(0x55667788);
        reference.r[15] = pc;
        oracle_warm_dwrite(&reference, start, true);
        statik = reference;
        before = statik;
        ram_before = hash_ram();
        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block) ||
            !a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                        &completed) || completed != 0u ||
            memcmp(statik.r, before.r, sizeof statik.r) != 0 ||
            statik.cpsr != before.cpsr || statik.cycles != before.cycles ||
            statik.dwrite_hits != before.dwrite_hits ||
            statik.dwrite_misses != before.dwrite_misses ||
            hash_ram() != ram_before) {
            fprintf(stderr, "jitbench: cross-block STM was not transactional\n");
            free(baseline);
            free(expected);
            return false;
        }
        if (arm_step(&reference) != ARM_OK || arm_step(&statik) != ARM_OK) {
            free(baseline);
            free(expected);
            return false;
        }
        capture_state(&reference_state, &reference, ARM_OK, JIT_EXIT_NEXT);
        capture_state(&static_state, &statik, ARM_OK, JIT_EXIT_NEXT);
        if (!architectural_states_equal(&reference_state, &static_state) ||
            reference.dwrite_hits != statik.dwrite_hits ||
            reference.dwrite_misses != statik.dwrite_misses) {
            fprintf(stderr, "jitbench: cross-block STM fallback diverged\n");
            free(baseline);
            free(expected);
            return false;
        }
    }

    /* Cold cache, unaligned addressing and revoked write consent all return a
     * zero prefix without changing registers, counters, flags, cycles or RAM. */
    for (unsigned refusal = 0u; refusal < 3u; refusal++) {
        const uint32_t insn = A32_BLOCK(
            14, 0, 1, 0, 1, 0, 12, UINT32_C(0x4106));
        const uint32_t pc = UINT32_C(0x12b00) + refusal * 0x100u;
        arm_cpu_t statik, before;
        a64_static_block_t block;
        unsigned completed = 99u;
        uint64_t ram_before;

        seed_cpu_at(&statik, &insn, 1u, false, pc);
        statik.bus = refusal == 2u ? &g_bus : &write_bus;
        statik.r[12] = ordinary + (refusal == 1u ? 1u : 0u);
        if (refusal != 0u)
            oracle_warm_dwrite(&statik, statik.r[12] + 4u, true);
        before = statik;
        ram_before = hash_ram();
        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block) ||
            !a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                        &completed) || completed != 0u ||
            memcmp(statik.r, before.r, sizeof statik.r) != 0 ||
            statik.cpsr != before.cpsr || statik.cycles != before.cycles ||
            statik.dwrite_hits != before.dwrite_hits ||
            statik.dwrite_misses != before.dwrite_misses ||
            hash_ram() != ram_before) {
            fprintf(stderr, "jitbench: STM refusal %u changed state\n",
                    refusal);
            free(baseline);
            free(expected);
            return false;
        }
    }

    free(baseline);
    free(expected);
    printf("STATIC-STM-ORACLE exact=yes cases=%zu hit-words=%u modes=4 "
           "writeback=yes pc-source=yes conditional=yes max-words=16 "
           "one-block=yes cross-block-rollback=yes alignment=yes "
           "cold-cache=yes consent=yes\n",
           sizeof cases / sizeof cases[0], hit_words);
    return true;
}


static bool validate_static_ldm_oracles(void) {
    typedef struct {
        const char *name;
        uint32_t insn;
        uint32_t pc;
        unsigned rn;
        uint32_t base;
        uint32_t start;
        unsigned words;
        bool executes;
    } ldm_case_t;
    const uint32_t ordinary = DATA_BASE + UINT32_C(0x800);
    const uint32_t exact_boundary = DATA_BASE + UINT32_C(0xfc4);
    const ldm_case_t cases[] = {
        {"ia-base-in-list", A32_BLOCK(
             14,0,1,0,0,1, 4,UINT32_C(0x4111)),
         UINT32_C(0x13400), 4u, ordinary, ordinary, 4u, true},
        {"ib-writeback", A32_BLOCK(
             14,1,1,0,1,1,12,UINT32_C(0x4106)),
         UINT32_C(0x13500), 12u, ordinary, ordinary + 4u, 4u, true},
        {"da-sp-base", A32_BLOCK(
             14,0,0,0,0,1,13,UINT32_C(0x4210)),
         UINT32_C(0x13600), 13u, ordinary + 0x40u,
         ordinary + 0x38u, 3u, true},
        {"db-writeback", A32_BLOCK(
             14,1,0,0,1,1,11,UINT32_C(0x4481)),
         UINT32_C(0x13700), 11u, ordinary, ordinary - 16u, 4u, true},
        {"failed-condition", A32_BLOCK(
              0,0,1,0,0,1, 4,UINT32_C(0x4004)),
         UINT32_C(0x13800), 4u, ordinary, ordinary, 2u, false},
        {"fifteen-word-boundary", A32_BLOCK(
             14,0,1,0,0,1, 7,UINT32_C(0x7fff)),
         UINT32_C(0x13900), 7u, exact_boundary, exact_boundary, 15u, true},
    };
    unsigned hit_words = 0u;

    if (!a64_static_host_available()) {
        printf("STATIC-LDM-ORACLE SKIP: no signed AArch64 handlers\n");
        return true;
    }

    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        const ldm_case_t *lc = &cases[i];
        arm_cpu_t reference, statik;
        final_state_t reference_state, static_state;
        a64_static_block_t block;
        unsigned completed = 99u;
        arm_status_t status;

        seed_cpu_at(&reference, &lc->insn, 1u, false, lc->pc);
        for (unsigned reg = 0u; reg < 15u; reg++)
            reference.r[reg] = UINT32_C(0xa1000000) |
                               (reg * UINT32_C(0x010101));
        reference.r[lc->rn] = lc->base;
        reference.r[15] = lc->pc;
        for (unsigned word = 0u; word < lc->words; word++)
            mem_w32(NULL, lc->start + word * 4u,
                    UINT32_C(0x51000000) |
                    (i << 16) | (word * UINT32_C(0x010101)));
        if (lc->executes) oracle_warm_dread(&reference, lc->start);
        statik = reference;

        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[lc->pc], 1u, false, lc->pc, &block) ||
            !block.ldm_direct_reads || block.direct_writes) {
            fprintf(stderr, "jitbench: LDM oracle decode failed for %s\n",
                    lc->name);
            return false;
        }
        status = arm_step(&reference);
        capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);
        if (!a64_static_run_read_hits(
                &statik, &block, g_ram, sizeof g_ram, &completed)) {
            fprintf(stderr, "jitbench: signed LDM refused for %s\n",
                    lc->name);
            return false;
        }
        capture_state(&static_state, &statik, ARM_OK, JIT_EXIT_NEXT);
        if (status != ARM_OK || completed != 1u ||
            !architectural_states_equal(&reference_state, &static_state) ||
            reference.dread_hits != statik.dread_hits ||
            reference.dread_misses != statik.dread_misses ||
            statik.dread_hits != (lc->executes ? lc->words : 0u) ||
            statik.dread_misses != 0u) {
            fprintf(stderr, "jitbench: signed LDM mismatch for %s\n",
                    lc->name);
            return false;
        }
        if (lc->executes) hit_words += lc->words;
    }

    /* A cross-block run refuses before its first destination register. The
     * literal fallback must then converge with a pure reference, including
     * DREAD fills and hit/miss accounting. */
    {
        const uint32_t insn = A32_BLOCK(
            14, 0, 1, 0, 0, 1, 4, UINT32_C(0x4109));
        const uint32_t pc = UINT32_C(0x13a00);
        const uint32_t start = DATA_BASE + UINT32_C(0x13f8);
        arm_cpu_t reference, statik, before;
        final_state_t reference_state, static_state;
        a64_static_block_t block;
        unsigned completed = 99u;

        seed_cpu_at(&reference, &insn, 1u, false, pc);
        reference.r[4] = start;
        for (unsigned word = 0u; word < 4u; word++)
            mem_w32(NULL, start + word * 4u,
                    UINT32_C(0x62000000) + word);
        oracle_warm_dread(&reference, start);
        statik = reference;
        before = statik;
        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block) ||
            !a64_static_run_read_hits(
                &statik, &block, g_ram, sizeof g_ram, &completed) ||
            completed != 0u || memcmp(&statik, &before, sizeof statik) != 0) {
            fprintf(stderr, "jitbench: cross-block LDM was not transactional\n");
            return false;
        }
        if (arm_step(&reference) != ARM_OK || arm_step(&statik) != ARM_OK) {
            fprintf(stderr, "jitbench: cross-block LDM fallback trapped\n");
            return false;
        }
        capture_state(&reference_state, &reference, ARM_OK, JIT_EXIT_NEXT);
        capture_state(&static_state, &statik, ARM_OK, JIT_EXIT_NEXT);
        if (!architectural_states_equal(&reference_state, &static_state) ||
            reference.dread_hits != statik.dread_hits ||
            reference.dread_misses != statik.dread_misses) {
            fprintf(stderr, "jitbench: cross-block LDM fallback diverged\n");
            return false;
        }
    }

    /* Cold, unaligned and stale-generation preflights all return a zero
     * prefix without changing registers, cache state, counters or cycles. */
    for (unsigned refusal = 0u; refusal < 3u; refusal++) {
        const uint32_t insn = A32_BLOCK(
            14, 0, 1, 0, 1, 1, 12, UINT32_C(0x4106));
        const uint32_t pc = UINT32_C(0x13b00) + refusal * 0x100u;
        arm_cpu_t statik, before;
        a64_static_block_t block;
        unsigned completed = 99u;

        seed_cpu_at(&statik, &insn, 1u, false, pc);
        statik.r[12] = ordinary + (refusal == 1u ? 1u : 0u);
        if (refusal != 0u)
            oracle_warm_dread(&statik, statik.r[12]);
        if (refusal == 2u) statik.tlb_gen++;
        before = statik;
        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block) ||
            !a64_static_run_read_hits(
                &statik, &block, g_ram, sizeof g_ram, &completed) ||
            completed != 0u || memcmp(&statik, &before, sizeof statik) != 0) {
            fprintf(stderr, "jitbench: LDM refusal %u changed state\n",
                    refusal);
            return false;
        }
    }

    /* Metadata flags are part of the validator contract, not advisory. */
    {
        const uint32_t insn = A32_BLOCK(
            14, 0, 1, 0, 0, 1, 4, UINT32_C(0x4109));
        const uint32_t pc = UINT32_C(0x13e00);
        arm_cpu_t statik, before;
        a64_static_block_t block;
        unsigned completed = UINT_MAX;

        seed_cpu_at(&statik, &insn, 1u, false, pc);
        statik.r[4] = ordinary;
        oracle_warm_dread(&statik, ordinary);
        before = statik;
        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block)) {
            fprintf(stderr, "jitbench: LDM mutation setup failed\n");
            return false;
        }
        block.ldm_direct_reads = false;
        if (a64_static_run_read_hits(
                &statik, &block, g_ram, sizeof g_ram, &completed) ||
            memcmp(&statik, &before, sizeof statik) != 0 ||
            completed != UINT_MAX) {
            fprintf(stderr,
                    "jitbench: mutated LDM contract did not fail closed\n");
            return false;
        }
    }

    printf("STATIC-LDM-ORACLE exact=yes cases=%zu hit-words=%u modes=4 "
           "writeback=yes base-in-list=yes conditional=yes max-words=15 "
           "one-block=yes cross-block-rollback=yes alignment=yes "
           "cold-cache=yes stale-cache=yes contract=yes nonterminal=yes\n",
           sizeof cases / sizeof cases[0], hit_words);
    return true;
}


static void seed_vfp_oracle(arm_cpu_t *cpu, const uint32_t *program,
                            unsigned insns, uint32_t pc, bool enabled) {
    seed_cpu_at(cpu, program, insns, false, pc);
    cpu->cp15.cpacr |= 0xfu << ARM_CPACR_CP10_SHIFT;
    cpu->vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u;
    cpu->vfp_fpscr = 0u;
    for (unsigned i = 0u; i < 32u; i++)
        cpu->vfp_s[i] = UINT32_C(0x80000000) ^
                        (UINT32_C(0x01020304) * (i + 1u));
    cpu->vfp_s[12] = UINT32_C(0xff800001); /* signalling NaN payload */
}

static bool static_vfp_arch_states_equal(const arm_cpu_t *a,
                                         const arm_cpu_t *b) {
    return memcmp(a->r, b->r, sizeof a->r) == 0 &&
           a->cpsr == b->cpsr && a->cycles == b->cycles &&
           a->vfp_fpexc == b->vfp_fpexc &&
           a->vfp_fpscr == b->vfp_fpscr &&
           memcmp(a->vfp_s, b->vfp_s, sizeof a->vfp_s) == 0;
}

static bool static_vfp_states_equal(const arm_cpu_t *a,
                                    const arm_cpu_t *b) {
    return static_vfp_arch_states_equal(a, b) &&
           a->dread_hits == b->dread_hits &&
           a->dread_misses == b->dread_misses &&
           a->dwrite_hits == b->dwrite_hits &&
           a->dwrite_misses == b->dwrite_misses;
}

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
static uint64_t static_host_fpcr_read(void) {
    uint64_t value;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(value) :: "memory");
    return value;
}

static uint64_t static_host_fpsr_read(void) {
    uint64_t value;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(value) :: "memory");
    return value;
}

static void static_host_fpcr_write(uint64_t value) {
    __asm__ __volatile__("msr fpcr, %0" :: "r"(value) : "memory");
}

static void static_host_fpsr_write(uint64_t value) {
    __asm__ __volatile__("msr fpsr, %0" :: "r"(value) : "memory");
}
#endif

#define VFP_ARITH_CONTROL \
    (ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_IXC | \
     ARM_FPSCR_N | ARM_FPSCR_C)

typedef struct {
    const char *name;
    uint32_t insn;
    uint64_t d;
    uint64_t n;
    uint64_t m;
    uint32_t fpscr;
    bool access;
    bool enabled;
} static_vfp_arith_fallback_t;

static void static_vfp_arith_indices(uint32_t insn, bool *dbl,
                                     unsigned *rd, unsigned *rn,
                                     unsigned *rm) {
    *dbl = (insn & (1u << 8)) != 0u;
    if (*dbl) {
        *rd = (insn >> 12) & 15u;
        *rn = (insn >> 16) & 15u;
        *rm = insn & 15u;
    } else {
        *rd = ((insn >> 12) & 15u) * 2u + ((insn >> 22) & 1u);
        *rn = ((insn >> 16) & 15u) * 2u + ((insn >> 7) & 1u);
        *rm = (insn & 15u) * 2u + ((insn >> 5) & 1u);
    }
}

static void static_vfp_arith_set_operands(arm_cpu_t *cpu, uint32_t insn,
                                          uint64_t d, uint64_t n,
                                          uint64_t m) {
    bool dbl;
    unsigned rd, rn, rm;
    static_vfp_arith_indices(insn, &dbl, &rd, &rn, &rm);
    if (dbl) {
        vfp_set_d(cpu, rd, d);
        vfp_set_d(cpu, rn, n);
        vfp_set_d(cpu, rm, m);
    } else {
        vfp_set_s(cpu, rd, (uint32_t)d);
        vfp_set_s(cpu, rn, (uint32_t)n);
        vfp_set_s(cpu, rm, (uint32_t)m);
    }
}

static bool static_vfp_arith_decode_one(const uint32_t *insn, uint32_t pc,
                                        arm_cpu_t *cpu,
                                        a64_static_block_t *block) {
    seed_vfp_oracle(cpu, insn, 1u, pc, true);
    return a64_static_decode_read_hits_bytes_at(
        &g_ram[pc], 1u, false, pc, block);
}

static const static_vfp_arith_fallback_t VFP_ARITH_FALLBACKS[] = {
        {"missing-ixc", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x40400000),
         UINT32_C(0x40a00000), ARM_FPSCR_FZ | ARM_FPSCR_DN, true, true},
        {"missing-fz", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x40400000),
         UINT32_C(0x40a00000), ARM_FPSCR_DN | ARM_FPSCR_IXC, true, true},
        {"missing-dn", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x40400000),
         UINT32_C(0x40a00000), ARM_FPSCR_FZ | ARM_FPSCR_IXC, true, true},
        {"directed-rounding", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x40400000),
         UINT32_C(0x40a00000), VFP_ARITH_CONTROL | (1u << 22), true, true},
        {"vector-length", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x40400000),
         UINT32_C(0x40a00000), VFP_ARITH_CONTROL | ARM_FPSCR_LEN, true, true},
        {"exception-enable", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x40400000),
         UINT32_C(0x40a00000), VFP_ARITH_CONTROL | ARM_FPSCR_IOE, true, true},
        {"other-sticky", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x40400000),
         UINT32_C(0x40a00000), VFP_ARITH_CONTROL | ARM_FPSCR_IOC, true, true},
        {"cpacr", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x40400000),
         UINT32_C(0x40a00000), VFP_ARITH_CONTROL, false, true},
        {"fpexc", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x40400000),
         UINT32_C(0x40a00000), VFP_ARITH_CONTROL, true, false},
        {"s-subnormal", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x00000001),
         UINT32_C(0x3f800000), VFP_ARITH_CONTROL, true, true},
        {"s-infinity", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x7f800000),
         UINT32_C(0x3f800000), VFP_ARITH_CONTROL, true, true},
        {"s-qnan", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x7fc00001),
         UINT32_C(0x3f800000), VFP_ARITH_CONTROL, true, true},
        {"s-snan", VFP_ARITH_S(3,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x7f800001),
         UINT32_C(0x3f800000), VFP_ARITH_CONTROL, true, true},
        {"s-overflow", VFP_ARITH_S(2,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x7f7fffff),
         UINT32_C(0x40000000), VFP_ARITH_CONTROL, true, true},
        {"s-underflow", VFP_ARITH_S(2,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x00800000),
         UINT32_C(0x3e800000), VFP_ARITH_CONTROL, true, true},
        {"s-div-zero", VFP_ARITH_S(4,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x3f800000),
         UINT32_C(0x00000000), VFP_ARITH_CONTROL, true, true},
        {"s-mla-intermediate-underflow", VFP_ARITH_S(0,0,2,0,1),
         UINT32_C(0x3f800000), UINT32_C(0x00800000),
         UINT32_C(0x3f000000), VFP_ARITH_CONTROL, true, true},
        {"d-subnormal", VFP_ARITH_D(3,0,2,0,1),
         UINT64_C(0x3ff0000000000000), UINT64_C(0x0000000000000001),
         UINT64_C(0x3ff0000000000000), VFP_ARITH_CONTROL, true, true},
        {"d-infinity", VFP_ARITH_D(3,0,2,0,1),
         UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff0000000000000),
         UINT64_C(0x3ff0000000000000), VFP_ARITH_CONTROL, true, true},
        {"d-qnan", VFP_ARITH_D(3,0,2,0,1),
         UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000001),
         UINT64_C(0x3ff0000000000000), VFP_ARITH_CONTROL, true, true},
        {"d-snan", VFP_ARITH_D(3,0,2,0,1),
         UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff0000000000001),
         UINT64_C(0x3ff0000000000000), VFP_ARITH_CONTROL, true, true},
        {"d-overflow", VFP_ARITH_D(2,0,2,0,1),
         UINT64_C(0x3ff0000000000000), UINT64_C(0x7fefffffffffffff),
         UINT64_C(0x4000000000000000), VFP_ARITH_CONTROL, true, true},
        {"d-underflow", VFP_ARITH_D(2,0,2,0,1),
         UINT64_C(0x3ff0000000000000), UINT64_C(0x0010000000000000),
         UINT64_C(0x3fd0000000000000), VFP_ARITH_CONTROL, true, true},
        {"d-div-zero", VFP_ARITH_D(4,0,2,0,1),
         UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000),
         UINT64_C(0x0000000000000000), VFP_ARITH_CONTROL, true, true},
        {"d-mla-intermediate-underflow", VFP_ARITH_D(0,0,2,0,1),
         UINT64_C(0x3ff0000000000000), UINT64_C(0x0010000000000000),
         UINT64_C(0x3fe0000000000000), VFP_ARITH_CONTROL, true, true},
};

static bool validate_static_vfp_arithmetic_oracles(void) {
    a64_static_block_t block;
    arm_cpu_t reference, statik, before;
    unsigned completed = 0u;
    unsigned exact_cases = 0u;

    if (!a64_static_host_available()) {
        printf("STATIC-VFP-ARITH-ORACLE SKIP: no signed AArch64 handlers\n");
        return true;
    }

    /* Every operation/width gets its own interpreter equality proof. The
     * division cases are inexact; IXC is already sticky, exactly as in the
     * measured firmware window, so that new host flag is unobservable. */
    for (unsigned width = 0u; width < 2u; width++) {
        const uint32_t *program = width ? VFP_ARITH64_OPS : VFP_ARITH32_OPS;
        for (unsigned operation = 0u; operation < 9u; operation++) {
            uint32_t pc = UINT32_C(0x14000) +
                          (width * 9u + operation) * 4u;
            uint64_t d = width ? UINT64_C(0x4059000000000000)
                               : UINT32_C(0x42c80000); /* 100 */
            uint64_t n = width ? UINT64_C(0x4008000000000000)
                               : UINT32_C(0x40400000); /* 3 */
            uint64_t m = width ? UINT64_C(0x4014000000000000)
                               : UINT32_C(0x40a00000); /* 5 */
            if (!static_vfp_arith_decode_one(
                    &program[operation], pc, &reference, &block))
                return false;
            reference.vfp_fpscr = VFP_ARITH_CONTROL;
            static_vfp_arith_set_operands(
                &reference, program[operation], d, n, m);
            statik = reference;
            if (arm_step(&reference) != ARM_OK ||
                !a64_static_run_read_hits(
                    &statik, &block, g_ram, sizeof g_ram, &completed) ||
                completed != 1u ||
                !static_vfp_states_equal(&reference, &statik)) {
                fprintf(stderr,
                        "jitbench: VFP arithmetic oracle mismatch "
                        "width=%u operation=%u\n", width, operation);
                return false;
            }
            exact_cases++;
        }
    }

    /* Signed zero is inside the admitted class and must retain its sign. */
    for (unsigned width = 0u; width < 2u; width++) {
        uint32_t insn = width ? VFP_ARITH_D(2,0,2,0,1)
                              : VFP_ARITH_S(2,0,2,0,1);
        uint32_t pc = UINT32_C(0x14100) + width * 4u;
        uint64_t negative_zero = width ? UINT64_C(0x8000000000000000)
                                       : UINT32_C(0x80000000);
        uint64_t two = width ? UINT64_C(0x4000000000000000)
                             : UINT32_C(0x40000000);
        if (!static_vfp_arith_decode_one(&insn, pc, &reference, &block))
            return false;
        reference.vfp_fpscr = VFP_ARITH_CONTROL;
        static_vfp_arith_set_operands(
            &reference, insn, two, negative_zero, two);
        statik = reference;
        if (arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(
                &statik, &block, g_ram, sizeof g_ram, &completed) ||
            completed != 1u ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr,
                    "jitbench: VFP signed-zero arithmetic mismatch width=%u\n",
                    width);
            return false;
        }
        exact_cases++;
    }

    for (unsigned i = 0u;
         i < sizeof VFP_ARITH_FALLBACKS / sizeof VFP_ARITH_FALLBACKS[0];
         i++) {
        const static_vfp_arith_fallback_t *test = &VFP_ARITH_FALLBACKS[i];
        uint32_t pc = UINT32_C(0x14200) + i * 4u;
        if (!static_vfp_arith_decode_one(
                &test->insn, pc, &statik, &block))
            return false;
        statik.vfp_fpscr = test->fpscr;
        statik.vfp_fpexc = test->enabled ? ARM_FPEXC_EN : 0u;
        if (!test->access)
            statik.cp15.cpacr &= ~(UINT32_C(0xf) <<
                                   ARM_CPACR_CP10_SHIFT);
        static_vfp_arith_set_operands(
            &statik, test->insn, test->d, test->n, test->m);
        before = statik;
        completed = UINT_MAX;
        if (!a64_static_run_read_hits(
                &statik, &block, g_ram, sizeof g_ram, &completed) ||
            completed != 0u || !static_vfp_states_equal(&before, &statik)) {
            fprintf(stderr,
                    "jitbench: VFP arithmetic fallback changed state for %s\n",
                    test->name);
            return false;
        }
    }

    /* The ARM condition guard owns the skip before every VFP live-state gate. */
    {
        uint32_t insn = VFP_ARITH_S(3,0,2,0,1) & UINT32_C(0x0fffffff);
        if (!static_vfp_arith_decode_one(
                &insn, UINT32_C(0x14300), &reference, &block))
            return false;
        reference.cp15.cpacr = 0u;
        reference.vfp_fpexc = 0u;
        reference.vfp_fpscr = 0u;
        statik = reference;
        if (arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(
                &statik, &block, g_ram, sizeof g_ram, &completed) ||
            completed != 1u ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr, "jitbench: conditional VFP arithmetic skip mismatch\n");
            return false;
        }
    }

    /* One arithmetic result and one VMSR retire; the following arithmetic
     * sees the newly-invalid live FPSCR and returns the exact two-insn prefix. */
    {
        static const uint32_t partial[] = {
            VFP_ARITH_S(3,0,2,0,1), VFP_VMSR(1, 0),
            VFP_ARITH_S(2,0,3,1,2),
        };
        seed_vfp_oracle(&reference, partial, 3u, UINT32_C(0x14400), true);
        reference.vfp_fpscr = VFP_ARITH_CONTROL;
        reference.r[0] = ARM_FPSCR_FZ | ARM_FPSCR_DN;
        reference.vfp_s[0] = UINT32_C(0x3f800000);
        reference.vfp_s[1] = UINT32_C(0x40000000);
        statik = reference;
        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[0x14400], 3u, false, UINT32_C(0x14400), &block) ||
            arm_step(&reference) != ARM_OK || arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(
                &statik, &block, g_ram, sizeof g_ram, &completed) ||
            completed != 2u ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr, "jitbench: VFP arithmetic partial-prefix mismatch\n");
            return false;
        }
    }

    /* The feature flag is part of the validated data contract. */
    {
        uint32_t insn = VFP_ARITH_S(3,0,2,0,1);
        if (!static_vfp_arith_decode_one(
                &insn, UINT32_C(0x14500), &statik, &block))
            return false;
        statik.vfp_fpscr = VFP_ARITH_CONTROL;
        static_vfp_arith_set_operands(
            &statik, insn, UINT32_C(0x3f800000),
            UINT32_C(0x40400000), UINT32_C(0x40a00000));
        before = statik;
        block.vfp_arithmetic = false;
        completed = UINT_MAX;
        if (a64_static_run_read_hits(
                &statik, &block, g_ram, sizeof g_ram, &completed) ||
            completed != UINT_MAX ||
            !static_vfp_states_equal(&before, &statik)) {
            fprintf(stderr,
                    "jitbench: mutated VFP arithmetic contract ran\n");
            return false;
        }
    }

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    /* Force a non-RN caller mode plus pre-existing FPSR flags. Success must
     * still equal the RN interpreter, and both success and a post-FMUL
     * overflow rejection must restore every caller-visible host bit. */
    for (unsigned rejection = 0u; rejection < 2u; rejection++) {
        uint32_t insn = rejection ? VFP_ARITH_S(2,0,2,0,1)
                                  : VFP_ARITH_S(4,0,2,0,1);
        uint32_t pc = UINT32_C(0x14600) + rejection * 4u;
        uint64_t original_fpcr = static_host_fpcr_read();
        uint64_t original_fpsr = static_host_fpsr_read();
        uint64_t installed_fpcr;
        uint64_t installed_fpsr;
        uint64_t after_fpcr;
        uint64_t after_fpsr;
        bool run_ok;

        if (!static_vfp_arith_decode_one(&insn, pc, &statik, &block))
            return false;
        statik.vfp_fpscr = VFP_ARITH_CONTROL;
        static_vfp_arith_set_operands(
            &statik, insn, UINT32_C(0x3f800000),
            rejection ? UINT32_C(0x7f7fffff) : UINT32_C(0x3f800000),
            rejection ? UINT32_C(0x40000000) : UINT32_C(0x40400000));
        if (!rejection) {
            reference = statik;
            if (arm_step(&reference) != ARM_OK) return false;
        } else {
            before = statik;
        }

        static_host_fpcr_write(
            (original_fpcr & ~(UINT64_C(3) << 22)) |
            (UINT64_C(1) << 22));
        static_host_fpsr_write(UINT64_C(0x08000015));
        installed_fpcr = static_host_fpcr_read();
        installed_fpsr = static_host_fpsr_read();
        completed = UINT_MAX;
        run_ok = a64_static_run_read_hits(
            &statik, &block, g_ram, sizeof g_ram, &completed);
        after_fpcr = static_host_fpcr_read();
        after_fpsr = static_host_fpsr_read();
        static_host_fpsr_write(original_fpsr);
        static_host_fpcr_write(original_fpcr);

        if (!run_ok || after_fpcr != installed_fpcr ||
            after_fpsr != installed_fpsr ||
            (rejection
                 ? completed != 0u ||
                       !static_vfp_states_equal(&before, &statik)
                 : completed != 1u ||
                       !static_vfp_states_equal(&reference, &statik))) {
            fprintf(stderr,
                    "jitbench: VFP arithmetic host-state %s mismatch\n",
                    rejection ? "rejection" : "success");
            return false;
        }
    }

    /* Prove both preservation policies and both sides of their exit boundary.
     * Two successful guest operations restore the caller environment, while a
     * fault-sensitive second operation returns the exact one-instruction
     * prefix and restores the same environment before fallback. */
    for (unsigned scenario = 0u; scenario < 4u; scenario++) {
        unsigned session = scenario / 2u;
        unsigned rejection = scenario % 2u;
        static const uint32_t session_program[] = {
            VFP_ARITH_S(3,0,2,0,1), /* VADD s2,s0,s1 */
            VFP_ARITH_S(2,0,5,3,4), /* VMUL s5,s3,s4 */
        };
        uint32_t pc = UINT32_C(0x14700) +
                      (session * 2u + rejection) * 8u;
        uint64_t original_fpcr = static_host_fpcr_read();
        uint64_t original_fpsr = static_host_fpsr_read();
        uint64_t installed_fpcr;
        uint64_t installed_fpsr;
        uint64_t after_fpcr;
        uint64_t after_fpsr;
        bool run_ok;

        seed_vfp_oracle(&reference, session_program, 2u, pc, true);
        reference.vfp_fpscr = VFP_ARITH_CONTROL;
        static_vfp_arith_set_operands(
            &reference, session_program[0], UINT32_C(0x3f800000),
            UINT32_C(0x40400000), UINT32_C(0x40a00000));
        static_vfp_arith_set_operands(
            &reference, session_program[1], UINT32_C(0x3f800000),
            rejection ? UINT32_C(0x7f7fffff) : UINT32_C(0x40000000),
            rejection ? UINT32_C(0x40000000) : UINT32_C(0x40800000));
        statik = reference;
        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[pc], 2u, false, pc, &block) ||
            arm_step(&reference) != ARM_OK ||
            (!rejection && arm_step(&reference) != ARM_OK))
            return false;

        static_host_fpcr_write(
            (original_fpcr & ~(UINT64_C(3) << 22)) |
            (UINT64_C(2) << 22));
        static_host_fpsr_write(UINT64_C(0x08000015));
        installed_fpcr = static_host_fpcr_read();
        installed_fpsr = static_host_fpsr_read();
        completed = UINT_MAX;
        run_ok = a64_static_run_memory_hits_decoded(
            &statik, &block, g_ram, sizeof g_ram,
            session != 0u, &completed);
        after_fpcr = static_host_fpcr_read();
        after_fpsr = static_host_fpsr_read();
        static_host_fpsr_write(original_fpsr);
        static_host_fpcr_write(original_fpcr);

        if (!run_ok || after_fpcr != installed_fpcr ||
            after_fpsr != installed_fpsr ||
            completed != (rejection ? 1u : 2u) ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr,
                    "jitbench: %s VFP host-state %s mismatch\n",
                    session ? "batched" : "unbatched",
                    rejection ? "partial rejection" : "success");
            return false;
        }
    }
#endif

    printf("STATIC-VFP-ARITH-ORACLE exact=yes operations=9 widths=2 "
           "accepted=%u signed-zero=yes inexact=yes fallbacks=%zu "
           "conditions=yes partial-prefix=yes host-fp-state=yes "
           "host-fp-session=yes host-fp-control=yes\n",
           exact_cases,
           sizeof VFP_ARITH_FALLBACKS / sizeof VFP_ARITH_FALLBACKS[0]);
    return true;
}

#undef VFP_ARITH_CONTROL

static bool validate_static_vfp_register_oracles(void) {
    static const uint32_t LAZY_ENABLE[] = {
        VFP_VMRS(0, 0),                 /* FPSID while disabled */
        VFP_VMRS(1, 8),                 /* FPEXC while disabled */
        VFP_VMSR(8, 2),                 /* enable VFP */
        VFP_VMRS(3, 1),                 /* FPSCR after enable */
        VFP_VMOV_S_R(0, 4),             /* ordinary transfer after enable */
    };
    static const uint32_t PARTIAL_DISABLED[] = {
        VFP_VMRS(0, 0),                 /* privileged FPSID succeeds */
        VFP_VMOV_S_R(0, 1),             /* ordinary VFP must fall back */
    };
    static const uint32_t LEN_GUARD[] = {
        VFP_UN_S(0, 0, 1, 0),           /* VMOV.F32 with Len != 0 */
    };
    a64_static_block_t block;
    arm_cpu_t reference, statik, before;
    unsigned completed = 0u;
    arm_status_t status = ARM_OK;

    if (!a64_static_host_available()) {
        printf("STATIC-VFP-REGISTER-ORACLE SKIP: no signed AArch64 "
               "handlers\n");
        return true;
    }

    seed_vfp_oracle(&reference, VFP_REGISTER_OPS,
                    (unsigned)(sizeof VFP_REGISTER_OPS /
                               sizeof VFP_REGISTER_OPS[0]),
                    0xe400u, true);
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(
            &g_ram[0xe400u],
            (unsigned)(sizeof VFP_REGISTER_OPS /
                       sizeof VFP_REGISTER_OPS[0]),
            false, 0xe400u, &block))
        return false;
    for (unsigned i = 0u; i < block.insn_count; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    if (!a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || status != ARM_OK ||
        completed != block.insn_count ||
        !static_vfp_states_equal(&reference, &statik)) {
        fprintf(stderr, "jitbench: VFP register oracle mismatch\n");
        return false;
    }

    seed_vfp_oracle(&reference, LAZY_ENABLE,
                    (unsigned)(sizeof LAZY_ENABLE / sizeof LAZY_ENABLE[0]),
                    0xe600u, false);
    reference.r[2] = ARM_FPEXC_EN;
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(
            &g_ram[0xe600u],
            (unsigned)(sizeof LAZY_ENABLE / sizeof LAZY_ENABLE[0]),
            false, 0xe600u, &block))
        return false;
    for (unsigned i = 0u; i < block.insn_count; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    if (!a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || status != ARM_OK ||
        completed != block.insn_count ||
        !static_vfp_states_equal(&reference, &statik)) {
        fprintf(stderr, "jitbench: VFP lazy-enable oracle mismatch\n");
        return false;
    }

    seed_vfp_oracle(&reference, PARTIAL_DISABLED, 2u, 0xe700u, false);
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xe700u], 2u, false,
                                              0xe700u, &block) ||
        arm_step(&reference) != ARM_OK ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 1u ||
        !static_vfp_states_equal(&reference, &statik)) {
        fprintf(stderr, "jitbench: VFP partial-prefix guard mismatch\n");
        return false;
    }

    seed_vfp_oracle(&statik, LEN_GUARD, 1u, 0xe800u, true);
    statik.vfp_fpscr = ARM_FPSCR_LEN;
    before = statik;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xe800u], 1u, false,
                                              0xe800u, &block) ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 0u ||
        !static_vfp_states_equal(&before, &statik)) {
        fprintf(stderr, "jitbench: VFP Len guard changed state\n");
        return false;
    }

    /* A failed ARM condition retires without consulting CPACR/FPEXC. */
    {
        uint32_t condition_skip = VFP_VMOV_S_R(0, 0) & UINT32_C(0x0fffffff);
        seed_vfp_oracle(&reference, &condition_skip, 1u, 0xe900u, false);
        reference.cp15.cpacr = 0u;
        statik = reference;
        if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xe900u], 1u,
                                                  false, 0xe900u, &block) ||
            arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) || completed != 1u ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr, "jitbench: conditional VFP skip mismatch\n");
            return false;
        }
    }

    printf("STATIC-VFP-REGISTER-ORACLE exact=yes ops=16 lazy=yes "
           "zero-prefix=yes partial-prefix=yes\n");
    return true;
}

#define VFP_COMPARE_CONTROL \
    (ARM_FPSCR_DN | ARM_FPSCR_RMODE | ARM_FPSCR_DZC)

typedef struct {
    const char *name;
    uint32_t insn;
    uint32_t s0;
    uint32_t s1;
    uint32_t s2;
    uint32_t s3;
    uint32_t extra_control;
    uint32_t expected_result;
} static_vfp_compare_case_t;

static bool validate_static_vfp_compare_oracles(void) {
    static const static_vfp_compare_case_t cases[] = {
        {"s-less", VFP_UN_S(4, 0, 0, 2),
         UINT32_C(0xc0000000), 0u, UINT32_C(0x3f800000), 0u,
         0u, ARM_FPSCR_N},
        {"s-greater", VFP_UN_S(4, 0, 0, 2),
         UINT32_C(0x40000000), 0u, UINT32_C(0xbf800000), 0u,
         0u, ARM_FPSCR_C},
        {"s-signed-zero", VFP_UN_S(4, 0, 0, 2),
         UINT32_C(0x80000000), 0u, 0u, 0u,
         0u, ARM_FPSCR_Z | ARM_FPSCR_C},
        {"s-infinity", VFP_UN_S(4, 0, 0, 2),
         UINT32_C(0x7f800000), 0u, UINT32_C(0x7f7fffff), 0u,
         0u, ARM_FPSCR_C},
        {"s-qnan-vcmp", VFP_UN_S(4, 0, 0, 2),
         UINT32_C(0x7fc01234), 0u, UINT32_C(0x3f800000), 0u,
         0u, ARM_FPSCR_C | ARM_FPSCR_V},
        {"s-snan-vcmp", VFP_UN_S(4, 0, 0, 2),
         UINT32_C(0x7f801234), 0u, UINT32_C(0x3f800000), 0u,
         0u, ARM_FPSCR_C | ARM_FPSCR_V | ARM_FPSCR_IOC},
        {"s-qnan-vcmpe", VFP_UN_S(4, 1, 0, 2),
         UINT32_C(0x7fc01234), 0u, UINT32_C(0x3f800000), 0u,
         0u, ARM_FPSCR_C | ARM_FPSCR_V | ARM_FPSCR_IOC},
        {"s-fz-denormal", VFP_UN_S(5, 0, 0, 0),
         UINT32_C(0x00000001), 0u, 0u, 0u,
         ARM_FPSCR_FZ, ARM_FPSCR_Z | ARM_FPSCR_C | ARM_FPSCR_IDC},
        {"s-fz-negative-denormal", VFP_UN_S(5, 0, 0, 0),
         UINT32_C(0x80000001), 0u, 0u, 0u,
         ARM_FPSCR_FZ, ARM_FPSCR_Z | ARM_FPSCR_C | ARM_FPSCR_IDC},
        {"d-less", VFP_UN_D(4, 0, 0, 1),
         0u, UINT32_C(0xc0000000), 0u, UINT32_C(0x3ff00000),
         0u, ARM_FPSCR_N},
        {"d-greater", VFP_UN_D(4, 0, 0, 1),
         0u, UINT32_C(0x40000000), 0u, UINT32_C(0xbff00000),
         0u, ARM_FPSCR_C},
        {"d-signed-zero", VFP_UN_D(4, 0, 0, 1),
         0u, UINT32_C(0x80000000), 0u, 0u,
         0u, ARM_FPSCR_Z | ARM_FPSCR_C},
        {"d-qnan-vcmp", VFP_UN_D(4, 0, 0, 1),
         UINT32_C(0x00001234), UINT32_C(0x7ff80000),
         0u, UINT32_C(0x3ff00000),
         0u, ARM_FPSCR_C | ARM_FPSCR_V},
        {"d-snan-vcmpe", VFP_UN_D(4, 1, 0, 1),
         UINT32_C(0x00000001), UINT32_C(0x7ff00000),
         0u, UINT32_C(0x3ff00000),
         0u, ARM_FPSCR_C | ARM_FPSCR_V | ARM_FPSCR_IOC},
        {"d-fz-denormal", VFP_UN_D(5, 0, 0, 0),
         UINT32_C(0x00000001), 0u, 0u, 0u,
         ARM_FPSCR_FZ, ARM_FPSCR_Z | ARM_FPSCR_C | ARM_FPSCR_IDC},
    };
    static const uint32_t PARTIAL[] = {
        VFP_UN_S(4, 0, 0, 2),           /* compare succeeds */
        VFP_VMSR(1, 0),                 /* enable trapped invalid */
        VFP_UN_S(4, 1, 0, 2),           /* compare must fall back */
    };
    static const uint32_t GUARD[] = {VFP_UN_S(4, 0, 0, 2)};
    a64_static_block_t block;
    arm_cpu_t reference, statik, before;
    unsigned completed = 0u;

    if (!a64_static_host_available()) {
        printf("STATIC-VFP-COMPARE-ORACLE SKIP: no signed AArch64 handlers\n");
        return true;
    }

    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        const static_vfp_compare_case_t *test = &cases[i];
        uint32_t expected = VFP_COMPARE_CONTROL | test->extra_control |
                            test->expected_result;
        seed_vfp_oracle(&reference, &test->insn, 1u,
                        0xea00u + i * 4u, true);
        reference.vfp_s[0] = test->s0;
        reference.vfp_s[1] = test->s1;
        reference.vfp_s[2] = test->s2;
        reference.vfp_s[3] = test->s3;
        reference.vfp_fpscr = VFP_COMPARE_CONTROL | test->extra_control |
                              ARM_FPSCR_NZCV;
        statik = reference;
        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[reference.r[15]], 1u, false, reference.r[15], &block) ||
            arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) ||
            completed != 1u ||
            reference.vfp_fpscr != expected ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr, "jitbench: VFP compare oracle mismatch for %s\n",
                    test->name);
            return false;
        }
    }

    /* Len and every trap-enable bit are live guards. They must refuse before
     * changing even cumulative flags or the host-visible instruction prefix. */
    for (unsigned i = 0u; i < 2u; i++) {
        seed_vfp_oracle(&statik, GUARD, 1u, 0xeb00u + i * 4u, true);
        statik.vfp_fpscr = VFP_COMPARE_CONTROL |
                           (i == 0u ? ARM_FPSCR_LEN : ARM_FPSCR_IOE);
        before = statik;
        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[statik.r[15]], 1u, false, statik.r[15], &block) ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) ||
            completed != 0u || !static_vfp_states_equal(&before, &statik)) {
            fprintf(stderr, "jitbench: VFP compare live guard changed state\n");
            return false;
        }
    }

    seed_vfp_oracle(&reference, PARTIAL, 3u, 0xec00u, true);
    reference.vfp_s[0] = UINT32_C(0xbf800000);
    reference.vfp_s[2] = UINT32_C(0x3f800000);
    reference.r[0] = VFP_COMPARE_CONTROL | ARM_FPSCR_IOE;
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xec00u], 3u, false,
                                              0xec00u, &block) ||
        arm_step(&reference) != ARM_OK || arm_step(&reference) != ARM_OK ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 2u ||
        !static_vfp_states_equal(&reference, &statik)) {
        fprintf(stderr, "jitbench: VFP compare partial-prefix mismatch\n");
        return false;
    }

    /* A failed ARM condition retires before the VFP access/mode guards. */
    {
        uint32_t condition_skip = VFP_UN_S(4, 0, 0, 2) &
                                  UINT32_C(0x0fffffff);
        seed_vfp_oracle(&reference, &condition_skip, 1u, 0xed00u, false);
        reference.cp15.cpacr = 0u;
        statik = reference;
        if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xed00u], 1u,
                                                  false, 0xed00u, &block) ||
            arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) || completed != 1u ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr, "jitbench: conditional VFP compare skip mismatch\n");
            return false;
        }
    }

    printf("STATIC-VFP-COMPARE-ORACLE exact=yes ops=%u single=yes double=yes "
           "nan=yes fz=yes zero-prefix=yes partial-prefix=yes\n",
           (unsigned)(sizeof cases / sizeof cases[0]));
    return true;
}

#undef VFP_COMPARE_CONTROL

#define VFP_WIDEN_CONTROL \
    (ARM_FPSCR_RMODE | ARM_FPSCR_DZC | ARM_FPSCR_N | ARM_FPSCR_C)

typedef struct {
    const char *name;
    uint32_t input;
    uint32_t mode;
    uint64_t output;
    uint32_t raised;
} static_vfp_widen_case_t;

static bool validate_static_vfp_widen_oracles(void) {
    static const static_vfp_widen_case_t cases[] = {
        {"+zero", UINT32_C(0x00000000), 0u,
         UINT64_C(0x0000000000000000), 0u},
        {"-zero", UINT32_C(0x80000000), 0u,
         UINT64_C(0x8000000000000000), 0u},
        {"one", UINT32_C(0x3f800000), 0u,
         UINT64_C(0x3ff0000000000000), 0u},
        {"largest-finite", UINT32_C(0x7f7fffff), 0u,
         UINT64_C(0x47efffffe0000000), 0u},
        {"smallest-normal", UINT32_C(0x00800000), 0u,
         UINT64_C(0x3810000000000000), 0u},
        {"smallest-subnormal", UINT32_C(0x00000001), 0u,
         UINT64_C(0x36a0000000000000), 0u},
        {"largest-subnormal", UINT32_C(0x007fffff), 0u,
         UINT64_C(0x380fffffc0000000), 0u},
        {"fz-positive", UINT32_C(0x00000001), ARM_FPSCR_FZ,
         UINT64_C(0x0000000000000000), ARM_FPSCR_IDC},
        {"fz-negative", UINT32_C(0x80000001), ARM_FPSCR_FZ,
         UINT64_C(0x8000000000000000), ARM_FPSCR_IDC},
        {"+infinity", UINT32_C(0x7f800000), 0u,
         UINT64_C(0x7ff0000000000000), 0u},
        {"-infinity", UINT32_C(0xff800000), 0u,
         UINT64_C(0xfff0000000000000), 0u},
        {"quiet-nan", UINT32_C(0x7fc12345), 0u,
         UINT64_C(0x7ff82468a0000000), 0u},
        {"signalling-nan", UINT32_C(0xff812345), 0u,
         UINT64_C(0xfff82468a0000000), ARM_FPSCR_IOC},
        {"default-nan", UINT32_C(0xffc12345), ARM_FPSCR_DN,
         UINT64_C(0x7ff8000000000000), 0u},
        {"default-signalling-nan", UINT32_C(0xff812345), ARM_FPSCR_DN,
         UINT64_C(0x7ff8000000000000), ARM_FPSCR_IOC},
    };
    static const uint32_t ONE[] = {VFP_WIDEN(2, 1)};
    static const uint32_t PARTIAL[] = {
        VFP_WIDEN(2, 1),                 /* widening succeeds */
        VFP_VMSR(1, 0),                 /* enable trapped invalid */
        VFP_WIDEN(3, 2),                 /* widening must fall back */
    };
    a64_static_block_t block;
    arm_cpu_t reference, statik, before;
    unsigned completed = 0u;

    if (!a64_static_host_available()) {
        printf("STATIC-VFP-WIDEN-ORACLE SKIP: no signed AArch64 handlers\n");
        return true;
    }

    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        const static_vfp_widen_case_t *test = &cases[i];
        uint32_t pc = 0xee00u + i * 4u;
        uint32_t expected_fpscr = VFP_WIDEN_CONTROL | test->mode |
                                  test->raised;
        seed_vfp_oracle(&reference, ONE, 1u, pc, true);
        reference.vfp_s[1] = test->input;
        reference.vfp_fpscr = VFP_WIDEN_CONTROL | test->mode;
        statik = reference;
        if (!a64_static_decode_read_hits_bytes_at(&g_ram[pc], 1u, false,
                                                  pc, &block) ||
            arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) ||
            completed != 1u ||
            vfp_get_d(&reference, 2) != test->output ||
            reference.vfp_fpscr != expected_fpscr ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr, "jitbench: VFP widen oracle mismatch for %s\n",
                    test->name);
            return false;
        }
    }

    for (unsigned i = 0u; i < 2u; i++) {
        uint32_t pc = 0xef00u + i * 4u;
        seed_vfp_oracle(&statik, ONE, 1u, pc, true);
        statik.vfp_fpscr = VFP_WIDEN_CONTROL |
                           (i == 0u ? ARM_FPSCR_LEN : ARM_FPSCR_IOE);
        before = statik;
        if (!a64_static_decode_read_hits_bytes_at(&g_ram[pc], 1u, false,
                                                  pc, &block) ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) ||
            completed != 0u || !static_vfp_states_equal(&before, &statik)) {
            fprintf(stderr, "jitbench: VFP widen live guard changed state\n");
            return false;
        }
    }

    seed_vfp_oracle(&reference, PARTIAL, 3u, 0xf700u, true);
    reference.vfp_s[1] = UINT32_C(0x3f800000);
    reference.vfp_s[2] = UINT32_C(0xbf800000);
    reference.r[0] = VFP_WIDEN_CONTROL | ARM_FPSCR_IOE;
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xf700u], 3u, false,
                                              0xf700u, &block) ||
        arm_step(&reference) != ARM_OK || arm_step(&reference) != ARM_OK ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 2u ||
        !static_vfp_states_equal(&reference, &statik)) {
        fprintf(stderr, "jitbench: VFP widen partial-prefix mismatch\n");
        return false;
    }

    {
        uint32_t condition_skip = VFP_WIDEN(2, 1) & UINT32_C(0x0fffffff);
        seed_vfp_oracle(&reference, &condition_skip, 1u, 0xf800u, false);
        reference.cp15.cpacr = 0u;
        statik = reference;
        if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xf800u], 1u,
                                                  false, 0xf800u, &block) ||
            arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) || completed != 1u ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr, "jitbench: conditional VFP widen skip mismatch\n");
            return false;
        }
    }

    printf("STATIC-VFP-WIDEN-ORACLE exact=yes ops=%u finite=yes subnormal=yes "
           "nan=yes fz=yes zero-prefix=yes partial-prefix=yes\n",
           (unsigned)(sizeof cases / sizeof cases[0]));
    return true;
}

#undef VFP_WIDEN_CONTROL

#define VFP_NARROW_CONTROL \
    (ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_IXC | \
     ARM_FPSCR_N | ARM_FPSCR_C)

static bool validate_static_vfp_narrow_oracles(void) {
    static const struct {
        uint32_t insn;
        uint64_t input;
    } ACCEPTED[] = {
        {VFP_NARROW( 0,  0), UINT64_C(0x0000000000000000)},
        {VFP_NARROW( 1,  0), UINT64_C(0x8000000000000000)},
        {VFP_NARROW( 2,  1), UINT64_C(0x3ff8000000000000)},
        {VFP_NARROW( 3,  7), UINT64_C(0x3fd5555555555555)},
        {VFP_NARROW(15,  7), UINT64_C(0xbfd5555555555555)},
        {VFP_NARROW(16,  8), UINT64_C(0x47efffffe0000000)},
        {VFP_NARROW(30, 14), UINT64_C(0x3810000000000000)},
        {VFP_NARROW(31, 15), UINT64_C(0xc00921fb54442d18)},
    };
    static const struct {
        const char *name;
        uint64_t input;
        uint32_t fpscr;
        bool enabled;
        bool access;
    } FALLBACKS[] = {
        {"missing-runfast", UINT64_C(0x3ff0000000000000),
         ARM_FPSCR_IXC, true, true},
        {"missing-sticky", UINT64_C(0x3ff0000000000000),
         ARM_FPSCR_FZ | ARM_FPSCR_DN, true, true},
        {"directed-rounding", UINT64_C(0x3fd5555555555555),
         VFP_NARROW_CONTROL | (1u << 22), true, true},
        {"extra-sticky", UINT64_C(0x3ff0000000000000),
         VFP_NARROW_CONTROL | ARM_FPSCR_IOC, true, true},
        {"short-vector", UINT64_C(0x3ff0000000000000),
         VFP_NARROW_CONTROL | ARM_FPSCR_LEN, true, true},
        {"exception-enable", UINT64_C(0x3ff0000000000000),
         VFP_NARROW_CONTROL | ARM_FPSCR_IOE, true, true},
        {"disabled", UINT64_C(0x3ff0000000000000),
         VFP_NARROW_CONTROL, false, true},
        {"access-denied", UINT64_C(0x3ff0000000000000),
         VFP_NARROW_CONTROL, true, false},
        {"infinity", UINT64_C(0x7ff0000000000000),
         VFP_NARROW_CONTROL, true, true},
        {"overflow", UINT64_C(0x7fefffffffffffff),
         VFP_NARROW_CONTROL, true, true},
        {"subnormal-result", UINT64_C(0x36a0000000000000),
         VFP_NARROW_CONTROL, true, true},
        {"fz-boundary", UINT64_C(0x380fffffe0000000),
         VFP_NARROW_CONTROL, true, true},
    };
    static const uint32_t PARTIAL[] = {
        VFP_NARROW(0, 1), VFP_VMSR(1, 0), VFP_NARROW(2, 2),
    };
    a64_static_block_t block;
    arm_cpu_t reference, statik, before;
    unsigned completed = UINT_MAX;

    if (!a64_static_host_available()) {
        printf("STATIC-VFP-NARROW-ORACLE SKIP: no signed AArch64 handlers\n");
        return true;
    }

    for (unsigned i = 0u; i < sizeof ACCEPTED / sizeof ACCEPTED[0]; i++) {
        uint32_t pc = UINT32_C(0xfa00) + i * 4u;
        seed_vfp_oracle(&reference, &ACCEPTED[i].insn, 1u, pc, true);
        reference.vfp_fpscr = VFP_NARROW_CONTROL;
        vfp_set_d(&reference, ACCEPTED[i].insn & 15u,
                  ACCEPTED[i].input);
        statik = reference;
        if (!a64_static_decode_read_hits_bytes_at(&g_ram[pc], 1u, false,
                                                  pc, &block) ||
            arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) ||
            completed != 1u ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr,
                    "jitbench: static VFP narrow accepted case %u mismatch\n",
                    i);
            return false;
        }
    }

    for (unsigned i = 0u; i < sizeof FALLBACKS / sizeof FALLBACKS[0]; i++) {
        const uint32_t insn = VFP_NARROW(15, 7);
        uint32_t pc = UINT32_C(0xfb00) + i * 4u;
        seed_vfp_oracle(&statik, &insn, 1u, pc, true);
        statik.vfp_fpscr = FALLBACKS[i].fpscr;
        statik.vfp_fpexc = FALLBACKS[i].enabled ? ARM_FPEXC_EN : 0u;
        if (!FALLBACKS[i].access)
            statik.cp15.cpacr &= ~(UINT32_C(0xf) <<
                                    ARM_CPACR_CP10_SHIFT);
        vfp_set_d(&statik, 7u, FALLBACKS[i].input);
        before = statik;
        completed = UINT_MAX;
        if (!a64_static_decode_read_hits_bytes_at(&g_ram[pc], 1u, false,
                                                  pc, &block) ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) ||
            completed != 0u || !static_vfp_states_equal(&before, &statik)) {
            fprintf(stderr,
                    "jitbench: static VFP narrow fallback changed state "
                    "for %s\n", FALLBACKS[i].name);
            return false;
        }
    }

    seed_vfp_oracle(&reference, PARTIAL, 3u, UINT32_C(0xfc00), true);
    reference.vfp_fpscr = VFP_NARROW_CONTROL;
    reference.r[0] = ARM_FPSCR_FZ | ARM_FPSCR_DN;
    vfp_set_d(&reference, 1u, UINT64_C(0x3fd5555555555555));
    vfp_set_d(&reference, 2u, UINT64_C(0x400921fb54442d18));
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(
            &g_ram[UINT32_C(0xfc00)], 3u, false,
            UINT32_C(0xfc00), &block) ||
        arm_step(&reference) != ARM_OK || arm_step(&reference) != ARM_OK ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 2u ||
        !static_vfp_states_equal(&reference, &statik)) {
        fprintf(stderr, "jitbench: static VFP narrow partial-prefix mismatch\n");
        return false;
    }

    {
        uint32_t condition_skip =
            VFP_NARROW(15, 7) & UINT32_C(0x0fffffff);
        seed_vfp_oracle(&reference, &condition_skip, 1u,
                        UINT32_C(0xfd00), false);
        reference.cp15.cpacr = 0u;
        reference.vfp_fpexc = 0u;
        reference.vfp_fpscr = 0u;
        statik = reference;
        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[UINT32_C(0xfd00)], 1u, false,
                UINT32_C(0xfd00), &block) ||
            arm_step(&reference) != ARM_OK ||
            !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                      &completed) ||
            completed != 1u ||
            !static_vfp_states_equal(&reference, &statik)) {
            fprintf(stderr,
                    "jitbench: conditional static VFP narrow skip mismatch\n");
            return false;
        }
    }

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    for (unsigned rejection = 0u; rejection < 2u; rejection++) {
        const uint32_t insn = VFP_NARROW(15, 7);
        const uint32_t pc = UINT32_C(0xfe00) + rejection * 4u;
        const uint64_t original_fpcr = static_host_fpcr_read();
        const uint64_t original_fpsr = static_host_fpsr_read();
        uint64_t installed_fpcr, installed_fpsr, after_fpcr, after_fpsr;
        bool run_ok;

        seed_vfp_oracle(&statik, &insn, 1u, pc, true);
        statik.vfp_fpscr = VFP_NARROW_CONTROL;
        vfp_set_d(&statik, 7u,
                  rejection ? UINT64_C(0x7fefffffffffffff)
                            : UINT64_C(0x3fd5555555555555));
        if (rejection) {
            before = statik;
        } else {
            reference = statik;
            if (arm_step(&reference) != ARM_OK) return false;
        }
        if (!a64_static_decode_read_hits_bytes_at(&g_ram[pc], 1u, false,
                                                  pc, &block))
            return false;

        static_host_fpcr_write(
            (original_fpcr & ~(UINT64_C(3) << 22)) |
            (UINT64_C(1) << 22));
        static_host_fpsr_write(UINT64_C(0x08000015));
        installed_fpcr = static_host_fpcr_read();
        installed_fpsr = static_host_fpsr_read();
        completed = UINT_MAX;
        run_ok = a64_static_run_read_hits(
            &statik, &block, g_ram, sizeof g_ram, &completed);
        after_fpcr = static_host_fpcr_read();
        after_fpsr = static_host_fpsr_read();
        static_host_fpsr_write(original_fpsr);
        static_host_fpcr_write(original_fpcr);

        if (!run_ok || after_fpcr != installed_fpcr ||
            after_fpsr != installed_fpsr ||
            (rejection
                 ? completed != 0u ||
                       !static_vfp_states_equal(&before, &statik)
                 : completed != 1u ||
                       !static_vfp_states_equal(&reference, &statik))) {
            fprintf(stderr,
                    "jitbench: static VFP narrow host-state %s mismatch\n",
                    rejection ? "rejection" : "success");
            return false;
        }
    }
#endif

    printf("STATIC-VFP-NARROW-ORACLE exact=yes accepted=%zu fallbacks=%zu "
           "aliases=yes inexact=yes conditions=yes partial-prefix=yes "
           "host-fp-state=yes runtime-codegen=no\n",
           sizeof ACCEPTED / sizeof ACCEPTED[0],
           sizeof FALLBACKS / sizeof FALLBACKS[0]);
    return true;
}

#undef VFP_NARROW_CONTROL

static void seed_vfp_read_oracle(arm_cpu_t *cpu, bool warm) {
    seed_vfp_oracle(cpu, VFP_READ_HITS,
                    (unsigned)(sizeof VFP_READ_HITS /
                               sizeof VFP_READ_HITS[0]),
                    0xf000u, true);
    cpu->r[0] = DATA_BASE;
    cpu->r[1] = DATA_BASE + 0x40u;
    mem_w32(NULL, DATA_BASE + 0x00u, UINT32_C(0x11223344));
    mem_w32(NULL, DATA_BASE + 0x04u, UINT32_C(0x55667788));
    mem_w32(NULL, DATA_BASE + 0x08u, UINT32_C(0x99aabbcc));
    mem_w32(NULL, DATA_BASE + 0x0cu, UINT32_C(0xddeeff00));
    mem_w32(NULL, DATA_BASE + 0x38u, UINT32_C(0x13579bdf));
    mem_w32(NULL, DATA_BASE + 0x3cu, UINT32_C(0x2468ace0));
    mem_w32(NULL, 0xf09cu, UINT32_C(0xcafef00d));
    mem_w32(NULL, 0xf0a4u, UINT32_C(0x0badc0de));
    mem_w32(NULL, 0xf0a8u, UINT32_C(0xfeedface));
    if (warm) {
        oracle_warm_dread(cpu, DATA_BASE);
        oracle_warm_dread(cpu, 0xf000u);
    }
}

static bool validate_static_vfp_read_oracles(void) {
    static const uint32_t PARTIAL[] = {
        VFP_VMOV_S_R(1, 0),
        VFP_LDST(14, 1, 1, 0, 0, 1, 7, 0, 0, 0),
    };
    static const uint32_t ONE_SINGLE[] = {
        VFP_LDST(14, 1, 1, 0, 0, 1, 0, 0, 0, 0),
    };
    static const uint32_t ONE_DOUBLE[] = {
        VFP_LDST(14, 1, 1, 0, 0, 1, 0, 0, 1, 0),
    };
    a64_static_block_t block;
    arm_cpu_t reference, statik, before;
    unsigned completed = 0u;
    arm_status_t status = ARM_OK;

    if (!a64_static_host_available()) {
        printf("STATIC-VFP-READ-ORACLE SKIP: no signed AArch64 handlers\n");
        return true;
    }

    seed_vfp_read_oracle(&reference, true);
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(
            &g_ram[0xf000u],
            (unsigned)(sizeof VFP_READ_HITS / sizeof VFP_READ_HITS[0]),
            false, 0xf000u, &block))
        return false;
    for (unsigned i = 0u; i < block.insn_count; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    if (!a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || status != ARM_OK ||
        completed != block.insn_count ||
        !static_vfp_states_equal(&reference, &statik) ||
        statik.dread_hits != 10u || statik.dread_misses != 0u) {
        fprintf(stderr, "jitbench: VFP read-hit oracle mismatch\n");
        return false;
    }

    seed_vfp_oracle(&reference, PARTIAL, 2u, 0xf200u, true);
    reference.r[7] = DATA_BASE;
    mem_w32(NULL, DATA_BASE, UINT32_C(0xa5a55a5a));
    statik = reference;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xf200u], 2u, false,
                                              0xf200u, &block) ||
        arm_step(&reference) != ARM_OK ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 1u ||
        !static_vfp_states_equal(&reference, &statik)) {
        fprintf(stderr, "jitbench: VFP read partial-prefix mismatch\n");
        return false;
    }

    seed_vfp_oracle(&statik, ONE_SINGLE, 1u, 0xf300u, true);
    statik.r[0] = DATA_BASE;
    before = statik;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xf300u], 1u, false,
                                              0xf300u, &block) ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 0u ||
        !static_vfp_states_equal(&before, &statik)) {
        fprintf(stderr, "jitbench: cold VFP read miss changed state\n");
        return false;
    }

    seed_vfp_oracle(&statik, ONE_SINGLE, 1u, 0xf400u, true);
    statik.r[0] = DATA_BASE + 1u;
    oracle_warm_dread(&statik, DATA_BASE);
    before = statik;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xf400u], 1u, false,
                                              0xf400u, &block) ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 0u ||
        !static_vfp_states_equal(&before, &statik)) {
        fprintf(stderr, "jitbench: misaligned VFP read changed state\n");
        return false;
    }

    seed_vfp_oracle(&statik, ONE_DOUBLE, 1u, 0xf500u, true);
    statik.r[0] = DATA_BASE + 0x3fcu;
    oracle_warm_dread(&statik, statik.r[0]);
    before = statik;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xf500u], 1u, false,
                                              0xf500u, &block) ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 0u ||
        !static_vfp_states_equal(&before, &statik)) {
        fprintf(stderr, "jitbench: cross-block VFP read changed state\n");
        return false;
    }

    seed_vfp_oracle(&statik, ONE_SINGLE, 1u, 0xf600u, false);
    statik.r[0] = DATA_BASE;
    oracle_warm_dread(&statik, DATA_BASE);
    before = statik;
    if (!a64_static_decode_read_hits_bytes_at(&g_ram[0xf600u], 1u, false,
                                              0xf600u, &block) ||
        !a64_static_run_read_hits(&statik, &block, g_ram, sizeof g_ram,
                                  &completed) || completed != 0u ||
        !static_vfp_states_equal(&before, &statik)) {
        fprintf(stderr, "jitbench: disabled VFP read changed state\n");
        return false;
    }

    printf("STATIC-VFP-READ-ORACLE exact=yes hits=10 double=yes "
           "zero-prefix=yes partial-prefix=yes alignment=yes boundary=yes\n");
    return true;
}

static bool validate_static_vfp_write_oracles(void) {
    typedef struct {
        const char *name;
        uint32_t insn;
        uint32_t pc;
        unsigned rn;
        uint32_t base;
        uint32_t va;
        bool dbl;
        bool executes;
    } vfp_write_case_t;
    const uint32_t ordinary = DATA_BASE + UINT32_C(0x800);
    const uint32_t boundary = DATA_BASE + UINT32_C(0xbfc);
    const vfp_write_case_t cases[] = {
        {"s0", VFP_WRITE_HITS[0], UINT32_C(0x10500),
         0u, ordinary, ordinary, false, true},
        {"s31", VFP_WRITE_HITS[1], UINT32_C(0x10600),
         0u, ordinary, ordinary + 4u, false, true},
        {"s2-negative", VFP_WRITE_HITS[2], UINT32_C(0x10700),
         1u, ordinary + 0x40u, ordinary + 0x3cu, false, true},
        {"d2-boundary", VFP_WRITE_HITS[3], UINT32_C(0x10800),
         0u, boundary - 8u, boundary, true, true},
        {"d15-negative", VFP_WRITE_HITS[4], UINT32_C(0x10900),
         1u, ordinary + 0x80u, ordinary + 0x78u, true, true},
        {"pc-relative", VFP_WRITE_HITS[5], UINT32_C(0x10a00),
         15u, 0u, UINT32_C(0x10a88), false, true},
        {"failed-condition", VFP_WRITE_HITS[6], UINT32_C(0x10b00),
         0u, ordinary, ordinary + 252u, false, false},
    };
    static const uint32_t DOUBLE_BOUNDARY =
        VFP_LDST(14, 1, 1, 0, 0, 0, 0, 3, 1, 0);
    static const uint32_t ONE_SINGLE =
        VFP_LDST(14, 1, 1, 0, 0, 0, 0, 0, 0, 0);
    arm_bus_t write_bus = g_bus;
    uint8_t *baseline = (uint8_t *)malloc(sizeof g_ram);
    uint8_t *expected = (uint8_t *)malloc(sizeof g_ram);
    unsigned hit_words = 0u;

    if (!a64_static_host_available()) {
        printf("STATIC-VFP-WRITE-ORACLE SKIP: no signed AArch64 handlers\n");
        free(baseline);
        free(expected);
        return true;
    }
    if (!baseline || !expected) {
        fprintf(stderr, "jitbench: VFP write-oracle allocation failed\n");
        free(baseline);
        free(expected);
        return false;
    }
    write_bus.host_ram_write = mem_host_ram;

    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        const vfp_write_case_t *sc = &cases[i];
        arm_cpu_t reference, statik;
        a64_static_block_t block;
        unsigned completed = 99u;
        arm_status_t status;

        seed_vfp_oracle(&reference, &sc->insn, 1u, sc->pc, true);
        reference.bus = &write_bus;
        if (sc->rn != 15u) reference.r[sc->rn] = sc->base;
        if (!sc->executes) {
            /* A failed condition must bypass CPACR, FPEXC and DWRITE. */
            reference.cp15.cpacr = 0u;
            reference.vfp_fpexc = 0u;
        } else {
            oracle_warm_dwrite(&reference, sc->va, true);
            if (sc->dbl) oracle_warm_dwrite(&reference, sc->va + 4u, true);
        }
        statik = reference;
        memcpy(baseline, g_ram, sizeof g_ram);

        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[sc->pc], 1u, false, sc->pc, &block)) {
            fprintf(stderr, "jitbench: VFP write decode failed for %s\n",
                    sc->name);
            free(baseline);
            free(expected);
            return false;
        }
        status = arm_step(&reference);
        memcpy(expected, g_ram, sizeof g_ram);
        memcpy(g_ram, baseline, sizeof g_ram);
        if (!a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                        &completed) ||
            status != ARM_OK || completed != 1u ||
            !static_vfp_states_equal(&reference, &statik) ||
            memcmp(expected, g_ram, sizeof g_ram) != 0 ||
            statik.dwrite_hits !=
                (sc->executes ? (sc->dbl ? 2u : 1u) : 0u) ||
            statik.dwrite_misses != 0u) {
            fprintf(stderr, "jitbench: VFP write mismatch for %s\n",
                    sc->name);
            free(baseline);
            free(expected);
            return false;
        }
        if (sc->executes) hit_words += sc->dbl ? 2u : 1u;
    }

    /* If only the first word of a boundary-spanning D store is cached, signed
     * execution must leave both RAM and counters untouched. The literal step
     * then owns both architectural writes and converges with a pure reference
     * run, including its one hit and one miss. */
    {
        const uint32_t pc = UINT32_C(0x10c00);
        arm_cpu_t reference, statik, before;
        a64_static_block_t block;
        unsigned completed = 99u;

        seed_vfp_oracle(&reference, &DOUBLE_BOUNDARY, 1u, pc, true);
        reference.bus = &write_bus;
        reference.r[0] = boundary;
        oracle_warm_dwrite(&reference, boundary, true);
        statik = reference;
        before = statik;
        memcpy(baseline, g_ram, sizeof g_ram);

        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block) ||
            arm_step(&reference) != ARM_OK) {
            fprintf(stderr, "jitbench: VFP boundary rollback setup failed\n");
            free(baseline);
            free(expected);
            return false;
        }
        memcpy(expected, g_ram, sizeof g_ram);
        memcpy(g_ram, baseline, sizeof g_ram);
        if (!a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                        &completed) ||
            completed != 0u ||
            !static_vfp_states_equal(&before, &statik) ||
            memcmp(baseline, g_ram, sizeof g_ram) != 0 ||
            arm_step(&statik) != ARM_OK ||
            !static_vfp_states_equal(&reference, &statik) ||
            memcmp(expected, g_ram, sizeof g_ram) != 0 ||
            statik.dwrite_hits != 1u || statik.dwrite_misses != 1u) {
            fprintf(stderr,
                    "jitbench: VFP boundary rollback/fallback mismatch\n");
            free(baseline);
            free(expected);
            return false;
        }
    }

    /* Alignment and live VFP access remain literal concerns. Each refusal is
     * zero-prefix and must be wholly observational. */
    for (unsigned disabled = 0u; disabled < 2u; disabled++) {
        const uint32_t pc = UINT32_C(0x10d00) + disabled * 0x100u;
        arm_cpu_t statik, before;
        a64_static_block_t block;
        unsigned completed = 99u;

        seed_vfp_oracle(&statik, &ONE_SINGLE, 1u, pc, disabled == 0u);
        statik.bus = &write_bus;
        statik.r[0] = disabled ? ordinary : ordinary + 1u;
        oracle_warm_dwrite(&statik, statik.r[0], true);
        before = statik;
        memcpy(baseline, g_ram, sizeof g_ram);
        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block) ||
            !a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                        &completed) ||
            completed != 0u ||
            !static_vfp_states_equal(&before, &statik) ||
            memcmp(baseline, g_ram, sizeof g_ram) != 0) {
            fprintf(stderr, "jitbench: VFP write %s refusal changed state\n",
                    disabled ? "disabled" : "alignment");
            free(baseline);
            free(expected);
            return false;
        }
    }

    free(baseline);
    free(expected);
    printf("STATIC-VFP-WRITE-ORACLE exact=yes cases=%zu hit-words=%u "
           "single=yes double=yes pc-relative=yes conditional=yes "
           "alignment=yes boundary=yes rollback=yes zero-prefix=yes\n",
           sizeof cases / sizeof cases[0], hit_words);
    return true;
}


static bool validate_static_vstm_write_oracles(void) {
    typedef struct {
        const char *name;
        uint32_t insn;
        uint32_t pc;
        unsigned rn;
        uint32_t base;
        uint32_t start;
        unsigned words;
        bool executes;
    } vstm_write_case_t;
    const uint32_t ordinary = DATA_BASE + UINT32_C(0x800);
    const vstm_write_case_t cases[] = {
        {"ia-s-odd", VSTM_WRITE_HITS[0], UINT32_C(0x10f00),
         0u, ordinary, ordinary, 1u, true},
        {"ia-wb-s31", VSTM_WRITE_HITS[1], UINT32_C(0x11000),
         1u, ordinary + 0x40u, ordinary + 0x40u, 1u, true},
        {"db-wb-d0-d15", VSTM_WRITE_HITS[2], UINT32_C(0x11100),
         13u, ordinary + 0x400u, ordinary + 0x380u, 32u, true},
        {"ia-d14-d15", VSTM_WRITE_HITS[3], UINT32_C(0x11200),
         2u, ordinary + 0x100u, ordinary + 0x100u, 4u, true},
        {"failed-condition", VSTM_WRITE_HITS[4], UINT32_C(0x11300),
         3u, ordinary + 0x180u, ordinary + 0x180u, 2u, false},
        {"ia-wb-s0-s31",
         VFP_LDST(14, 0, 1, 0, 1, 0, 4, 0, 0, 32),
         UINT32_C(0x11400), 4u, ordinary + 0x200u,
         ordinary + 0x200u, 32u, true},
    };
    arm_bus_t write_bus = g_bus;
    uint8_t *baseline = (uint8_t *)malloc(sizeof g_ram);
    uint8_t *expected = (uint8_t *)malloc(sizeof g_ram);
    unsigned hit_words = 0u;

    if (!a64_static_host_available()) {
        printf("STATIC-VSTM-WRITE-ORACLE SKIP: no signed AArch64 handlers\n");
        free(baseline);
        free(expected);
        return true;
    }
    if (!baseline || !expected) {
        fprintf(stderr, "jitbench: VSTM write-oracle allocation failed\n");
        free(baseline);
        free(expected);
        return false;
    }
    write_bus.host_ram_write = mem_host_ram;

    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        const vstm_write_case_t *sc = &cases[i];
        arm_cpu_t reference, statik;
        a64_static_block_t block;
        unsigned completed = 99u;
        arm_status_t status;

        seed_vfp_oracle(&reference, &sc->insn, 1u, sc->pc, true);
        reference.bus = &write_bus;
        reference.r[sc->rn] = sc->base;
        if (!sc->executes) {
            /* A failed condition must bypass VFP and DWRITE guards. */
            reference.cp15.cpacr = 0u;
            reference.vfp_fpexc = 0u;
        } else {
            oracle_warm_dwrite(&reference, sc->start, true);
        }
        statik = reference;
        memcpy(baseline, g_ram, sizeof g_ram);

        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[sc->pc], 1u, false, sc->pc, &block) ||
            !block.vstm_direct_writes || block.vfp_direct_writes ||
            block.stm_direct_writes) {
            fprintf(stderr, "jitbench: VSTM decode failed for %s\n",
                    sc->name);
            free(baseline);
            free(expected);
            return false;
        }
        status = arm_step(&reference);
        memcpy(expected, g_ram, sizeof g_ram);
        memcpy(g_ram, baseline, sizeof g_ram);
        if (!a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                         &completed) ||
            status != ARM_OK || completed != 1u ||
            !static_vfp_states_equal(&reference, &statik) ||
            memcmp(expected, g_ram, sizeof g_ram) != 0 ||
            statik.dwrite_hits != (sc->executes ? sc->words : 0u) ||
            statik.dwrite_misses != 0u) {
            fprintf(stderr, "jitbench: VSTM mismatch for %s\n", sc->name);
            free(baseline);
            free(expected);
            return false;
        }
        if (sc->executes) hit_words += sc->words;
    }

    /* A transfer that crosses a DWRITE block is refused before its first word
     * even when the first block is hot. Literal fallback owns the complete
     * transfer, fills the second block, and must converge exactly. */
    {
        const uint32_t insn =
            VFP_LDST(14, 0, 1, 0, 0, 0, 4, 0, 0, 4);
        const uint32_t pc = UINT32_C(0x11500);
        const uint32_t start = ordinary + UINT32_C(0x3f8);
        arm_cpu_t reference, statik, before;
        a64_static_block_t block;
        unsigned completed = 99u;

        seed_vfp_oracle(&reference, &insn, 1u, pc, true);
        reference.bus = &write_bus;
        reference.r[4] = start;
        oracle_warm_dwrite(&reference, start, true);
        statik = reference;
        before = statik;
        memcpy(baseline, g_ram, sizeof g_ram);
        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block) ||
            arm_step(&reference) != ARM_OK) {
            fprintf(stderr, "jitbench: VSTM cross-block setup failed\n");
            free(baseline);
            free(expected);
            return false;
        }
        memcpy(expected, g_ram, sizeof g_ram);
        memcpy(g_ram, baseline, sizeof g_ram);
        if (!a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                         &completed) ||
            completed != 0u || !static_vfp_states_equal(&before, &statik) ||
            memcmp(baseline, g_ram, sizeof g_ram) != 0 ||
            arm_step(&statik) != ARM_OK ||
            !static_vfp_states_equal(&reference, &statik) ||
            memcmp(expected, g_ram, sizeof g_ram) != 0) {
            fprintf(stderr,
                    "jitbench: VSTM cross-block rollback/fallback mismatch\n");
            free(baseline);
            free(expected);
            return false;
        }
    }

    /* Cold DWRITE, alignment, revoked consent and disabled VFP are all
     * zero-prefix refusals with no architectural or diagnostic side effect. */
    for (unsigned refusal = 0u; refusal < 4u; refusal++) {
        const uint32_t insn =
            VFP_LDST(14, 0, 1, 0, 1, 0, 4, 0, 0, 4);
        const uint32_t pc = UINT32_C(0x11600) + refusal * 0x100u;
        arm_cpu_t statik, before;
        a64_static_block_t block;
        unsigned completed = 99u;

        seed_vfp_oracle(&statik, &insn, 1u, pc, refusal != 3u);
        statik.bus = refusal == 2u ? &g_bus : &write_bus;
        statik.r[4] = ordinary + UINT32_C(0x100) +
                      (refusal == 1u ? 1u : 0u);
        if (refusal != 0u)
            oracle_warm_dwrite(&statik, statik.r[4], true);
        before = statik;
        memcpy(baseline, g_ram, sizeof g_ram);
        if (!a64_static_decode_memory_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block) ||
            !a64_static_run_memory_hits(&statik, &block, g_ram, sizeof g_ram,
                                         &completed) ||
            completed != 0u || !static_vfp_states_equal(&before, &statik) ||
            memcmp(baseline, g_ram, sizeof g_ram) != 0) {
            fprintf(stderr, "jitbench: VSTM refusal %u changed state\n",
                    refusal);
            free(baseline);
            free(expected);
            return false;
        }
    }

    free(baseline);
    free(expected);
    printf("STATIC-VSTM-WRITE-ORACLE exact=yes cases=%zu hit-words=%u "
           "modes=3 single=yes double=yes odd-single=yes max-words=32 "
           "writeback=yes conditional=yes one-block=yes "
           "cross-block-rollback=yes alignment=yes cold-cache=yes "
           "consent=yes VFP-guard=yes FSTMX=no\n",
           sizeof cases / sizeof cases[0], hit_words);
    return true;
}

static bool validate_static_oracles(void) {
    unsigned i;
    for (i = 0u; i < sizeof STATIC_CASES / sizeof STATIC_CASES[0]; i++) {
        const static_case_t *sc = &STATIC_CASES[i];
        a64_static_block_t block;
        arm_cpu_t cpu;
        final_state_t interp, statik;
        arm_status_t status = ARM_OK;
        unsigned retired;

        if (!a64_static_decode_at(sc->program, sc->insns, sc->thumb,
                                  sc->pc, &block))
            return false;
        seed_cpu_at(&cpu, sc->program, sc->insns, sc->thumb, sc->pc);
        for (retired = 0u; retired < sc->insns; retired++) {
            status = arm_step(&cpu);
            if (status != ARM_OK) break;
        }
        capture_state(&interp, &cpu, status, JIT_EXIT_NEXT);

        seed_cpu_at(&cpu, sc->program, sc->insns, sc->thumb, sc->pc);
        if (!a64_static_run(&cpu, &block, 1u, g_ram, sizeof g_ram)) {
            fprintf(stderr, "jitbench: static execution failed for %s\n",
                    sc->name);
            return false;
        }
        capture_state(&statik, &cpu, ARM_OK, JIT_EXIT_NEXT);
        if (retired != sc->insns ||
            !architectural_states_equal(&interp, &statik)) {
            fprintf(stderr,
                    "jitbench: static/interpreter oracle mismatch for %s\n",
                    sc->name);
            return false;
        }
        printf("STATIC-BLOCK-ORACLE case=%s exact=yes pc=%08" PRIx32
               " cycles=%" PRIu64 "\n",
               sc->name, statik.r[15], statik.cycles);
    }
    return true;
}

static uint32_t branch_nzcv(unsigned mask) {
    uint32_t flags = 0u;
    if (mask & 1u) flags |= ARM_CPSR_N;
    if (mask & 2u) flags |= ARM_CPSR_Z;
    if (mask & 4u) flags |= ARM_CPSR_C;
    if (mask & 8u) flags |= ARM_CPSR_V;
    return flags;
}

static bool branch_condition_flags(unsigned condition, bool passed,
                                   uint32_t *flags_out) {
    arm_cpu_t probe;
    if (!flags_out || condition >= 14u) return false;
    memset(&probe, 0, sizeof probe);
    for (unsigned mask = 0u; mask < 16u; mask++) {
        probe.cpsr = ARM_MODE_SYS | branch_nzcv(mask);
        if (arm_cond_passed(&probe, condition) == passed) {
            *flags_out = branch_nzcv(mask);
            return true;
        }
    }
    return false;
}

static bool validate_static_branch_oracles(void) {
    const uint32_t pc = UINT32_C(0x1800);
    const uint32_t nzcv_mask = ARM_CPSR_N | ARM_CPSR_Z |
                               ARM_CPSR_C | ARM_CPSR_V;
    unsigned cases = 0u;

    for (unsigned link = 0u; link < 2u; link++) {
        for (unsigned condition = 0u; condition < 15u; condition++) {
            const unsigned outcomes = condition == 14u ? 1u : 2u;
            for (unsigned outcome = 0u; outcome < outcomes; outcome++) {
                const bool passed = condition == 14u || outcome != 0u;
                const uint32_t imm24 = (cases & 1u) != 0u
                    ? UINT32_C(0x000002)
                    : UINT32_C(0xfffffc);
                const uint32_t insn = (condition << 28) |
                    UINT32_C(0x0a000000) | (link << 24) | imm24;
                const int32_t displacement = (int32_t)(insn << 8) >> 6;
                const uint32_t target = pc + 8u + (uint32_t)displacement;
                const uint32_t expected_pc = passed ? target : pc + 4u;
                const uint32_t expected_lr = link != 0u && passed
                    ? pc + 4u : UINT32_C(0xdead00ff);
                uint32_t flags = 0u;
                a64_static_block_t block;
                arm_cpu_t interp_cpu;
                arm_cpu_t static_cpu;
                final_state_t interp;
                final_state_t statik;
                arm_status_t status;

                if (condition != 14u &&
                    !branch_condition_flags(condition, passed, &flags)) {
                    fprintf(stderr,
                            "jitbench: no NZCV witness for cond=%u pass=%u\n",
                            condition, passed ? 1u : 0u);
                    return false;
                }
                if (!a64_static_decode_at(&insn, 1u, false, pc, &block)) {
                    fprintf(stderr,
                            "jitbench: branch oracle decode failed "
                            "link=%u cond=%u pass=%u\n",
                            link, condition, passed ? 1u : 0u);
                    return false;
                }

                seed_cpu_at(&interp_cpu, &insn, 1u, false, pc);
                interp_cpu.cpsr =
                    (interp_cpu.cpsr & ~nzcv_mask) | flags;
                status = arm_step(&interp_cpu);
                capture_state(&interp, &interp_cpu, status, JIT_EXIT_NEXT);

                seed_cpu_at(&static_cpu, &insn, 1u, false, pc);
                static_cpu.cpsr =
                    (static_cpu.cpsr & ~nzcv_mask) | flags;
                if (!a64_static_run(&static_cpu, &block, 1u,
                                    g_ram, sizeof g_ram)) {
                    fprintf(stderr,
                            "jitbench: branch oracle execution failed "
                            "link=%u cond=%u pass=%u\n",
                            link, condition, passed ? 1u : 0u);
                    return false;
                }
                capture_state(&statik, &static_cpu, ARM_OK, JIT_EXIT_NEXT);
                if (status != ARM_OK ||
                    !architectural_states_equal(&interp, &statik) ||
                    statik.r[15] != expected_pc ||
                    statik.r[14] != expected_lr ||
                    statik.cycles != 1u) {
                    fprintf(stderr,
                            "jitbench: branch oracle mismatch "
                            "link=%u cond=%u pass=%u pc=%08" PRIx32
                            " lr=%08" PRIx32 "\n",
                            link, condition, passed ? 1u : 0u,
                            statik.r[15], statik.r[14]);
                    return false;
                }
                cases++;
            }
        }
    }

    {
        const uint32_t insn = UINT32_C(0x0a000002); /* BEQ forward */
        const uint8_t bytes[4] = {
            (uint8_t)insn, (uint8_t)(insn >> 8),
            (uint8_t)(insn >> 16), (uint8_t)(insn >> 24)
        };
        a64_static_block_t block;
        arm_cpu_t cpu;
        arm_cpu_t before;
        unsigned completed = UINT_MAX;
        uint32_t flags;

        if (!branch_condition_flags(0u, true, &flags) ||
            !a64_static_decode_read_hits_bytes_at(
                bytes, 1u, false, pc, &block)) {
            fprintf(stderr, "jitbench: decoded branch contract setup failed\n");
            return false;
        }
        seed_cpu_at(&cpu, &insn, 1u, false, pc);
        cpu.cpsr = (cpu.cpsr & ~nzcv_mask) | flags;
        before = cpu;
        block.dynamic_exit = false;
        if (a64_static_run_read_hits_decoded(
                &cpu, &block, g_ram, sizeof g_ram, &completed) ||
            memcmp(&before, &cpu, sizeof cpu) != 0 ||
            completed != UINT_MAX) {
            fprintf(stderr,
                    "jitbench: mutated decoded branch contract did not "
                    "fail closed\n");
            return false;
        }
    }

    if (cases != 58u) {
        fprintf(stderr, "jitbench: incomplete branch oracle matrix\n");
        return false;
    }
    printf("STATIC-BRANCH-ORACLE exact=yes cases=58 taken=yes "
           "fallthrough=yes link=yes backward=yes contract=yes\n");
    return true;
}

static bool validate_static_thumb_cond_branch_oracles(void) {
    const uint32_t pc = UINT32_C(0x1900);
    const uint32_t nzcv_mask = ARM_CPSR_N | ARM_CPSR_Z |
                               ARM_CPSR_C | ARM_CPSR_V;
    unsigned cases = 0u;

    for (unsigned condition = 0u; condition < 14u; condition++) {
        for (unsigned outcome = 0u; outcome < 2u; outcome++) {
            const bool passed = outcome != 0u;
            const uint8_t imm8 = ((condition + outcome) & 1u) != 0u
                ? UINT8_C(2) : UINT8_C(0xfc);
            const uint16_t insn = (uint16_t)(UINT16_C(0xd000) |
                (condition << 8) | imm8);
            const int32_t displacement =
                (int32_t)((uint32_t)imm8 << 24) >> 23;
            const uint32_t target = pc + 4u + (uint32_t)displacement;
            const uint32_t expected_pc = passed ? target : pc + 2u;
            uint32_t flags = 0u;
            a64_static_block_t block;
            arm_cpu_t interp_cpu;
            arm_cpu_t static_cpu;
            final_state_t interp;
            final_state_t statik;
            arm_status_t status;

            if (!branch_condition_flags(condition, passed, &flags)) {
                fprintf(stderr,
                        "jitbench: no Thumb NZCV witness cond=%u pass=%u\n",
                        condition, passed ? 1u : 0u);
                return false;
            }
            if (!a64_static_decode_at(&insn, 1u, true, pc, &block)) {
                fprintf(stderr,
                        "jitbench: Thumb conditional decode failed "
                        "cond=%u pass=%u\n",
                        condition, passed ? 1u : 0u);
                return false;
            }

            seed_cpu_at(&interp_cpu, &insn, 1u, true, pc);
            interp_cpu.cpsr =
                (interp_cpu.cpsr & ~nzcv_mask) | flags;
            status = arm_step(&interp_cpu);
            capture_state(&interp, &interp_cpu, status, JIT_EXIT_NEXT);

            seed_cpu_at(&static_cpu, &insn, 1u, true, pc);
            static_cpu.cpsr =
                (static_cpu.cpsr & ~nzcv_mask) | flags;
            if (!a64_static_run(&static_cpu, &block, 1u,
                                g_ram, sizeof g_ram)) {
                fprintf(stderr,
                        "jitbench: Thumb conditional execution failed "
                        "cond=%u pass=%u\n",
                        condition, passed ? 1u : 0u);
                return false;
            }
            capture_state(&statik, &static_cpu, ARM_OK, JIT_EXIT_NEXT);
            if (status != ARM_OK ||
                !architectural_states_equal(&interp, &statik) ||
                statik.r[15] != expected_pc ||
                statik.r[14] != UINT32_C(0xdead00ff) ||
                (statik.cpsr & ARM_CPSR_T) == 0u || statik.cycles != 1u) {
                fprintf(stderr,
                        "jitbench: Thumb conditional mismatch cond=%u "
                        "pass=%u pc=%08" PRIx32 " lr=%08" PRIx32 "\n",
                        condition, passed ? 1u : 0u,
                        statik.r[15], statik.r[14]);
                return false;
            }
            cases++;
        }
    }

    {
        const uint16_t insn = UINT16_C(0xd0fc);
        const uint8_t bytes[2] = {(uint8_t)insn, (uint8_t)(insn >> 8)};
        a64_static_block_t block;
        arm_cpu_t cpu;
        arm_cpu_t before;
        unsigned completed = UINT_MAX;
        uint32_t flags;

        if (!branch_condition_flags(0u, true, &flags) ||
            !a64_static_decode_read_hits_bytes_at(
                bytes, 1u, true, pc, &block)) {
            fprintf(stderr,
                    "jitbench: Thumb decoded-branch contract setup failed\n");
            return false;
        }
        seed_cpu_at(&cpu, &insn, 1u, true, pc);
        cpu.cpsr = (cpu.cpsr & ~nzcv_mask) | flags;
        before = cpu;
        block.thumb_conditional_exit = false;
        if (a64_static_run_read_hits_decoded(
                &cpu, &block, g_ram, sizeof g_ram, &completed) ||
            memcmp(&before, &cpu, sizeof cpu) != 0 ||
            completed != UINT_MAX) {
            fprintf(stderr,
                    "jitbench: mutated Thumb conditional contract did not "
                    "fail closed\n");
            return false;
        }
    }

    if (cases != 28u) {
        fprintf(stderr,
                "jitbench: incomplete Thumb conditional oracle matrix\n");
        return false;
    }
    printf("STATIC-THUMB-COND-BRANCH-ORACLE exact=yes cases=28 "
           "conditions=all taken=yes fallthrough=yes backward=yes "
           "thumb-state=yes lr-preserved=yes contract=yes\n");
    return true;
}

static bool indirect_register_states_equal(const arm_cpu_t *reference,
                                           const arm_cpu_t *statik) {
    return reference && statik &&
           memcmp(reference->r, statik->r, sizeof reference->r) == 0 &&
           reference->cpsr == statik->cpsr &&
           reference->cycles == statik->cycles;
}

static bool run_static_indirect_case(bool thumb, bool link,
                                     unsigned condition, unsigned rm,
                                     uint32_t target, uint32_t flags,
                                     bool passed, unsigned *cases) {
    const uint32_t pc = thumb ? UINT32_C(0x1900) : UINT32_C(0x1800);
    const uint32_t nzcv_mask = ARM_CPSR_N | ARM_CPSR_Z |
                               ARM_CPSR_C | ARM_CPSR_V;
    uint32_t a32 = (condition << 28) | UINT32_C(0x012fff10) |
                   ((link ? 1u : 0u) << 5) | rm;
    uint16_t tinsn = (uint16_t)(UINT16_C(0x4700) |
                                ((link ? 1u : 0u) << 7) | (rm << 3));
    const void *program = thumb ? (const void *)&tinsn : (const void *)&a32;
    arm_cpu_t reference;
    arm_cpu_t statik;
    a64_static_block_t block;
    arm_status_t status;
    unsigned completed = UINT_MAX;
    uint32_t source;
    uint32_t expected_pc;
    uint32_t expected_lr;

    seed_cpu_at(&reference, program, 1u, thumb, pc);
    reference.cpsr = (reference.cpsr & ~nzcv_mask) | flags;
    if (rm != 15u) reference.r[rm] = target;
    source = rm == 15u ? pc + (thumb ? 4u : 8u) : target;
    expected_pc = passed ? source & ~UINT32_C(1) : pc + (thumb ? 2u : 4u);
    expected_lr = link && passed
        ? (pc + (thumb ? 2u : 4u)) | (thumb ? 1u : 0u)
        : reference.r[14];
    statik = reference;

    if (!a64_static_decode_read_hits_bytes_at(
            &g_ram[pc], 1u, thumb, pc, &block)) {
        fprintf(stderr,
                "jitbench: indirect oracle decode failed "
                "thumb=%u link=%u cond=%u rm=%u\n",
                thumb ? 1u : 0u, link ? 1u : 0u, condition, rm);
        return false;
    }
    status = arm_step(&reference);
    if (!a64_static_run_read_hits(
            &statik, &block, g_ram, sizeof g_ram, &completed) ||
        status != ARM_OK || completed != 1u ||
        !indirect_register_states_equal(&reference, &statik) ||
        statik.r[15] != expected_pc || statik.r[14] != expected_lr ||
        (((statik.cpsr & ARM_CPSR_T) != 0u) !=
         (passed ? (source & 1u) != 0u : thumb))) {
        fprintf(stderr,
                "jitbench: indirect oracle mismatch thumb=%u link=%u "
                "cond=%u rm=%u pass=%u pc=%08" PRIx32
                " lr=%08" PRIx32 " completed=%u\n",
                thumb ? 1u : 0u, link ? 1u : 0u, condition, rm,
                passed ? 1u : 0u, statik.r[15], statik.r[14], completed);
        return false;
    }
    if (cases) (*cases)++;
    return true;
}

static bool validate_static_indirect_branch_oracles(void) {
    const uint32_t targets[] = {UINT32_C(0x1a00), UINT32_C(0x1a01)};
    const uint32_t invalid_target = UINT32_C(0x1a02);
    unsigned cases = 0u;

    /* Cross every A32 condition/outcome with both destination states. BLX
     * reads LR here, which also proves the target is captured before LR is
     * replaced with the return address. The separate AL matrix below covers
     * every legal register, including BX pc. */
    for (unsigned link = 0u; link < 2u; link++) {
        const unsigned rm = link ? 14u : 0u;
        for (unsigned condition = 0u; condition < 15u; condition++) {
            const unsigned outcomes = condition == 14u ? 1u : 2u;
            for (unsigned outcome = 0u; outcome < outcomes; outcome++) {
                const bool passed = condition == 14u || outcome != 0u;
                uint32_t flags = 0u;
                if (condition != 14u &&
                    !branch_condition_flags(condition, passed, &flags)) {
                    fprintf(stderr,
                            "jitbench: no indirect NZCV witness cond=%u "
                            "pass=%u\n", condition, passed ? 1u : 0u);
                    return false;
                }
                for (unsigned target = 0u; target < 2u; target++) {
                    if (!run_static_indirect_case(
                            false, link != 0u, condition, rm,
                            targets[target], flags, passed, &cases))
                        return false;
                }
            }
        }
    }

    for (unsigned link = 0u; link < 2u; link++) {
        const unsigned registers = link ? 15u : 16u;
        for (unsigned rm = 0u; rm < registers; rm++) {
            const unsigned target_count = rm == 15u ? 1u : 2u;
            for (unsigned target = 0u; target < target_count; target++) {
                if (!run_static_indirect_case(
                        false, link != 0u, 14u, rm, targets[target],
                        ARM_CPSR_C, true, &cases))
                    return false;
            }
        }
    }

    for (unsigned link = 0u; link < 2u; link++) {
        const unsigned registers = link ? 15u : 16u;
        for (unsigned rm = 0u; rm < registers; rm++) {
            const unsigned target_count = rm == 15u ? 1u : 2u;
            for (unsigned target = 0u; target < target_count; target++) {
                if (!run_static_indirect_case(
                        true, link != 0u, 14u, rm, targets[target],
                        ARM_CPSR_C, true, &cases))
                    return false;
            }
        }
    }

    /* A 0b10 live target is a runtime refusal, not a partially executed
     * instruction. The signed path must report a zero prefix unchanged; the
     * caller's one interpreter step then reproduces ARM_UNDEFINED exactly. */
    for (unsigned thumb = 0u; thumb < 2u; thumb++) {
        for (unsigned link = 0u; link < 2u; link++) {
            const uint32_t pc = thumb ? UINT32_C(0x1c00)
                                      : UINT32_C(0x1b00);
            uint32_t a32 = UINT32_C(0xe12fff10) | (link << 5);
            uint16_t tinsn = (uint16_t)(UINT16_C(0x4700) | (link << 7));
            const void *program = thumb ? (const void *)&tinsn
                                        : (const void *)&a32;
            arm_cpu_t reference;
            arm_cpu_t statik;
            arm_cpu_t before;
            a64_static_block_t block;
            arm_status_t reference_status;
            arm_status_t static_status;
            unsigned completed = UINT_MAX;

            seed_cpu_at(&reference, program, 1u, thumb != 0u, pc);
            reference.r[0] = invalid_target;
            statik = reference;
            before = statik;
            if (!a64_static_decode_read_hits_bytes_at(
                    &g_ram[pc], 1u, thumb != 0u, pc, &block)) {
                fprintf(stderr, "jitbench: invalid indirect setup failed\n");
                return false;
            }
            reference_status = arm_step(&reference);
            if (!a64_static_run_read_hits(
                    &statik, &block, g_ram, sizeof g_ram, &completed) ||
                completed != 0u ||
                !indirect_register_states_equal(&before, &statik)) {
                fprintf(stderr,
                        "jitbench: invalid indirect target changed state "
                        "thumb=%u link=%u\n", thumb, link);
                return false;
            }
            static_status = arm_step(&statik);
            if (reference_status != ARM_UNDEFINED ||
                static_status != reference_status ||
                !indirect_register_states_equal(&reference, &statik)) {
                fprintf(stderr,
                        "jitbench: invalid indirect fallback diverged "
                        "thumb=%u link=%u\n", thumb, link);
                return false;
            }
        }
    }

    /* A failed condition retires without consulting an otherwise invalid
     * target and, for BLX, without touching LR. */
    for (unsigned link = 0u; link < 2u; link++) {
        uint32_t flags = 0u;
        if (!branch_condition_flags(0u, false, &flags) ||
            !run_static_indirect_case(
                false, link != 0u, 0u, 0u, invalid_target, flags,
                false, &cases))
            return false;
    }

    /* Cached decoded callers must fail closed if runtime-guard metadata is
     * corrupted; no register, flag, PC or cycle is allowed to move. */
    {
        const uint32_t pc = UINT32_C(0x1d00);
        const uint32_t insn = UINT32_C(0xe12fff10);
        arm_cpu_t cpu;
        arm_cpu_t before;
        a64_static_block_t block;
        unsigned completed = UINT_MAX;
        seed_cpu_at(&cpu, &insn, 1u, false, pc);
        cpu.r[0] = targets[1];
        before = cpu;
        if (!a64_static_decode_read_hits_bytes_at(
                &g_ram[pc], 1u, false, pc, &block))
            return false;
        block.uops[0].metadata ^= 1u;
        if (a64_static_run_read_hits_decoded(
                &cpu, &block, g_ram, sizeof g_ram, &completed) ||
            !indirect_register_states_equal(&before, &cpu) ||
            completed != UINT_MAX) {
            fprintf(stderr,
                    "jitbench: mutated indirect contract did not fail closed\n");
            return false;
        }
    }

    if (cases != 240u) {
        fprintf(stderr,
                "jitbench: incomplete indirect branch oracle matrix (%u)\n",
                cases);
        return false;
    }
    printf("STATIC-INDIRECT-BRANCH-ORACLE exact=yes cases=240 "
           "a32-conditions=yes registers=all arm-target=yes thumb-target=yes "
           "link=yes pc-source=yes invalid-rollback=yes contract=yes\n");
    return true;
}

static bool run_interpreter(const bench_case_t *bc, uint64_t total,
                            final_state_t *out, double *seconds) {
    arm_cpu_t cpu;
    arm_status_t status = ARM_OK;
    uint64_t i;
    double start, end;

    seed_cpu(&cpu, bc);
    start = now_seconds();
    for (i = 0; i < total; i++) {
        status = arm_step(&cpu);
        if (status != ARM_OK) break;
    }
    end = now_seconds();
    capture_state(out, &cpu, status, -1);
    *seconds = end - start;
    return i == total && status == ARM_OK && *seconds > 0.0;
}

static bool run_native(const bench_case_t *bc, const jit_buf_t *arena,
                       const jit_block_t *block, uint64_t blocks,
                       final_state_t *out, double *seconds) {
    arm_cpu_t cpu;
    uint64_t i;
    int exit_reason = JIT_EXIT_INTERPRET;
    double start, end;

    seed_cpu(&cpu, bc);
    start = now_seconds();
    for (i = 0; i < blocks; i++) {
        exit_reason = jit_enter(arena, block, &cpu);
        if (exit_reason != JIT_EXIT_NEXT) break;
    }
    end = now_seconds();
    capture_state(out, &cpu, ARM_OK, exit_reason);
    *seconds = end - start;
    return i == blocks && exit_reason == JIT_EXIT_NEXT && *seconds > 0.0;
}

static bool run_static(const bench_case_t *bc,
                       const a64_static_block_t *block, uint64_t blocks,
                       final_state_t *out, double *seconds) {
    arm_cpu_t cpu;
    double start, end;

    seed_cpu(&cpu, bc);
    start = now_seconds();
    bool ran = a64_static_run(&cpu, block, blocks, g_ram, sizeof g_ram);
    end = now_seconds();
    capture_state(out, &cpu, ARM_OK, JIT_EXIT_NEXT);
    *seconds = end - start;
    return ran && *seconds > 0.0;
}

static bool run_compact_raw(const bench_case_t *bc, uint64_t total,
                            final_state_t *out, double *seconds) {
    arm_cpu_t cpu;
    uint64_t done = 0u;
    double start, end;

    if (!bc || bc->thumb || bc->insns > UINT32_MAX / 4u) return false;
    seed_cpu(&cpu, bc);
    start = now_seconds();
    while (done < total) {
        uint64_t left = total - done;
        unsigned chunk = left > UINT_MAX ? UINT_MAX : (unsigned)left;
        unsigned completed = 0u;
        if (!a64_compact_raw_run(&cpu, g_ram, 0u, bc->insns * 4u,
                                 chunk, g_ram, sizeof g_ram, &completed) ||
            completed != chunk)
            break;
        done += completed;
    }
    end = now_seconds();
    capture_state(out, &cpu, ARM_OK, JIT_EXIT_NEXT);
    *seconds = end - start;
    return done == total && *seconds > 0.0;
}

static bool compact_raw_admission_supported(
        a64_compact_raw_admission_t admission) {
    return (unsigned)admission < (unsigned)A64_COMPACT_RAW_ADMITTED_COUNT;
}

static bool validate_compact_raw_admission_shapes(void) {
    typedef struct {
        uint32_t insn;
        bool thumb;
        a64_compact_raw_admission_t expected;
    } admission_case_t;
    static const admission_case_t CASES[] = {
        { UINT32_C(0xe2800001), false, A64_COMPACT_RAW_ADMIT_EXECUTE },
        /* EQ fails with the seeded Z=0. Decode must not inspect the SVC. */
        { UINT32_C(0x0f000000), false,
          A64_COMPACT_RAW_ADMIT_CONDITION_SKIP },
        { UINT32_C(0x00003001), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x0000b401), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x0000b255), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x0000bc01), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x0000c703), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x0000cf03), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x0000e801), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x0000f001), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x0000f801), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x0000b400), true,  A64_COMPACT_RAW_REJECT_THUMB },
        { UINT32_C(0x0000c600), true,  A64_COMPACT_RAW_REJECT_THUMB },
        { UINT32_C(0x0000c641), true,  A64_COMPACT_RAW_REJECT_THUMB },
        { UINT32_C(0x0000d000), true,
          A64_COMPACT_RAW_ADMIT_CONDITION_SKIP },
        { UINT32_C(0x0000d100), true,  A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0x00006008), true,
          A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT },
        { UINT32_C(0x00004710), true,  A64_COMPACT_RAW_REJECT_THUMB },
        { UINT32_C(0xf2800001), false, A64_COMPACT_RAW_REJECT_NV },
        { UINT32_C(0xe28f0001), false, A64_COMPACT_RAW_REJECT_DP_PC },
        { UINT32_C(0xe3000000), false,
          A64_COMPACT_RAW_REJECT_DP_TEST_WITHOUT_S },
        { UINT32_C(0xe0810312), false,
          A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0xe0810f12), false,
          A64_COMPACT_RAW_REJECT_DP_PC },
        { UINT32_C(0xe081031f), false,
          A64_COMPACT_RAW_REJECT_DP_RM_PC },
        { UINT32_C(0xe0810392), false,
          A64_COMPACT_RAW_REJECT_DP_REGISTER_SHIFT },
        { UINT32_C(0xe081000f), false,
          A64_COMPACT_RAW_REJECT_DP_RM_PC },
        { UINT32_C(0xe7970000), false,
          A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0xe597f000), false,
          A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0xe5970001), false,
          A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT },
        { UINT32_C(0xee100e10), false, A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0xee070f5a), false, A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0xee1d0f50), false, A64_COMPACT_RAW_ADMIT_EXECUTE },
        { UINT32_C(0xee072f90), false, A64_COMPACT_RAW_REJECT_CLASS },
        { VFP_UN_S(0, 0, 0, 1), false, A64_COMPACT_RAW_REJECT_VFP },
        { UINT32_C(0xef000000), false, A64_COMPACT_RAW_REJECT_CLASS },
    };
    arm_cpu_t cpu;
    uint32_t seed = UINT32_C(0xe2800001);

    seed_cpu_at(&cpu, &seed, 1u, false, UINT32_C(0x1000));
    for (unsigned i = 0u; i < sizeof CASES / sizeof CASES[0]; i++) {
        a64_compact_raw_admission_t actual =
            a64_compact_raw_classify_instruction(
                &cpu, CASES[i].insn, CASES[i].thumb);
        if (actual != CASES[i].expected) {
            fprintf(stderr,
                    "jitbench: compact raw admission case %u returned %u, "
                    "expected %u\n",
                    i, (unsigned)actual, (unsigned)CASES[i].expected);
            return false;
        }
    }
    cpu.cpsr = (cpu.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
    if (a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xee070f5a), false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xee1d0f50), false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xee1d0f70), false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xee100e10), false) !=
            A64_COMPACT_RAW_REJECT_CLASS ||
        a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xee0d0f70), false) !=
            A64_COMPACT_RAW_REJECT_CLASS ||
        a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xee1d0f90), false) !=
            A64_COMPACT_RAW_REJECT_CLASS) {
        fprintf(stderr,
                "jitbench: compact raw system-coprocessor privilege guard "
                "mismatch\n");
        return false;
    }
    cpu.cpsr = (cpu.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    cpu.r[0] = UINT32_C(0x1a01);
    if (a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xe12fff10), false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xe12fff30), false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE) {
        fprintf(stderr,
                "jitbench: compact raw A32 indirect admission refused\n");
        return false;
    }
    cpu.r[0] = UINT32_C(0x1a02);
    if (a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xe12fff10), false) !=
            A64_COMPACT_RAW_REJECT_CLASS ||
        a64_compact_raw_classify_instruction(
            &cpu, UINT32_C(0xe12fff3f), false) !=
            A64_COMPACT_RAW_REJECT_CLASS) {
        fprintf(stderr,
                "jitbench: compact raw A32 indirect guard admitted "
                "invalid target\n");
        return false;
    }
    cpu.r[4] = DATA_BASE + UINT32_C(0x800);
    if (a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,0,0,0,4,UINT32_C(0x8109)),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,1,0,0,1,0,4,UINT32_C(0x0110)),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,0,0,1,4,UINT32_C(0x4119)),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,1,0,0,1,1,4,UINT32_C(0x4109)),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,0,0,1,4,UINT32_C(0x8001)),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE) {
        fprintf(stderr,
                "jitbench: compact raw A32 block admission refused\n");
        return false;
    }
    if (a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(0,0,1,1,0,0,4,UINT32_C(0x0003)),
            false) != A64_COMPACT_RAW_ADMIT_CONDITION_SKIP ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,1,0,0,4,UINT32_C(0x0003)),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_FORM ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,0,0,0,15,UINT32_C(0x0003)),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_FORM ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,0,0,0,4,UINT32_C(0x0000)),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_FORM ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,0,1,0,4,UINT32_C(0x0011)),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_FORM ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,0,1,1,4,UINT32_C(0x0010)),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_FORM) {
        fprintf(stderr,
                "jitbench: compact raw A32 block shape guard admitted\n");
        return false;
    }
    cpu.r[4] = DATA_BASE + UINT32_C(0x801);
    if (a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,0,0,0,4,UINT32_C(0x0003)),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT) {
        fprintf(stderr,
                "jitbench: compact raw A32 block alignment admitted\n");
        return false;
    }
    cpu.r[4] = DATA_BASE + UINT32_C(0x3f8);
    if (a64_compact_raw_classify_instruction(
            &cpu, A32_BLOCK(14,0,1,0,0,0,4,UINT32_C(0x000f)),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT) {
        fprintf(stderr,
                "jitbench: compact raw A32 block cross-cache span admitted\n");
        return false;
    }
    cpu.r[4] = DATA_BASE + UINT32_C(0x800);
    cpu.r[1] = 8u;
    if (a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,0,1,1,0,0,1,15,0,0x100),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,0,1,1,1,0,1,4,0,0x001),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,0,0,1,0,0,1,4,0,0x004),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,0,0,1,0,1,1,4,0,0x004),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,1,1,1,0,0,1,4,0,0x061),
            false) != A64_COMPACT_RAW_ADMIT_EXECUTE) {
        fprintf(stderr,
                "jitbench: compact raw broad A32 single admission refused\n");
        return false;
    }
    if (a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,0,1,1,0,1,1,4,4,0x004),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_FORM ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,0,0,1,0,0,1,15,0,0x004),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_PC ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,1,1,1,0,0,1,4,0,0x010),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_FORM ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,1,1,1,0,0,1,4,0,0x00f),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_PC ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,0,1,1,1,0,1,4,15,0x000),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_PC ||
        a64_compact_raw_classify_instruction(
            &cpu, A32_SINGLE_MODE2(14,0,0,1,0,1,1,4,15,0x004),
            false) != A64_COMPACT_RAW_REJECT_MEMORY_PC) {
        fprintf(stderr,
                "jitbench: compact raw broad A32 single guard admitted\n");
        return false;
    }
    cpu.r[0] = UINT32_C(0x1a00);
    cpu.cp15.cpacr |= 0xfu << ARM_CPACR_CP10_SHIFT;
    cpu.vfp_fpexc = ARM_FPEXC_EN;
    cpu.vfp_fpscr = 0u;
    if (a64_compact_raw_classify_instruction(
            &cpu, VFP_UN_S(0, 0, 0, 1), false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, VFP_UN_S(4, 0, 0, 1), false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, VFP_READ_HITS[0], false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE ||
        a64_compact_raw_classify_instruction(
            &cpu, VSTM_WRITE_HITS[0], false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE) {
        fprintf(stderr, "jitbench: compact raw VFP admission refused\n");
        return false;
    }
    {
        const uint32_t arithmetic = VFP_ARITH_S(3,0,2,0,1);
        cpu.vfp_fpscr = ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_IXC |
                        ARM_FPSCR_N | ARM_FPSCR_C;
        static_vfp_arith_set_operands(
            &cpu, arithmetic, UINT32_C(0x3f800000),
            UINT32_C(0x40400000), UINT32_C(0x40a00000));
        if (a64_compact_raw_classify_instruction(
                &cpu, arithmetic, false) !=
            A64_COMPACT_RAW_ADMIT_EXECUTE) {
            fprintf(stderr,
                    "jitbench: compact raw VFP arithmetic admission "
                    "refused RunFast values\n");
            return false;
        }
        cpu.vfp_s[0] = UINT32_C(0x00000001);
        if (a64_compact_raw_classify_instruction(
                &cpu, arithmetic, false) != A64_COMPACT_RAW_REJECT_VFP) {
            fprintf(stderr,
                    "jitbench: compact raw VFP arithmetic admitted "
                    "subnormal input\n");
            return false;
        }
        cpu.vfp_s[0] = UINT32_C(0x40400000);
        cpu.vfp_fpscr |= ARM_FPSCR_IOC;
        if (a64_compact_raw_classify_instruction(
                &cpu, arithmetic, false) != A64_COMPACT_RAW_REJECT_VFP) {
            fprintf(stderr,
                    "jitbench: compact raw VFP arithmetic admitted "
                    "non-IXC sticky state\n");
            return false;
        }
    }
    {
        const uint32_t narrow = VFP_NARROW(15, 7);
        cpu.vfp_fpscr = ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_IXC |
                        ARM_FPSCR_N | ARM_FPSCR_C;
        vfp_set_d(&cpu, 7u, UINT64_C(0x3fd5555555555555));
        if (a64_compact_raw_classify_instruction(
                &cpu, narrow, false) != A64_COMPACT_RAW_ADMIT_EXECUTE) {
            fprintf(stderr,
                    "jitbench: compact raw VFP narrowing admission "
                    "refused audited RunFast value\n");
            return false;
        }
        cpu.vfp_fpscr &= ~ARM_FPSCR_IXC;
        if (a64_compact_raw_classify_instruction(
                &cpu, narrow, false) != A64_COMPACT_RAW_REJECT_VFP) {
            fprintf(stderr,
                    "jitbench: compact raw VFP narrowing admitted "
                    "non-sticky IXC state\n");
            return false;
        }
        cpu.vfp_fpscr |= ARM_FPSCR_IXC;
        vfp_set_d(&cpu, 7u, UINT64_C(0x7ff0000000000000));
        if (a64_compact_raw_classify_instruction(
                &cpu, narrow, false) != A64_COMPACT_RAW_REJECT_VFP) {
            fprintf(stderr,
                    "jitbench: compact raw VFP narrowing admitted "
                    "non-simple input\n");
            return false;
        }
    }
    cpu.vfp_fpscr = ARM_FPSCR_LEN;
    if (a64_compact_raw_classify_instruction(
            &cpu, VFP_UN_S(4, 0, 0, 1), false) !=
            A64_COMPACT_RAW_REJECT_VFP) {
        fprintf(stderr, "jitbench: compact raw VFP Len guard admitted\n");
        return false;
    }
    printf("COMPACT-RAW-ADMISSION-MODEL exact-shapes=yes cases=80 "
           "outcomes=13 condition-before-decode=yes machine-gates=excluded\n");
    return true;
}

static unsigned compact_raw_modeled_prefix(const uint32_t *program,
                                            unsigned insns, uint32_t pc,
                                            unsigned budget) {
    arm_cpu_t model;
    unsigned completed = 0u;

    seed_cpu_at(&model, program, insns, false, pc);
    while (completed < budget) {
        uint32_t current = model.r[15];
        uint64_t offset;
        a64_compact_raw_admission_t admission;

        if (current < pc || (current & 3u) != 0u) break;
        offset = (uint64_t)current - pc;
        if (offset + 4u > (uint64_t)insns * 4u) break;
        admission = a64_compact_raw_classify_instruction(
            &model, mem_r32(NULL, current), false);
        if (!compact_raw_admission_supported(admission)) break;
        if (arm_step(&model) != ARM_OK) break;
        completed++;
    }
    return completed;
}

static bool compact_raw_compare(const char *name, const uint32_t *program,
                                unsigned insns, uint32_t pc,
                                unsigned reference_steps, unsigned budget,
                                unsigned expected_completed) {
    arm_cpu_t reference, compact;
    final_state_t reference_state, compact_state;
    arm_status_t status = ARM_OK;
    unsigned completed = 0u;
    unsigned modeled;

    seed_cpu_at(&reference, program, insns, false, pc);
    for (unsigned i = 0u; i < reference_steps; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);

    modeled = compact_raw_modeled_prefix(program, insns, pc, budget);

    seed_cpu_at(&compact, program, insns, false, pc);
    if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, insns * 4u,
                             budget, g_ram, sizeof g_ram, &completed)) {
        fprintf(stderr, "jitbench: compact raw %s contract refused\n", name);
        return false;
    }
    capture_state(&compact_state, &compact, ARM_OK, JIT_EXIT_NEXT);
    if (status != ARM_OK || modeled != expected_completed ||
        completed != modeled ||
        !architectural_states_equal(&reference_state, &compact_state)) {
        fprintf(stderr,
                "jitbench: compact raw %s mismatch (completed/model/expected "
                "%u/%u/%u, reference steps %u)\n",
                name, completed, modeled, expected_completed,
                reference_steps);
        return false;
    }
    return true;
}

static bool compact_raw_window_compare(const char *name,
                                       const uint32_t *program,
                                       unsigned insns, uint32_t pc,
                                       unsigned reference_steps,
                                       unsigned budget,
                                       unsigned expected_completed) {
    arm_cpu_t reference, window;
    final_state_t reference_state, window_state;
    arm_status_t status = ARM_OK;
    unsigned completed = 0u;

    seed_cpu_at(&reference, program, insns, false, pc);
    for (unsigned i = 0u; i < reference_steps; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);

    seed_cpu_at(&window, program, insns, false, pc);
    window.cp15.sctlr |= ARM_SCTLR_M;
    if (!a64_compact_raw_run_code_window(
            &window, &g_ram[pc], pc, insns * 4u, budget, &completed)) {
        fprintf(stderr,
                "jitbench: compact raw code-window %s contract refused\n",
                name);
        return false;
    }
    capture_state(&window_state, &window, ARM_OK, JIT_EXIT_NEXT);
    if (status != ARM_OK || completed != expected_completed ||
        !architectural_states_equal(&reference_state, &window_state)) {
        fprintf(stderr,
                "jitbench: compact raw code-window %s mismatch "
                "(completed %u/%u, reference steps %u)\n",
                name, completed, expected_completed, reference_steps);
        return false;
    }
    return true;
}

static void seed_compact_raw_thumb(arm_cpu_t *cpu, const uint16_t *program,
                                   unsigned insns, uint32_t pc) {
    seed_cpu_at(cpu, program, insns, true, pc);
    cpu->r[0] = UINT32_C(0x80000001);
    cpu->r[1] = 0u;
    cpu->r[2] = 1u;
    cpu->r[3] = 31u;
    cpu->r[4] = 32u;
    cpu->r[5] = 33u;
    cpu->r[6] = DATA_BASE;
    cpu->r[7] = DATA_BASE;
    mem_w32(NULL, DATA_BASE, UINT32_C(0x8001ff80));
}

static bool compact_raw_thumb_instruction_compare(uint16_t insn,
                                                  uint32_t pc) {
    const uint16_t program[2] = {
        insn,
        UINT16_C(0xba00), /* unsupported REV sentinel */
    };
    arm_cpu_t reference, compact;
    final_state_t reference_state, compact_state;
    arm_status_t status;
    unsigned completed = 0u;

    seed_compact_raw_thumb(&reference, program, 2u, pc);
    status = arm_step(&reference);
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);

    seed_compact_raw_thumb(&compact, program, 2u, pc);
    if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                             g_ram, sizeof g_ram, &completed)) {
        fprintf(stderr,
                "jitbench: compact raw Thumb 0x%04x contract refused\n",
                (unsigned)insn);
        return false;
    }
    capture_state(&compact_state, &compact, ARM_OK, JIT_EXIT_NEXT);
    if (status != ARM_OK || completed != 1u ||
        !architectural_states_equal(&reference_state, &compact_state)) {
        fprintf(stderr,
                "jitbench: compact raw Thumb 0x%04x mismatch "
                "(completed %u, status %u)\n",
                (unsigned)insn, completed, (unsigned)status);
        return false;
    }
    return true;
}

static bool compact_raw_thumb_program_compare(
        const char *name, const uint16_t *program, unsigned insns,
        uint32_t pc, unsigned reference_steps, unsigned budget,
        unsigned expected_completed) {
    arm_cpu_t reference;
    arm_cpu_t compact;
    final_state_t reference_state;
    final_state_t compact_state;
    arm_status_t status = ARM_OK;
    unsigned completed = UINT_MAX;

    seed_compact_raw_thumb(&reference, program, insns, pc);
    for (unsigned i = 0u; i < reference_steps; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);

    seed_compact_raw_thumb(&compact, program, insns, pc);
    if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, insns * 2u,
                             budget, g_ram, sizeof g_ram, &completed)) {
        fprintf(stderr,
                "jitbench: compact raw Thumb program %s contract refused\n",
                name);
        return false;
    }
    capture_state(&compact_state, &compact, ARM_OK, JIT_EXIT_NEXT);
    if (status != ARM_OK || completed != expected_completed ||
        !architectural_states_equal(&reference_state, &compact_state)) {
        fprintf(stderr,
                "jitbench: compact raw Thumb program %s mismatch "
                "(completed %u/%u, status=%u)\n",
                name, completed, expected_completed, (unsigned)status);
        return false;
    }
    return true;
}

static uint32_t compact_raw_a32_register_shift_instruction(
        unsigned opcode, bool set_flags, unsigned rn, unsigned rd,
        unsigned rm, unsigned rs, unsigned type) {
    return UINT32_C(0xe0000010) | (opcode << 21) |
           ((set_flags ? 1u : 0u) << 20) | (rn << 16) | (rd << 12) |
           (rs << 8) | (type << 5) | rm;
}

static void seed_compact_raw_a32_register_shift(
        arm_cpu_t *cpu, uint32_t insn, uint32_t pc) {
    seed_cpu_at(cpu, &insn, 1u, false, pc);
    for (unsigned reg = 0u; reg < 15u; reg++)
        cpu->r[reg] = UINT32_C(0x61000000) |
                      (reg * UINT32_C(0x010203));
    cpu->r[15] = pc;
}

static bool compact_raw_a32_register_shift_exact_case(
        const char *name, const arm_cpu_t *initial, unsigned opcode,
        bool set_flags, unsigned type, unsigned amount, unsigned carry) {
    arm_cpu_t reference = *initial;
    arm_cpu_t compact = *initial;
    final_state_t reference_state;
    final_state_t compact_state;
    arm_status_t status;
    unsigned completed = UINT_MAX;
    const uint32_t pc = initial->r[15];

    status = arm_step(&reference);
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);
    if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                             g_ram, sizeof g_ram, &completed)) {
        fprintf(stderr,
                "jitbench: compact raw A32 register shift %s contract "
                "refused\n", name);
        return false;
    }
    capture_state(&compact_state, &compact, ARM_OK, JIT_EXIT_NEXT);
    if (status != ARM_OK || completed != 1u ||
        !architectural_states_equal(&reference_state, &compact_state)) {
        fprintf(stderr,
                "jitbench: compact raw A32 register shift %s mismatch "
                "opcode=%u S=%u type=%u amount=%u carry=%u "
                "completed=%u status=%u\n",
                name, opcode, set_flags ? 1u : 0u, type, amount, carry,
                completed, (unsigned)status);
        return false;
    }
    return true;
}

static bool compact_raw_a32_register_shift_refusal_case(
        const char *name, uint32_t insn, uint32_t pc) {
    arm_cpu_t reference;
    arm_cpu_t compact;
    arm_cpu_t before;
    arm_status_t reference_status;
    arm_status_t compact_status;
    unsigned completed = UINT_MAX;

    seed_compact_raw_a32_register_shift(&reference, insn, pc);
    compact = reference;
    before = compact;
    reference_status = arm_step(&reference);
    if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                             g_ram, sizeof g_ram, &completed) ||
        completed != 0u || memcmp(&before, &compact, sizeof compact) != 0) {
        fprintf(stderr,
                "jitbench: compact raw A32 register shift refusal %s "
                "changed state (completed=%u)\n", name, completed);
        return false;
    }
    compact_status = arm_step(&compact);
    if (reference_status != ARM_UNDEFINED ||
        compact_status != reference_status ||
        memcmp(&reference, &compact, sizeof compact) != 0) {
        fprintf(stderr,
                "jitbench: compact raw A32 register shift refusal %s "
                "fallback diverged (status=%u/%u)\n", name,
                (unsigned)reference_status, (unsigned)compact_status);
        return false;
    }
    return true;
}

static bool validate_compact_raw_a32_register_shift_oracles(void) {
    static const unsigned RESULT_OPS[] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 12u, 13u, 14u, 15u,
    };
    static const unsigned TEST_OPS[] = {8u, 9u, 10u, 11u};
    static const unsigned AMOUNTS[] = {
        0u, 1u, 7u, 31u, 32u, 33u, 64u, 255u,
    };
    typedef struct {
        const char *name;
        unsigned opcode;
        bool set_flags;
        unsigned rn;
        unsigned rd;
        unsigned rm;
        unsigned rs;
        unsigned type;
        uint32_t r[4];
    } alias_case_t;
    static const alias_case_t ALIASES[] = {
        {"rd-rs", 13u, true, 0u, 3u, 2u, 3u, 0u,
         {UINT32_C(0x7fffffff), UINT32_C(0x13579bdf),
          UINT32_C(0x80000001), UINT32_C(0x00000001)}},
        {"rd-rm", 4u, true, 0u, 2u, 2u, 3u, 1u,
         {UINT32_C(0x7fffffff), UINT32_C(0x13579bdf),
          UINT32_C(0x80000001), UINT32_C(0x00000001)}},
        {"rd-rn", 2u, false, 1u, 1u, 2u, 3u, 2u,
         {UINT32_C(0x2468ace0), UINT32_C(0x7fffffff),
          UINT32_C(0x80000001), UINT32_C(0x0000001f)}},
        {"rs-rm", 12u, true, 0u, 1u, 2u, 2u, 3u,
         {UINT32_C(0x7fffffff), UINT32_C(0x13579bdf),
          UINT32_C(0x80000001), UINT32_C(0x2468ace0)}},
        {"rn-rs", 5u, true, 3u, 1u, 2u, 3u, 0u,
         {UINT32_C(0x2468ace0), UINT32_C(0x13579bdf),
          UINT32_C(0x80000001), UINT32_C(0x7fffff01)}},
    };
    const uint32_t pc = UINT32_C(0x7c00);
    const uint32_t nzcv_mask = ARM_CPSR_N | ARM_CPSR_Z |
                               ARM_CPSR_C | ARM_CPSR_V;
    unsigned cases = 0u;

    for (unsigned group = 0u; group < 2u; group++) {
        const unsigned *ops = group == 0u ? RESULT_OPS : TEST_OPS;
        const unsigned op_count = group == 0u
            ? (unsigned)(sizeof RESULT_OPS / sizeof RESULT_OPS[0])
            : (unsigned)(sizeof TEST_OPS / sizeof TEST_OPS[0]);
        for (unsigned op_index = 0u; op_index < op_count; op_index++) {
            const unsigned opcode = ops[op_index];
            const unsigned first_s = group == 0u ? 0u : 1u;
            for (unsigned set_flags = first_s; set_flags < 2u; set_flags++) {
                for (unsigned type = 0u; type < 4u; type++) {
                    for (unsigned amount_index = 0u;
                         amount_index < sizeof AMOUNTS / sizeof AMOUNTS[0];
                         amount_index++) {
                        const unsigned amount = AMOUNTS[amount_index];
                        for (unsigned carry = 0u; carry < 2u; carry++) {
                            const uint32_t insn =
                                compact_raw_a32_register_shift_instruction(
                                    opcode, set_flags != 0u, 0u, 1u,
                                    2u, 3u, type);
                            arm_cpu_t initial;

                            seed_compact_raw_a32_register_shift(
                                &initial, insn, pc);
                            initial.r[0] = UINT32_C(0x7fffffff);
                            initial.r[1] = UINT32_C(0x13579bdf);
                            initial.r[2] = UINT32_C(0x80000001);
                            initial.r[3] = UINT32_C(0x5a5a0000) | amount;
                            initial.cpsr =
                                (initial.cpsr & ~nzcv_mask) |
                                ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_V |
                                (carry ? ARM_CPSR_C : 0u);
                            if (!compact_raw_a32_register_shift_exact_case(
                                    "matrix", &initial, opcode,
                                    set_flags != 0u, type, amount, carry))
                                return false;
                            cases++;
                        }
                    }
                }
            }
        }
    }

    for (unsigned i = 0u; i < sizeof ALIASES / sizeof ALIASES[0]; i++) {
        const alias_case_t *ac = &ALIASES[i];
        const uint32_t insn = compact_raw_a32_register_shift_instruction(
            ac->opcode, ac->set_flags, ac->rn, ac->rd,
            ac->rm, ac->rs, ac->type);
        arm_cpu_t initial;

        seed_compact_raw_a32_register_shift(&initial, insn, pc);
        for (unsigned reg = 0u; reg < 4u; reg++) initial.r[reg] = ac->r[reg];
        initial.cpsr = (initial.cpsr & ~nzcv_mask) |
                       ARM_CPSR_N | ARM_CPSR_V | ARM_CPSR_C;
        if (!compact_raw_a32_register_shift_exact_case(
                ac->name, &initial, ac->opcode, ac->set_flags,
                ac->type, initial.r[ac->rs] & 255u, 1u))
            return false;
        cases++;
    }

    if (cases != 1797u) {
        fprintf(stderr,
                "jitbench: incomplete compact raw A32 register shift "
                "matrix (%u)\n", cases);
        return false;
    }
    if (!compact_raw_a32_register_shift_refusal_case(
            "rn-pc", compact_raw_a32_register_shift_instruction(
                4u, true, 15u, 1u, 2u, 3u, 0u), pc) ||
        !compact_raw_a32_register_shift_refusal_case(
            "rd-pc", compact_raw_a32_register_shift_instruction(
                4u, true, 0u, 15u, 2u, 3u, 1u), pc) ||
        !compact_raw_a32_register_shift_refusal_case(
            "rm-pc", compact_raw_a32_register_shift_instruction(
                4u, true, 0u, 1u, 15u, 3u, 2u), pc) ||
        !compact_raw_a32_register_shift_refusal_case(
            "rs-pc", compact_raw_a32_register_shift_instruction(
                4u, true, 0u, 1u, 2u, 15u, 3u), pc))
        return false;

    printf("COMPACT-RAW-A32-REGISTER-SHIFT-ORACLE exact=yes cases=1797 "
           "refusals=4 opcodes=all result-flags=both types=all "
           "amounts=0-1-7-31-32-33-64-255 amount-low8=yes carry=both "
           "aliases=rd-rs-rd-rm-rd-rn-rs-rm-rn-rs pc-guards=all "
           "runtime-codegen=no\n");
    return true;
}

static bool run_compact_raw_a32_indirect_case(
        bool link, unsigned condition, unsigned rm, uint32_t target,
        uint32_t flags, bool passed, unsigned *cases) {
    const uint32_t pc = UINT32_C(0x7600);
    const uint32_t nzcv_mask = ARM_CPSR_N | ARM_CPSR_Z |
                               ARM_CPSR_C | ARM_CPSR_V;
    const uint32_t insn = (condition << 28) | UINT32_C(0x012fff10) |
                          ((link ? 1u : 0u) << 5) | rm;
    arm_cpu_t reference;
    arm_cpu_t compact;
    arm_status_t status;
    unsigned completed = UINT_MAX;
    uint32_t source;
    uint32_t expected_pc;
    uint32_t expected_lr;

    seed_cpu_at(&reference, &insn, 1u, false, pc);
    reference.cpsr = (reference.cpsr & ~nzcv_mask) | flags;
    if (rm != 15u) reference.r[rm] = target;
    source = rm == 15u ? pc + 8u : target;
    expected_pc = passed ? source & ~UINT32_C(1) : pc + 4u;
    expected_lr = link && passed ? pc + 4u : reference.r[14];
    compact = reference;

    status = arm_step(&reference);
    if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                             g_ram, sizeof g_ram, &completed) ||
        status != ARM_OK || completed != 1u ||
        !indirect_register_states_equal(&reference, &compact) ||
        compact.r[15] != expected_pc || compact.r[14] != expected_lr ||
        (((compact.cpsr & ARM_CPSR_T) != 0u) !=
         (passed ? (source & 1u) != 0u : false))) {
        fprintf(stderr,
                "jitbench: compact raw A32 indirect mismatch "
                "link=%u cond=%u rm=%u pass=%u pc=%08" PRIx32
                " lr=%08" PRIx32 " completed=%u status=%u\n",
                link ? 1u : 0u, condition, rm, passed ? 1u : 0u,
                compact.r[15], compact.r[14], completed,
                (unsigned)status);
        return false;
    }
    if (cases) (*cases)++;
    return true;
}

static bool validate_compact_raw_a32_indirect_oracles(void) {
    static const uint32_t TARGETS[] = {
        UINT32_C(0x7a00), UINT32_C(0x7a01),
    };
    const uint32_t invalid_target = UINT32_C(0x7a02);
    unsigned cases = 0u;

    /* This crosses every ordinary condition result with both destination
     * states. BLX reads LR as its target, proving the source is captured
     * before the architectural return address replaces LR. */
    for (unsigned link = 0u; link < 2u; link++) {
        const unsigned rm = link ? 14u : 0u;
        for (unsigned condition = 0u; condition < 15u; condition++) {
            const unsigned outcomes = condition == 14u ? 1u : 2u;
            for (unsigned outcome = 0u; outcome < outcomes; outcome++) {
                const bool passed = condition == 14u || outcome != 0u;
                uint32_t flags = 0u;
                if (condition != 14u &&
                    !branch_condition_flags(condition, passed, &flags)) {
                    fprintf(stderr,
                            "jitbench: no compact indirect NZCV witness "
                            "cond=%u pass=%u\n",
                            condition, passed ? 1u : 0u);
                    return false;
                }
                for (unsigned target = 0u; target < 2u; target++) {
                    if (!run_compact_raw_a32_indirect_case(
                            link != 0u, condition, rm, TARGETS[target],
                            flags, passed, &cases))
                        return false;
                }
            }
        }
    }

    /* BX accepts all sixteen source registers; BLX(register) accepts r0-r14.
     * Cross each ordinary source with ARM and Thumb targets, plus BX pc. */
    for (unsigned link = 0u; link < 2u; link++) {
        const unsigned registers = link ? 15u : 16u;
        for (unsigned rm = 0u; rm < registers; rm++) {
            const unsigned target_count = rm == 15u ? 1u : 2u;
            for (unsigned target = 0u; target < target_count; target++) {
                if (!run_compact_raw_a32_indirect_case(
                        link != 0u, 14u, rm, TARGETS[target],
                        ARM_CPSR_C, true, &cases))
                    return false;
            }
        }
    }

    /* Low bits 0b10 are neither a valid ARM word nor a Thumb halfword. The
     * compact engine must return a zero prefix without changing any state;
     * the caller's interpreter step then reproduces ARM_UNDEFINED exactly. */
    for (unsigned invalid_case = 0u; invalid_case < 3u; invalid_case++) {
        const bool link = invalid_case != 0u;
        const unsigned rm = invalid_case == 2u ? 15u : 0u;
        const uint32_t pc = UINT32_C(0x7800) + invalid_case * 4u;
        const uint32_t insn = UINT32_C(0xe12fff10) |
                              ((link ? 1u : 0u) << 5) | rm;
        arm_cpu_t reference;
        arm_cpu_t compact;
        arm_cpu_t before;
        arm_status_t reference_status;
        arm_status_t compact_status;
        unsigned completed = UINT_MAX;

        seed_cpu_at(&reference, &insn, 1u, false, pc);
        if (rm != 15u) reference.r[rm] = invalid_target;
        compact = reference;
        before = compact;
        reference_status = arm_step(&reference);
        if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                                 g_ram, sizeof g_ram, &completed) ||
            completed != 0u ||
            !indirect_register_states_equal(&before, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw invalid A32 indirect changed "
                    "state link=%u rm=%u completed=%u\n",
                    link ? 1u : 0u, rm, completed);
            return false;
        }
        compact_status = arm_step(&compact);
        if (reference_status != ARM_UNDEFINED ||
            compact_status != reference_status ||
            !indirect_register_states_equal(&reference, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw invalid A32 indirect fallback "
                    "diverged link=%u rm=%u\n", link ? 1u : 0u, rm);
            return false;
        }
    }

    /* Condition evaluation precedes every runtime target guard. A failed EQ
     * therefore retires even when the unobserved target would be invalid. */
    for (unsigned link = 0u; link < 2u; link++) {
        uint32_t flags = 0u;
        if (!branch_condition_flags(0u, false, &flags) ||
            !run_compact_raw_a32_indirect_case(
                link != 0u, 0u, 0u, invalid_target, flags, false, NULL))
            return false;
    }
    {
        uint32_t flags = 0u;
        if (!branch_condition_flags(0u, false, &flags) ||
            !run_compact_raw_a32_indirect_case(
                true, 0u, 15u, 0u, flags, false, NULL))
            return false;
    }

    if (cases != 177u) {
        fprintf(stderr,
                "jitbench: incomplete compact raw A32 indirect matrix "
                "(%u)\n", cases);
        return false;
    }
    printf("COMPACT-RAW-A32-INDIRECT-ORACLE exact=yes cases=177 "
           "conditions=all registers=all arm-target=yes thumb-target=yes "
           "link=yes pc-source=yes invalid-rollback=yes "
           "condition-before-guard=yes runtime-codegen=no\n");
    return true;
}

static uint16_t thumb_high_instruction(unsigned operation, unsigned rd,
                                       unsigned rm) {
    return (uint16_t)(UINT16_C(0x4400) | (operation << 8) |
                      ((rd & 8u) << 4) | ((rm & 8u) << 3) |
                      ((rm & 7u) << 3) | (rd & 7u));
}

static bool validate_compact_raw_thumb_oracles(void) {
    static const unsigned SHIFT_AMOUNTS[] = {0u, 1u, 7u, 31u};
    static const unsigned REGISTER_AMOUNTS[] = {1u, 2u, 3u, 4u, 5u};
    static const unsigned SHIFT_OPS[] = {2u, 3u, 4u, 7u};
    unsigned cases = 0u;

#define THUMB_ORACLE(insn_)                                                   \
    do {                                                                      \
        if (!compact_raw_thumb_instruction_compare((uint16_t)(insn_),         \
                                                    UINT32_C(0x7000)))         \
            return false;                                                     \
        cases++;                                                              \
    } while (0)

    for (unsigned type = 0u; type < 3u; type++) {
        for (unsigned i = 0u;
             i < sizeof SHIFT_AMOUNTS / sizeof SHIFT_AMOUNTS[0]; i++) {
            THUMB_ORACLE((type << 11) | (SHIFT_AMOUNTS[i] << 6) |
                         (0u << 3) | 5u);
        }
    }

    for (unsigned immediate = 0u; immediate < 2u; immediate++) {
        for (unsigned subtract = 0u; subtract < 2u; subtract++) {
            THUMB_ORACLE(UINT16_C(0x1800) | (immediate << 10) |
                         (subtract << 9) | (2u << 6) | (0u << 3) | 5u);
        }
    }

    for (unsigned operation = 0u; operation < 4u; operation++)
        THUMB_ORACLE(UINT16_C(0x2000) | (operation << 11) |
                     (5u << 8) | 0x81u);

    for (unsigned operation = 0u; operation < 16u; operation++)
        THUMB_ORACLE(UINT16_C(0x4000) | (operation << 6) |
                     (2u << 3) | 0u);

    for (unsigned op = 0u; op < sizeof SHIFT_OPS / sizeof SHIFT_OPS[0]; op++) {
        for (unsigned i = 0u;
             i < sizeof REGISTER_AMOUNTS / sizeof REGISTER_AMOUNTS[0]; i++) {
            THUMB_ORACLE(UINT16_C(0x4000) | (SHIFT_OPS[op] << 6) |
                         (REGISTER_AMOUNTS[i] << 3) | 0u);
        }
    }

    THUMB_ORACLE(thumb_high_instruction(0u, 8u, 9u));
    THUMB_ORACLE(thumb_high_instruction(1u, 15u, 8u));
    THUMB_ORACLE(thumb_high_instruction(2u, 10u, 15u));
    THUMB_ORACLE(UINT16_C(0x4801) | (5u << 8));

    for (unsigned operation = 0u; operation < 8u; operation++)
        THUMB_ORACLE(UINT16_C(0x5000) | (operation << 9) |
                     (1u << 6) | (6u << 3) | 0u);

    for (unsigned byte = 0u; byte < 2u; byte++) {
        for (unsigned load = 0u; load < 2u; load++)
            THUMB_ORACLE(UINT16_C(0x6000) | (byte << 12) | (load << 11) |
                         (1u << 6) | (6u << 3) | 0u);
    }
    for (unsigned load = 0u; load < 2u; load++)
        THUMB_ORACLE(UINT16_C(0x8000) | (load << 11) |
                     (1u << 6) | (6u << 3) | 0u);
    for (unsigned load = 0u; load < 2u; load++)
        THUMB_ORACLE(UINT16_C(0x9000) | (load << 11) | 1u);

    THUMB_ORACLE(UINT16_C(0xa001));
    THUMB_ORACLE(UINT16_C(0xa801));
    THUMB_ORACLE(UINT16_C(0xb001));
    THUMB_ORACLE(UINT16_C(0xb081));

    for (unsigned operation = 0u; operation < 4u; operation++)
        THUMB_ORACLE(UINT16_C(0xb200) | (operation << 6) |
                     (2u << 3) | 5u);

    THUMB_ORACLE(UINT16_C(0xb405)); /* PUSH {r0,r2} */
    THUMB_ORACLE(UINT16_C(0xb500)); /* PUSH {lr} */
    THUMB_ORACLE(UINT16_C(0xb5a5)); /* PUSH {r0,r2,r5,r7,lr} */
    THUMB_ORACLE(UINT16_C(0xbc05)); /* POP {r0,r2} */
    THUMB_ORACLE(UINT16_C(0xbd00)); /* POP {pc} */
    THUMB_ORACLE(UINT16_C(0xbda5)); /* POP {r0,r2,r5,r7,pc} */

    THUMB_ORACLE(UINT16_C(0xc603)); /* STMIA r6!,{r0,r1} */
    THUMB_ORACLE(UINT16_C(0xc6c0)); /* STMIA r6!,{r6,r7} */
    THUMB_ORACLE(UINT16_C(0xce03)); /* LDMIA r6!,{r0,r1} */
    THUMB_ORACLE(UINT16_C(0xcec0)); /* LDMIA r6!,{r6,r7} */

    for (unsigned condition = 0u; condition < 14u; condition++)
        THUMB_ORACLE(UINT16_C(0xd001) | (condition << 8));
    THUMB_ORACLE(UINT16_C(0xe001));
    THUMB_ORACLE(UINT16_C(0xe801)); /* BLX suffix */
    THUMB_ORACLE(UINT16_C(0xf001)); /* BL/BLX prefix */
    THUMB_ORACLE(UINT16_C(0xf801)); /* BL suffix */
    THUMB_ORACLE(UINT16_C(0x4730)); /* BX r6: Thumb -> A32 */
    THUMB_ORACLE(UINT16_C(0x4710)); /* BX r2: remain Thumb */
    THUMB_ORACLE(UINT16_C(0x47b0)); /* BLX r6 */
    THUMB_ORACLE(UINT16_C(0x4778)); /* BX pc */

#undef THUMB_ORACLE

    {
        const uint16_t bl_pair[] = {
            UINT16_C(0xf000), UINT16_C(0xf801),
        };
        const uint16_t blx_pair[] = {
            UINT16_C(0xf000), UINT16_C(0xe801),
        };
        if (!compact_raw_thumb_program_compare(
                "bl-pair", bl_pair, 2u, UINT32_C(0x7200), 2u, 2u, 2u) ||
            !compact_raw_thumb_program_compare(
                "blx-pair", blx_pair, 2u, UINT32_C(0x7300), 2u, 2u, 2u))
            return false;
    }

    printf("COMPACT-RAW-THUMB-ORACLE exact=yes cases=%u "
           "immediate-shifts=boundaries register-shifts=0-1-31-32-33 "
           "alu=all high-register=yes extend=yes memory-kinds=all "
           "stack-multiple=yes branches=all-conditions "
           "long-branch-pairs=2 state-switch=yes\n",
           cases);
    return true;
}

typedef struct {
    arm_cpu_t *cpu;
    arm_status_t status;
    unsigned calls;
    unsigned fast_refills;
    unsigned stop_after;
    const uint8_t *code;
    uint32_t code_base;
    uint32_t code_bytes;
    uint32_t active_block;
    bool refuse_window;
    bool fast_refill_window;
    unsigned omit_window_after;
} compact_raw_resident_oracle_context_t;

static a64_compact_raw_fallback_result_t compact_raw_resident_oracle_step(
        void *opaque, a64_compact_raw_code_window_t *next_window) {
    compact_raw_resident_oracle_context_t *context =
        (compact_raw_resident_oracle_context_t *)opaque;
    uint32_t block;
    uint32_t offset;

    if (!context || !context->cpu || !next_window)
        return A64_COMPACT_RAW_FALLBACK_NO_RETIRE;
    block = context->cpu->r[15] & ~UINT32_C(0x3ff);
    offset = block - context->code_base;
    if (context->fast_refill_window && block != context->active_block) {
        if (!context->code ||
            (context->cpu->r[15] &
             ((context->cpu->cpsr & ARM_CPSR_T) ? 1u : 3u)) != 0u ||
            offset > context->code_bytes ||
            context->code_bytes - offset < UINT32_C(0x400))
            return A64_COMPACT_RAW_FALLBACK_NO_RETIRE;
        next_window->code = context->code + offset;
        next_window->code_base = block;
        next_window->code_bytes = UINT32_C(0x400);
        context->active_block = block;
        context->fast_refills++;
        return A64_COMPACT_RAW_FALLBACK_NO_RETIRE_CONTINUE;
    }
    context->status = arm_step(context->cpu);
    if (context->status != ARM_OK)
        return A64_COMPACT_RAW_FALLBACK_NO_RETIRE;
    context->calls++;
    if (context->stop_after && context->calls >= context->stop_after)
        return A64_COMPACT_RAW_FALLBACK_RETIRE_STOP;
    if (context->refuse_window)
        return A64_COMPACT_RAW_FALLBACK_RETIRE_STOP;
    if (context->omit_window_after &&
        context->calls >= context->omit_window_after)
        return A64_COMPACT_RAW_FALLBACK_RETIRE_CONTINUE;
    block = context->cpu->r[15] & ~UINT32_C(0x3ff);
    offset = block - context->code_base;
    if (!context->code ||
        (context->cpu->r[15] &
         ((context->cpu->cpsr & ARM_CPSR_T) ? 1u : 3u)) != 0u ||
        offset > context->code_bytes ||
        context->code_bytes - offset < UINT32_C(0x400))
        return A64_COMPACT_RAW_FALLBACK_RETIRE_STOP;
    next_window->code = context->code + offset;
    next_window->code_base = block;
    next_window->code_bytes = UINT32_C(0x400);
    context->active_block = block;
    return A64_COMPACT_RAW_FALLBACK_RETIRE_CONTINUE;
}

static bool compact_raw_resident_compare(
        const char *name, const void *program, unsigned insns, bool thumb,
        uint32_t pc, uint32_t initial_code_base,
        unsigned initial_code_bytes, unsigned reference_steps, unsigned budget,
        unsigned expected_native, unsigned expected_fallback,
        unsigned stop_after, bool stale_write_witness, bool refuse_window,
        unsigned omit_window_after, bool fast_refill_window,
        unsigned expected_fast_refills, bool user_mode,
        bool window_cache_enabled,
        uint64_t expected_window_cache_hits) {
    arm_cpu_t reference, resident;
    final_state_t reference_state, resident_state;
    compact_raw_resident_oracle_context_t context;
    uint8_t *baseline = (uint8_t *)malloc(sizeof g_ram);
    uint8_t *expected_ram = (uint8_t *)malloc(sizeof g_ram);
    arm_status_t status = ARM_OK;
    unsigned completed = 0u;
    unsigned native_completed = 0u;
    unsigned fallback_completed = 0u;
    uint64_t window_cache_hits = 0u;
    bool ok = false;
    arm_bus_t write_bus = g_bus;

    if (!baseline || !expected_ram) {
        fprintf(stderr,
                "jitbench: compact raw resident oracle allocation failed\n");
        goto done;
    }

    if (thumb)
        seed_compact_raw_thumb(&reference, (const uint16_t *)program,
                               insns, pc);
    else
        seed_cpu_at(&reference, program, insns, false, pc);
    if (user_mode) {
        reference.cpsr =
            (reference.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_USR;
    }
    resident = reference;
    /* The generic synthetic bus neither populates DREAD nor grants the live
     * frontend write consent required to expose DWRITE. The Thumb cache-path
     * case makes both witnesses and that consent explicit. Its leading REV
     * remains unsupported natively, so fallback, Thumb window publication,
     * and native continuation are still exercised before the memory body. */
    if (thumb) {
        write_bus.host_ram_write = mem_host_ram;
        resident.bus = &write_bus;
        oracle_warm_dread(&resident, DATA_BASE);
        oracle_warm_dwrite(&resident, DATA_BASE, true);
    }
    /* A derived DWRITE entry without the separate live frontend callback is
     * deliberately insufficient. This catches a wrapper that exposes stale
     * write authority to the resident loop after consent is absent/revoked. */
    if (stale_write_witness)
        oracle_warm_dwrite(&resident, DATA_BASE, true);
    memcpy(baseline, g_ram, sizeof g_ram);
    for (unsigned i = 0u; i < reference_steps; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);
    memcpy(expected_ram, g_ram, sizeof g_ram);

    memcpy(g_ram, baseline, sizeof g_ram);
    memset(&context, 0, sizeof context);
    context.cpu = &resident;
    context.status = ARM_OK;
    context.calls = 0u;
    context.stop_after = stop_after;
    context.code = g_ram;
    context.code_base = 0u;
    context.code_bytes = (uint32_t)sizeof g_ram;
    context.active_block = initial_code_base;
    context.refuse_window = refuse_window;
    context.fast_refill_window = fast_refill_window;
    context.omit_window_after = omit_window_after;
    if (!a64_compact_raw_run_code_window_resident_cached(
            &resident, &g_ram[initial_code_base], initial_code_base,
            initial_code_bytes, budget,
            compact_raw_resident_oracle_step, &context,
            window_cache_enabled, &window_cache_hits, &completed,
            &native_completed, &fallback_completed)) {
        fprintf(stderr,
                "jitbench: compact raw resident %s contract refused\n",
                name);
        goto done;
    }
    capture_state(&resident_state, &resident, context.status, JIT_EXIT_NEXT);
    if (status != ARM_OK || completed != reference_steps ||
        native_completed != expected_native ||
        fallback_completed != expected_fallback ||
        context.calls != expected_fallback ||
        context.fast_refills != expected_fast_refills ||
        window_cache_hits != expected_window_cache_hits ||
        native_completed + fallback_completed != completed ||
        memcmp(expected_ram, g_ram, sizeof g_ram) != 0 ||
        !architectural_states_equal(&reference_state, &resident_state)) {
        fprintf(stderr,
                "jitbench: compact raw resident %s mismatch "
                "(completed/native/fallback/calls/fast-refills/cache-hits "
                "%u/%u/%u/%u/%u/%" PRIu64 ", expected "
                "%u/%u/%u/%u/%u/%" PRIu64 ")\n",
                name, completed, native_completed, fallback_completed,
                context.calls, context.fast_refills, window_cache_hits,
                reference_steps,
                expected_native, expected_fallback, expected_fallback,
                expected_fast_refills, expected_window_cache_hits);
        goto done;
    }
    ok = true;

done:
    free(baseline);
    free(expected_ram);
    return ok;
}

static bool compact_raw_vfp_run_pair(const char *name,
                                     arm_cpu_t *reference,
                                     arm_cpu_t *compact,
                                     uint32_t code_base,
                                     unsigned code_insns,
                                     unsigned reference_steps,
                                     unsigned budget,
                                     unsigned expected_completed) {
    arm_status_t status = ARM_OK;
    unsigned completed = UINT_MAX;

    for (unsigned i = 0u; i < reference_steps; i++) {
        status = arm_step(reference);
        if (status != ARM_OK) break;
    }
    if (!a64_compact_raw_run(compact, &g_ram[code_base], code_base,
                             code_insns * 4u, budget, g_ram, sizeof g_ram,
                             &completed) ||
        status != ARM_OK || completed != expected_completed ||
        !static_vfp_states_equal(reference, compact)) {
        fprintf(stderr,
                "jitbench: compact raw VFP %s mismatch "
                "(completed/expected %u/%u status=%d)\n",
                name, completed, expected_completed, (int)status);
        return false;
    }
    return true;
}

static bool validate_compact_raw_vfp_nonarith_oracles(void) {
    static const uint32_t SYSTEM_READS[] = {
        VFP_VMRS(0, 0), /* VMRS r0,FPSID */
        VFP_VMRS(1, 8), /* VMRS r1,FPEXC */
    };
    static const struct {
        const char *name;
        uint32_t insn;
        uint32_t s0, s1, s2, s3;
        uint32_t fpscr;
    } COMPARES[] = {
        {"compare-snan", VFP_UN_S(4, 0, 0, 2),
         UINT32_C(0x7f801234), 0u, UINT32_C(0x3f800000), 0u, 0u},
        {"compare-fz", VFP_UN_S(5, 0, 0, 0),
         UINT32_C(0x80000001), 0u, 0u, 0u, ARM_FPSCR_FZ},
        {"compare-double-qnan", VFP_UN_D(4, 0, 0, 1),
         UINT32_C(0x00001234), UINT32_C(0x7ff80000),
         0u, UINT32_C(0x3ff00000), 0u},
        {"compare-double-zero", VFP_UN_D(4, 0, 0, 1),
         0u, UINT32_C(0x80000000), 0u, 0u, 0u},
    };
    static const struct {
        const char *name;
        uint32_t input;
        uint32_t fpscr;
    } WIDENS[] = {
        {"widen-subnormal", UINT32_C(0x00000001), 0u},
        {"widen-fz-negative", UINT32_C(0x80000001), ARM_FPSCR_FZ},
        {"widen-snan", UINT32_C(0xff812345), 0u},
        {"widen-default-nan", UINT32_C(0xffc12345), ARM_FPSCR_DN},
    };
    arm_cpu_t reference, compact, before;
    unsigned completed = UINT_MAX;
    unsigned cases = 0u;

    seed_vfp_oracle(&reference, VFP_REGISTER_OPS,
                    (unsigned)(sizeof VFP_REGISTER_OPS /
                               sizeof VFP_REGISTER_OPS[0]),
                    UINT32_C(0x7000), true);
    compact = reference;
    if (!compact_raw_vfp_run_pair(
            "register", &reference, &compact, UINT32_C(0x7000),
            (unsigned)(sizeof VFP_REGISTER_OPS /
                       sizeof VFP_REGISTER_OPS[0]),
            (unsigned)(sizeof VFP_REGISTER_OPS /
                       sizeof VFP_REGISTER_OPS[0]),
            (unsigned)(sizeof VFP_REGISTER_OPS /
                       sizeof VFP_REGISTER_OPS[0]),
            (unsigned)(sizeof VFP_REGISTER_OPS /
                       sizeof VFP_REGISTER_OPS[0])))
        return false;
    cases += (unsigned)(sizeof VFP_REGISTER_OPS /
                        sizeof VFP_REGISTER_OPS[0]);

    seed_vfp_oracle(&reference, VFP_COMPARE_OPS,
                    (unsigned)(sizeof VFP_COMPARE_OPS /
                               sizeof VFP_COMPARE_OPS[0]),
                    UINT32_C(0x7200), true);
    compact = reference;
    if (!compact_raw_vfp_run_pair(
            "compare-family", &reference, &compact, UINT32_C(0x7200),
            (unsigned)(sizeof VFP_COMPARE_OPS / sizeof VFP_COMPARE_OPS[0]),
            (unsigned)(sizeof VFP_COMPARE_OPS / sizeof VFP_COMPARE_OPS[0]),
            (unsigned)(sizeof VFP_COMPARE_OPS / sizeof VFP_COMPARE_OPS[0]),
            (unsigned)(sizeof VFP_COMPARE_OPS / sizeof VFP_COMPARE_OPS[0])))
        return false;
    cases += (unsigned)(sizeof VFP_COMPARE_OPS / sizeof VFP_COMPARE_OPS[0]);

    seed_vfp_oracle(&reference, VFP_WIDEN_OPS,
                    (unsigned)(sizeof VFP_WIDEN_OPS /
                               sizeof VFP_WIDEN_OPS[0]),
                    UINT32_C(0x7400), true);
    compact = reference;
    if (!compact_raw_vfp_run_pair(
            "widen-family", &reference, &compact, UINT32_C(0x7400),
            (unsigned)(sizeof VFP_WIDEN_OPS / sizeof VFP_WIDEN_OPS[0]),
            (unsigned)(sizeof VFP_WIDEN_OPS / sizeof VFP_WIDEN_OPS[0]),
            (unsigned)(sizeof VFP_WIDEN_OPS / sizeof VFP_WIDEN_OPS[0]),
            (unsigned)(sizeof VFP_WIDEN_OPS / sizeof VFP_WIDEN_OPS[0])))
        return false;
    cases += (unsigned)(sizeof VFP_WIDEN_OPS / sizeof VFP_WIDEN_OPS[0]);

    seed_vfp_oracle(&reference, SYSTEM_READS, 2u, UINT32_C(0x7600), true);
    compact = reference;
    if (!compact_raw_vfp_run_pair("system-reads", &reference, &compact,
                                  UINT32_C(0x7600), 2u, 2u, 2u, 2u))
        return false;
    cases += 2u;

    for (unsigned i = 0u; i < sizeof COMPARES / sizeof COMPARES[0]; i++) {
        const uint32_t pc = UINT32_C(0x7700) + i * 4u;
        seed_vfp_oracle(&reference, &COMPARES[i].insn, 1u, pc, true);
        reference.vfp_s[0] = COMPARES[i].s0;
        reference.vfp_s[1] = COMPARES[i].s1;
        reference.vfp_s[2] = COMPARES[i].s2;
        reference.vfp_s[3] = COMPARES[i].s3;
        reference.vfp_fpscr = COMPARES[i].fpscr;
        compact = reference;
        if (!compact_raw_vfp_run_pair(COMPARES[i].name, &reference, &compact,
                                      pc, 1u, 1u, 1u, 1u))
            return false;
        cases++;
    }

    for (unsigned i = 0u; i < sizeof WIDENS / sizeof WIDENS[0]; i++) {
        const uint32_t pc = UINT32_C(0x7800) + i * 4u;
        const uint32_t insn = VFP_WIDEN(2, 1);
        seed_vfp_oracle(&reference, &insn, 1u, pc, true);
        reference.vfp_s[1] = WIDENS[i].input;
        reference.vfp_fpscr = WIDENS[i].fpscr;
        compact = reference;
        if (!compact_raw_vfp_run_pair(WIDENS[i].name, &reference, &compact,
                                      pc, 1u, 1u, 1u, 1u))
            return false;
        cases++;
    }

    {
        const uint32_t insn = VFP_VMSR(8, 2);
        seed_vfp_oracle(&reference, &insn, 1u, UINT32_C(0x7900), false);
        reference.r[2] = ARM_FPEXC_EN;
        compact = reference;
        if (!compact_raw_vfp_run_pair("enable-fpexc", &reference, &compact,
                                      UINT32_C(0x7900), 1u, 1u, 1u, 1u))
            return false;
        cases++;
    }

    /* Failed conditions retire before access checks, while live access/Len
     * failures must leave the current instruction wholly untouched. */
    {
        const uint32_t insn = VFP_UN_S(4, 0, 0, 2) & UINT32_C(0x0fffffff);
        seed_vfp_oracle(&reference, &insn, 1u, UINT32_C(0x7a00), false);
        reference.cp15.cpacr = 0u;
        compact = reference;
        if (!compact_raw_vfp_run_pair("condition-skip", &reference, &compact,
                                      UINT32_C(0x7a00), 1u, 1u, 1u, 1u))
            return false;
        cases++;
    }
    for (unsigned i = 0u; i < 2u; i++) {
        const uint32_t insn = VFP_UN_S(4, 0, 0, 2);
        const uint32_t pc = UINT32_C(0x7b00) + i * 4u;
        seed_vfp_oracle(&compact, &insn, 1u, pc, true);
        if (i == 0u)
            compact.cp15.cpacr = 0u;
        else
            compact.vfp_fpscr = ARM_FPSCR_LEN;
        before = compact;
        if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                                 g_ram, sizeof g_ram, &completed) ||
            completed != 0u ||
            !static_vfp_states_equal(&before, &compact)) {
            fprintf(stderr, "jitbench: compact raw VFP guard mutated state\n");
            return false;
        }
        cases++;
    }

    printf("COMPACT-RAW-VFP-NONARITH-ORACLE exact=yes cases=%u "
           "core-system=yes unary=yes compare=yes widen=yes nan=yes fz=yes "
           "lazy-access=yes condition-before-guard=yes\n", cases);
    return true;
}

#define COMPACT_VFP_ARITH_CONTROL \
    (ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_IXC | \
     ARM_FPSCR_N | ARM_FPSCR_C)

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
typedef struct {
    uint64_t expected_fpcr;
    uint64_t expected_fpsr;
    bool observed_restored;
} compact_raw_vfp_fp_callback_context_t;

static a64_compact_raw_fallback_result_t compact_raw_vfp_fp_callback(
        void *opaque, a64_compact_raw_code_window_t *next_window) {
    compact_raw_vfp_fp_callback_context_t *context =
        (compact_raw_vfp_fp_callback_context_t *)opaque;
    (void)next_window;
    if (!context)
        return A64_COMPACT_RAW_FALLBACK_NO_RETIRE;
    context->observed_restored =
        static_host_fpcr_read() == context->expected_fpcr &&
        static_host_fpsr_read() == context->expected_fpsr;
    /* Refuse retirement: the oracle owns only the external-boundary proof.
     * Executing arm_step here under an intentionally non-RN caller FPCR and
     * comparing it with a reference produced under RN would be invalid. */
    return A64_COMPACT_RAW_FALLBACK_NO_RETIRE;
}
#endif

static bool validate_compact_raw_vfp_arithmetic_oracles(void) {
    arm_cpu_t reference, compact, before;
    unsigned completed = UINT_MAX;
    unsigned exact_cases = 0u;

    /* Every operation and architectural width gets an interpreter equality
     * proof. Division is intentionally inexact; guest IXC starts sticky, so
     * the host's new IXC is the only cumulative flag that may be ignored. */
    for (unsigned width = 0u; width < 2u; width++) {
        const uint32_t *program = width ? VFP_ARITH64_OPS : VFP_ARITH32_OPS;
        for (unsigned operation = 0u; operation < 9u; operation++) {
            const uint32_t pc = UINT32_C(0x17800) +
                                (width * 9u + operation) * 4u;
            const uint64_t d = width ? UINT64_C(0x4059000000000000)
                                     : UINT32_C(0x42c80000);
            const uint64_t n = width ? UINT64_C(0x4008000000000000)
                                     : UINT32_C(0x40400000);
            const uint64_t m = width ? UINT64_C(0x4014000000000000)
                                     : UINT32_C(0x40a00000);
            seed_vfp_oracle(&reference, &program[operation], 1u, pc, true);
            reference.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
            static_vfp_arith_set_operands(
                &reference, program[operation], d, n, m);
            compact = reference;
            if (arm_step(&reference) != ARM_OK ||
                !a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                                     g_ram, sizeof g_ram, &completed) ||
                completed != 1u ||
                !static_vfp_states_equal(&reference, &compact)) {
                fprintf(stderr,
                        "jitbench: compact raw VFP arithmetic mismatch "
                        "width=%u operation=%u completed=%u\n",
                        width, operation, completed);
                return false;
            }
            exact_cases++;
        }
    }

    /* Signed zero is admitted and must preserve its sign exactly. */
    for (unsigned width = 0u; width < 2u; width++) {
        const uint32_t insn = width ? VFP_ARITH_D(2,0,2,0,1)
                                    : VFP_ARITH_S(2,0,2,0,1);
        const uint32_t pc = UINT32_C(0x17900) + width * 4u;
        const uint64_t negative_zero =
            width ? UINT64_C(0x8000000000000000)
                  : UINT32_C(0x80000000);
        const uint64_t two = width ? UINT64_C(0x4000000000000000)
                                   : UINT32_C(0x40000000);
        seed_vfp_oracle(&reference, &insn, 1u, pc, true);
        reference.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
        static_vfp_arith_set_operands(
            &reference, insn, two, negative_zero, two);
        compact = reference;
        if (arm_step(&reference) != ARM_OK ||
            !a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                                 g_ram, sizeof g_ram, &completed) ||
            completed != 1u ||
            !static_vfp_states_equal(&reference, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw VFP signed-zero mismatch "
                    "width=%u\n", width);
            return false;
        }
        exact_cases++;
    }

    /* Every live-mode, access, operand and post-host-operation rejection must
     * stop before guest mutation and restore any opened FP session. */
    for (unsigned i = 0u;
         i < sizeof VFP_ARITH_FALLBACKS / sizeof VFP_ARITH_FALLBACKS[0];
         i++) {
        const static_vfp_arith_fallback_t *test = &VFP_ARITH_FALLBACKS[i];
        const uint32_t pc = UINT32_C(0x17a00) + i * 4u;
        seed_vfp_oracle(&compact, &test->insn, 1u, pc, true);
        compact.vfp_fpscr = test->fpscr;
        compact.vfp_fpexc = test->enabled ? ARM_FPEXC_EN : 0u;
        if (!test->access)
            compact.cp15.cpacr &= ~(UINT32_C(0xf) <<
                                     ARM_CPACR_CP10_SHIFT);
        static_vfp_arith_set_operands(
            &compact, test->insn, test->d, test->n, test->m);
        before = compact;
        completed = UINT_MAX;
        if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                                 g_ram, sizeof g_ram, &completed) ||
            completed != 0u ||
            !static_vfp_states_equal(&before, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw VFP arithmetic fallback "
                    "changed state for %s (completed=%u)\n",
                    test->name, completed);
            return false;
        }
    }

    /* ARM conditions retire before any VFP access or arithmetic guard. */
    {
        const uint32_t insn =
            VFP_ARITH_S(3,0,2,0,1) & UINT32_C(0x0fffffff);
        const uint32_t pc = UINT32_C(0x17b00);
        seed_vfp_oracle(&reference, &insn, 1u, pc, false);
        reference.cp15.cpacr = 0u;
        compact = reference;
        if (arm_step(&reference) != ARM_OK ||
            !a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                                 g_ram, sizeof g_ram, &completed) ||
            completed != 1u ||
            !static_vfp_states_equal(&reference, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw conditional VFP arithmetic "
                    "skip mismatch\n");
            return false;
        }
    }

    /* A successful arithmetic operation and VMSR retire; the next arithmetic
     * instruction sees the newly-invalid FPSCR and returns that exact prefix.
     * This also forces an open FP session through an integer-only VFP op. */
    {
        static const uint32_t partial[] = {
            VFP_ARITH_S(3,0,2,0,1), VFP_VMSR(1, 0),
            VFP_ARITH_S(2,0,3,1,2),
        };
        const uint32_t pc = UINT32_C(0x17c00);
        seed_vfp_oracle(&reference, partial, 3u, pc, true);
        reference.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
        reference.r[0] = ARM_FPSCR_FZ | ARM_FPSCR_DN;
        reference.vfp_s[0] = UINT32_C(0x3f800000);
        reference.vfp_s[1] = UINT32_C(0x40000000);
        compact = reference;
        if (arm_step(&reference) != ARM_OK || arm_step(&reference) != ARM_OK ||
            !a64_compact_raw_run(&compact, &g_ram[pc], pc, 12u, 3u,
                                 g_ram, sizeof g_ram, &completed) ||
            completed != 2u ||
            !static_vfp_states_equal(&reference, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw VFP arithmetic partial-prefix "
                    "mismatch (completed=%u)\n", completed);
            return false;
        }
    }

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    /* Success and post-FMUL rejection must both preserve every caller-visible
     * host FPCR/FPSR bit despite running internally under RN FPCR=0. */
    for (unsigned rejection = 0u; rejection < 2u; rejection++) {
        const uint32_t insn = rejection ? VFP_ARITH_S(2,0,2,0,1)
                                        : VFP_ARITH_S(4,0,2,0,1);
        const uint32_t pc = UINT32_C(0x17d00) + rejection * 4u;
        const uint64_t original_fpcr = static_host_fpcr_read();
        const uint64_t original_fpsr = static_host_fpsr_read();
        uint64_t installed_fpcr, installed_fpsr, after_fpcr, after_fpsr;
        bool run_ok;

        seed_vfp_oracle(&compact, &insn, 1u, pc, true);
        compact.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
        static_vfp_arith_set_operands(
            &compact, insn, UINT32_C(0x3f800000),
            rejection ? UINT32_C(0x7f7fffff) : UINT32_C(0x3f800000),
            rejection ? UINT32_C(0x40000000) : UINT32_C(0x40400000));
        if (rejection) {
            before = compact;
        } else {
            reference = compact;
            if (arm_step(&reference) != ARM_OK) return false;
        }

        static_host_fpcr_write(
            (original_fpcr & ~(UINT64_C(3) << 22)) |
            (UINT64_C(1) << 22));
        static_host_fpsr_write(UINT64_C(0x08000015));
        installed_fpcr = static_host_fpcr_read();
        installed_fpsr = static_host_fpsr_read();
        completed = UINT_MAX;
        run_ok = a64_compact_raw_run(
            &compact, &g_ram[pc], pc, 4u, 1u,
            g_ram, sizeof g_ram, &completed);
        after_fpcr = static_host_fpcr_read();
        after_fpsr = static_host_fpsr_read();
        static_host_fpsr_write(original_fpsr);
        static_host_fpcr_write(original_fpcr);

        if (!run_ok || after_fpcr != installed_fpcr ||
            after_fpsr != installed_fpsr ||
            (rejection
                 ? completed != 0u ||
                       !static_vfp_states_equal(&before, &compact)
                 : completed != 1u ||
                       !static_vfp_states_equal(&reference, &compact))) {
            fprintf(stderr,
                    "jitbench: compact raw VFP arithmetic host-state %s "
                    "mismatch\n", rejection ? "rejection" : "success");
            return false;
        }
    }

    /* Two adjacent operations share one internal session, while a rejecting
     * second operation returns the exact first-instruction prefix. */
    for (unsigned rejection = 0u; rejection < 2u; rejection++) {
        static const uint32_t session_program[] = {
            VFP_ARITH_S(3,0,2,0,1),
            VFP_ARITH_S(2,0,5,3,4),
        };
        const uint32_t pc = UINT32_C(0x17e00) + rejection * 8u;
        const uint64_t original_fpcr = static_host_fpcr_read();
        const uint64_t original_fpsr = static_host_fpsr_read();
        uint64_t installed_fpcr, installed_fpsr, after_fpcr, after_fpsr;
        bool run_ok;

        seed_vfp_oracle(&reference, session_program, 2u, pc, true);
        reference.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
        static_vfp_arith_set_operands(
            &reference, session_program[0], UINT32_C(0x3f800000),
            UINT32_C(0x40400000), UINT32_C(0x40a00000));
        static_vfp_arith_set_operands(
            &reference, session_program[1], UINT32_C(0x3f800000),
            rejection ? UINT32_C(0x7f7fffff) : UINT32_C(0x40000000),
            rejection ? UINT32_C(0x40000000) : UINT32_C(0x40800000));
        compact = reference;
        if (arm_step(&reference) != ARM_OK ||
            (!rejection && arm_step(&reference) != ARM_OK))
            return false;

        static_host_fpcr_write(
            (original_fpcr & ~(UINT64_C(3) << 22)) |
            (UINT64_C(2) << 22));
        static_host_fpsr_write(UINT64_C(0x08000015));
        installed_fpcr = static_host_fpcr_read();
        installed_fpsr = static_host_fpsr_read();
        completed = UINT_MAX;
        run_ok = a64_compact_raw_run(
            &compact, &g_ram[pc], pc, 8u, 2u,
            g_ram, sizeof g_ram, &completed);
        after_fpcr = static_host_fpcr_read();
        after_fpsr = static_host_fpsr_read();
        static_host_fpsr_write(original_fpsr);
        static_host_fpcr_write(original_fpcr);

        if (!run_ok || after_fpcr != installed_fpcr ||
            after_fpsr != installed_fpsr ||
            completed != (rejection ? 1u : 2u) ||
            !static_vfp_states_equal(&reference, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw VFP arithmetic session %s "
                    "mismatch\n", rejection ? "partial" : "success");
            return false;
        }
    }

    /* A result rejection handed to a refusing resident callback must restore
     * the host environment before that external C boundary observes it. */
    {
        const uint32_t insn = VFP_ARITH_S(2,0,2,0,1);
        const uint32_t pc = UINT32_C(0x17f00);
        const uint64_t original_fpcr = static_host_fpcr_read();
        const uint64_t original_fpsr = static_host_fpsr_read();
        compact_raw_vfp_fp_callback_context_t context;
        unsigned native_completed = UINT_MAX;
        unsigned fallback_completed = UINT_MAX;
        uint64_t after_fpcr, after_fpsr;
        bool run_ok;

        seed_vfp_oracle(&compact, &insn, 1u, pc, true);
        compact.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
        static_vfp_arith_set_operands(
            &compact, insn, UINT32_C(0x3f800000),
            UINT32_C(0x7f7fffff), UINT32_C(0x40000000));
        before = compact;

        static_host_fpcr_write(
            (original_fpcr & ~(UINT64_C(3) << 22)) |
            (UINT64_C(1) << 22));
        static_host_fpsr_write(UINT64_C(0x08000015));
        memset(&context, 0, sizeof context);
        context.expected_fpcr = static_host_fpcr_read();
        context.expected_fpsr = static_host_fpsr_read();
        completed = UINT_MAX;
        run_ok = a64_compact_raw_run_code_window_resident(
            &compact, &g_ram[pc], pc, 4u, 1u,
            compact_raw_vfp_fp_callback, &context, &completed,
            &native_completed, &fallback_completed);
        after_fpcr = static_host_fpcr_read();
        after_fpsr = static_host_fpsr_read();
        static_host_fpsr_write(original_fpsr);
        static_host_fpcr_write(original_fpcr);

        if (!run_ok || !context.observed_restored ||
            completed != 0u || native_completed != 0u ||
            fallback_completed != 0u ||
            after_fpcr != context.expected_fpcr ||
            after_fpsr != context.expected_fpsr ||
            !static_vfp_states_equal(&before, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw VFP arithmetic callback "
                    "boundary mismatch\n");
            return false;
        }
    }
#endif

    printf("COMPACT-RAW-VFP-ARITH-ORACLE exact=yes operations=9 widths=2 "
           "accepted=%u signed-zero=yes inexact=yes fallbacks=%zu "
           "conditions=yes partial-prefix=yes host-fp-state=yes "
           "host-fp-session=yes callback-boundary=yes "
           "callback-no-retire=yes runtime-codegen=no\n",
           exact_cases,
           sizeof VFP_ARITH_FALLBACKS / sizeof VFP_ARITH_FALLBACKS[0]);
    return true;
}

static bool validate_compact_raw_vfp_narrow_oracles(void) {
    static const struct {
        uint32_t insn;
        uint64_t input;
    } ACCEPTED[] = {
        {VFP_NARROW( 0,  0), UINT64_C(0x0000000000000000)},
        {VFP_NARROW( 1,  0), UINT64_C(0x8000000000000000)},
        {VFP_NARROW( 2,  1), UINT64_C(0x3ff8000000000000)},
        {VFP_NARROW( 3,  7), UINT64_C(0x3fd5555555555555)},
        {VFP_NARROW(15,  7), UINT64_C(0xbfd5555555555555)},
        {VFP_NARROW(16,  8), UINT64_C(0x47efffffe0000000)},
        {VFP_NARROW(30, 14), UINT64_C(0x3810000000000000)},
        {VFP_NARROW(31, 15), UINT64_C(0xc00921fb54442d18)},
    };
    static const struct {
        const char *name;
        uint64_t input;
        uint32_t fpscr;
        bool enabled;
        bool access;
    } FALLBACKS[] = {
        {"missing-runfast", UINT64_C(0x3ff0000000000000),
         ARM_FPSCR_IXC, true, true},
        {"missing-sticky", UINT64_C(0x3ff0000000000000),
         ARM_FPSCR_FZ | ARM_FPSCR_DN, true, true},
        {"directed-rounding", UINT64_C(0x3fd5555555555555),
         COMPACT_VFP_ARITH_CONTROL | (1u << 22), true, true},
        {"extra-sticky", UINT64_C(0x3ff0000000000000),
         COMPACT_VFP_ARITH_CONTROL | ARM_FPSCR_IOC, true, true},
        {"short-vector", UINT64_C(0x3ff0000000000000),
         COMPACT_VFP_ARITH_CONTROL | ARM_FPSCR_LEN, true, true},
        {"exception-enable", UINT64_C(0x3ff0000000000000),
         COMPACT_VFP_ARITH_CONTROL | ARM_FPSCR_IOE, true, true},
        {"disabled", UINT64_C(0x3ff0000000000000),
         COMPACT_VFP_ARITH_CONTROL, false, true},
        {"access-denied", UINT64_C(0x3ff0000000000000),
         COMPACT_VFP_ARITH_CONTROL, true, false},
        {"infinity", UINT64_C(0x7ff0000000000000),
         COMPACT_VFP_ARITH_CONTROL, true, true},
        {"overflow", UINT64_C(0x7fefffffffffffff),
         COMPACT_VFP_ARITH_CONTROL, true, true},
        {"subnormal-result", UINT64_C(0x36a0000000000000),
         COMPACT_VFP_ARITH_CONTROL, true, true},
        {"fz-boundary", UINT64_C(0x380fffffe0000000),
         COMPACT_VFP_ARITH_CONTROL, true, true},
    };
    static const uint32_t PARTIAL[] = {
        VFP_NARROW(0, 1), VFP_VMSR(1, 0), VFP_NARROW(2, 2),
    };
    arm_cpu_t reference, compact, before;
    unsigned completed = UINT_MAX;

    if (!a64_static_host_available()) {
        printf("COMPACT-RAW-VFP-NARROW-ORACLE SKIP: "
               "no signed AArch64 handlers\n");
        return true;
    }

    for (unsigned i = 0u; i < sizeof ACCEPTED / sizeof ACCEPTED[0]; i++) {
        uint32_t pc = UINT32_C(0x18000) + i * 4u;
        seed_vfp_oracle(&reference, &ACCEPTED[i].insn, 1u, pc, true);
        reference.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
        vfp_set_d(&reference, ACCEPTED[i].insn & 15u,
                  ACCEPTED[i].input);
        compact = reference;
        if (arm_step(&reference) != ARM_OK ||
            !a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                                 g_ram, sizeof g_ram, &completed) ||
            completed != 1u ||
            !static_vfp_states_equal(&reference, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw VFP narrow accepted case %u "
                    "mismatch\n", i);
            return false;
        }
    }

    for (unsigned i = 0u; i < sizeof FALLBACKS / sizeof FALLBACKS[0]; i++) {
        const uint32_t insn = VFP_NARROW(15, 7);
        uint32_t pc = UINT32_C(0x18100) + i * 4u;
        seed_vfp_oracle(&compact, &insn, 1u, pc, true);
        compact.vfp_fpscr = FALLBACKS[i].fpscr;
        compact.vfp_fpexc = FALLBACKS[i].enabled ? ARM_FPEXC_EN : 0u;
        if (!FALLBACKS[i].access)
            compact.cp15.cpacr &= ~(UINT32_C(0xf) <<
                                     ARM_CPACR_CP10_SHIFT);
        vfp_set_d(&compact, 7u, FALLBACKS[i].input);
        before = compact;
        completed = UINT_MAX;
        if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, 4u, 1u,
                                 g_ram, sizeof g_ram, &completed) ||
            completed != 0u || !static_vfp_states_equal(&before, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw VFP narrow fallback changed "
                    "state for %s\n", FALLBACKS[i].name);
            return false;
        }
    }

    seed_vfp_oracle(&reference, PARTIAL, 3u, UINT32_C(0x18200), true);
    reference.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
    reference.r[0] = ARM_FPSCR_FZ | ARM_FPSCR_DN;
    vfp_set_d(&reference, 1u, UINT64_C(0x3fd5555555555555));
    vfp_set_d(&reference, 2u, UINT64_C(0x400921fb54442d18));
    compact = reference;
    if (arm_step(&reference) != ARM_OK || arm_step(&reference) != ARM_OK ||
        !a64_compact_raw_run(
            &compact, &g_ram[UINT32_C(0x18200)], UINT32_C(0x18200),
            12u, 3u, g_ram, sizeof g_ram, &completed) ||
        completed != 2u ||
        !static_vfp_states_equal(&reference, &compact)) {
        fprintf(stderr,
                "jitbench: compact raw VFP narrow partial-prefix mismatch\n");
        return false;
    }

    {
        uint32_t condition_skip =
            VFP_NARROW(15, 7) & UINT32_C(0x0fffffff);
        seed_vfp_oracle(&reference, &condition_skip, 1u,
                        UINT32_C(0x18300), false);
        reference.cp15.cpacr = 0u;
        reference.vfp_fpexc = 0u;
        reference.vfp_fpscr = 0u;
        compact = reference;
        if (arm_step(&reference) != ARM_OK ||
            !a64_compact_raw_run(
                &compact, &g_ram[UINT32_C(0x18300)], UINT32_C(0x18300),
                4u, 1u, g_ram, sizeof g_ram, &completed) ||
            completed != 1u ||
            !static_vfp_states_equal(&reference, &compact)) {
            fprintf(stderr,
                    "jitbench: conditional compact VFP narrow skip "
                    "mismatch\n");
            return false;
        }
    }

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    for (unsigned rejection = 0u; rejection < 2u; rejection++) {
        const uint32_t insn = VFP_NARROW(15, 7);
        const uint32_t pc = UINT32_C(0x18400) + rejection * 4u;
        const uint64_t original_fpcr = static_host_fpcr_read();
        const uint64_t original_fpsr = static_host_fpsr_read();
        uint64_t installed_fpcr, installed_fpsr, after_fpcr, after_fpsr;
        bool run_ok;

        seed_vfp_oracle(&compact, &insn, 1u, pc, true);
        compact.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
        vfp_set_d(&compact, 7u,
                  rejection ? UINT64_C(0x7fefffffffffffff)
                            : UINT64_C(0x3fd5555555555555));
        if (rejection) {
            before = compact;
        } else {
            reference = compact;
            if (arm_step(&reference) != ARM_OK) return false;
        }

        static_host_fpcr_write(
            (original_fpcr & ~(UINT64_C(3) << 22)) |
            (UINT64_C(2) << 22));
        static_host_fpsr_write(UINT64_C(0x08000015));
        installed_fpcr = static_host_fpcr_read();
        installed_fpsr = static_host_fpsr_read();
        completed = UINT_MAX;
        run_ok = a64_compact_raw_run(
            &compact, &g_ram[pc], pc, 4u, 1u,
            g_ram, sizeof g_ram, &completed);
        after_fpcr = static_host_fpcr_read();
        after_fpsr = static_host_fpsr_read();
        static_host_fpsr_write(original_fpsr);
        static_host_fpcr_write(original_fpcr);

        if (!run_ok || after_fpcr != installed_fpcr ||
            after_fpsr != installed_fpsr ||
            (rejection
                 ? completed != 0u ||
                       !static_vfp_states_equal(&before, &compact)
                 : completed != 1u ||
                       !static_vfp_states_equal(&reference, &compact))) {
            fprintf(stderr,
                    "jitbench: compact raw VFP narrow host-state %s "
                    "mismatch\n", rejection ? "rejection" : "success");
            return false;
        }
    }

    {
        const uint32_t insn = VFP_NARROW(15, 7);
        const uint32_t pc = UINT32_C(0x18500);
        const uint64_t original_fpcr = static_host_fpcr_read();
        const uint64_t original_fpsr = static_host_fpsr_read();
        compact_raw_vfp_fp_callback_context_t context;
        unsigned native_completed = UINT_MAX;
        unsigned fallback_completed = UINT_MAX;
        uint64_t after_fpcr, after_fpsr;
        bool run_ok;

        seed_vfp_oracle(&compact, &insn, 1u, pc, true);
        compact.vfp_fpscr = COMPACT_VFP_ARITH_CONTROL;
        vfp_set_d(&compact, 7u, UINT64_C(0x7fefffffffffffff));
        before = compact;
        static_host_fpcr_write(
            (original_fpcr & ~(UINT64_C(3) << 22)) |
            (UINT64_C(1) << 22));
        static_host_fpsr_write(UINT64_C(0x08000015));
        memset(&context, 0, sizeof context);
        context.expected_fpcr = static_host_fpcr_read();
        context.expected_fpsr = static_host_fpsr_read();
        completed = UINT_MAX;
        run_ok = a64_compact_raw_run_code_window_resident(
            &compact, &g_ram[pc], pc, 4u, 1u,
            compact_raw_vfp_fp_callback, &context, &completed,
            &native_completed, &fallback_completed);
        after_fpcr = static_host_fpcr_read();
        after_fpsr = static_host_fpsr_read();
        static_host_fpsr_write(original_fpsr);
        static_host_fpcr_write(original_fpcr);

        if (!run_ok || !context.observed_restored ||
            completed != 0u || native_completed != 0u ||
            fallback_completed != 0u ||
            after_fpcr != context.expected_fpcr ||
            after_fpsr != context.expected_fpsr ||
            !static_vfp_states_equal(&before, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw VFP narrow callback boundary "
                    "mismatch\n");
            return false;
        }
    }
#endif

    printf("COMPACT-RAW-VFP-NARROW-ORACLE exact=yes accepted=%zu "
           "fallbacks=%zu aliases=yes inexact=yes post-round-rejection=yes "
           "conditions=yes partial-prefix=yes host-fp-state=yes "
           "callback-boundary=yes runtime-codegen=no\n",
           sizeof ACCEPTED / sizeof ACCEPTED[0],
           sizeof FALLBACKS / sizeof FALLBACKS[0]);
    return true;
}

#undef COMPACT_VFP_ARITH_CONTROL

static bool compact_raw_vfp_flat_memory_case(const char *name,
                                             unsigned insns,
                                             uint32_t pc,
                                             arm_cpu_t *initial,
                                             uint8_t *baseline,
                                             uint8_t *expected) {
    arm_cpu_t reference = *initial;
    arm_cpu_t compact = *initial;
    arm_status_t status = ARM_OK;
    unsigned completed = UINT_MAX;

    memcpy(baseline, g_ram, sizeof g_ram);
    for (unsigned i = 0u; i < insns; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);
    if (!a64_compact_raw_run(&compact, &g_ram[pc], pc, insns * 4u,
                             insns, g_ram, sizeof g_ram, &completed) ||
        status != ARM_OK || completed != insns ||
        !static_vfp_arch_states_equal(&reference, &compact) ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw VFP flat-memory %s mismatch "
                "(completed/expected %u/%u status=%d)\n",
                name, completed, insns, (int)status);
        for (unsigned i = 0u; i < 16u; i++) {
            if (reference.r[i] != compact.r[i]) {
                fprintf(stderr,
                        "jitbench: compact raw VFP %s r%u "
                        "reference=%08" PRIx32 " compact=%08" PRIx32 "\n",
                        name, i, reference.r[i], compact.r[i]);
            }
        }
        if (reference.cpsr != compact.cpsr ||
            reference.cycles != compact.cycles ||
            reference.vfp_fpexc != compact.vfp_fpexc ||
            reference.vfp_fpscr != compact.vfp_fpscr) {
            fprintf(stderr,
                    "jitbench: compact raw VFP %s scalar-state "
                    "cpsr=%08" PRIx32 "/%08" PRIx32 " "
                    "cycles=%" PRIu64 "/%" PRIu64 " "
                    "fpexc=%08" PRIx32 "/%08" PRIx32 " "
                    "fpscr=%08" PRIx32 "/%08" PRIx32 "\n",
                    name, reference.cpsr, compact.cpsr,
                    reference.cycles, compact.cycles,
                    reference.vfp_fpexc, compact.vfp_fpexc,
                    reference.vfp_fpscr, compact.vfp_fpscr);
        }
        for (unsigned i = 0u; i < 32u; i++) {
            if (reference.vfp_s[i] != compact.vfp_s[i]) {
                fprintf(stderr,
                        "jitbench: compact raw VFP %s s%u "
                        "reference=%08" PRIx32 " compact=%08" PRIx32 "\n",
                        name, i, reference.vfp_s[i], compact.vfp_s[i]);
            }
        }
        if (reference.dread_hits != compact.dread_hits ||
            reference.dread_misses != compact.dread_misses ||
            reference.dwrite_hits != compact.dwrite_hits ||
            reference.dwrite_misses != compact.dwrite_misses) {
            fprintf(stderr,
                    "jitbench: compact raw VFP %s cache-state "
                    "dread=%" PRIu64 "/%" PRIu64 " hits, "
                    "%" PRIu64 "/%" PRIu64 " misses; "
                    "dwrite=%" PRIu64 "/%" PRIu64 " hits, "
                    "%" PRIu64 "/%" PRIu64 " misses\n",
                    name, reference.dread_hits, compact.dread_hits,
                    reference.dread_misses, compact.dread_misses,
                    reference.dwrite_hits, compact.dwrite_hits,
                    reference.dwrite_misses, compact.dwrite_misses);
        }
        for (unsigned i = 0u; i < sizeof g_ram; i++) {
            if (expected[i] != g_ram[i]) {
                fprintf(stderr,
                        "jitbench: compact raw VFP %s RAM[%08x] "
                        "reference=%02x compact=%02x\n",
                        name, i, expected[i], g_ram[i]);
                break;
            }
        }
        return false;
    }
    return true;
}

static bool compact_raw_vfp_resident_memory_case(
        const char *name, unsigned insns, uint32_t pc,
        arm_cpu_t *initial, uint8_t *baseline, uint8_t *expected) {
    arm_cpu_t reference = *initial;
    arm_cpu_t resident = *initial;
    compact_raw_resident_oracle_context_t context;
    arm_status_t status = ARM_OK;
    unsigned completed = UINT_MAX;
    unsigned native_completed = UINT_MAX;
    unsigned fallback_completed = UINT_MAX;

    memcpy(baseline, g_ram, sizeof g_ram);
    for (unsigned i = 0u; i < insns; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);

    memset(&context, 0, sizeof context);
    context.cpu = &resident;
    context.status = ARM_OK;
    context.code = g_ram;
    context.code_bytes = (uint32_t)sizeof g_ram;
    if (!a64_compact_raw_run_code_window_resident(
            &resident, &g_ram[pc], pc, insns * 4u, insns,
            compact_raw_resident_oracle_step, &context, &completed,
            &native_completed, &fallback_completed) ||
        status != ARM_OK || context.status != ARM_OK ||
        completed != insns || native_completed != insns ||
        fallback_completed != 0u || context.calls != 0u ||
        !static_vfp_states_equal(&reference, &resident) ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw resident VFP memory %s mismatch "
                "(completed/native/fallback/calls %u/%u/%u/%u, "
                "expected %u/%u/0/0, status=%d/%d, "
                "dread=%" PRIu64 "/%" PRIu64 ", "
                "dwrite=%" PRIu64 "/%" PRIu64 ")\n",
                name, completed, native_completed, fallback_completed,
                context.calls, insns, insns, (int)status,
                (int)context.status, reference.dread_hits,
                resident.dread_hits, reference.dwrite_hits,
                resident.dwrite_hits);
        return false;
    }
    return true;
}

static bool validate_compact_raw_vfp_memory_oracles(void) {
    uint8_t *baseline = (uint8_t *)malloc(sizeof g_ram);
    uint8_t *expected = (uint8_t *)malloc(sizeof g_ram);
    arm_cpu_t initial;
    arm_bus_t write_bus = g_bus;
    bool ok = false;

    if (!baseline || !expected) {
        fprintf(stderr,
                "jitbench: compact raw VFP memory allocation failed\n");
        goto done;
    }

    seed_vfp_read_oracle(&initial, false);
    if (!compact_raw_vfp_flat_memory_case(
            "loads",
            (unsigned)(sizeof VFP_READ_HITS / sizeof VFP_READ_HITS[0]),
            UINT32_C(0xf000), &initial, baseline, expected))
        goto done;

    seed_vfp_oracle(&initial, VFP_WRITE_HITS,
                    (unsigned)(sizeof VFP_WRITE_HITS /
                               sizeof VFP_WRITE_HITS[0]),
                    UINT32_C(0x8000), true);
    initial.r[0] = DATA_BASE + UINT32_C(0x800);
    initial.r[1] = DATA_BASE + UINT32_C(0x880);
    if (!compact_raw_vfp_flat_memory_case(
            "stores",
            (unsigned)(sizeof VFP_WRITE_HITS / sizeof VFP_WRITE_HITS[0]),
            UINT32_C(0x8000), &initial, baseline, expected))
        goto done;

    seed_vfp_oracle(&initial, VSTM_WRITE_HITS,
                    (unsigned)(sizeof VSTM_WRITE_HITS /
                               sizeof VSTM_WRITE_HITS[0]),
                    UINT32_C(0x8200), true);
    initial.r[0] = DATA_BASE + UINT32_C(0x800);
    initial.r[1] = DATA_BASE + UINT32_C(0x840);
    initial.r[13] = DATA_BASE + UINT32_C(0xc00);
    initial.r[2] = DATA_BASE + UINT32_C(0xd00);
    initial.r[3] = DATA_BASE + UINT32_C(0xe00);
    if (!compact_raw_vfp_flat_memory_case(
            "multiple-stores",
            (unsigned)(sizeof VSTM_WRITE_HITS /
                       sizeof VSTM_WRITE_HITS[0]),
            UINT32_C(0x8200), &initial, baseline, expected))
        goto done;

    seed_vfp_read_oracle(&initial, false);
    oracle_warm_dread(&initial, DATA_BASE);
    oracle_warm_dread(&initial, UINT32_C(0xf000));
    if (!compact_raw_vfp_resident_memory_case(
            "loads",
            (unsigned)(sizeof VFP_READ_HITS / sizeof VFP_READ_HITS[0]),
            UINT32_C(0xf000), &initial, baseline, expected))
        goto done;

    write_bus.host_ram_write = mem_host_ram;
    seed_vfp_oracle(&initial, VFP_WRITE_HITS,
                    (unsigned)(sizeof VFP_WRITE_HITS /
                               sizeof VFP_WRITE_HITS[0]),
                    UINT32_C(0x8000), true);
    initial.bus = &write_bus;
    initial.r[0] = DATA_BASE + UINT32_C(0x800);
    initial.r[1] = DATA_BASE + UINT32_C(0x880);
    oracle_warm_dwrite(&initial, DATA_BASE + UINT32_C(0x800), true);
    oracle_warm_dwrite(&initial, UINT32_C(0x8000), true);
    if (!compact_raw_vfp_resident_memory_case(
            "stores",
            (unsigned)(sizeof VFP_WRITE_HITS / sizeof VFP_WRITE_HITS[0]),
            UINT32_C(0x8000), &initial, baseline, expected))
        goto done;

    seed_vfp_oracle(&initial, VSTM_WRITE_HITS,
                    (unsigned)(sizeof VSTM_WRITE_HITS /
                               sizeof VSTM_WRITE_HITS[0]),
                    UINT32_C(0x8200), true);
    initial.bus = &write_bus;
    initial.r[0] = DATA_BASE + UINT32_C(0x800);
    initial.r[1] = DATA_BASE + UINT32_C(0x840);
    initial.r[13] = DATA_BASE + UINT32_C(0xc00);
    initial.r[2] = DATA_BASE + UINT32_C(0xd00);
    initial.r[3] = DATA_BASE + UINT32_C(0xe00);
    oracle_warm_dwrite(&initial, DATA_BASE + UINT32_C(0x800), true);
    oracle_warm_dwrite(&initial, DATA_BASE + UINT32_C(0xc00), true);
    if (!compact_raw_vfp_resident_memory_case(
            "multiple-stores",
            (unsigned)(sizeof VSTM_WRITE_HITS /
                       sizeof VSTM_WRITE_HITS[0]),
            UINT32_C(0x8200), &initial, baseline, expected))
        goto done;

    printf("COMPACT-RAW-VFP-MEMORY-ORACLE exact=yes cases=21 "
           "vldr=yes vstr=yes vstm=yes single=yes double=yes pc-relative=yes "
           "writeback=yes condition-before-guard=yes transactional=yes "
           "flat-ram=yes cache-telemetry=excluded runtime-codegen=no\n");
    printf("COMPACT-RAW-VFP-RESIDENT-MEMORY-ORACLE exact=yes cases=21 "
           "vldr=yes vstr=yes vstm=yes dread=yes dwrite=yes "
           "hit-accounting=yes native-only=yes fallback=zero "
           "transactional=yes runtime-codegen=no\n");
    ok = true;

done:
    free(baseline);
    free(expected);
    return ok;
}

typedef struct {
    const char *name;
    uint16_t insn;
    uint32_t pc;
    uint32_t base;
    uint32_t start;
    unsigned words;
    uint32_t final_word;
} compact_raw_thumb_multi_case_t;

static void seed_compact_raw_thumb_multi_case(
        arm_cpu_t *cpu, const compact_raw_thumb_multi_case_t *sc) {
    const bool stack = (sc->insn & UINT16_C(0xf600)) ==
                       UINT16_C(0xb400);
    const unsigned rb = stack ? 13u : (sc->insn >> 8) & 7u;

    seed_compact_raw_thumb(cpu, &sc->insn, 1u, sc->pc);
    for (unsigned reg = 0u; reg < 15u; reg++)
        cpu->r[reg] = UINT32_C(0x75000000) |
                      (reg * UINT32_C(0x010203));
    cpu->r[rb] = sc->base;
    cpu->r[15] = sc->pc;
    for (unsigned word = 0u; word < sc->words; word++)
        mem_w32(NULL, sc->start + word * 4u,
                UINT32_C(0x53000000) |
                ((sc->pc & UINT32_C(0xff)) << 16) |
                (word * UINT32_C(0x010101)));
    if (sc->final_word && sc->words)
        mem_w32(NULL, sc->start + (sc->words - 1u) * 4u,
                sc->final_word);
}

static bool compact_raw_thumb_multi_exact_case(
        const compact_raw_thumb_multi_case_t *sc, bool resident,
        uint8_t *baseline, uint8_t *expected) {
    const bool load = (sc->insn & (1u << 11)) != 0u;
    arm_bus_t write_bus = g_bus;
    arm_cpu_t initial;
    arm_cpu_t reference;
    arm_cpu_t compact;
    compact_raw_resident_oracle_context_t context;
    arm_status_t status;
    unsigned completed = UINT_MAX;
    unsigned native_completed = UINT_MAX;
    unsigned fallback_completed = UINT_MAX;
    bool run_ok;

    write_bus.host_ram_write = mem_host_ram;
    seed_compact_raw_thumb_multi_case(&initial, sc);
    if (resident) {
        if (load) {
            oracle_warm_dread(&initial, sc->start);
        } else {
            initial.bus = &write_bus;
            oracle_warm_dwrite(&initial, sc->start, true);
        }
    }
    reference = initial;
    compact = initial;
    memcpy(baseline, g_ram, sizeof g_ram);
    status = arm_step(&reference);
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);

    if (resident) {
        memset(&context, 0, sizeof context);
        context.cpu = &compact;
        context.status = ARM_OK;
        context.code = g_ram;
        context.code_bytes = (uint32_t)sizeof g_ram;
        run_ok = a64_compact_raw_run_code_window_resident(
            &compact, &g_ram[sc->pc], sc->pc, 4u, 1u,
            compact_raw_resident_oracle_step, &context, &completed,
            &native_completed, &fallback_completed);
    } else {
        memset(&context, 0, sizeof context);
        run_ok = a64_compact_raw_run(
            &compact, &g_ram[sc->pc], sc->pc, 4u, 1u,
            g_ram, sizeof g_ram, &completed);
        native_completed = completed;
        fallback_completed = 0u;
    }

    if (!run_ok || status != ARM_OK || completed != 1u ||
        native_completed != 1u || fallback_completed != 0u ||
        (resident && (context.status != ARM_OK || context.calls != 0u)) ||
        !(resident ? static_vfp_states_equal(&reference, &compact)
                    : static_vfp_arch_states_equal(&reference, &compact)) ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw Thumb multi %s %s mismatch "
                "(completed/native/fallback/calls %u/%u/%u/%u, "
                "status=%d/%d, dread=%" PRIu64 "/%" PRIu64 ", "
                "dwrite=%" PRIu64 "/%" PRIu64 ")\n",
                resident ? "resident" : "flat", sc->name,
                completed, native_completed, fallback_completed,
                resident ? context.calls : 0u, (int)status,
                resident ? (int)context.status : (int)ARM_OK,
                reference.dread_hits, compact.dread_hits,
                reference.dwrite_hits, compact.dwrite_hits);
        return false;
    }
    return true;
}

static bool compact_raw_thumb_multi_refusal_case(
        const compact_raw_thumb_multi_case_t *sc,
        uint8_t *baseline, uint8_t *expected) {
    arm_cpu_t reference;
    arm_cpu_t compact;
    arm_cpu_t before;
    arm_status_t reference_status;
    arm_status_t compact_status;
    unsigned completed = UINT_MAX;

    seed_compact_raw_thumb_multi_case(&reference, sc);
    compact = reference;
    before = compact;
    memcpy(baseline, g_ram, sizeof g_ram);
    reference_status = arm_step(&reference);
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);
    if (!a64_compact_raw_run(
            &compact, &g_ram[sc->pc], sc->pc, 4u, 1u,
            g_ram, sizeof g_ram, &completed) ||
        completed != 0u || memcmp(&before, &compact, sizeof compact) != 0 ||
        memcmp(baseline, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw Thumb multi refusal %s changed "
                "state (completed=%u)\n", sc->name, completed);
        return false;
    }
    compact_status = arm_step(&compact);
    if (compact_status != reference_status ||
        memcmp(&reference, &compact, sizeof compact) != 0 ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw Thumb multi refusal %s fallback "
                "diverged (status=%d/%d)\n",
                sc->name, (int)reference_status, (int)compact_status);
        return false;
    }
    return true;
}

static bool compact_raw_thumb_multi_resident_fallback_case(
        const compact_raw_thumb_multi_case_t *sc, unsigned mode,
        uint8_t *baseline, uint8_t *expected) {
    const bool load = (sc->insn & (1u << 11)) != 0u;
    const bool no_retire = mode == 4u;
    arm_bus_t write_bus = g_bus;
    arm_cpu_t reference;
    arm_cpu_t resident;
    compact_raw_resident_oracle_context_t context;
    arm_status_t reference_status;
    unsigned completed = UINT_MAX;
    unsigned native_completed = UINT_MAX;
    unsigned fallback_completed = UINT_MAX;

    write_bus.host_ram_write = mem_host_ram;
    seed_compact_raw_thumb_multi_case(&reference, sc);
    if (load) {
        if (mode == 1u) {
            oracle_warm_dread(&reference, sc->start);
            reference.tlb_gen++;
        } else if (mode == 4u) {
            oracle_warm_dread(&reference, sc->start);
        }
    } else {
        if (mode != 2u) reference.bus = &write_bus;
        oracle_warm_dwrite(&reference, sc->start, true);
    }
    resident = reference;
    memcpy(baseline, g_ram, sizeof g_ram);
    reference_status = arm_step(&reference);
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);

    memset(&context, 0, sizeof context);
    context.cpu = &resident;
    context.status = ARM_OK;
    context.code = g_ram;
    context.code_bytes = (uint32_t)sizeof g_ram;
    if (!a64_compact_raw_run_code_window_resident(
            &resident, &g_ram[sc->pc], sc->pc, 4u, 1u,
            compact_raw_resident_oracle_step, &context, &completed,
            &native_completed, &fallback_completed) ||
        completed != (no_retire ? 0u : 1u) || native_completed != 0u ||
        fallback_completed != (no_retire ? 0u : 1u) ||
        context.calls != (no_retire ? 0u : 1u) ||
        context.status != reference_status ||
        memcmp(&reference, &resident, sizeof resident) != 0 ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw resident Thumb multi fallback %s "
                "mode=%u diverged (completed/native/fallback/calls "
                "%u/%u/%u/%u status=%d/%d)\n",
                sc->name, mode, completed, native_completed,
                fallback_completed, context.calls,
                (int)reference_status, (int)context.status);
        return false;
    }
    return true;
}

static bool validate_compact_raw_thumb_multi_oracles(void) {
    const uint32_t ordinary = DATA_BASE + UINT32_C(0x1800);
    const compact_raw_thumb_multi_case_t cases[] = {
        {"push-registers", UINT16_C(0xb405), UINT32_C(0xd200),
         ordinary + UINT32_C(0x40), ordinary + UINT32_C(0x38), 2u, 0u},
        {"push-lr", UINT16_C(0xb500), UINT32_C(0xd300),
         ordinary + UINT32_C(0x80), ordinary + UINT32_C(0x7c), 1u, 0u},
        {"push-nine-words", UINT16_C(0xb5ff), UINT32_C(0xd400),
         ordinary + UINT32_C(0x100), ordinary + UINT32_C(0xdc), 9u, 0u},
        {"pop-registers", UINT16_C(0xbc05), UINT32_C(0xd500),
         ordinary + UINT32_C(0x140), ordinary + UINT32_C(0x140), 2u, 0u},
        {"pop-pc-arm", UINT16_C(0xbd00), UINT32_C(0xd600),
         ordinary + UINT32_C(0x180), ordinary + UINT32_C(0x180), 1u,
         UINT32_C(0xec00)},
        {"pop-pc-thumb-low1", UINT16_C(0xbd01), UINT32_C(0xd700),
         ordinary + UINT32_C(0x1c0), ordinary + UINT32_C(0x1c0), 2u,
         UINT32_C(0xed01)},
        {"pop-pc-thumb-low3", UINT16_C(0xbd02), UINT32_C(0xd800),
         ordinary + UINT32_C(0x200), ordinary + UINT32_C(0x200), 2u,
         UINT32_C(0xee03)},
        {"stmia", UINT16_C(0xc603), UINT32_C(0xd900),
         ordinary + UINT32_C(0x240), ordinary + UINT32_C(0x240), 2u, 0u},
        {"stmia-base-lowest", UINT16_C(0xc6c0), UINT32_C(0xda00),
         ordinary + UINT32_C(0x280), ordinary + UINT32_C(0x280), 2u, 0u},
        {"ldmia", UINT16_C(0xce03), UINT32_C(0xdb00),
         ordinary + UINT32_C(0x2c0), ordinary + UINT32_C(0x2c0), 2u, 0u},
        {"ldmia-base-in-list", UINT16_C(0xcec0), UINT32_C(0xdc00),
         ordinary + UINT32_C(0x300), ordinary + UINT32_C(0x300), 2u, 0u},
    };
    const compact_raw_thumb_multi_case_t refusals[] = {
        {"empty-push", UINT16_C(0xb400), UINT32_C(0xdd00),
         ordinary, ordinary, 0u, 0u},
        {"empty-pop", UINT16_C(0xbc00), UINT32_C(0xde00),
         ordinary, ordinary, 0u, 0u},
        {"empty-stmia", UINT16_C(0xc600), UINT32_C(0xdf00),
         ordinary, ordinary, 0u, 0u},
        {"stmia-base-not-lowest", UINT16_C(0xc641), UINT32_C(0xe000),
         ordinary + UINT32_C(0x40), ordinary + UINT32_C(0x40), 2u, 0u},
        {"unaligned-push", UINT16_C(0xb401), UINT32_C(0xe100),
         ordinary + UINT32_C(0x81), ordinary + UINT32_C(0x7d), 1u, 0u},
        {"cross-cache-block-push", UINT16_C(0xb5ff), UINT32_C(0xe200),
         DATA_BASE + UINT32_C(0x2404), DATA_BASE + UINT32_C(0x23e0),
         9u, 0u},
        {"pop-pc-invalid-target-low2", UINT16_C(0xbd00),
         UINT32_C(0xe300), ordinary + UINT32_C(0x100),
         ordinary + UINT32_C(0x100), 1u, UINT32_C(0xef02)},
    };
    uint8_t *baseline = (uint8_t *)malloc(sizeof g_ram);
    uint8_t *expected = (uint8_t *)malloc(sizeof g_ram);
    bool ok = false;

    if (!baseline || !expected) {
        fprintf(stderr,
                "jitbench: compact raw Thumb multi allocation failed\n");
        goto done;
    }
    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        if (!compact_raw_thumb_multi_exact_case(
                &cases[i], false, baseline, expected) ||
            !compact_raw_thumb_multi_exact_case(
                &cases[i], true, baseline, expected))
            goto done;
    }
    for (unsigned i = 0u; i < sizeof refusals / sizeof refusals[0]; i++) {
        if (!compact_raw_thumb_multi_refusal_case(
                &refusals[i], baseline, expected))
            goto done;
    }
    if (!compact_raw_thumb_multi_resident_fallback_case(
            &cases[3], 0u, baseline, expected) ||
        !compact_raw_thumb_multi_resident_fallback_case(
            &cases[3], 1u, baseline, expected) ||
        !compact_raw_thumb_multi_resident_fallback_case(
            &cases[0], 2u, baseline, expected) ||
        !compact_raw_thumb_multi_resident_fallback_case(
            &refusals[5], 3u, baseline, expected) ||
        !compact_raw_thumb_multi_resident_fallback_case(
            &refusals[6], 4u, baseline, expected))
        goto done;

    printf("COMPACT-RAW-THUMB-MULTI-ORACLE exact=yes cases=11 refusals=7 "
           "push=yes pop=yes stmia=yes ldmia=yes lr=yes "
           "pc-interwork=yes base-lowest=yes base-in-list=yes "
           "max-words=9 transactional=yes flat-ram=yes "
           "runtime-codegen=no\n");
    printf("COMPACT-RAW-THUMB-MULTI-RESIDENT-ORACLE exact=yes cases=11 "
           "dread=yes dwrite=yes hit-accounting=yes native-only=yes "
           "fallback=zero resident-refusals=5 cold-cache=yes "
           "stale-cache=yes consent=yes cross-block=yes "
           "invalid-target=yes transactional=yes runtime-codegen=no\n");
    ok = true;

done:
    free(baseline);
    free(expected);
    return ok;
}

typedef struct {
    const char *name;
    uint32_t insn;
    uint32_t pc;
    uint32_t base;
    uint32_t rm_value;
    uint32_t address;
    uint32_t load_value;
    bool executes;
    bool clear_carry;
} compact_raw_single_case_t;

static void seed_compact_raw_single_case(
        arm_cpu_t *cpu, const compact_raw_single_case_t *sc) {
    const bool indexed = (sc->insn & (1u << 25)) != 0u;
    const bool byte = (sc->insn & (1u << 22)) != 0u;
    const bool load = (sc->insn & (1u << 20)) != 0u;
    const unsigned rn = (sc->insn >> 16) & 15u;
    const unsigned rm = sc->insn & 15u;

    seed_cpu_at(cpu, &sc->insn, 1u, false, sc->pc);
    for (unsigned reg = 0u; reg < 15u; reg++)
        cpu->r[reg] = UINT32_C(0x73000000) |
                      (reg * UINT32_C(0x010203));
    if (rn < 15u) cpu->r[rn] = sc->base;
    if (indexed && rm < 15u) cpu->r[rm] = sc->rm_value;
    cpu->r[15] = sc->pc;
    cpu->cpsr &= ~ARM_CPSR_Z;
    if (sc->clear_carry) cpu->cpsr &= ~ARM_CPSR_C;

    if (byte)
        mem_w8(NULL, sc->address, (uint8_t)sc->load_value);
    else
        mem_w32(NULL, sc->address, sc->load_value);
    /* Stores start from a non-source sentinel while loads consume the exact
     * witness above. Full-RAM comparison catches width and address errors. */
    if (!load) {
        if (byte)
            mem_w8(NULL, sc->address, UINT8_C(0x5a));
        else
            mem_w32(NULL, sc->address, UINT32_C(0x5aa55aa5));
    }
}

static bool compact_raw_single_exact_case(
        const compact_raw_single_case_t *sc, bool resident,
        uint8_t *baseline, uint8_t *expected) {
    const bool pre = (sc->insn & (1u << 24)) != 0u;
    const bool write = (sc->insn & (1u << 21)) != 0u;
    const bool load = (sc->insn & (1u << 20)) != 0u;
    const bool access_priv = !(!pre && write);
    arm_bus_t write_bus = g_bus;
    arm_cpu_t initial;
    arm_cpu_t reference;
    arm_cpu_t compact;
    compact_raw_resident_oracle_context_t context;
    arm_status_t status;
    unsigned completed = UINT_MAX;
    unsigned native_completed = UINT_MAX;
    unsigned fallback_completed = UINT_MAX;
    bool run_ok;

    write_bus.host_ram_write = mem_host_ram;
    seed_compact_raw_single_case(&initial, sc);
    if (resident && sc->executes) {
        if (load) {
            oracle_warm_dread_as(&initial, sc->address, access_priv);
        } else {
            initial.bus = &write_bus;
            oracle_warm_dwrite(&initial, sc->address, access_priv);
        }
    }
    reference = initial;
    compact = initial;
    memcpy(baseline, g_ram, sizeof g_ram);
    status = arm_step(&reference);
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);

    if (resident) {
        memset(&context, 0, sizeof context);
        context.cpu = &compact;
        context.status = ARM_OK;
        context.code = g_ram;
        context.code_bytes = (uint32_t)sizeof g_ram;
        run_ok = a64_compact_raw_run_code_window_resident(
            &compact, &g_ram[sc->pc], sc->pc, 4u, 1u,
            compact_raw_resident_oracle_step, &context, &completed,
            &native_completed, &fallback_completed);
    } else {
        memset(&context, 0, sizeof context);
        run_ok = a64_compact_raw_run(
            &compact, &g_ram[sc->pc], sc->pc, 4u, 1u,
            g_ram, sizeof g_ram, &completed);
        native_completed = completed;
        fallback_completed = 0u;
    }

    if (!run_ok || status != ARM_OK || completed != 1u ||
        native_completed != 1u || fallback_completed != 0u ||
        (resident && (context.status != ARM_OK || context.calls != 0u)) ||
        !(resident ? static_vfp_states_equal(&reference, &compact)
                    : static_vfp_arch_states_equal(&reference, &compact)) ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw A32 single %s %s mismatch "
                "(completed/native/fallback/calls %u/%u/%u/%u, "
                "status=%d/%d, dread=%" PRIu64 "/%" PRIu64 ", "
                "dwrite=%" PRIu64 "/%" PRIu64 ")\n",
                resident ? "resident" : "flat", sc->name,
                completed, native_completed, fallback_completed,
                resident ? context.calls : 0u, (int)status,
                resident ? (int)context.status : (int)ARM_OK,
                reference.dread_hits, compact.dread_hits,
                reference.dwrite_hits, compact.dwrite_hits);
        return false;
    }
    return true;
}

static bool compact_raw_single_refusal_case(
        const compact_raw_single_case_t *sc,
        uint8_t *baseline, uint8_t *expected) {
    arm_cpu_t reference;
    arm_cpu_t compact;
    arm_cpu_t before;
    arm_status_t reference_status;
    arm_status_t compact_status;
    unsigned completed = UINT_MAX;

    seed_compact_raw_single_case(&reference, sc);
    compact = reference;
    before = compact;
    memcpy(baseline, g_ram, sizeof g_ram);
    reference_status = arm_step(&reference);
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);
    if (!a64_compact_raw_run(
            &compact, &g_ram[sc->pc], sc->pc, 4u, 1u,
            g_ram, sizeof g_ram, &completed) ||
        completed != 0u || memcmp(&before, &compact, sizeof compact) != 0 ||
        memcmp(baseline, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw A32 single refusal %s changed state "
                "(completed=%u)\n", sc->name, completed);
        return false;
    }
    compact_status = arm_step(&compact);
    if (compact_status != reference_status ||
        memcmp(&reference, &compact, sizeof compact) != 0 ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw A32 single refusal %s fallback "
                "diverged (status=%d/%d)\n",
                sc->name, (int)reference_status, (int)compact_status);
        return false;
    }
    return true;
}

static bool compact_raw_single_resident_fallback_case(
        const compact_raw_single_case_t *sc, unsigned mode,
        uint8_t *baseline, uint8_t *expected) {
    const bool pre = (sc->insn & (1u << 24)) != 0u;
    const bool write = (sc->insn & (1u << 21)) != 0u;
    const bool load = (sc->insn & (1u << 20)) != 0u;
    const bool access_priv = !(!pre && write);
    const bool no_retire = mode == 4u;
    arm_bus_t write_bus = g_bus;
    arm_cpu_t reference;
    arm_cpu_t resident;
    compact_raw_resident_oracle_context_t context;
    arm_status_t reference_status;
    unsigned completed = UINT_MAX;
    unsigned native_completed = UINT_MAX;
    unsigned fallback_completed = UINT_MAX;

    write_bus.host_ram_write = mem_host_ram;
    seed_compact_raw_single_case(&reference, sc);
    if (load) {
        if (mode == 1u) {
            oracle_warm_dread_as(&reference, sc->address, access_priv);
            reference.tlb_gen++;
        } else if (mode == 3u) {
            /* The translation form requires an unprivileged tag. A valid
             * privileged witness must not be reused. */
            oracle_warm_dread_as(&reference, sc->address, true);
        } else if (mode == 4u) {
            oracle_warm_dread_as(&reference, sc->address, access_priv);
        }
    } else {
        if (mode != 2u) reference.bus = &write_bus;
        oracle_warm_dwrite(&reference, sc->address, access_priv);
    }
    resident = reference;
    memcpy(baseline, g_ram, sizeof g_ram);
    reference_status = arm_step(&reference);
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);

    memset(&context, 0, sizeof context);
    context.cpu = &resident;
    context.status = ARM_OK;
    context.code = g_ram;
    context.code_bytes = (uint32_t)sizeof g_ram;
    if (!a64_compact_raw_run_code_window_resident(
            &resident, &g_ram[sc->pc], sc->pc, 4u, 1u,
            compact_raw_resident_oracle_step, &context, &completed,
            &native_completed, &fallback_completed) ||
        completed != (no_retire ? 0u : 1u) || native_completed != 0u ||
        fallback_completed != (no_retire ? 0u : 1u) ||
        context.calls != (no_retire ? 0u : 1u) ||
        context.status != reference_status ||
        memcmp(&reference, &resident, sizeof resident) != 0 ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw resident A32 single fallback %s "
                "mode=%u diverged (completed/native/fallback/calls "
                "%u/%u/%u/%u status=%d/%d)\n",
                sc->name, mode, completed, native_completed,
                fallback_completed, context.calls,
                (int)reference_status, (int)context.status);
        return false;
    }
    return true;
}

static bool validate_compact_raw_a32_single_oracles(void) {
    const uint32_t ordinary = DATA_BASE + UINT32_C(0x800);
    const compact_raw_single_case_t cases[] = {
        {"str-word-imm-pre-up",
         A32_SINGLE_MODE2(14,0,1,1,0,0,0,4,3,12),
         UINT32_C(0xb000), ordinary, 0u, ordinary + 12u, 0u, true, false},
        {"ldr-word-imm-pre-down",
         A32_SINGLE_MODE2(14,0,1,0,0,0,1,4,3,16),
         UINT32_C(0xb100), ordinary + UINT32_C(0x40), 0u,
         ordinary + UINT32_C(0x30), UINT32_C(0x11223344), true, false},
        {"str-byte-imm-pre-up",
         A32_SINGLE_MODE2(14,0,1,1,1,0,0,4,3,5),
         UINT32_C(0xb200), ordinary + UINT32_C(0x80), 0u,
         ordinary + UINT32_C(0x85), 0u, true, false},
        {"ldr-byte-imm-pre-down",
         A32_SINGLE_MODE2(14,0,1,0,1,0,1,4,3,3),
         UINT32_C(0xb300), ordinary + UINT32_C(0xc0), 0u,
         ordinary + UINT32_C(0xbd), UINT32_C(0xa5), true, false},
        {"str-word-post-up",
         A32_SINGLE_MODE2(14,0,0,1,0,0,0,4,3,12),
         UINT32_C(0xb400), ordinary + UINT32_C(0x100), 0u,
         ordinary + UINT32_C(0x100), 0u, true, false},
        {"ldr-word-post-down",
         A32_SINGLE_MODE2(14,0,0,0,0,0,1,4,3,8),
         UINT32_C(0xb500), ordinary + UINT32_C(0x140), 0u,
         ordinary + UINT32_C(0x140), UINT32_C(0x22334455), true, false},
        {"strt-word-unprivileged",
         A32_SINGLE_MODE2(14,0,0,1,0,1,0,4,3,4),
         UINT32_C(0xb600), ordinary + UINT32_C(0x180), 0u,
         ordinary + UINT32_C(0x180), 0u, true, false},
        {"ldrt-word-unprivileged",
         A32_SINGLE_MODE2(14,0,0,1,0,1,1,4,3,4),
         UINT32_C(0xb700), ordinary + UINT32_C(0x1c0), 0u,
         ordinary + UINT32_C(0x1c0), UINT32_C(0x33445566), true, false},
        {"strbt-byte-unprivileged",
         A32_SINGLE_MODE2(14,0,0,1,1,1,0,4,3,1),
         UINT32_C(0xb800), ordinary + UINT32_C(0x200), 0u,
         ordinary + UINT32_C(0x200), 0u, true, false},
        {"ldrbt-byte-unprivileged",
         A32_SINGLE_MODE2(14,0,0,1,1,1,1,4,3,1),
         UINT32_C(0xb900), ordinary + UINT32_C(0x240), 0u,
         ordinary + UINT32_C(0x240), UINT32_C(0x6d), true, false},
        {"str-word-pre-writeback",
         A32_SINGLE_MODE2(14,0,1,1,0,1,0,4,3,8),
         UINT32_C(0xba00), ordinary + UINT32_C(0x280), 0u,
         ordinary + UINT32_C(0x288), 0u, true, false},
        {"ldr-word-pre-writeback",
         A32_SINGLE_MODE2(14,0,1,0,0,1,1,4,3,8),
         UINT32_C(0xbb00), ordinary + UINT32_C(0x2c0), 0u,
         ordinary + UINT32_C(0x2b8), UINT32_C(0x44556677), true, false},
        {"ldr-word-pc-base",
         A32_SINGLE_MODE2(14,0,1,1,0,0,1,15,2,0x100),
         UINT32_C(0xbc00), 0u, 0u, UINT32_C(0xbd08),
         UINT32_C(0x55667788), true, false},
        {"str-word-pc-source",
         A32_SINGLE_MODE2(14,0,1,1,0,0,0,4,15,0),
         UINT32_C(0xbd00), ordinary + UINT32_C(0x300), 0u,
         ordinary + UINT32_C(0x300), 0u, true, false},
        {"ldr-register-lsl-2",
         A32_SINGLE_MODE2(14,1,1,1,0,0,1,4,3,0x101),
         UINT32_C(0xbe00), ordinary + UINT32_C(0x340), 3u,
         ordinary + UINT32_C(0x34c), UINT32_C(0x66778899), true, false},
        {"str-register-lsr-0",
         A32_SINGLE_MODE2(14,1,1,1,0,0,0,4,3,0x021),
         UINT32_C(0xbf00), ordinary + UINT32_C(0x380), UINT32_MAX,
         ordinary + UINT32_C(0x380), 0u, true, false},
        {"ldr-register-asr-0",
         A32_SINGLE_MODE2(14,1,1,1,0,0,1,4,3,0x041),
         UINT32_C(0xc000), ordinary + UINT32_C(0x3c0),
         UINT32_C(0x7fffffff), ordinary + UINT32_C(0x3c0),
         UINT32_C(0x778899aa), true, false},
        {"str-register-ror-8",
         A32_SINGLE_MODE2(14,1,1,1,0,0,0,4,3,0x461),
         UINT32_C(0xc100), ordinary + UINT32_C(0x400), UINT32_C(0x400),
         ordinary + UINT32_C(0x404), 0u, true, false},
        {"ldr-register-rrx",
         A32_SINGLE_MODE2(14,1,1,1,0,0,1,4,3,0x061),
         UINT32_C(0xc200), ordinary + UINT32_C(0x440), 8u,
         ordinary + UINT32_C(0x444), UINT32_C(0x8899aabb), true, true},
        {"ldr-pc-arm-target",
         A32_SINGLE_MODE2(14,0,1,1,0,0,1,4,15,0),
         UINT32_C(0xc300), ordinary + UINT32_C(0x480), 0u,
         ordinary + UINT32_C(0x480), UINT32_C(0xc800), true, false},
        {"ldr-pc-thumb-target-low1",
         A32_SINGLE_MODE2(14,0,1,1,0,0,1,4,15,0),
         UINT32_C(0xc400), ordinary + UINT32_C(0x4c0), 0u,
         ordinary + UINT32_C(0x4c0), UINT32_C(0xd001), true, false},
        {"ldr-pc-thumb-target-low3",
         A32_SINGLE_MODE2(14,0,1,1,0,0,1,4,15,0),
         UINT32_C(0xc500), ordinary + UINT32_C(0x500), 0u,
         ordinary + UINT32_C(0x500), UINT32_C(0xd103), true, false},
        {"failed-condition-valid-cold",
         A32_SINGLE_MODE2(0,0,1,1,0,0,1,4,3,0),
         UINT32_C(0xc600), ordinary + UINT32_C(0x540), 0u,
         ordinary + UINT32_C(0x540), UINT32_C(0x99aabbcc), false, false},
        {"failed-condition-before-invalid-byte-pc",
         A32_SINGLE_MODE2(0,0,1,1,1,0,1,4,15,0),
         UINT32_C(0xc700), ordinary + UINT32_C(0x580), 0u,
         ordinary + UINT32_C(0x580), UINT32_C(0x7e), false, false},
    };
    const compact_raw_single_case_t refusals[] = {
        {"writeback-base-data-alias",
         A32_SINGLE_MODE2(14,0,1,1,0,1,1,4,4,4),
         UINT32_C(0xc900), ordinary, 0u, ordinary + 4u,
         UINT32_C(0x10203040), true, false},
        {"pc-base-writeback",
         A32_SINGLE_MODE2(14,0,1,1,0,1,1,15,0,0x100),
         UINT32_C(0xca00), 0u, 0u, UINT32_C(0xcb08),
         UINT32_C(0x21314151), true, false},
        {"register-offset-pc",
         A32_SINGLE_MODE2(14,1,1,1,0,0,1,4,0,0x00f),
         UINT32_C(0xcb00), ordinary + UINT32_C(0x40), 0u,
         ordinary + UINT32_C(0x40), UINT32_C(0x32425262), true, false},
        {"register-offset-reserved-bit4",
         A32_SINGLE_MODE2(14,1,1,1,0,0,1,4,0,0x010),
         UINT32_C(0xcc00), ordinary + UINT32_C(0x80), 0u,
         ordinary + UINT32_C(0x80), UINT32_C(0x43536373), true, false},
        {"byte-data-pc",
         A32_SINGLE_MODE2(14,0,1,1,1,0,1,4,15,0),
         UINT32_C(0xcd00), ordinary + UINT32_C(0xc0), 0u,
         ordinary + UINT32_C(0xc0), UINT32_C(0x7f), true, false},
        {"ldrt-pc",
         A32_SINGLE_MODE2(14,0,0,1,0,1,1,4,15,4),
         UINT32_C(0xce00), ordinary + UINT32_C(0x100), 0u,
         ordinary + UINT32_C(0x100), UINT32_C(0xe001), true, false},
        {"unaligned-word",
         A32_SINGLE_MODE2(14,0,1,1,0,0,1,4,0,0),
         UINT32_C(0xcf00), ordinary + UINT32_C(0x141), 0u,
         ordinary + UINT32_C(0x141), UINT32_C(0x65758595), true, false},
        {"ldr-pc-invalid-target-low2",
         A32_SINGLE_MODE2(14,0,1,1,0,0,1,4,15,0),
         UINT32_C(0xd000), ordinary + UINT32_C(0x180), 0u,
         ordinary + UINT32_C(0x180), UINT32_C(0xe002), true, false},
    };
    uint8_t *baseline = (uint8_t *)malloc(sizeof g_ram);
    uint8_t *expected = (uint8_t *)malloc(sizeof g_ram);
    bool ok = false;

    if (!baseline || !expected) {
        fprintf(stderr,
                "jitbench: compact raw A32 single allocation failed\n");
        goto done;
    }
    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        if (!compact_raw_single_exact_case(
                &cases[i], false, baseline, expected) ||
            !compact_raw_single_exact_case(
                &cases[i], true, baseline, expected))
            goto done;
    }
    for (unsigned i = 0u; i < sizeof refusals / sizeof refusals[0]; i++) {
        if (!compact_raw_single_refusal_case(
                &refusals[i], baseline, expected))
            goto done;
    }
    /* Exact interpreter convergence from each guarded resident refusal:
     * cold and stale reads, revoked write consent, a wrong privilege tag and
     * an invalid interworking target after a valid DREAD lookup. */
    if (!compact_raw_single_resident_fallback_case(
            &cases[1], 0u, baseline, expected) ||
        !compact_raw_single_resident_fallback_case(
            &cases[1], 1u, baseline, expected) ||
        !compact_raw_single_resident_fallback_case(
            &cases[0], 2u, baseline, expected) ||
        !compact_raw_single_resident_fallback_case(
            &cases[7], 3u, baseline, expected) ||
        !compact_raw_single_resident_fallback_case(
            &refusals[7], 4u, baseline, expected))
        goto done;

    printf("COMPACT-RAW-A32-SINGLE-ORACLE exact=yes cases=24 refusals=8 "
           "word=yes byte=yes immediate=yes register-shifts=all "
           "indexing=pre-post writeback=yes unprivileged=yes pc-base=yes "
           "pc-store=yes pc-load-interwork=yes condition-before-guard=yes "
           "transactional=yes fallback-convergence=yes flat-ram=yes "
           "runtime-codegen=no\n");
    printf("COMPACT-RAW-A32-SINGLE-RESIDENT-ORACLE exact=yes cases=24 "
           "dread=yes dwrite=yes privilege-tags=yes hit-accounting=yes "
           "native-only=yes fallback=zero resident-refusals=5 "
           "cold-cache=yes stale-cache=yes consent=yes invalid-target=yes "
           "transactional=yes runtime-codegen=no\n");
    ok = true;

done:
    free(baseline);
    free(expected);
    return ok;
}

typedef struct {
    const char *name;
    uint32_t insn;
    uint32_t pc;
    unsigned rn;
    uint32_t base;
    uint32_t start;
    unsigned words;
    bool executes;
    uint32_t final_word;
} compact_raw_block_case_t;

static void seed_compact_raw_block_case(
        arm_cpu_t *cpu, const compact_raw_block_case_t *sc) {
    seed_cpu_at(cpu, &sc->insn, 1u, false, sc->pc);
    for (unsigned reg = 0u; reg < 15u; reg++)
        cpu->r[reg] = UINT32_C(0x71000000) |
                      (reg * UINT32_C(0x010101));
    if (sc->rn < 15u) cpu->r[sc->rn] = sc->base;
    cpu->r[15] = sc->pc;
    for (unsigned word = 0u; word < sc->words; word++)
        mem_w32(NULL, sc->start + word * 4u,
                UINT32_C(0x51000000) |
                ((sc->pc & UINT32_C(0xff)) << 16) |
                (word * UINT32_C(0x010101)));
    if (sc->final_word && sc->words)
        mem_w32(NULL, sc->start + (sc->words - 1u) * 4u,
                sc->final_word);
}

static bool compact_raw_block_exact_case(
        const compact_raw_block_case_t *sc, bool resident,
        uint8_t *baseline, uint8_t *expected) {
    const bool load = (sc->insn & (1u << 20)) != 0u;
    arm_bus_t write_bus = g_bus;
    arm_cpu_t initial;
    arm_cpu_t reference;
    arm_cpu_t compact;
    compact_raw_resident_oracle_context_t context;
    arm_status_t status;
    unsigned completed = UINT_MAX;
    unsigned native_completed = UINT_MAX;
    unsigned fallback_completed = UINT_MAX;
    bool run_ok;

    write_bus.host_ram_write = mem_host_ram;
    seed_compact_raw_block_case(&initial, sc);
    if (resident) {
        if (load) {
            if (sc->executes) oracle_warm_dread(&initial, sc->start);
        } else {
            initial.bus = &write_bus;
            if (sc->executes)
                oracle_warm_dwrite(&initial, sc->start, true);
        }
    }
    reference = initial;
    compact = initial;
    memcpy(baseline, g_ram, sizeof g_ram);
    status = arm_step(&reference);
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);

    if (resident) {
        memset(&context, 0, sizeof context);
        context.cpu = &compact;
        context.status = ARM_OK;
        context.code = g_ram;
        context.code_bytes = (uint32_t)sizeof g_ram;
        run_ok = a64_compact_raw_run_code_window_resident(
            &compact, &g_ram[sc->pc], sc->pc, 4u, 1u,
            compact_raw_resident_oracle_step, &context, &completed,
            &native_completed, &fallback_completed);
    } else {
        memset(&context, 0, sizeof context);
        run_ok = a64_compact_raw_run(
            &compact, &g_ram[sc->pc], sc->pc, 4u, 1u,
            g_ram, sizeof g_ram, &completed);
        native_completed = completed;
        fallback_completed = 0u;
    }

    if (!run_ok || status != ARM_OK || completed != 1u ||
        native_completed != 1u || fallback_completed != 0u ||
        (resident && (context.status != ARM_OK || context.calls != 0u)) ||
        !(resident ? static_vfp_states_equal(&reference, &compact)
                    : static_vfp_arch_states_equal(&reference, &compact)) ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw A32 block %s %s mismatch "
                "(completed/native/fallback/calls %u/%u/%u/%u, "
                "status=%d/%d, dread=%" PRIu64 "/%" PRIu64 ", "
                "dwrite=%" PRIu64 "/%" PRIu64 ")\n",
                resident ? "resident" : "flat", sc->name,
                completed, native_completed, fallback_completed,
                resident ? context.calls : 0u, (int)status,
                resident ? (int)context.status : (int)ARM_OK,
                reference.dread_hits, compact.dread_hits,
                reference.dwrite_hits, compact.dwrite_hits);
        return false;
    }
    return true;
}

static bool compact_raw_block_refusal_case(
        const compact_raw_block_case_t *sc,
        uint8_t *baseline, uint8_t *expected) {
    arm_cpu_t reference;
    arm_cpu_t compact;
    arm_cpu_t before;
    arm_status_t reference_status;
    arm_status_t compact_status;
    unsigned completed = UINT_MAX;

    seed_compact_raw_block_case(&reference, sc);
    compact = reference;
    before = compact;
    memcpy(baseline, g_ram, sizeof g_ram);
    reference_status = arm_step(&reference);
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);
    if (!a64_compact_raw_run(
            &compact, &g_ram[sc->pc], sc->pc, 4u, 1u,
            g_ram, sizeof g_ram, &completed) ||
        completed != 0u || memcmp(&before, &compact, sizeof compact) != 0 ||
        memcmp(baseline, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw A32 block refusal %s changed state "
                "(completed=%u)\n", sc->name, completed);
        return false;
    }
    compact_status = arm_step(&compact);
    if (compact_status != reference_status ||
        memcmp(&reference, &compact, sizeof compact) != 0 ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw A32 block refusal %s fallback "
                "diverged (status=%d/%d)\n",
                sc->name, (int)reference_status, (int)compact_status);
        return false;
    }
    return true;
}

static bool compact_raw_block_resident_fallback_case(
        const compact_raw_block_case_t *sc, unsigned mode,
        uint8_t *baseline, uint8_t *expected) {
    const bool load = (sc->insn & (1u << 20)) != 0u;
    const bool no_retire = mode == 4u;
    arm_bus_t write_bus = g_bus;
    arm_cpu_t reference;
    arm_cpu_t resident;
    compact_raw_resident_oracle_context_t context;
    arm_status_t reference_status;
    unsigned completed = UINT_MAX;
    unsigned native_completed = UINT_MAX;
    unsigned fallback_completed = UINT_MAX;

    write_bus.host_ram_write = mem_host_ram;
    seed_compact_raw_block_case(&reference, sc);
    if (load) {
        if (mode == 1u) {
            oracle_warm_dread(&reference, sc->start);
            reference.tlb_gen++;
        } else if (mode == 4u) {
            oracle_warm_dread(&reference, sc->start);
        }
    } else {
        if (mode != 2u) reference.bus = &write_bus;
        oracle_warm_dwrite(&reference, sc->start, true);
    }
    resident = reference;
    memcpy(baseline, g_ram, sizeof g_ram);
    reference_status = arm_step(&reference);
    memcpy(expected, g_ram, sizeof g_ram);
    memcpy(g_ram, baseline, sizeof g_ram);

    memset(&context, 0, sizeof context);
    context.cpu = &resident;
    context.status = ARM_OK;
    context.code = g_ram;
    context.code_bytes = (uint32_t)sizeof g_ram;
    if (!a64_compact_raw_run_code_window_resident(
            &resident, &g_ram[sc->pc], sc->pc, 4u, 1u,
            compact_raw_resident_oracle_step, &context, &completed,
            &native_completed, &fallback_completed) ||
        completed != (no_retire ? 0u : 1u) || native_completed != 0u ||
        fallback_completed != (no_retire ? 0u : 1u) ||
        context.calls != (no_retire ? 0u : 1u) ||
        context.status != reference_status ||
        memcmp(&reference, &resident, sizeof resident) != 0 ||
        memcmp(expected, g_ram, sizeof g_ram) != 0) {
        fprintf(stderr,
                "jitbench: compact raw resident A32 block fallback %s "
                "mode=%u diverged (completed/native/fallback/calls "
                "%u/%u/%u/%u status=%d/%d)\n",
                sc->name, mode, completed, native_completed,
                fallback_completed, context.calls,
                (int)reference_status, (int)context.status);
        return false;
    }
    return true;
}

static bool validate_compact_raw_a32_block_oracles(void) {
    const uint32_t ordinary = DATA_BASE + UINT32_C(0x800);
    const compact_raw_block_case_t cases[] = {
        {"stm-ia", A32_BLOCK(14,0,1,0,0,0,4,UINT32_C(0x8109)),
         UINT32_C(0x8c00), 4u, ordinary, ordinary, 4u, true, 0u},
        {"stm-ib-writeback",
         A32_BLOCK(14,1,1,0,1,0,12,UINT32_C(0x4106)),
         UINT32_C(0x8d00), 12u, ordinary, ordinary + 4u, 4u, true, 0u},
        {"stm-da-sp", A32_BLOCK(14,0,0,0,0,0,13,UINT32_C(0x4210)),
         UINT32_C(0x8e00), 13u, ordinary + UINT32_C(0x40),
         ordinary + UINT32_C(0x38), 3u, true, 0u},
        {"stm-db-writeback",
         A32_BLOCK(14,1,0,0,1,0,11,UINT32_C(0x8481)),
         UINT32_C(0x8f00), 11u, ordinary, ordinary - 16u, 4u, true, 0u},
        {"stm-db-base-lowest-writeback",
         A32_BLOCK(14,1,0,0,1,0,4,UINT32_C(0x4110)),
         UINT32_C(0x9000), 4u, ordinary, ordinary - 12u, 3u, true, 0u},
        {"stm-failed-condition",
         A32_BLOCK(0,0,1,0,0,0,4,UINT32_C(0x4004)),
         UINT32_C(0x9100), 4u, ordinary, ordinary, 2u, false, 0u},
        {"stm-sixteen-words",
         A32_BLOCK(14,0,1,0,0,0,7,UINT32_C(0xffff)),
         UINT32_C(0x9200), 7u, DATA_BASE + UINT32_C(0xfc0),
         DATA_BASE + UINT32_C(0xfc0), 16u, true, 0u},
        {"ldm-ia-base-in-list",
         A32_BLOCK(14,0,1,0,0,1,4,UINT32_C(0x4119)),
         UINT32_C(0x9300), 4u, ordinary, ordinary, 5u, true, 0u},
        {"ldm-ib-writeback",
         A32_BLOCK(14,1,1,0,1,1,12,UINT32_C(0x4106)),
         UINT32_C(0x9400), 12u, ordinary, ordinary + 4u, 4u, true, 0u},
        {"ldm-da-sp", A32_BLOCK(14,0,0,0,0,1,13,UINT32_C(0x4210)),
         UINT32_C(0x9500), 13u, ordinary + UINT32_C(0x40),
         ordinary + UINT32_C(0x38), 3u, true, 0u},
        {"ldm-db-writeback",
         A32_BLOCK(14,1,0,0,1,1,11,UINT32_C(0x4481)),
         UINT32_C(0x9600), 11u, ordinary, ordinary - 16u, 4u, true, 0u},
        {"ldm-failed-condition",
         A32_BLOCK(0,0,1,0,0,1,4,UINT32_C(0x4004)),
         UINT32_C(0x9700), 4u, ordinary, ordinary, 2u, false, 0u},
        {"ldm-fifteen-words",
         A32_BLOCK(14,0,1,0,0,1,7,UINT32_C(0x7fff)),
         UINT32_C(0x9800), 7u, DATA_BASE + UINT32_C(0xfc4),
         DATA_BASE + UINT32_C(0xfc4), 15u, true, 0u},
        {"failed-condition-before-invalid-shape",
         A32_BLOCK(0,0,1,1,0,0,4,UINT32_C(0x0003)),
         UINT32_C(0x9900), 4u, ordinary, ordinary, 2u, false, 0u},
        {"ldm-pc-arm-target",
         A32_BLOCK(14,0,1,0,0,1,4,UINT32_C(0x8009)),
         UINT32_C(0xa200), 4u, ordinary + UINT32_C(0x100),
         ordinary + UINT32_C(0x100), 3u, true, UINT32_C(0xe400)},
        {"ldm-pc-thumb-target-low1",
         A32_BLOCK(14,1,0,0,1,1,11,UINT32_C(0x8044)),
         UINT32_C(0xa300), 11u, ordinary + UINT32_C(0x180),
         ordinary + UINT32_C(0x174), 3u, true, UINT32_C(0xe501)},
        {"ldm-pc-thumb-target-low3",
         A32_BLOCK(14,0,1,0,0,1,4,UINT32_C(0x8021)),
         UINT32_C(0xa400), 4u, ordinary + UINT32_C(0x200),
         ordinary + UINT32_C(0x200), 3u, true, UINT32_C(0xe603)},
    };
    const compact_raw_block_case_t refusals[] = {
        {"user-bank", A32_BLOCK(14,0,1,1,0,0,4,UINT32_C(0x0003)),
         UINT32_C(0x9a00), 4u, ordinary, ordinary, 2u, true, 0u},
        {"pc-base", A32_BLOCK(14,0,1,0,0,0,15,UINT32_C(0x0003)),
         UINT32_C(0x9b00), 15u, ordinary, ordinary, 2u, true, 0u},
        {"empty-list", A32_BLOCK(14,0,1,0,0,0,4,UINT32_C(0x0000)),
         UINT32_C(0x9c00), 4u, ordinary, ordinary, 0u, true, 0u},
        {"stm-writeback-alias",
         A32_BLOCK(14,0,1,0,1,0,4,UINT32_C(0x0011)),
         UINT32_C(0x9d00), 4u, ordinary, ordinary, 2u, true, 0u},
        {"ldm-writeback-alias",
         A32_BLOCK(14,0,1,0,1,1,4,UINT32_C(0x0010)),
         UINT32_C(0x9e00), 4u, ordinary, ordinary, 1u, true, 0u},
        {"ldm-pc-invalid-target-low2",
         A32_BLOCK(14,0,1,0,0,1,4,UINT32_C(0x8001)),
         UINT32_C(0x9f00), 4u, ordinary, ordinary, 2u, true,
         UINT32_C(0xe702)},
        {"unaligned", A32_BLOCK(14,0,1,0,0,0,4,UINT32_C(0x0003)),
         UINT32_C(0xa000), 4u, ordinary + 1u, ordinary + 1u, 2u, true, 0u},
        {"cross-cache-block",
         A32_BLOCK(14,0,1,0,0,0,4,UINT32_C(0x000f)),
         UINT32_C(0xa100), 4u, DATA_BASE + UINT32_C(0x13f8),
         DATA_BASE + UINT32_C(0x13f8), 4u, true, 0u},
    };
    uint8_t *baseline = (uint8_t *)malloc(sizeof g_ram);
    uint8_t *expected = (uint8_t *)malloc(sizeof g_ram);
    bool ok = false;

    if (!baseline || !expected) {
        fprintf(stderr,
                "jitbench: compact raw A32 block allocation failed\n");
        goto done;
    }
    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        if (!compact_raw_block_exact_case(
                &cases[i], false, baseline, expected) ||
            !compact_raw_block_exact_case(
                &cases[i], true, baseline, expected))
            goto done;
    }
    for (unsigned i = 0u; i < sizeof refusals / sizeof refusals[0]; i++) {
        if (!compact_raw_block_refusal_case(
                &refusals[i], baseline, expected))
            goto done;
    }
    /* Cache misses, stale translations, revoked write consent and a
     * cross-block store all enter the interpreter with a pristine
     * instruction. The callback must own the only retirement. */
    if (!compact_raw_block_resident_fallback_case(
            &cases[7], 0u, baseline, expected) ||
        !compact_raw_block_resident_fallback_case(
            &cases[7], 1u, baseline, expected) ||
        !compact_raw_block_resident_fallback_case(
            &cases[0], 2u, baseline, expected) ||
        !compact_raw_block_resident_fallback_case(
            &refusals[7], 3u, baseline, expected) ||
        !compact_raw_block_resident_fallback_case(
            &refusals[5], 4u, baseline, expected))
        goto done;

    printf("COMPACT-RAW-A32-BLOCK-ORACLE exact=yes cases=17 refusals=8 "
           "stm=yes ldm=yes modes=4 writeback=yes "
           "base-lowest-writeback=yes base-in-list=yes pc-store=yes "
           "pc-load-interwork=yes max-words=16 "
           "condition-before-guard=yes transactional=yes "
           "fallback-convergence=yes flat-ram=yes runtime-codegen=no\n");
    printf("COMPACT-RAW-A32-BLOCK-RESIDENT-ORACLE exact=yes cases=17 "
           "stm=yes ldm=yes dread=yes dwrite=yes hit-accounting=yes "
           "native-only=yes fallback=zero resident-refusals=5 "
           "cold-cache=yes stale-cache=yes consent=yes cross-block=yes "
           "invalid-target=yes transactional=yes "
           "runtime-codegen=no\n");
    ok = true;

done:
    free(baseline);
    free(expected);
    return ok;
}

static bool compact_raw_system_coprocessor_case(
        const char *name, const uint32_t *program, unsigned insns,
        uint32_t mode, unsigned expected_completed) {
    const uint32_t pc = UINT32_C(0x2c000);
    arm_cpu_t reference, compact;
    final_state_t reference_state, compact_state;
    arm_status_t status = ARM_OK;
    unsigned completed = UINT_MAX;

    seed_cpu_at(&reference, program, insns, false, pc);
    reference.cpsr = (reference.cpsr & ~ARM_CPSR_MODE_MASK) | mode;
    reference.cp15.tpidrurw = UINT32_C(0x13579bdf);
    reference.cp15.tpidruro = UINT32_C(0x2468ace0);
    reference.cp15.tpidrprw = UINT32_C(0x55aa55aa);
    compact = reference;

    for (unsigned i = 0u; i < expected_completed; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);
    if (!a64_compact_raw_run(
            &compact, &g_ram[pc], pc, insns * 4u, insns,
            g_ram, sizeof g_ram, &completed)) {
        fprintf(stderr,
                "jitbench: compact raw system-coprocessor %s refused "
                "runner contract\n", name);
        return false;
    }
    capture_state(&compact_state, &compact, ARM_OK, JIT_EXIT_NEXT);
    if (status != ARM_OK || completed != expected_completed ||
        !architectural_states_equal(&reference_state, &compact_state) ||
        memcmp(&reference.cp15, &compact.cp15, sizeof reference.cp15) != 0) {
        fprintf(stderr,
                "jitbench: compact raw system-coprocessor %s mismatch "
                "completed=%u/%u\n",
                name, completed, expected_completed);
        return false;
    }
    return true;
}

static bool validate_compact_raw_system_coprocessor_oracles(void) {
    static const uint32_t PRIVILEGED[] = {
        A32_COPROC_TRANSFER(14,0,1,0,0,14,0,0),  /* CP14 MRC -> zero */
        A32_COPROC_TRANSFER(14,0,0,0,1,14,0,0),  /* CP14 MCR ignored */
        UINT32_C(0xee070f5a),                     /* hot c7 cache op */
        A32_COPROC_TRANSFER(14,0,1,7,2,15,1,3),  /* c7 MRC -> zero */
        A32_COPROC_TRANSFER(14,0,1,13,3,15,2,0), /* TPIDRURW read */
        A32_COPROC_TRANSFER(14,0,0,13,4,15,2,0), /* TPIDRURW write */
        A32_COPROC_TRANSFER(14,0,1,13,5,15,3,0), /* TPIDRURO read */
        A32_COPROC_TRANSFER(14,0,0,13,6,15,3,0), /* TPIDRURO write */
        A32_COPROC_TRANSFER(14,0,1,13,7,15,4,0), /* TPIDRPRW read */
        A32_COPROC_TRANSFER(14,0,0,13,15,15,4,7),/* PC+8 write */
    };
    static const uint32_t USER[] = {
        UINT32_C(0xee070f5a),
        A32_COPROC_TRANSFER(14,0,1,7,0,15,1,3),
        A32_COPROC_TRANSFER(14,0,1,13,1,15,2,0),
        A32_COPROC_TRANSFER(14,0,0,13,2,15,2,0),
        A32_COPROC_TRANSFER(14,0,1,13,3,15,3,0),
    };
    static const uint32_t WFI_PREFIX[] = {
        UINT32_C(0xe2800001),
        UINT32_C(0xee072f90),
        UINT32_C(0xe2811001),
    };
    static const uint32_t FAILED_CONDITION_WFI[] = {
        UINT32_C(0x0e072f90),
        UINT32_C(0xe2800001),
    };
    static const uint32_t USER_CP14[] = {
        A32_COPROC_TRANSFER(14,0,1,0,0,14,0,0),
    };
    static const uint32_t USER_TPIDRURO_WRITE[] = {
        A32_COPROC_TRANSFER(14,0,0,13,0,15,3,0),
    };
    static const uint32_t USER_BAD_CRM[] = {
        A32_COPROC_TRANSFER(14,0,1,13,0,15,2,1),
    };
    static const uint32_t PRIV_CONTEXT_ID[] = {
        A32_COPROC_TRANSFER(14,0,1,13,0,15,1,0),
    };

    if (!a64_static_host_available()) {
        printf("COMPACT-RAW-SYSTEM-COPROCESSOR-ORACLE skip "
               "reason=signed-aarch64-unavailable\n");
        return true;
    }
    if (!compact_raw_system_coprocessor_case(
            "privileged", PRIVILEGED,
            (unsigned)(sizeof PRIVILEGED / sizeof PRIVILEGED[0]),
            ARM_MODE_SYS,
            (unsigned)(sizeof PRIVILEGED / sizeof PRIVILEGED[0])) ||
        !compact_raw_system_coprocessor_case(
            "user", USER, (unsigned)(sizeof USER / sizeof USER[0]),
            ARM_MODE_USR, (unsigned)(sizeof USER / sizeof USER[0])) ||
        !compact_raw_system_coprocessor_case(
            "wfi-prefix", WFI_PREFIX,
            (unsigned)(sizeof WFI_PREFIX / sizeof WFI_PREFIX[0]),
            ARM_MODE_SYS, 1u) ||
        !compact_raw_system_coprocessor_case(
            "failed-condition-wfi", FAILED_CONDITION_WFI,
            (unsigned)(sizeof FAILED_CONDITION_WFI /
                       sizeof FAILED_CONDITION_WFI[0]),
            ARM_MODE_SYS, 2u) ||
        !compact_raw_system_coprocessor_case(
            "user-cp14", USER_CP14, 1u, ARM_MODE_USR, 0u) ||
        !compact_raw_system_coprocessor_case(
            "user-tpidruro-write", USER_TPIDRURO_WRITE, 1u,
            ARM_MODE_USR, 0u) ||
        !compact_raw_system_coprocessor_case(
            "user-bad-crm", USER_BAD_CRM, 1u, ARM_MODE_USR, 0u) ||
        !compact_raw_system_coprocessor_case(
            "priv-context-id", PRIV_CONTEXT_ID, 1u, ARM_MODE_SYS, 0u))
        return false;

    printf("COMPACT-RAW-SYSTEM-COPROCESSOR-ORACLE exact=yes "
           "cp14=yes cp15-c7=yes wfi=fallback thread-id=rw "
           "privilege=guarded cases=8\n");
    return true;
}

/* The generated loop retains instruction width across ordinary retirements.
 * Prove both native state transitions and the following fetch in one call:
 * A32 BX enters Thumb, one Thumb ALU instruction retires, Thumb BX returns to
 * A32, and the next A32 ALU instruction retires. A stale width would decode
 * the second, third, or fourth instruction from the wrong byte boundary. */
static bool validate_compact_raw_mode_continuity_oracle(void) {
    const uint32_t pc = UINT32_C(0x7c00);
    const uint32_t program[] = {
        UINT32_C(0xe12fff10), /* A32 BX r0 -> pc+4 in Thumb state */
        UINT32_C(0x47103101), /* Thumb ADDS r1,#1; BX r2 -> A32 */
        UINT32_C(0xe2833001), /* A32 ADD r3,r3,#1 */
        UINT32_C(0xe2844001), /* unexecuted padding */
    };
    arm_cpu_t reference;
    arm_cpu_t compact;
    final_state_t reference_state;
    final_state_t compact_state;
    arm_status_t status = ARM_OK;
    unsigned completed = UINT_MAX;

    seed_cpu_at(&reference, program,
                (unsigned)(sizeof program / sizeof program[0]), false, pc);
    reference.r[0] = pc + UINT32_C(5);
    reference.r[1] = UINT32_C(41);
    reference.r[2] = pc + UINT32_C(8);
    reference.r[3] = UINT32_C(99);
    compact = reference;

    for (unsigned i = 0u; i < 4u; i++) {
        status = arm_step(&reference);
        if (status != ARM_OK) break;
    }
    capture_state(&reference_state, &reference, status, JIT_EXIT_NEXT);

    if (!a64_compact_raw_run(
            &compact, &g_ram[pc], pc, (uint32_t)sizeof program, 4u,
            g_ram, sizeof g_ram, &completed)) {
        fprintf(stderr,
                "jitbench: compact raw mode-continuity contract refused\n");
        return false;
    }
    capture_state(&compact_state, &compact, ARM_OK, JIT_EXIT_NEXT);
    if (status != ARM_OK || completed != 4u ||
        compact.r[1] != UINT32_C(42) ||
        compact.r[3] != UINT32_C(100) || compact.r[15] != pc + 12u ||
        (compact.cpsr & ARM_CPSR_T) != 0u ||
        !architectural_states_equal(&reference_state, &compact_state)) {
        fprintf(stderr,
                "jitbench: compact raw mode-continuity mismatch "
                "completed=%u r1=%08" PRIx32 " r3=%08" PRIx32
                " pc=%08" PRIx32 " cpsr=%08" PRIx32 "\n",
                completed, compact.r[1], compact.r[3], compact.r[15],
                compact.cpsr);
        return false;
    }

    printf("COMPACT-RAW-MODE-CONTINUITY-ORACLE exact=yes "
           "transitions=a32-thumb-a32 native-following-fetch=yes "
           "runtime-codegen=no\n");
    return true;
}

static bool validate_compact_raw_oracles(void) {
    static const unsigned result_ops[] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 12u, 13u, 14u, 15u,
    };
    uint32_t dp_program[sizeof result_ops / sizeof result_ops[0] * 2u];
    uint32_t flag_program[32];
    uint32_t shift_program[16];
    const uint32_t memory_branch[] = {
        UINT32_C(0xe5870000), /* STR r0,[r7,#0] */
        UINT32_C(0xe5971000), /* LDR r1,[r7,#0] */
        UINT32_C(0xe5072004), /* STR r2,[r7,#-4] */
        UINT32_C(0xe5173004), /* LDR r3,[r7,#-4] */
        UINT32_C(0xeb000000), /* BL to instruction 6 */
        UINT32_C(0xe2844001), /* skipped */
        UINT32_C(0xe2855001), /* ADD r5,r5,#1 */
        UINT32_C(0xea000000), /* branch beyond the byte window */
    };
    const uint32_t unsupported_prefix[] = {
        UINT32_C(0xe2800001), /* supported ADD */
        UINT32_C(0xe28f0001), /* PC operand: deliberately unsupported */
    };
    const uint32_t unsupported_first[] = {
        UINT32_C(0xe28f0001), /* PC operand */
    };
    const uint32_t window_data_stop[] = {
        UINT32_C(0xe2800001), /* supported ADD */
        UINT32_C(0xe5971000), /* data translation deliberately unavailable */
    };
    const uint32_t resident_mixed[] = {
        UINT32_C(0xe2800001), /* native ADD r0,r0,#1 */
        UINT32_C(0xe5870000), /* fallback STR r0,[r7,#0] */
        UINT32_C(0xe2422001), /* native SUB r2,r2,#1 */
        UINT32_C(0xe0000090), /* fallback MUL r0,r0,r0 */
        UINT32_C(0xe2855001), /* native ADD r5,r5,#1 */
    };
    const uint32_t resident_cross_sequential[] = {
        UINT32_C(0xe2800001), /* native ADD at 0x5ffc */
        UINT32_C(0xe0000090), /* fallback MUL at next 1 KiB window */
        UINT32_C(0xe2855001), /* native ADD after window publication */
    };
    const uint32_t resident_cross_fast[] = {
        UINT32_C(0xe2800001), /* native ADD at 0x5ffc */
        UINT32_C(0xe2844001), /* native ADD after no-retire publication */
        UINT32_C(0xe0000090), /* fallback MUL in the published window */
    };
    const uint32_t resident_cross_branch[] = {
        UINT32_C(0xe2800001), /* native ADD at 0x63f8 */
        UINT32_C(0xea000000), /* native branch 0x63fc -> 0x6404 */
        UINT32_C(0xe2844001), /* skipped */
        UINT32_C(0xe0000090), /* fallback MUL at branch target */
        UINT32_C(0xe2855001), /* native ADD in published window */
    };
    const uint32_t resident_stale_window[] = {
        UINT32_C(0xe2800001), /* native ADD at 0x67fc */
        UINT32_C(0xe0000090), /* first fallback publishes 0x6800 */
        UINT32_C(0xe2855001), /* native ADD in the published window */
        UINT32_C(0xe0000090), /* fallback continues without publication */
        UINT32_C(0xe2866001), /* must not execute via the stale window */
    };
    const uint32_t resident_fallback_interworking[] = {
        UINT32_C(0xe597f004), /* fallback LDR pc,[r7,#4] enters Thumb */
        DATA_BASE + UINT32_C(9), /* odd target at the following word */
        UINT32_C(0x00003101), /* native Thumb ADDS r1,#1 at target */
    };
    const uint32_t resident_window_cache[] = {
        UINT32_C(0xea000000), /* branch 0x73fc -> 0x7404 */
        UINT32_C(0xe2844001), /* skipped padding at 0x7400 */
        UINT32_C(0xeafffffc), /* branch 0x7404 -> 0x73fc */
    };
    const uint16_t resident_thumb_memory[] = {
        UINT16_C(0xba00), /* fallback REV publishes the Thumb window */
        UINT16_C(0x6833), /* native LDR r3,[r6,#0] via seeded DREAD */
        UINT16_C(0x6834), /* native LDR r4,[r6,#0] */
        UINT16_C(0x6030), /* native STR r0,[r6,#0] via consented DWRITE */
        UINT16_C(0x6032), /* native STR r2,[r6,#0] */
        UINT16_C(0xb401), /* unexecuted padding for a 12-byte window */
    };
    arm_cpu_t contract;
    final_state_t before, after;
    unsigned completed = UINT_MAX;

    if (!validate_compact_raw_system_coprocessor_oracles())
        return false;
    if (!validate_compact_raw_vfp_nonarith_oracles())
        return false;
    if (!validate_compact_raw_vfp_arithmetic_oracles())
        return false;
    if (!validate_compact_raw_vfp_narrow_oracles())
        return false;
    if (!validate_compact_raw_vfp_memory_oracles())
        return false;
    if (!validate_compact_raw_a32_single_oracles())
        return false;
    if (!validate_compact_raw_a32_block_oracles())
        return false;
    if (!validate_compact_raw_a32_register_shift_oracles())
        return false;

    for (unsigned i = 0u; i < sizeof result_ops / sizeof result_ops[0]; i++) {
        unsigned opcode = result_ops[i];
        unsigned rn = (i + 1u) & 7u;
        unsigned rd = (i + 2u) & 7u;
        unsigned rm = (i + 3u) & 7u;
        unsigned rot = i & 3u;
        unsigned imm = 0x11u + i;
        dp_program[i * 2u] = UINT32_C(0xe2000000) |
            (opcode << 21) | (rn << 16) | (rd << 12) |
            (rot << 8) | imm;
        dp_program[i * 2u + 1u] = UINT32_C(0xe0000000) |
            (opcode << 21) | (rn << 16) | (rd << 12) | rm;
    }
    for (unsigned opcode = 0u; opcode < 16u; opcode++) {
        unsigned rn = (opcode + 1u) & 7u;
        unsigned rd = (opcode + 2u) & 7u;
        unsigned rm = (opcode + 3u) & 7u;
        unsigned rot = opcode & 3u;
        unsigned imm = 0x41u + opcode;
        flag_program[opcode * 2u] = UINT32_C(0xe2100000) |
            (opcode << 21) | (rn << 16) | (rd << 12) |
            (rot << 8) | imm;
        flag_program[opcode * 2u + 1u] = UINT32_C(0xe0100000) |
            (opcode << 21) | (rn << 16) | (rd << 12) | rm;
    }
    for (unsigned type = 0u; type < 4u; type++) {
        static const unsigned amounts[4] = {0u, 1u, 7u, 31u};
        for (unsigned a = 0u; a < 4u; a++) {
            unsigned index = type * 4u + a;
            unsigned rd = index & 7u;
            unsigned rm = (index + 1u) & 7u;
            shift_program[index] = UINT32_C(0xe0100000) |
                (13u << 21) | (rd << 12) | (amounts[a] << 7) |
                (type << 5) | rm;
        }
    }
    if (!compact_raw_compare("data-processing", dp_program,
                             (unsigned)(sizeof dp_program /
                                        sizeof dp_program[0]),
                             UINT32_C(0x1000),
                             (unsigned)(sizeof dp_program /
                                        sizeof dp_program[0]),
                             (unsigned)(sizeof dp_program /
                                        sizeof dp_program[0]) + 1u,
                             (unsigned)(sizeof dp_program /
                                        sizeof dp_program[0])))
        return false;
    if (!validate_compact_raw_thumb_oracles())
        return false;
    if (!validate_compact_raw_thumb_multi_oracles())
        return false;
    if (!validate_compact_raw_a32_indirect_oracles())
        return false;
    if (!validate_compact_raw_mode_continuity_oracle())
        return false;
    if (!compact_raw_compare("data-processing-flags", flag_program,
                             (unsigned)(sizeof flag_program /
                                        sizeof flag_program[0]),
                             UINT32_C(0x1400),
                             (unsigned)(sizeof flag_program /
                                        sizeof flag_program[0]),
                             (unsigned)(sizeof flag_program /
                                        sizeof flag_program[0]) + 1u,
                             (unsigned)(sizeof flag_program /
                                        sizeof flag_program[0])))
        return false;
    if (!compact_raw_compare("register-immediate-shifts", shift_program,
                             (unsigned)(sizeof shift_program /
                                        sizeof shift_program[0]),
                             UINT32_C(0x1600),
                             (unsigned)(sizeof shift_program /
                                        sizeof shift_program[0]),
                             (unsigned)(sizeof shift_program /
                                        sizeof shift_program[0]) + 1u,
                             (unsigned)(sizeof shift_program /
                                        sizeof shift_program[0])))
        return false;
    if (!compact_raw_compare("conditions", A32_IMM_CONDITIONS,
                             (unsigned)(sizeof A32_IMM_CONDITIONS /
                                        sizeof A32_IMM_CONDITIONS[0]),
                             UINT32_C(0x1800),
                             (unsigned)(sizeof A32_IMM_CONDITIONS /
                                        sizeof A32_IMM_CONDITIONS[0]),
                             (unsigned)(sizeof A32_IMM_CONDITIONS /
                                        sizeof A32_IMM_CONDITIONS[0]) + 1u,
                             (unsigned)(sizeof A32_IMM_CONDITIONS /
                                        sizeof A32_IMM_CONDITIONS[0])))
        return false;
    if (!compact_raw_compare("memory-branch", memory_branch,
                             (unsigned)(sizeof memory_branch /
                                        sizeof memory_branch[0]),
                             UINT32_C(0x2000), 7u, 16u, 7u))
        return false;
    if (!compact_raw_compare("unsupported-prefix", unsupported_prefix,
                             2u, UINT32_C(0x3000), 1u, 2u, 1u))
        return false;
    if (!compact_raw_compare("unsupported-first", unsupported_first,
                             1u, UINT32_C(0x4000), 0u, 1u, 0u))
        return false;
    if (!compact_raw_window_compare("mmu-compute", dp_program,
                                    (unsigned)(sizeof dp_program /
                                               sizeof dp_program[0]),
                                    UINT32_C(0x4400),
                                    (unsigned)(sizeof dp_program /
                                               sizeof dp_program[0]),
                                    (unsigned)(sizeof dp_program /
                                               sizeof dp_program[0]) + 1u,
                                    (unsigned)(sizeof dp_program /
                                               sizeof dp_program[0])))
        return false;
    if (!compact_raw_window_compare("data-stop", window_data_stop, 2u,
                                    UINT32_C(0x4800), 1u, 2u, 1u))
        return false;
    if (!compact_raw_resident_compare(
            "continue", resident_mixed, 5u, false, UINT32_C(0x4c00),
            UINT32_C(0x4c00), 20u, 5u, 5u, 3u, 2u, 0u, true,
            false, 0u, false, 0u, false, false, 0u))
        return false;
    if (!compact_raw_resident_compare(
            "boundary-stop", resident_mixed, 5u, false, UINT32_C(0x4c00),
            UINT32_C(0x4c00), 20u, 2u, 5u, 1u, 1u, 1u, false,
            false, 0u, false, 0u, false, false, 0u))
        return false;
    if (!compact_raw_resident_compare(
            "sequential-window", resident_cross_sequential, 3u, false,
            UINT32_C(0x5ffc), UINT32_C(0x5c00), UINT32_C(0x400),
            3u, 3u, 2u, 1u, 0u, false, false, 0u, false, 0u,
            false, false, 0u))
        return false;
    if (!compact_raw_resident_compare(
            "no-retire-window", resident_cross_fast, 3u, false,
            UINT32_C(0x5ffc), UINT32_C(0x5c00), UINT32_C(0x400),
            3u, 3u, 2u, 1u, 0u, false, false, 0u, true, 1u,
            false, false, 0u))
        return false;
    if (!compact_raw_resident_compare(
            "window-refusal", resident_cross_sequential, 3u, false,
            UINT32_C(0x5ffc), UINT32_C(0x5c00), UINT32_C(0x400),
            2u, 3u, 1u, 1u, 0u, false, true, 0u, false, 0u,
            false, false, 0u))
        return false;
    if (!compact_raw_resident_compare(
            "branch-window", resident_cross_branch, 5u, false,
            UINT32_C(0x63f8), UINT32_C(0x6000), UINT32_C(0x400),
            4u, 4u, 3u, 1u, 0u, false, false, 0u, false, 0u,
            false, false, 0u))
        return false;
    if (!compact_raw_resident_compare(
            "stale-window", resident_stale_window, 5u, false,
            UINT32_C(0x67fc), UINT32_C(0x6400), UINT32_C(0x400),
            4u, 5u, 2u, 2u, 0u, false, false, 2u, false, 0u,
            false, false, 0u))
        return false;
    if (!compact_raw_resident_compare(
            "fallback-interworking", resident_fallback_interworking, 3u,
            false, DATA_BASE, DATA_BASE, 12u, 2u, 2u, 1u, 1u, 0u,
            false, false, 0u, false, 0u, false, false, 0u))
        return false;
    if (!compact_raw_resident_compare(
            "thumb-memory", resident_thumb_memory, 6u, true,
            UINT32_C(0x6c00), UINT32_C(0x6c00), 12u,
            5u, 5u, 4u, 1u, 0u, false, false, 0u, false, 0u,
            false, false, 0u))
        return false;
    if (!compact_raw_resident_compare(
            "window-cache", resident_window_cache, 3u, false,
            UINT32_C(0x73fc), UINT32_C(0x7000), UINT32_C(0x400),
            8u, 8u, 8u, 0u, 0u, false, false, 0u, true, 1u,
            true, true, UINT64_C(6)))
        return false;

    seed_cpu_at(&contract, unsupported_prefix, 2u, false,
                UINT32_C(0x5000));
    contract.cp15.sctlr |= ARM_SCTLR_M;
    capture_state(&before, &contract, ARM_OK, JIT_EXIT_NEXT);
    if (a64_compact_raw_run(&contract, &g_ram[0x5000], UINT32_C(0x5000),
                            8u, 1u, g_ram, sizeof g_ram, &completed) ||
        completed != 0u) {
        fprintf(stderr, "jitbench: compact raw invalid contract was accepted\n");
        return false;
    }
    capture_state(&after, &contract, ARM_OK, JIT_EXIT_NEXT);
    if (!architectural_states_equal(&before, &after)) {
        fprintf(stderr, "jitbench: compact raw contract refusal mutated state\n");
        return false;
    }

    printf("COMPACT-RAW-ORACLE exact=yes cases=10 result-opcodes=12 "
           "flag-opcodes=16 conditions=14 immediate-rotate=yes "
           "register-immediate-shifts=all word-load-store=yes branch-link=yes "
           "live-bytes=yes mmu-code-window=yes data-ops-stop=yes "
           "out-of-window=yes unsupported-prefix=yes "
           "invalid-contract-rollback=yes\n");
    printf("COMPACT-RAW-RESIDENT-ORACLE exact=yes cases=10 "
           "native-fallback-partition=yes interpreter-continue=yes "
           "boundary-stop=yes window-sequential=yes window-branch=yes "
           "window-refusal=yes stale-window-rejection=yes "
           "no-retire-window=yes repeated-window-cache=yes "
           "fallback-interworking=yes thumb-memory=yes "
           "write-consent=yes ram-equality=yes "
           "runtime-codegen=no\n");
    return true;
}

static bool run_product_entry(const bench_case_t *bc,
                              const a64_static_block_t *block,
                              uint64_t blocks, bool decoded,
                              final_state_t *out, double *seconds) {
    arm_cpu_t cpu;
    uint64_t i;
    unsigned completed = 0u;
    double start, end;

    seed_cpu(&cpu, bc);
    start = now_seconds();
    for (i = 0u; i < blocks; i++) {
        completed = 0u;
        bool ran = decoded
                       ? a64_static_run_read_hits_decoded(
                             &cpu, block, g_ram, sizeof g_ram, &completed)
                       : a64_static_run_read_hits(
                             &cpu, block, g_ram, sizeof g_ram, &completed);
        if (!ran ||
            completed != block->insn_count)
            break;
    }
    end = now_seconds();
    capture_state(out, &cpu, ARM_OK, JIT_EXIT_NEXT);
    *seconds = end - start;
    return i == blocks && *seconds > 0.0;
}

static int cmp_double(const void *lhs, const void *rhs) {
    double a = *(const double *)lhs, b = *(const double *)rhs;
    return (a > b) - (a < b);
}

static bool bench_compact_raw(const bench_case_t *bc, uint64_t requested,
                              unsigned reps) {
    a64_static_block_t static_block;
    double *interp_rates = NULL, *static_rates = NULL, *compact_rates = NULL;
    uint64_t blocks = (requested + bc->insns - 1u) / bc->insns;
    uint64_t total = blocks * bc->insns;
    bool ok = false;

    if (!bc || bc->thumb ||
        !a64_static_decode(bc->program, bc->insns, false, &static_block)) {
        fprintf(stderr, "jitbench: compact raw setup failed\n");
        return false;
    }
    interp_rates = (double *)calloc(reps, sizeof *interp_rates);
    static_rates = (double *)calloc(reps, sizeof *static_rates);
    compact_rates = (double *)calloc(reps, sizeof *compact_rates);
    if (!interp_rates || !static_rates || !compact_rates) {
        fprintf(stderr, "jitbench: out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        final_state_t interp, statik, compact;
        double interp_s = 0.0, static_s = 0.0, compact_s = 0.0;
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "interp-static-compact";
            ran = run_interpreter(bc, total, &interp, &interp_s) &&
                  run_static(bc, &static_block, blocks, &statik, &static_s) &&
                  run_compact_raw(bc, total, &compact, &compact_s);
        } else if (rep % 3u == 1u) {
            order = "static-compact-interp";
            ran = run_static(bc, &static_block, blocks, &statik, &static_s) &&
                  run_compact_raw(bc, total, &compact, &compact_s) &&
                  run_interpreter(bc, total, &interp, &interp_s);
        } else {
            order = "compact-interp-static";
            ran = run_compact_raw(bc, total, &compact, &compact_s) &&
                  run_interpreter(bc, total, &interp, &interp_s) &&
                  run_static(bc, &static_block, blocks, &statik, &static_s);
        }
        if (!ran || !states_equal(&interp, &statik) ||
            !states_equal(&interp, &compact)) {
            fprintf(stderr,
                    "jitbench: compact raw %s repetition %u failed exact "
                    "state equality\n",
                    bc->name, rep + 1u);
            goto done;
        }
        interp_rates[rep] = (double)total / interp_s / 1.0e6;
        static_rates[rep] = (double)total / static_s / 1.0e6;
        compact_rates[rep] = (double)total / compact_s / 1.0e6;
        printf("COMPACT-RAW-SAMPLE case=%s rep=%u order=%s "
               "interpreter=%.3f static-threaded=%.3f compact-raw=%.3f "
               "Minsn/s\n",
               bc->name, rep + 1u, order, interp_rates[rep],
               static_rates[rep], compact_rates[rep]);
    }

    qsort(interp_rates, reps, sizeof *interp_rates, cmp_double);
    qsort(static_rates, reps, sizeof *static_rates, cmp_double);
    qsort(compact_rates, reps, sizeof *compact_rates, cmp_double);
    printf("COMPACT-RAW-CEILING case=%s guest-insns=%" PRIu64
           " reps=%u live-bytes=yes decoded-cache=no graph=no "
           "runtime-codegen=no interpreter-median=%.3f "
           "static-threaded-median=%.3f compact-raw-median=%.3f "
           "static-speedup=%.3fx compact-speedup=%.3fx\n",
           bc->name, total, reps, interp_rates[reps / 2u],
           static_rates[reps / 2u], compact_rates[reps / 2u],
           static_rates[reps / 2u] / interp_rates[reps / 2u],
           compact_rates[reps / 2u] / interp_rates[reps / 2u]);
    ok = true;

done:
    free(interp_rates);
    free(static_rates);
    free(compact_rates);
    return ok;
}

/* Measure the wrapper the SoC actually calls once per cached product block.
 * The program is already decoded and every access hits one cache entry, so
 * this compares full public-contract validation with the cache-owned decoded
 * contract. Both include context construction and native entry/exit, and both
 * deliberately exclude the SoC cache lookup, timer/IRQ gates and decode
 * misses. They are still ceilings, just much less flattering ones than a
 * single native call repeating 16-instruction blocks. */
static bool bench_product_entry(unsigned length, uint64_t requested,
                                unsigned reps) {
    uint32_t program[A64_STATIC_MAX_INSNS];
    bench_case_t bc = {"a32-product-entry", program, length, false, false};
    a64_static_block_t block;
    double *interp_rates = NULL, *validated_rates = NULL;
    double *decoded_rates = NULL;
    uint64_t blocks, total;
    bool ok = false;

    if (!prepare_product_entry(length, program, &block)) {
        fprintf(stderr, "jitbench: product-entry decode failed at length %u\n",
                length);
        return false;
    }

    blocks = (requested + length - 1u) / length;
    total = blocks * length;
    interp_rates = (double *)calloc(reps, sizeof *interp_rates);
    validated_rates = (double *)calloc(reps, sizeof *validated_rates);
    decoded_rates = (double *)calloc(reps, sizeof *decoded_rates);
    if (!interp_rates || !validated_rates || !decoded_rates) {
        fprintf(stderr, "jitbench: out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        final_state_t interp, validated, decoded;
        double interp_s = 0.0, validated_s = 0.0, decoded_s = 0.0;
        bool ran;
        const char *order;

        if (rep % 3u == 0u) {
            order = "interp-validated-decoded";
            ran = run_interpreter(&bc, total, &interp, &interp_s) &&
                  run_product_entry(&bc, &block, blocks, false, &validated,
                                    &validated_s) &&
                  run_product_entry(&bc, &block, blocks, true, &decoded,
                                    &decoded_s);
        } else if (rep % 3u == 1u) {
            order = "validated-decoded-interp";
            ran = run_product_entry(&bc, &block, blocks, false, &validated,
                                    &validated_s) &&
                  run_product_entry(&bc, &block, blocks, true, &decoded,
                                    &decoded_s) &&
                  run_interpreter(&bc, total, &interp, &interp_s);
        } else {
            order = "decoded-interp-validated";
            ran = run_product_entry(&bc, &block, blocks, true, &decoded,
                                    &decoded_s) &&
                  run_interpreter(&bc, total, &interp, &interp_s) &&
                  run_product_entry(&bc, &block, blocks, false, &validated,
                                    &validated_s);
        }
        if (!ran || !architectural_states_equal(&interp, &validated) ||
            !architectural_states_equal(&interp, &decoded)) {
            fprintf(stderr,
                    "jitbench: product-entry length %u repetition %u failed\n",
                    length, rep + 1u);
            goto done;
        }
        interp_rates[rep] = (double)total / interp_s / 1.0e6;
        validated_rates[rep] = (double)total / validated_s / 1.0e6;
        decoded_rates[rep] = (double)total / decoded_s / 1.0e6;
        printf("PRODUCT-ENTRY-SAMPLE length=%u rep=%u order=%s "
               "interpreter=%.3f validated=%.3f decoded=%.3f Minsn/s\n",
               length, rep + 1u, order, interp_rates[rep],
               validated_rates[rep], decoded_rates[rep]);
    }

    qsort(interp_rates, reps, sizeof *interp_rates, cmp_double);
    qsort(validated_rates, reps, sizeof *validated_rates, cmp_double);
    qsort(decoded_rates, reps, sizeof *decoded_rates, cmp_double);
    printf("PRODUCT-ENTRY-CEILING length=%u uops=%u guest-insns=%" PRIu64
           " reps=%u predecoded=yes validated-wrapper=yes "
           "decoded-contract=yes cache-lookup=no interpreter-median=%.3f "
           "validated-median=%.3f decoded-median=%.3f "
           "validated-speedup=%.3fx decoded-speedup=%.3fx "
           "decoded-over-validated=%.3fx\n",
           length, block.uop_count, total, reps,
           interp_rates[reps / 2u], validated_rates[reps / 2u],
           decoded_rates[reps / 2u],
           validated_rates[reps / 2u] / interp_rates[reps / 2u],
           decoded_rates[reps / 2u] / interp_rates[reps / 2u],
           decoded_rates[reps / 2u] / validated_rates[reps / 2u]);
    ok = true;

done:
    free(interp_rates);
    free(validated_rates);
    free(decoded_rates);
    return ok;
}

typedef struct {
    uint8_t *snapshot;
    size_t snapshot_len;
    uint64_t signed_retired;
    uint64_t signed_chains;
    uint64_t graph_chains;
    uint64_t dread_hits;
    uint64_t dread_misses;
    uint64_t dwrite_hits;
    uint64_t dwrite_misses;
    uint64_t fetch_refill_attempts;
    uint64_t fetch_refill_hits;
    uint64_t fetch_refill_skips;
    uint64_t known_negative_bypasses;
    uint64_t compact_raw_attempts;
    uint64_t compact_raw_calls;
    uint64_t compact_raw_retired;
    uint64_t compact_raw_fallback_retired;
    uint64_t compact_raw_window_crossings;
    uint64_t compact_raw_window_reloads;
    uint64_t compact_raw_window_stops;
    uint64_t compact_raw_window_fast_refills;
    uint64_t compact_raw_window_cache_hits;
    uint64_t compact_raw_privileged_window_refills;
    uint64_t compact_raw_privileged_boundary_retired;
    double seconds;
} soc_run_result_t;

typedef enum {
    SOC_ENTRY_REFERENCE,
    SOC_ENTRY_COMPACT_RAW,
    SOC_ENTRY_COMPACT_RAW_WINDOW_CACHE,
    SOC_ENTRY_COMPACT_RAW_WINDOW_REFILL_OFF,
    SOC_ENTRY_SIGNED,
    SOC_ENTRY_GRAPH,
    SOC_ENTRY_GRAPH_EXTENDED,
    SOC_ENTRY_GRAPH_EXTENDED_INDIRECT_OFF,
    SOC_ENTRY_GRAPH_EXTENDED_THUMB_CONDITIONAL_OFF,
    SOC_ENTRY_GRAPH_EXTENDED_VSTR_OFF,
    SOC_ENTRY_GRAPH_EXTENDED_STM_OFF,
    SOC_ENTRY_GRAPH_EXTENDED_LDM_OFF,
    SOC_ENTRY_GRAPH_EXTENDED_VSTM_OFF,
    SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_OFF,
    SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_UNBATCHED,
    SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF,
    SOC_ENTRY_GRAPH_EXTENDED_KNOWN_NEGATIVE_BYPASS_OFF,
    SOC_ENTRY_GRAPH_EXTENDED_WRITES
} soc_entry_path_t;

static const char *soc_entry_path_name(soc_entry_path_t path) {
    switch (path) {
    case SOC_ENTRY_REFERENCE: return "reference";
    case SOC_ENTRY_COMPACT_RAW: return "compact-raw";
    case SOC_ENTRY_COMPACT_RAW_WINDOW_CACHE:
        return "compact-raw-window-cache";
    case SOC_ENTRY_COMPACT_RAW_WINDOW_REFILL_OFF:
        return "compact-raw-window-refill-off";
    case SOC_ENTRY_SIGNED:    return "signed";
    case SOC_ENTRY_GRAPH:     return "graph";
    case SOC_ENTRY_GRAPH_EXTENDED: return "graph-extended";
    case SOC_ENTRY_GRAPH_EXTENDED_INDIRECT_OFF:
        return "graph-extended-indirect-off";
    case SOC_ENTRY_GRAPH_EXTENDED_THUMB_CONDITIONAL_OFF:
        return "graph-extended-thumb-conditional-off";
    case SOC_ENTRY_GRAPH_EXTENDED_VSTR_OFF:
        return "graph-extended-vstr-off";
    case SOC_ENTRY_GRAPH_EXTENDED_STM_OFF:
        return "graph-extended-stm-off";
    case SOC_ENTRY_GRAPH_EXTENDED_LDM_OFF:
        return "graph-extended-ldm-off";
    case SOC_ENTRY_GRAPH_EXTENDED_VSTM_OFF:
        return "graph-extended-vstm-off";
    case SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_OFF:
        return "graph-extended-vfp-arithmetic-off";
    case SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_UNBATCHED:
        return "graph-extended-vfp-arithmetic-unbatched";
    case SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF:
        return "graph-extended-fetch-refill-off";
    case SOC_ENTRY_GRAPH_EXTENDED_KNOWN_NEGATIVE_BYPASS_OFF:
        return "graph-extended-known-negative-bypass-off";
    case SOC_ENTRY_GRAPH_EXTENDED_WRITES:
        return "graph-extended-writes";
    }
    return "invalid";
}

static void free_soc_run_result(soc_run_result_t *result) {
    if (!result) return;
    free(result->snapshot);
    memset(result, 0, sizeof *result);
}

typedef struct {
    const uint32_t *program;
    unsigned length;
    uint32_t seed_r7;
    bool indirect_workload;
    bool thumb_conditional_workload;
    bool vfp_workload;
    bool vfp_arithmetic_workload;
    bool fetch_refill_workload;
    bool fetch_refill_mix_workload;
    bool mmu_identity_workload;
    bool mmu_identity_privileged;
    unsigned fetch_refill_mix_long_insns;
} soc_entry_setup_t;

static uint32_t encode_a32_unconditional_b(uint32_t pc, uint32_t target) {
    int64_t delta = (int64_t)target - ((int64_t)pc + 8);
    return UINT32_C(0xea000000) |
           ((uint32_t)(delta / 4) & UINT32_C(0x00ffffff));
}

static bool setup_soc_entry_machine(s5l8900_t *machine,
                                    const soc_entry_setup_t *setup) {
    if (!machine || !setup) return false;
    if (setup->fetch_refill_mix_workload) {
        const uint32_t section = (3u << 10) | 2u;
        const unsigned long_insns = setup->fetch_refill_mix_long_insns
            ? setup->fetch_refill_mix_long_insns
            : FETCH_REFILL_MIX_LONG_INSNS;
        if (long_insns < 2u ||
            long_insns > FETCH_REFILL_MIX_MAX_LONG_INSNS)
            return false;
        for (unsigned i = 0u; i < FETCH_REFILL_MIX_BLOCKS; i++) {
            uint32_t program[FETCH_REFILL_MIX_MAX_LONG_INSNS];
            const bool single = i == 0u || i == 7u || i == 14u;
            const unsigned length = single ? 1u : long_insns;
            const uint32_t pc = i * UINT32_C(0x400) + i * 4u;
            const unsigned next_i = (i + 1u) % FETCH_REFILL_MIX_BLOCKS;
            const uint32_t target =
                next_i * UINT32_C(0x400) + next_i * 4u;
            for (unsigned j = 0u; j + 1u < length; j++)
                program[j] = UINT32_C(0xe1a00000); /* MOV r0,r0 */
            program[length - 1u] = encode_a32_unconditional_b(
                pc + (length - 1u) * 4u, target);
            s5l8900_load(machine, pc, program,
                         (size_t)length * sizeof program[0]);
        }
        /* Keep the 16 KiB first-level table beyond every synthetic block.
         * All twenty virtual 1 KiB witnesses map through one identity section. */
        s5l8900_load(machine, 0x10000u, &section, sizeof section);
        machine->cpu.cp15.ttbr0 = 0x10000u;
        machine->cpu.cp15.dacr = 1u;
        machine->cpu.cp15.sctlr |= ARM_SCTLR_M;
        machine->cpu.r[15] = 0u;
        machine->cpu.cpsr = ARM_MODE_SYS | ARM_CPSR_C;
        return true;
    }
    if (setup->fetch_refill_workload) {
        static const uint32_t branch[4u] = {
            UINT32_C(0xea0000fe), /* 0x000 -> 0x400 */
            UINT32_C(0xea0000fe), /* 0x400 -> 0x800 */
            UINT32_C(0xea0000fe), /* 0x800 -> 0xc00 */
            UINT32_C(0xeafffcfe), /* 0xc00 -> 0x000 */
        };
        static const uint32_t pc[4u] = {
            0x000u, 0x400u, 0x800u, 0xc00u
        };
        const uint32_t section = (3u << 10) | 2u;
        for (unsigned i = 0u; i < 4u; i++)
            s5l8900_load(machine, pc[i], &branch[i], sizeof branch[i]);
        s5l8900_load(machine, 0x4000u, &section, sizeof section);
        machine->cpu.cp15.ttbr0 = 0x4000u;
        machine->cpu.cp15.dacr = 1u;
        machine->cpu.cp15.sctlr |= ARM_SCTLR_M;
        machine->cpu.r[15] = 0u;
        machine->cpu.cpsr = ARM_MODE_SYS | ARM_CPSR_C;
        return true;
    }
    if (setup->thumb_conditional_workload) {
        s5l8900_load(machine, 0u, THUMB_SOC_CONDITIONAL,
                     sizeof THUMB_SOC_CONDITIONAL);
        machine->cpu.r[15] = 0u;
        machine->cpu.cpsr = ARM_MODE_SYS | ARM_CPSR_T | ARM_CPSR_C;
        return true;
    }
    if (!setup->indirect_workload) {
        if (!setup->program || !setup->length) return false;
        s5l8900_load(machine, 0u, setup->program,
                     (size_t)setup->length * sizeof *setup->program);
        machine->cpu.r[7] = setup->seed_r7;
        if (setup->vfp_workload || setup->vfp_arithmetic_workload) {
            machine->cpu.cp15.cpacr =
                0xfu << ARM_CPACR_CP10_SHIFT;
            machine->cpu.vfp_fpexc = ARM_FPEXC_EN;
            if (setup->vfp_arithmetic_workload) {
                machine->cpu.vfp_fpscr =
                    ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_IXC |
                    ARM_FPSCR_N | ARM_FPSCR_C;
                memset(machine->cpu.vfp_s, 0,
                       sizeof machine->cpu.vfp_s);
                vfp_set_d(&machine->cpu, 13u,
                          UINT64_C(0x3ff0000000000000));
                vfp_set_d(&machine->cpu, 14u,
                          UINT64_C(0x3ff0000000000000));
                vfp_set_s(&machine->cpu, 30u, UINT32_C(0x3f800000));
                vfp_set_s(&machine->cpu, 31u, UINT32_C(0x3f800000));
            } else {
                machine->cpu.vfp_fpscr = 0u;
                for (unsigned i = 0u; i < 32u; i++)
                    machine->cpu.vfp_s[i] =
                        UINT32_C(0x80000000) ^
                        (UINT32_C(0x01020304) * (i + 1u));
            }
        }
        if (setup->mmu_identity_workload) {
            const uint32_t section = (3u << 10) | 2u;
            s5l8900_load(machine, UINT32_C(0x4000),
                         &section, sizeof section);
            machine->cpu.cp15.ttbr0 = UINT32_C(0x4000);
            machine->cpu.cp15.dacr = 1u;
            machine->cpu.cp15.sctlr |= ARM_SCTLR_M;
            machine->cpu.cpsr =
                (setup->mmu_identity_privileged ? ARM_MODE_SYS
                                                : ARM_MODE_USR) |
                ARM_CPSR_C;
        }
        return true;
    }

    s5l8900_load(machine, 0x00u, A32_SOC_INDIRECT_ZERO,
                 sizeof A32_SOC_INDIRECT_ZERO);
    s5l8900_load(machine, 0x10u, THUMB_SOC_INDIRECT_TEN,
                 sizeof THUMB_SOC_INDIRECT_TEN);
    s5l8900_load(machine, 0x40u, A32_SOC_INDIRECT_FORTY,
                 sizeof A32_SOC_INDIRECT_FORTY);
    s5l8900_load(machine, 0x60u, THUMB_SOC_INDIRECT_SIXTY,
                 sizeof THUMB_SOC_INDIRECT_SIXTY);
    machine->cpu.r[4] = 0u;
    machine->cpu.r[5] = 0u;
    machine->cpu.r[6] = 0u;
    machine->cpu.r[7] = 0u;
    machine->cpu.r[8] = UINT32_C(0x11);
    machine->cpu.r[9] = UINT32_C(0x40);
    machine->cpu.r[10] = UINT32_C(0x61);
    machine->cpu.r[11] = UINT32_C(0x00);
    machine->cpu.r[15] = 0u;
    machine->cpu.cpsr = ARM_MODE_SYS | ARM_CPSR_C;
    return true;
}

/* Run the exact app-facing machine loop. Setup and the two-loop cache warmup
 * stay outside the timed region. The signed arm still pays the product cache
 * index, raw-byte SMC witness, dynamic gates, timer-boundary splitting and
 * device ticks. A complete machine snapshot is retained for comparison with
 * the interpreter arm; the signed cache and its counter are host diagnostics
 * and deliberately do not enter that architectural byte stream. */
static bool run_soc_entry_configured(const soc_entry_setup_t *setup,
                                     unsigned loop_insns, uint64_t total,
                                     soc_entry_path_t path,
                                     soc_run_result_t *out) {
    s5l8900_t machine = {0};
    arm_status_t status = ARM_OK;
    uint64_t remaining = total;
    uint64_t retired_before;
    uint64_t retired_after;
    uint64_t chains_before;
    uint64_t chains_after;
    uint64_t graph_before;
    uint64_t graph_after;
    uint64_t dread_hits_before;
    uint64_t dread_hits_after;
    uint64_t dread_misses_before;
    uint64_t dread_misses_after;
    uint64_t dwrite_hits_before;
    uint64_t dwrite_hits_after;
    uint64_t dwrite_misses_before;
    uint64_t dwrite_misses_after;
    uint64_t fetch_refill_attempts_before;
    uint64_t fetch_refill_attempts_after;
    uint64_t fetch_refill_hits_before;
    uint64_t fetch_refill_hits_after;
    uint64_t fetch_refill_skips_before;
    uint64_t fetch_refill_skips_after;
    uint64_t known_negative_bypasses_before;
    uint64_t known_negative_bypasses_after;
    uint64_t compact_raw_attempts_before;
    uint64_t compact_raw_attempts_after;
    uint64_t compact_raw_calls_before;
    uint64_t compact_raw_calls_after;
    uint64_t compact_raw_retired_before;
    uint64_t compact_raw_retired_after;
    uint64_t compact_raw_fallback_retired_before;
    uint64_t compact_raw_fallback_retired_after;
    uint64_t compact_raw_window_crossings_before;
    uint64_t compact_raw_window_crossings_after;
    uint64_t compact_raw_window_reloads_before;
    uint64_t compact_raw_window_reloads_after;
    uint64_t compact_raw_window_stops_before;
    uint64_t compact_raw_window_stops_after;
    uint64_t compact_raw_window_fast_refills_before;
    uint64_t compact_raw_window_fast_refills_after;
    uint64_t compact_raw_window_cache_hits_before;
    uint64_t compact_raw_window_cache_hits_after;
    uint64_t compact_raw_privileged_window_refills_before;
    uint64_t compact_raw_privileged_window_refills_after;
    uint64_t compact_raw_privileged_boundary_retired_before;
    uint64_t compact_raw_privileged_boundary_retired_after;
    double start, end;
    bool initialized = false;
    bool ok = false;

    bool signed_path = path != SOC_ENTRY_REFERENCE;
    bool compact_raw_path = path == SOC_ENTRY_COMPACT_RAW ||
        path == SOC_ENTRY_COMPACT_RAW_WINDOW_CACHE ||
        path == SOC_ENTRY_COMPACT_RAW_WINDOW_REFILL_OFF;
    bool compact_raw_window_cache_path =
        path == SOC_ENTRY_COMPACT_RAW_WINDOW_CACHE;
    bool compact_raw_window_refill_off_path =
        path == SOC_ENTRY_COMPACT_RAW_WINDOW_REFILL_OFF;
    bool graph_path = path == SOC_ENTRY_GRAPH ||
                      path == SOC_ENTRY_GRAPH_EXTENDED ||
                      path == SOC_ENTRY_GRAPH_EXTENDED_INDIRECT_OFF ||
                      path == SOC_ENTRY_GRAPH_EXTENDED_THUMB_CONDITIONAL_OFF ||
                      path == SOC_ENTRY_GRAPH_EXTENDED_VSTR_OFF ||
                      path == SOC_ENTRY_GRAPH_EXTENDED_STM_OFF ||
                      path == SOC_ENTRY_GRAPH_EXTENDED_LDM_OFF ||
                      path == SOC_ENTRY_GRAPH_EXTENDED_VSTM_OFF ||
                      path == SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_OFF ||
                      path ==
                          SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_UNBATCHED ||
                      path == SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF ||
                      path ==
                          SOC_ENTRY_GRAPH_EXTENDED_KNOWN_NEGATIVE_BYPASS_OFF ||
                      path == SOC_ENTRY_GRAPH_EXTENDED_WRITES;
    bool extended_path = path == SOC_ENTRY_GRAPH_EXTENDED ||
                         path == SOC_ENTRY_GRAPH_EXTENDED_INDIRECT_OFF ||
                         path == SOC_ENTRY_GRAPH_EXTENDED_THUMB_CONDITIONAL_OFF ||
                         path == SOC_ENTRY_GRAPH_EXTENDED_VSTR_OFF ||
                         path == SOC_ENTRY_GRAPH_EXTENDED_STM_OFF ||
                         path == SOC_ENTRY_GRAPH_EXTENDED_LDM_OFF ||
                         path == SOC_ENTRY_GRAPH_EXTENDED_VSTM_OFF ||
                         path == SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_OFF ||
                         path ==
                             SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_UNBATCHED ||
                         path == SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF ||
                         path ==
                             SOC_ENTRY_GRAPH_EXTENDED_KNOWN_NEGATIVE_BYPASS_OFF ||
                         path == SOC_ENTRY_GRAPH_EXTENDED_WRITES;
    bool indirect_off_path =
        path == SOC_ENTRY_GRAPH_EXTENDED_INDIRECT_OFF;
    bool thumb_conditional_off_path =
        path == SOC_ENTRY_GRAPH_EXTENDED_THUMB_CONDITIONAL_OFF;
    bool vstr_off_path = path == SOC_ENTRY_GRAPH_EXTENDED_VSTR_OFF;
    bool stm_off_path = path == SOC_ENTRY_GRAPH_EXTENDED_STM_OFF;
    bool ldm_off_path = path == SOC_ENTRY_GRAPH_EXTENDED_LDM_OFF;
    bool vstm_off_path = path == SOC_ENTRY_GRAPH_EXTENDED_VSTM_OFF;
    bool vfp_arithmetic_off_path =
        path == SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_OFF;
    bool vfp_arithmetic_unbatched_path =
        path == SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_UNBATCHED;
    bool fetch_refill_off_path =
        path == SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF;
    bool known_negative_bypass_off_path =
        path == SOC_ENTRY_GRAPH_EXTENDED_KNOWN_NEGATIVE_BYPASS_OFF;
    bool direct_write_path = path == SOC_ENTRY_GRAPH_EXTENDED_WRITES ||
                             vstr_off_path || stm_off_path || vstm_off_path;

    if (!setup || !loop_insns || !out ||
        path > SOC_ENTRY_GRAPH_EXTENDED_WRITES)
        return false;
    /* The compact MMU oracle models the app's canonical-bus contract on both
     * arms. This warms and measures DWRITE symmetrically instead of proving
     * only the read half of the new resident cache-hit path. */
    if (setup->mmu_identity_workload) direct_write_path = true;
    memset(out, 0, sizeof *out);
    if (!s5l8900_init(&machine, 0u, RAM_SIZE)) {
        fprintf(stderr, "jitbench: SoC entry machine init failed\n");
        goto done;
    }
    initialized = true;
    if (!setup_soc_entry_machine(&machine, setup)) {
        fprintf(stderr, "jitbench: SoC entry setup failed\n");
        goto done;
    }

    /* Clear reset's dirty-level gate before warming either path. This uses the
     * real 412 MHz:6 MHz board clocks installed by s5l8900_init(). */
    s5l8900_tick(&machine, 0u);
    if (direct_write_path &&
        !s5l8900_set_direct_ram_writes(&machine, true)) {
        fprintf(stderr, "jitbench: SoC entry direct writes unavailable\n");
        goto done;
    }
    if (signed_path &&
        !s5l8900_static_a64_set_enabled(&machine, true)) {
        fprintf(stderr, "jitbench: SoC entry signed engine unavailable\n");
        goto done;
    }
    if (compact_raw_window_refill_off_path &&
        !setup->mmu_identity_privileged &&
        !s5l8900_static_a64_set_compact_raw_window_refill(&machine, false)) {
        fprintf(stderr,
                "jitbench: SoC compact window-refill control unavailable\n");
        goto done;
    }
    if (compact_raw_window_cache_path &&
        !s5l8900_static_a64_set_compact_raw_window_cache(&machine, true)) {
        fprintf(stderr,
                "jitbench: SoC compact window-cache path unavailable\n");
        goto done;
    }
    if (compact_raw_path && setup->mmu_identity_privileged &&
        !s5l8900_static_a64_set_compact_raw_privileged_window_refill(
            &machine, !compact_raw_window_refill_off_path)) {
        fprintf(stderr,
                "jitbench: SoC privileged compact window-refill control "
                "unavailable\n");
        goto done;
    }
    if (indirect_off_path &&
        !s5l8900_static_a64_set_indirect_branches(&machine, false)) {
        fprintf(stderr, "jitbench: SoC indirect-off control unavailable\n");
        goto done;
    }
    if (thumb_conditional_off_path &&
        !s5l8900_static_a64_set_thumb_conditional_branches(
            &machine, false)) {
        fprintf(stderr,
                "jitbench: SoC Thumb-conditional-off control unavailable\n");
        goto done;
    }
    if (vstr_off_path &&
        !s5l8900_static_a64_set_vstr(&machine, false)) {
        fprintf(stderr, "jitbench: SoC VSTR-off control unavailable\n");
        goto done;
    }
    if (stm_off_path &&
        !s5l8900_static_a64_set_stm(&machine, false)) {
        fprintf(stderr, "jitbench: SoC STM-off control unavailable\n");
        goto done;
    }
    if (ldm_off_path &&
        !s5l8900_static_a64_set_ldm(&machine, false)) {
        fprintf(stderr, "jitbench: SoC LDM-off control unavailable\n");
        goto done;
    }
    if (vstm_off_path &&
        !s5l8900_static_a64_set_vstm(&machine, false)) {
        fprintf(stderr, "jitbench: SoC VSTM-off control unavailable\n");
        goto done;
    }
    if (vfp_arithmetic_off_path &&
        !s5l8900_static_a64_set_vfp_arithmetic(&machine, false)) {
        fprintf(stderr,
                "jitbench: SoC VFP-arithmetic-off control unavailable\n");
        goto done;
    }
    if (vfp_arithmetic_unbatched_path &&
        !s5l8900_static_a64_set_vfp_fp_session(&machine, false)) {
        fprintf(stderr,
                "jitbench: SoC VFP-arithmetic session control unavailable\n");
        goto done;
    }
    if (fetch_refill_off_path &&
        !s5l8900_static_a64_set_fetch_refill(&machine, false)) {
        fprintf(stderr,
                "jitbench: SoC fetch-refill-off control unavailable\n");
        goto done;
    }
    if (known_negative_bypass_off_path &&
        !s5l8900_static_a64_set_known_negative_bypass(&machine, false)) {
        fprintf(stderr,
                "jitbench: SoC known-negative bypass control unavailable\n");
        goto done;
    }
    if (compact_raw_path &&
        (!s5l8900_static_a64_set_compact_raw(&machine, true) ||
         !s5l8900_static_a64_set_chain_limit(
             &machine, A64_STATIC_MAX_CHAIN_INSNS))) {
        fprintf(stderr,
                "jitbench: SoC compact-raw code-window path unavailable\n");
        goto done;
    }
    if (graph_path &&
        !s5l8900_static_a64_set_graph(&machine, true)) {
        fprintf(stderr, "jitbench: SoC entry graph engine unavailable\n");
        goto done;
    }
    if (extended_path &&
        !s5l8900_static_a64_set_chain_limit(
            &machine, A64_STATIC_MAX_CHAIN_INSNS)) {
        fprintf(stderr, "jitbench: SoC entry extended limit unavailable\n");
        goto done;
    }

    /* The first instruction establishes the fetch pointer. The remainder of
     * the first loop and the second complete loop establish the cache-owned
     * decoded entries while returning PC to zero. */
    if (s5l8900_run(&machine, loop_insns * 2u, &status) !=
            loop_insns * 2u ||
        status != ARM_OK || machine.cpu.r[15] != 0u) {
        fprintf(stderr,
                "jitbench: SoC entry %s warmup failed status=%d pc=0x%08x\n",
                soc_entry_path_name(path), (int)status,
                machine.cpu.r[15]);
        goto done;
    }

    retired_before = s5l8900_static_a64_retired(&machine);
    chains_before = s5l8900_static_a64_chained_blocks(&machine);
    graph_before = s5l8900_static_a64_graph_chained_blocks(&machine);
    dread_hits_before = machine.cpu.dread_hits;
    dread_misses_before = machine.cpu.dread_misses;
    dwrite_hits_before = machine.cpu.dwrite_hits;
    dwrite_misses_before = machine.cpu.dwrite_misses;
    fetch_refill_attempts_before =
        s5l8900_static_a64_fetch_refill_attempts(&machine);
    fetch_refill_hits_before =
        s5l8900_static_a64_fetch_refill_hits(&machine);
    fetch_refill_skips_before =
        s5l8900_static_a64_fetch_refill_skips(&machine);
    known_negative_bypasses_before =
        s5l8900_static_a64_known_negative_bypasses(&machine);
    compact_raw_attempts_before =
        s5l8900_static_a64_compact_raw_attempts(&machine);
    compact_raw_calls_before =
        s5l8900_static_a64_compact_raw_calls(&machine);
    compact_raw_retired_before =
        s5l8900_static_a64_compact_raw_retired(&machine);
    compact_raw_fallback_retired_before =
        s5l8900_static_a64_compact_raw_fallback_retired(&machine);
    compact_raw_window_crossings_before =
        s5l8900_static_a64_compact_raw_window_crossings(&machine);
    compact_raw_window_reloads_before =
        s5l8900_static_a64_compact_raw_window_reloads(&machine);
    compact_raw_window_stops_before =
        s5l8900_static_a64_compact_raw_window_stops(&machine);
    compact_raw_window_fast_refills_before =
        s5l8900_static_a64_compact_raw_window_fast_refills(&machine);
    compact_raw_window_cache_hits_before =
        s5l8900_static_a64_compact_raw_window_cache_hits(&machine);
    compact_raw_privileged_window_refills_before =
        s5l8900_static_a64_compact_raw_privileged_window_refills(&machine);
    compact_raw_privileged_boundary_retired_before =
        s5l8900_static_a64_compact_raw_privileged_boundary_retired(&machine);
    start = now_seconds();
    while (remaining != 0u) {
        unsigned chunk = remaining > (uint64_t)UINT_MAX
                             ? UINT_MAX
                             : (unsigned)remaining;
        unsigned ran = s5l8900_run(&machine, chunk, &status);
        if (ran != chunk || status != ARM_OK) break;
        remaining -= ran;
    }
    end = now_seconds();
    retired_after = s5l8900_static_a64_retired(&machine);
    chains_after = s5l8900_static_a64_chained_blocks(&machine);
    graph_after = s5l8900_static_a64_graph_chained_blocks(&machine);
    dread_hits_after = machine.cpu.dread_hits;
    dread_misses_after = machine.cpu.dread_misses;
    dwrite_hits_after = machine.cpu.dwrite_hits;
    dwrite_misses_after = machine.cpu.dwrite_misses;
    fetch_refill_attempts_after =
        s5l8900_static_a64_fetch_refill_attempts(&machine);
    fetch_refill_hits_after =
        s5l8900_static_a64_fetch_refill_hits(&machine);
    fetch_refill_skips_after =
        s5l8900_static_a64_fetch_refill_skips(&machine);
    known_negative_bypasses_after =
        s5l8900_static_a64_known_negative_bypasses(&machine);
    compact_raw_attempts_after =
        s5l8900_static_a64_compact_raw_attempts(&machine);
    compact_raw_calls_after =
        s5l8900_static_a64_compact_raw_calls(&machine);
    compact_raw_retired_after =
        s5l8900_static_a64_compact_raw_retired(&machine);
    compact_raw_fallback_retired_after =
        s5l8900_static_a64_compact_raw_fallback_retired(&machine);
    compact_raw_window_crossings_after =
        s5l8900_static_a64_compact_raw_window_crossings(&machine);
    compact_raw_window_reloads_after =
        s5l8900_static_a64_compact_raw_window_reloads(&machine);
    compact_raw_window_stops_after =
        s5l8900_static_a64_compact_raw_window_stops(&machine);
    compact_raw_window_fast_refills_after =
        s5l8900_static_a64_compact_raw_window_fast_refills(&machine);
    compact_raw_window_cache_hits_after =
        s5l8900_static_a64_compact_raw_window_cache_hits(&machine);
    compact_raw_privileged_window_refills_after =
        s5l8900_static_a64_compact_raw_privileged_window_refills(&machine);
    compact_raw_privileged_boundary_retired_after =
        s5l8900_static_a64_compact_raw_privileged_boundary_retired(&machine);
    if (remaining != 0u || status != ARM_OK || end <= start ||
        machine.cpu.r[15] != 0u) {
        fprintf(stderr,
                "jitbench: SoC entry %s run failed remaining=%" PRIu64
                " status=%d pc=0x%08x\n",
                soc_entry_path_name(path), remaining,
                (int)status, machine.cpu.r[15]);
        goto done;
    }
    out->signed_retired = retired_after - retired_before;
    out->signed_chains = chains_after - chains_before;
    out->graph_chains = graph_after - graph_before;
    out->dread_hits = dread_hits_after - dread_hits_before;
    out->dread_misses = dread_misses_after - dread_misses_before;
    out->dwrite_hits = dwrite_hits_after - dwrite_hits_before;
    out->dwrite_misses = dwrite_misses_after - dwrite_misses_before;
    out->fetch_refill_attempts =
        fetch_refill_attempts_after - fetch_refill_attempts_before;
    out->fetch_refill_hits =
        fetch_refill_hits_after - fetch_refill_hits_before;
    out->fetch_refill_skips =
        fetch_refill_skips_after - fetch_refill_skips_before;
    out->known_negative_bypasses =
        known_negative_bypasses_after - known_negative_bypasses_before;
    out->compact_raw_attempts =
        compact_raw_attempts_after - compact_raw_attempts_before;
    out->compact_raw_calls =
        compact_raw_calls_after - compact_raw_calls_before;
    out->compact_raw_retired =
        compact_raw_retired_after - compact_raw_retired_before;
    out->compact_raw_fallback_retired =
        compact_raw_fallback_retired_after -
        compact_raw_fallback_retired_before;
    out->compact_raw_window_crossings =
        compact_raw_window_crossings_after -
        compact_raw_window_crossings_before;
    out->compact_raw_window_reloads =
        compact_raw_window_reloads_after - compact_raw_window_reloads_before;
    out->compact_raw_window_stops =
        compact_raw_window_stops_after - compact_raw_window_stops_before;
    out->compact_raw_window_fast_refills =
        compact_raw_window_fast_refills_after -
        compact_raw_window_fast_refills_before;
    out->compact_raw_window_cache_hits =
        compact_raw_window_cache_hits_after -
        compact_raw_window_cache_hits_before;
    out->compact_raw_privileged_window_refills =
        compact_raw_privileged_window_refills_after -
        compact_raw_privileged_window_refills_before;
    out->compact_raw_privileged_boundary_retired =
        compact_raw_privileged_boundary_retired_after -
        compact_raw_privileged_boundary_retired_before;
    /* A privileged compact refusal returns to the machine loop before
     * arm_step(), so those outer interpreter retirements deliberately do not
     * increment the resident fallback counter. User mode can execute that
     * fallback inside the resident callback and must still partition `total`
     * exactly between the two compact counters. */
    if ((signed_path && out->signed_retired > total) ||
        (!signed_path &&
         (out->signed_retired != 0u || out->signed_chains != 0u ||
          out->graph_chains != 0u ||
          (!setup->mmu_identity_workload &&
           (out->dwrite_hits != 0u || out->dwrite_misses != 0u)))) ||
        (path == SOC_ENTRY_SIGNED && out->graph_chains != 0u) ||
        (compact_raw_path &&
         ((setup->mmu_identity_privileged
               ? out->compact_raw_retired +
                         out->compact_raw_fallback_retired > total
               : out->compact_raw_retired +
                         out->compact_raw_fallback_retired != total) ||
          out->signed_retired != out->compact_raw_retired ||
          out->compact_raw_calls == 0u ||
          out->compact_raw_attempts < out->compact_raw_calls)) ||
        (!compact_raw_path &&
          (out->compact_raw_attempts != 0u ||
           out->compact_raw_calls != 0u ||
           out->compact_raw_retired != 0u ||
           out->compact_raw_fallback_retired != 0u ||
            out->compact_raw_window_crossings != 0u ||
            out->compact_raw_window_reloads != 0u ||
            out->compact_raw_window_stops != 0u ||
            out->compact_raw_window_fast_refills != 0u ||
            out->compact_raw_window_cache_hits != 0u ||
            out->compact_raw_privileged_window_refills != 0u ||
            out->compact_raw_privileged_boundary_retired != 0u)) ||
        (graph_path &&
         out->graph_chains != out->signed_chains)) {
        fprintf(stderr,
                "jitbench: SoC entry %s retired signed=%" PRIu64
                " fallback=%" PRIu64 " chains=%" PRIu64
                " graph=%" PRIu64
                " expected=%" PRIu64 "\n",
                soc_entry_path_name(path), out->signed_retired,
                out->compact_raw_fallback_retired, out->signed_chains,
                out->graph_chains,
                signed_path ? total : UINT64_C(0));
        goto done;
    }
    out->seconds = end - start;
    {
        snapshot_status_t snap =
            snapshot_save_mem(&machine, &out->snapshot, &out->snapshot_len);
        if (snap != SNAP_OK) {
            fprintf(stderr, "jitbench: SoC entry snapshot failed: %s\n",
                    snapshot_strerror(snap));
            goto done;
        }
    }
    ok = true;

done:
    if (initialized) s5l8900_free(&machine);
    if (!ok) free_soc_run_result(out);
    return ok;
}

static bool run_soc_entry_path(const uint32_t *program, unsigned length,
                               uint64_t total, soc_entry_path_t path,
                               uint32_t seed_r7,
                               soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .program = program,
        .length = length,
        .seed_r7 = seed_r7,
        .indirect_workload = false,
    };
    return run_soc_entry_configured(&setup, length, total, path, out);
}

static bool run_soc_compact_raw_path(const uint32_t *program,
                                     unsigned length, uint64_t total,
                                     soc_entry_path_t path,
                                     soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .program = program,
        .length = length,
        .seed_r7 = DATA_BASE,
        .mmu_identity_workload = true,
    };
    if (path != SOC_ENTRY_REFERENCE && path != SOC_ENTRY_COMPACT_RAW &&
        path != SOC_ENTRY_COMPACT_RAW_WINDOW_CACHE &&
        path != SOC_ENTRY_COMPACT_RAW_WINDOW_REFILL_OFF)
        return false;
    return run_soc_entry_configured(&setup, length, total, path, out);
}

static bool run_soc_compact_raw_privileged_path(
        const uint32_t *program, unsigned length, uint64_t total,
        soc_entry_path_t path, soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .program = program,
        .length = length,
        .seed_r7 = DATA_BASE,
        .mmu_identity_workload = true,
        .mmu_identity_privileged = true,
    };
    if (path != SOC_ENTRY_REFERENCE && path != SOC_ENTRY_COMPACT_RAW &&
        path != SOC_ENTRY_COMPACT_RAW_WINDOW_REFILL_OFF)
        return false;
    return run_soc_entry_configured(&setup, length, total, path, out);
}

static bool run_soc_indirect_path(uint64_t total, soc_entry_path_t path,
                                  soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .indirect_workload = true,
    };
    return run_soc_entry_configured(&setup, 8u, total, path, out);
}

static bool run_soc_thumb_conditional_path(uint64_t total,
                                           soc_entry_path_t path,
                                           soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .thumb_conditional_workload = true,
    };
    return run_soc_entry_configured(&setup, 16u, total, path, out);
}

static bool run_soc_fetch_refill_path(uint64_t total,
                                      soc_entry_path_t path,
                                      soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .fetch_refill_workload = true,
    };
    return run_soc_entry_configured(&setup, 4u, total, path, out);
}

static bool run_soc_fetch_refill_mix_length_path(
        uint64_t total, unsigned long_insns, soc_entry_path_t path,
        soc_run_result_t *out) {
    if (long_insns < 2u || long_insns > FETCH_REFILL_MIX_MAX_LONG_INSNS)
        return false;
    const unsigned loop_insns = FETCH_REFILL_MIX_SINGLE_BLOCKS +
        FETCH_REFILL_MIX_LONG_BLOCKS * long_insns;
    const soc_entry_setup_t setup = {
        .fetch_refill_mix_workload = true,
        .fetch_refill_mix_long_insns = long_insns,
    };
    return run_soc_entry_configured(&setup, loop_insns, total, path, out);
}

static bool run_soc_vstr_path(uint64_t total, soc_entry_path_t path,
                              soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .program = A32_SOC_VSTR,
        .length = (unsigned)(sizeof A32_SOC_VSTR /
                             sizeof A32_SOC_VSTR[0]),
        .seed_r7 = DATA_BASE,
        .vfp_workload = true,
    };
    return run_soc_entry_configured(&setup, setup.length, total, path, out);
}

static bool run_soc_stm_path(uint64_t total, soc_entry_path_t path,
                             soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .program = A32_SOC_STM,
        .length = (unsigned)(sizeof A32_SOC_STM / sizeof A32_SOC_STM[0]),
        .seed_r7 = DATA_BASE + UINT32_C(0x100),
    };
    return run_soc_entry_configured(&setup, setup.length, total, path, out);
}

static bool run_soc_ldm_path(uint64_t total, soc_entry_path_t path,
                             soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .program = A32_SOC_LDM,
        .length = (unsigned)(sizeof A32_SOC_LDM / sizeof A32_SOC_LDM[0]),
        .seed_r7 = DATA_BASE + UINT32_C(0x300),
    };
    return run_soc_entry_configured(&setup, setup.length, total, path, out);
}

static bool run_soc_vstm_path(uint64_t total, soc_entry_path_t path,
                              soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .program = A32_SOC_VSTM,
        .length = (unsigned)(sizeof A32_SOC_VSTM /
                             sizeof A32_SOC_VSTM[0]),
        .seed_r7 = DATA_BASE + UINT32_C(0x200),
        .vfp_workload = true,
    };
    return run_soc_entry_configured(&setup, setup.length, total, path, out);
}

static bool run_soc_vfp_arithmetic_path(uint64_t total,
                                         soc_entry_path_t path,
                                         soc_run_result_t *out) {
    const soc_entry_setup_t setup = {
        .program = A32_SOC_VFP_ARITH,
        .length = (unsigned)(sizeof A32_SOC_VFP_ARITH /
                             sizeof A32_SOC_VFP_ARITH[0]),
        .vfp_arithmetic_workload = true,
    };
    return run_soc_entry_configured(&setup, setup.length, total, path, out);
}

typedef struct {
    uint8_t *snapshot;
    size_t snapshot_len;
    uint64_t attempts;
    uint64_t calls;
    uint64_t native_retired;
    uint64_t fallback_retired;
    uint64_t privileged_attempts;
    uint64_t privileged_calls;
    uint64_t privileged_retired;
    uint64_t known_negative_bypasses;
    uint64_t window_crossings;
    uint64_t window_reloads;
    uint64_t window_stops;
    uint64_t window_fast_refills;
    s5l_static_a64_compact_raw_refusals_t refusals;
} compact_privileged_oracle_result_t;

static void free_compact_privileged_oracle_result(
        compact_privileged_oracle_result_t *result) {
    if (!result) return;
    free(result->snapshot);
    memset(result, 0, sizeof *result);
}

static void seed_compact_privileged_oracle_cpu(arm_cpu_t *cpu,
                                                uint32_t mode,
                                                bool thumb) {
    for (unsigned i = 0u; i < 15u; i++)
        cpu->r[i] = UINT32_C(0x10101010) +
                    UINT32_C(0x01020304) * i;
    for (unsigned i = 0u; i < ARM_BANK_COUNT; i++) {
        cpu->bank_r13[i] = UINT32_C(0x20000000) + i * UINT32_C(0x1000);
        cpu->bank_r14[i] = UINT32_C(0x30000000) + i * UINT32_C(0x1000);
        cpu->spsr[i] = ARM_MODE_USR | ARM_CPSR_C |
                       ((i & 1u) ? ARM_CPSR_T : 0u);
    }
    for (unsigned i = 0u; i < 5u; i++) {
        cpu->fiq_r8_12[i] = UINT32_C(0x40000000) +
                             i * UINT32_C(0x1111);
        cpu->usr_r8_12[i] = UINT32_C(0x50000000) +
                             i * UINT32_C(0x2222);
    }

    /* arm_set_mode(), rather than a direct CPSR edit, proves that the native
     * register-file view is the same active bank the interpreter selected. */
    arm_set_mode(cpu, mode);
    cpu->cpsr = mode | ARM_CPSR_C | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A |
                (thumb ? ARM_CPSR_T : 0u);
    cpu->r[15] = thumb ? 2u : 0u;
}

typedef enum {
    COMPACT_PRIVILEGED_NATIVE,
    COMPACT_PRIVILEGED_CONTROL_BOUNDARY,
    COMPACT_PRIVILEGED_WINDOW_BOUNDARY,
} compact_privileged_workload_t;

static bool run_compact_privileged_oracle_case(
        uint32_t mode, bool thumb, compact_privileged_workload_t workload,
        bool compact, bool privileged_enabled, bool bypass_enabled,
        bool window_refill_enabled,
        compact_privileged_oracle_result_t *out) {
    static const uint32_t A32_NATIVE[] = {
        UINT32_C(0xe2888001), /* ADD r8,r8,#1: FIQ-bank witness */
        UINT32_C(0xe28cc002), /* ADD r12,r12,#2: FIQ-bank witness */
        UINT32_C(0xe28dd004), /* ADD sp,sp,#4: per-mode bank witness */
        UINT32_C(0xe28ee008), /* ADD lr,lr,#8: per-mode bank witness */
    };
    static const uint32_t A32_BOUNDARY[] = {
        UINT32_C(0xe2888001), /* exact native prefix; machine gate may own it */
        UINT32_C(0xe28cc002), /* keep a native prefix after that gate */
        UINT32_C(0xe28dd004),
        UINT32_C(0xe28ee008),
        UINT32_C(0xef000000), /* SVC: always interpreter-owned */
    };
    static const uint32_t A32_WINDOW[] = {
        UINT32_C(0xe2800001), /* native ADD at 0x3f8 */
        UINT32_C(0xe2811001), /* native ADD at 0x3fc */
        UINT32_C(0xe2822001), /* first instruction in the 0x400 window */
        UINT32_C(0xe2833001),
        UINT32_C(0xe2844001),
    };
    static const uint16_t THUMB_NATIVE[] = {
        UINT16_C(0x3001), /* ADDS r0,#1 */
        UINT16_C(0x3102), /* ADDS r1,#2 */
        UINT16_C(0x3203), /* ADDS r2,#3 */
        UINT16_C(0x3304), /* ADDS r3,#4 */
    };
    const uint32_t section = (3u << 10) | 2u;
    const unsigned total = workload == COMPACT_PRIVILEGED_NATIVE ? 4u : 5u;
    s5l8900_t machine = {0};
    arm_status_t status = ARM_OK;
    bool initialized = false;
    bool ok = false;

    if (!out || workload > COMPACT_PRIVILEGED_WINDOW_BOUNDARY ||
        (workload != COMPACT_PRIVILEGED_NATIVE && thumb))
        return false;
    memset(out, 0, sizeof *out);
    if (!s5l8900_init(&machine, 0u, RAM_SIZE)) goto done;
    initialized = true;
    if (thumb)
        s5l8900_load(&machine, 2u, THUMB_NATIVE, sizeof THUMB_NATIVE);
    else if (workload == COMPACT_PRIVILEGED_CONTROL_BOUNDARY)
        s5l8900_load(&machine, 0u, A32_BOUNDARY, sizeof A32_BOUNDARY);
    else if (workload == COMPACT_PRIVILEGED_WINDOW_BOUNDARY)
        s5l8900_load(&machine, UINT32_C(0x3f8), A32_WINDOW,
                     sizeof A32_WINDOW);
    else
        s5l8900_load(&machine, 0u, A32_NATIVE, sizeof A32_NATIVE);
    s5l8900_load(&machine, UINT32_C(0x4000), &section, sizeof section);
    machine.cpu.cp15.ttbr0 = UINT32_C(0x4000);
    machine.cpu.cp15.dacr = 1u;
    machine.cpu.cp15.sctlr |= ARM_SCTLR_M;
    seed_compact_privileged_oracle_cpu(&machine.cpu, mode, thumb);
    if (workload == COMPACT_PRIVILEGED_WINDOW_BOUNDARY)
        machine.cpu.r[15] = UINT32_C(0x3f8);
    s5l8900_tick(&machine, 0u);

    if (compact &&
        (!s5l8900_static_a64_set_enabled(&machine, true) ||
         !s5l8900_static_a64_set_compact_raw(&machine, true) ||
         !s5l8900_static_a64_set_compact_raw_privileged(
             &machine, privileged_enabled) ||
         !s5l8900_static_a64_set_known_negative_bypass(
             &machine, bypass_enabled) ||
         !s5l8900_static_a64_set_compact_raw_window_refill(
             &machine, true) ||
         !s5l8900_static_a64_set_compact_raw_privileged_window_refill(
             &machine, window_refill_enabled) ||
         !s5l8900_static_a64_set_chain_limit(
             &machine, A64_STATIC_MAX_CHAIN_INSNS)))
        goto done;
    if (s5l8900_run(&machine, total, &status) != total || status != ARM_OK)
        goto done;
    if (snapshot_save_mem(&machine, &out->snapshot,
                          &out->snapshot_len) != SNAP_OK)
        goto done;

    out->attempts = s5l8900_static_a64_compact_raw_attempts(&machine);
    out->calls = s5l8900_static_a64_compact_raw_calls(&machine);
    out->native_retired =
        s5l8900_static_a64_compact_raw_retired(&machine);
    out->fallback_retired =
        s5l8900_static_a64_compact_raw_fallback_retired(&machine);
    out->privileged_attempts =
        s5l8900_static_a64_compact_raw_privileged_attempts(&machine);
    out->privileged_calls =
        s5l8900_static_a64_compact_raw_privileged_calls(&machine);
    out->privileged_retired =
        s5l8900_static_a64_compact_raw_privileged_retired(&machine);
    out->known_negative_bypasses =
        s5l8900_static_a64_known_negative_bypasses(&machine);
    out->window_crossings =
        s5l8900_static_a64_compact_raw_window_crossings(&machine);
    out->window_reloads =
        s5l8900_static_a64_compact_raw_window_reloads(&machine);
    out->window_stops =
        s5l8900_static_a64_compact_raw_window_stops(&machine);
    out->window_fast_refills =
        s5l8900_static_a64_compact_raw_window_fast_refills(&machine);
    s5l8900_static_a64_compact_raw_refusals(&machine, &out->refusals);
    ok = true;

done:
    if (initialized) s5l8900_free(&machine);
    if (!ok) free_compact_privileged_oracle_result(out);
    return ok;
}

static bool compact_privileged_snapshots_equal(
        const compact_privileged_oracle_result_t *a,
        const compact_privileged_oracle_result_t *b) {
    return a && b && a->snapshot && b->snapshot &&
           a->snapshot_len == b->snapshot_len &&
           memcmp(a->snapshot, b->snapshot, a->snapshot_len) == 0;
}

/* The old User-only gate discarded virtually every compact entry in the
 * measured kernel-heavy phone interval. Admit only the already-proved native
 * instruction set under a privileged fetch tag. Any unsupported/control
 * instruction reaches the callback, which must refuse to call arm_step() in
 * privileged mode and return to the ordinary machine loop first. */
static bool validate_soc_compact_raw_privileged_prefix(void) {
    static const struct {
        uint32_t mode;
        const char *name;
    } MODES[] = {
        { ARM_MODE_FIQ, "fiq" }, { ARM_MODE_IRQ, "irq" },
        { ARM_MODE_SVC, "svc" }, { ARM_MODE_ABT, "abt" },
        { ARM_MODE_UND, "und" }, { ARM_MODE_SYS, "sys" },
    };
    unsigned exact_cases = 0u;
    uint64_t minimum_native = UINT64_MAX;

    if (!s5l8900_static_a64_available()) {
        printf("SOC-COMPACT-RAW-PRIVILEGED-ORACLE skip "
               "reason=signed-aarch64-unavailable\n");
        return true;
    }

    for (unsigned i = 0u; i < sizeof MODES / sizeof MODES[0]; i++) {
        for (unsigned thumb = 0u; thumb <= 1u; thumb++) {
            compact_privileged_oracle_result_t reference = {0};
            compact_privileged_oracle_result_t native = {0};
            const bool ran = run_compact_privileged_oracle_case(
                                 MODES[i].mode, thumb != 0u,
                                 COMPACT_PRIVILEGED_NATIVE,
                                 false, false, false, true, &reference) &&
                             run_compact_privileged_oracle_case(
                                 MODES[i].mode, thumb != 0u,
                                 COMPACT_PRIVILEGED_NATIVE,
                                 true, true, true, true, &native);
            const bool snapshot_exact = ran &&
                compact_privileged_snapshots_equal(&reference, &native);
            const bool exact = snapshot_exact &&
                native.attempts >= 1u && native.calls >= 1u &&
                native.calls <= native.attempts &&
                native.native_retired >= 3u &&
                native.native_retired <= 4u &&
                native.fallback_retired == 0u &&
                native.privileged_attempts == native.attempts &&
                native.privileged_calls == native.calls &&
                native.privileged_retired == native.native_retired &&
                native.known_negative_bypasses == 0u &&
                native.refusals.guard == 0u &&
                native.refusals.privileged == 0u &&
                native.refusals.alignment == 0u &&
                native.refusals.fetch_witness == 0u &&
                native.refusals.runner == 0u &&
                native.refusals.zero_retired == 0u;
            if (!exact) {
                fprintf(stderr,
                        "jitbench: compact privileged %s/%s mismatch "
                        "attempts/calls/native/fallback/priv=%" PRIu64
                        "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
                        "/%" PRIu64 " snapshot=%s\n",
                        MODES[i].name, thumb ? "thumb" : "a32",
                        native.attempts, native.calls,
                        native.native_retired, native.fallback_retired,
                        native.privileged_retired,
                        snapshot_exact ? "exact" : "mismatch");
                free_compact_privileged_oracle_result(&reference);
                free_compact_privileged_oracle_result(&native);
                return false;
            }
            exact_cases++;
            if (native.native_retired < minimum_native)
                minimum_native = native.native_retired;
            free_compact_privileged_oracle_result(&reference);
            free_compact_privileged_oracle_result(&native);
        }
    }

    {
        compact_privileged_oracle_result_t reference = {0};
        compact_privileged_oracle_result_t control = {0};
        const bool ran = run_compact_privileged_oracle_case(
                             ARM_MODE_SVC, false,
                             COMPACT_PRIVILEGED_NATIVE,
                             false, false, false, true, &reference) &&
                         run_compact_privileged_oracle_case(
                             ARM_MODE_SVC, false,
                             COMPACT_PRIVILEGED_NATIVE,
                             true, false, true, true, &control);
        const bool snapshot_exact = ran &&
            compact_privileged_snapshots_equal(&reference, &control);
        const bool exact = snapshot_exact &&
            control.attempts >= 3u && control.attempts <= 4u &&
            control.calls == 0u &&
            control.native_retired == 0u && control.fallback_retired == 0u &&
            control.privileged_attempts == control.attempts &&
            control.privileged_calls == 0u &&
            control.privileged_retired == 0u &&
            control.known_negative_bypasses == 0u &&
            control.refusals.privileged == control.attempts &&
            control.refusals.guard == 0u &&
            control.refusals.alignment == 0u &&
            control.refusals.fetch_witness == 0u &&
            control.refusals.runner == 0u &&
            control.refusals.zero_retired == 0u;
        if (!exact) {
            fprintf(stderr,
                    "jitbench: compact privileged user-only control "
                    "mismatch attempts/calls/refused=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 " snapshot=%s\n",
                    control.attempts, control.calls,
                    control.refusals.privileged,
                    snapshot_exact ? "exact" : "mismatch");
            free_compact_privileged_oracle_result(&reference);
            free_compact_privileged_oracle_result(&control);
            return false;
        }
        free_compact_privileged_oracle_result(&reference);
        free_compact_privileged_oracle_result(&control);
    }

    {
        compact_privileged_oracle_result_t reference = {0};
        compact_privileged_oracle_result_t off = {0};
        compact_privileged_oracle_result_t on = {0};
        const bool ran = run_compact_privileged_oracle_case(
                             ARM_MODE_SVC, false,
                             COMPACT_PRIVILEGED_CONTROL_BOUNDARY,
                             false, false, false, true, &reference) &&
                         run_compact_privileged_oracle_case(
                             ARM_MODE_SVC, false,
                             COMPACT_PRIVILEGED_CONTROL_BOUNDARY,
                             true, true, false, true, &off) &&
                         run_compact_privileged_oracle_case(
                             ARM_MODE_SVC, false,
                             COMPACT_PRIVILEGED_CONTROL_BOUNDARY,
                             true, true, true, true, &on);
        const bool snapshot_exact = ran &&
            compact_privileged_snapshots_equal(&reference, &off) &&
            compact_privileged_snapshots_equal(&reference, &on);
        /* The first retirement can be owned by the machine timing gate. Four
         * native instructions ensure the admitted batch still reaches a
         * nonempty native prefix before the interpreter-owned SVC boundary.
         * The off arm reproduces the redundant zero-return re-entry; the on
         * arm must consume one exact post-tick witness and go directly to the
         * same literal interpreter step. */
        const bool exact = snapshot_exact &&
            off.attempts == 2u && off.calls == 1u &&
            on.attempts == 1u && on.calls == 1u &&
            off.native_retired >= 3u && off.native_retired <= 4u &&
            on.native_retired == off.native_retired &&
            off.fallback_retired == 0u && on.fallback_retired == 0u &&
            off.privileged_attempts == 2u && off.privileged_calls == 1u &&
            on.privileged_attempts == 1u && on.privileged_calls == 1u &&
            off.privileged_retired == off.native_retired &&
            on.privileged_retired == on.native_retired &&
            off.known_negative_bypasses == 0u &&
            on.known_negative_bypasses == 1u &&
            off.refusals.privileged == 0u &&
            on.refusals.privileged == 0u &&
            off.refusals.zero_retired == 1u &&
            on.refusals.zero_retired == 0u;
        if (!exact) {
            fprintf(stderr,
                    "jitbench: compact privileged boundary mismatch "
                    "off-attempts/calls/zero=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 " on-attempts/calls/zero/bypasses=%" PRIu64
                    "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 " snapshot=%s\n",
                    off.attempts, off.calls, off.refusals.zero_retired,
                    on.attempts, on.calls, on.refusals.zero_retired,
                    on.known_negative_bypasses,
                    snapshot_exact ? "exact" : "mismatch");
            free_compact_privileged_oracle_result(&reference);
            free_compact_privileged_oracle_result(&off);
            free_compact_privileged_oracle_result(&on);
            return false;
        }
        free_compact_privileged_oracle_result(&reference);
        free_compact_privileged_oracle_result(&off);
        free_compact_privileged_oracle_result(&on);
    }

    {
        compact_privileged_oracle_result_t reference = {0};
        compact_privileged_oracle_result_t refill_off = {0};
        compact_privileged_oracle_result_t guarded = {0};
        const bool ran = run_compact_privileged_oracle_case(
                             ARM_MODE_SVC, false,
                             COMPACT_PRIVILEGED_WINDOW_BOUNDARY,
                             false, false, false, true, &reference) &&
                         run_compact_privileged_oracle_case(
                             ARM_MODE_SVC, false,
                             COMPACT_PRIVILEGED_WINDOW_BOUNDARY,
                             true, true, true, false, &refill_off) &&
                         run_compact_privileged_oracle_case(
                             ARM_MODE_SVC, false,
                             COMPACT_PRIVILEGED_WINDOW_BOUNDARY,
                             true, true, true, true, &guarded);
        const bool snapshot_exact = ran &&
            compact_privileged_snapshots_equal(&reference, &refill_off) &&
            compact_privileged_snapshots_equal(&reference, &guarded);
        const bool exact = snapshot_exact &&
            refill_off.attempts == guarded.attempts &&
            refill_off.calls == guarded.calls &&
            refill_off.native_retired == guarded.native_retired &&
            refill_off.native_retired != 0u &&
            refill_off.fallback_retired == guarded.fallback_retired &&
            refill_off.privileged_attempts == guarded.privileged_attempts &&
            refill_off.privileged_calls == guarded.privileged_calls &&
            refill_off.privileged_retired == guarded.privileged_retired &&
            refill_off.known_negative_bypasses ==
                guarded.known_negative_bypasses &&
            refill_off.window_crossings == guarded.window_crossings &&
            refill_off.window_reloads == guarded.window_reloads &&
            refill_off.window_stops == guarded.window_stops &&
            refill_off.window_fast_refills == 0u &&
            guarded.window_fast_refills == 0u;
        if (!exact) {
            fprintf(stderr,
                    "jitbench: compact privileged window guard mismatch "
                    "off-attempts/calls/native/windows=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 " guarded=%" PRIu64 "/%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
                    " snapshot=%s\n",
                    refill_off.attempts, refill_off.calls,
                    refill_off.native_retired, refill_off.window_crossings,
                    refill_off.window_reloads, refill_off.window_stops,
                    refill_off.window_fast_refills, guarded.attempts,
                    guarded.calls, guarded.native_retired,
                    guarded.window_crossings, guarded.window_reloads,
                    guarded.window_stops, guarded.window_fast_refills,
                    snapshot_exact ? "exact" : "mismatch");
            free_compact_privileged_oracle_result(&reference);
            free_compact_privileged_oracle_result(&refill_off);
            free_compact_privileged_oracle_result(&guarded);
            return false;
        }
        free_compact_privileged_oracle_result(&reference);
        free_compact_privileged_oracle_result(&refill_off);
        free_compact_privileged_oracle_result(&guarded);
    }

    printf("SOC-COMPACT-RAW-PRIVILEGED-ORACLE exact=yes modes=6 "
           "states=a32,thumb cases=%u minimum-native=%" PRIu64 " "
           "banked-registers=yes mmu=on "
           "privileged-native-prefix=yes privileged-fallback=no "
           "control=user-only boundary=svc-zero-bypass "
           "privileged-window-refill=guarded "
           "boundary-attempts=2-to-1 boundary-zero=1-to-0 "
           "serialized-machine=yes "
           "runtime-codegen=no\n",
           exact_cases, minimum_native);
    return true;
}

/* The compact loop has always selected Thumb width from CPSR itself, but its
 * former machine entry required PC%4==0 before calling that loop. A valid
 * halfword-aligned entry at PC%4==2 therefore fell straight into arm_step().
 * Exercise the app-facing runner at precisely that boundary and compare the
 * complete serialized machine, not just r0/PC. */
static bool validate_soc_compact_raw_thumb_halfword_entry(void) {
    static const uint16_t INSN = UINT16_C(0x3001); /* adds r0,#1 */
    s5l8900_t reference = {0};
    s5l8900_t compact = {0};
    arm_status_t reference_status = ARM_OK;
    arm_status_t compact_status = ARM_OK;
    uint8_t *reference_snapshot = NULL;
    uint8_t *compact_snapshot = NULL;
    size_t reference_len = 0u;
    size_t compact_len = 0u;
    s5l_static_a64_compact_raw_refusals_t refusals;
    bool reference_initialized = false;
    bool compact_initialized = false;
    bool ok = false;

    if (!s5l8900_static_a64_available()) {
        printf("SOC-COMPACT-RAW-THUMB-HALFWORD-ORACLE skip "
               "reason=signed-aarch64-unavailable\n");
        return true;
    }
    if (!s5l8900_init(&reference, 0u, RAM_SIZE)) {
        fprintf(stderr,
                "jitbench: compact raw Thumb halfword init failed\n");
        goto done;
    }
    reference_initialized = true;
    if (!s5l8900_init(&compact, 0u, RAM_SIZE)) {
        fprintf(stderr,
                "jitbench: compact raw Thumb halfword init failed\n");
        goto done;
    }
    compact_initialized = true;
    s5l8900_load(&reference, 2u, &INSN, sizeof INSN);
    s5l8900_load(&compact, 2u, &INSN, sizeof INSN);
    reference.cpu.r[0] = compact.cpu.r[0] = UINT32_C(41);
    reference.cpu.r[15] = compact.cpu.r[15] = UINT32_C(2);
    reference.cpu.cpsr = compact.cpu.cpsr =
        ARM_MODE_USR | ARM_CPSR_T | ARM_CPSR_C;
    s5l8900_tick(&reference, 0u);
    s5l8900_tick(&compact, 0u);
    if (!s5l8900_static_a64_set_enabled(&compact, true) ||
        !s5l8900_static_a64_set_compact_raw(&compact, true) ||
        !s5l8900_static_a64_set_chain_limit(
            &compact, A64_STATIC_MAX_CHAIN_INSNS)) {
        fprintf(stderr,
                "jitbench: compact raw Thumb halfword path unavailable\n");
        goto done;
    }
    if (s5l8900_run(&reference, 1u, &reference_status) != 1u ||
        s5l8900_run(&compact, 1u, &compact_status) != 1u ||
        reference_status != ARM_OK || compact_status != ARM_OK ||
        snapshot_save_mem(&reference, &reference_snapshot,
                          &reference_len) != SNAP_OK ||
        snapshot_save_mem(&compact, &compact_snapshot,
                          &compact_len) != SNAP_OK) {
        fprintf(stderr,
                "jitbench: compact raw Thumb halfword execution failed\n");
        goto done;
    }
    s5l8900_static_a64_compact_raw_refusals(&compact, &refusals);
    if (reference_len != compact_len ||
        memcmp(reference_snapshot, compact_snapshot, reference_len) != 0 ||
        s5l8900_static_a64_compact_raw_attempts(&compact) != 1u ||
        s5l8900_static_a64_compact_raw_calls(&compact) != 1u ||
        s5l8900_static_a64_compact_raw_retired(&compact) != 1u ||
        s5l8900_static_a64_compact_raw_fallback_retired(&compact) != 0u ||
        refusals.guard != 0u || refusals.privileged != 0u ||
        refusals.alignment != 0u || refusals.fetch_witness != 0u ||
        refusals.runner != 0u || refusals.zero_retired != 0u) {
        fprintf(stderr,
                "jitbench: compact raw Thumb halfword oracle mismatch "
                "attempts/calls/native=%" PRIu64 "/%" PRIu64 "/%" PRIu64
                " refusals=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
                "/%" PRIu64 "/%" PRIu64 "\n",
                s5l8900_static_a64_compact_raw_attempts(&compact),
                s5l8900_static_a64_compact_raw_calls(&compact),
                s5l8900_static_a64_compact_raw_retired(&compact),
                refusals.guard, refusals.privileged, refusals.alignment,
                refusals.fetch_witness, refusals.runner,
                refusals.zero_retired);
        goto done;
    }
    printf("SOC-COMPACT-RAW-THUMB-HALFWORD-ORACLE exact=yes pc=0x2 "
           "alignment=halfword native=1 fallback=0 "
           "serialized-machine=yes runtime-codegen=no\n");
    ok = true;

done:
    free(reference_snapshot);
    free(compact_snapshot);
    if (reference_initialized) s5l8900_free(&reference);
    if (compact_initialized) s5l8900_free(&compact);
    return ok;
}

/* Prove the resident/native-interpreter partition through the actual machine
 * runner before measuring its compute-only control. The two MMU-on data
 * accesses use already-proven DREAD/DWRITE witnesses; only unsupported MUL
 * requires arm_step(). The other four ALU operations plus the loop branch
 * stay in the build-time-linked AArch64 loop. */
static bool validate_soc_compact_raw_resident(void) {
    enum { LOOP_INSNS = 8u, TOTAL_INSNS = 8192u };
    static const uint32_t PROGRAM[LOOP_INSNS] = {
        UINT32_C(0xe2800001), /* native ADD r0,r0,#1 */
        UINT32_C(0xe5870000), /* native DWRITE-hit STR r0,[r7,#0] */
        UINT32_C(0xe2422001), /* native SUB r2,r2,#1 */
        UINT32_C(0xe0000090), /* fallback MUL r0,r0,r0 */
        UINT32_C(0xe2855001), /* native ADD r5,r5,#1 */
        UINT32_C(0xe5971000), /* native DREAD-hit LDR r1,[r7,#0] */
        UINT32_C(0xe0266001), /* native EOR r6,r6,r1 */
        UINT32_C(0xeafffff7), /* native branch 0x1c -> 0x00 */
    };
    const uint64_t expected_native =
        (uint64_t)TOTAL_INSNS * 7u / LOOP_INSNS;
    const uint64_t expected_fallback =
        (uint64_t)TOTAL_INSNS / LOOP_INSNS;
    const uint64_t expected_data_hits =
        (uint64_t)TOTAL_INSNS / LOOP_INSNS;
    soc_run_result_t reference = {0};
    soc_run_result_t resident = {0};
    bool ok = false;

    if (!run_soc_compact_raw_path(
            PROGRAM, LOOP_INSNS, TOTAL_INSNS, SOC_ENTRY_REFERENCE,
            &reference) ||
        !run_soc_compact_raw_path(
            PROGRAM, LOOP_INSNS, TOTAL_INSNS, SOC_ENTRY_COMPACT_RAW,
            &resident) ||
        !reference.snapshot || !resident.snapshot ||
        reference.snapshot_len != resident.snapshot_len ||
        memcmp(reference.snapshot, resident.snapshot,
               reference.snapshot_len) != 0 ||
        reference.compact_raw_attempts != 0u ||
        reference.compact_raw_calls != 0u ||
        reference.compact_raw_retired != 0u ||
        reference.compact_raw_fallback_retired != 0u ||
        reference.compact_raw_window_crossings != 0u ||
        reference.compact_raw_window_reloads != 0u ||
        reference.compact_raw_window_stops != 0u ||
        reference.compact_raw_window_fast_refills != 0u ||
        reference.dread_hits != expected_data_hits ||
        reference.dread_misses != 0u ||
        reference.dwrite_hits != expected_data_hits ||
        reference.dwrite_misses != 0u ||
        resident.compact_raw_retired != expected_native ||
        resident.compact_raw_fallback_retired != expected_fallback ||
        resident.dread_hits != reference.dread_hits ||
        resident.dread_misses != reference.dread_misses ||
        resident.dwrite_hits != reference.dwrite_hits ||
        resident.dwrite_misses != reference.dwrite_misses ||
        resident.signed_retired != expected_native ||
        resident.compact_raw_calls == 0u ||
        resident.compact_raw_attempts < resident.compact_raw_calls ||
        resident.compact_raw_window_crossings != 0u ||
        resident.compact_raw_window_reloads != 0u ||
        resident.compact_raw_window_stops != 0u ||
        resident.compact_raw_window_fast_refills != 0u) {
        fprintf(stderr,
                "jitbench: SoC compact-raw resident oracle failed "
                "attempts/calls/native/fallback=%" PRIu64 "/%" PRIu64
                "/%" PRIu64 "/%" PRIu64 " expected=%" PRIu64 "/%" PRIu64
                " data-hits=%" PRIu64 "/%" PRIu64 " expected=%" PRIu64
                "\n",
                resident.compact_raw_attempts, resident.compact_raw_calls,
                resident.compact_raw_retired,
                resident.compact_raw_fallback_retired, expected_native,
                expected_fallback, resident.dread_hits,
                resident.dwrite_hits, expected_data_hits);
        goto done;
    }

    printf("SOC-COMPACT-RAW-RESIDENT-ORACLE exact=yes guest-insns=%u "
           "loop-insns=%u mmu=on native=%" PRIu64 " fallback=%" PRIu64
           " dread-hits=%" PRIu64 " dwrite-hits=%" PRIu64
           " data-cache-hit=yes write-consent=yes unsupported-fallback=yes "
           "timebase-bounded=yes device-tick=yes serialized-machine=yes "
           "runtime-codegen=no\n",
           TOTAL_INSNS, LOOP_INSNS, expected_native, expected_fallback,
           expected_data_hits, expected_data_hits);
    ok = true;

done:
    free_soc_run_result(&reference);
    free_soc_run_result(&resident);
    return ok;
}

/* Force the real timebase-bounded machine path across a 1 KiB fetch boundary
 * in both directions. The control interprets the first instruction in each
 * newly reached window. The enabled arm instead consumes the exact warm FETCH
 * witness, publishes the new window without retirement, and executes that
 * admitted instruction natively. All four complete machine snapshots must be
 * byte-identical. */
static bool validate_soc_compact_raw_windows(void) {
    enum { LOOP_INSNS = 259u, LOOP_COUNT = 32u,
           TOTAL_INSNS = LOOP_INSNS * LOOP_COUNT };
    uint32_t program[LOOP_INSNS];
    soc_run_result_t reference = {0};
    soc_run_result_t refill_off = {0};
    soc_run_result_t refill_on = {0};
    soc_run_result_t cached = {0};
    bool refill_off_snapshot_equal = false;
    bool refill_on_snapshot_equal = false;
    bool cached_snapshot_equal = false;
    size_t cached_snapshot_first_diff = SIZE_MAX;
    bool ok = false;

    for (unsigned i = 0u; i < 256u; i++)
        program[i] = UINT32_C(0xe2800001); /* ADD r0,r0,#1 */
    program[256] = UINT32_C(0xe2844001);   /* admitted ADD at 0x400 */
    program[257] = UINT32_C(0xe0000090);   /* fallback MUL at 0x404 */
    program[258] = UINT32_C(0xeafffefc);   /* branch 0x408 -> 0x000 */

    if (!run_soc_compact_raw_path(
            program, LOOP_INSNS, TOTAL_INSNS, SOC_ENTRY_REFERENCE,
            &reference) ||
        !run_soc_compact_raw_path(
            program, LOOP_INSNS, TOTAL_INSNS,
            SOC_ENTRY_COMPACT_RAW_WINDOW_REFILL_OFF, &refill_off) ||
        !run_soc_compact_raw_path(
            program, LOOP_INSNS, TOTAL_INSNS, SOC_ENTRY_COMPACT_RAW,
            &refill_on) ||
        !run_soc_compact_raw_path(
            program, LOOP_INSNS, TOTAL_INSNS,
            SOC_ENTRY_COMPACT_RAW_WINDOW_CACHE, &cached) ||
        !reference.snapshot || !refill_off.snapshot || !refill_on.snapshot ||
        !cached.snapshot) {
        fprintf(stderr,
                "jitbench: SoC compact-raw window oracle setup failed\n");
        goto done;
    }

    refill_off_snapshot_equal =
        reference.snapshot_len == refill_off.snapshot_len &&
        memcmp(reference.snapshot, refill_off.snapshot,
               reference.snapshot_len) == 0;
    refill_on_snapshot_equal =
        reference.snapshot_len == refill_on.snapshot_len &&
        memcmp(reference.snapshot, refill_on.snapshot,
               reference.snapshot_len) == 0;
    cached_snapshot_equal =
        reference.snapshot_len == cached.snapshot_len &&
        memcmp(reference.snapshot, cached.snapshot,
               reference.snapshot_len) == 0;
    if (!cached_snapshot_equal &&
        reference.snapshot_len == cached.snapshot_len) {
        for (size_t i = 0u; i < reference.snapshot_len; i++) {
            if (reference.snapshot[i] != cached.snapshot[i]) {
                cached_snapshot_first_diff = i;
                break;
            }
        }
    }

    if (!refill_off_snapshot_equal || !refill_on_snapshot_equal ||
        !cached_snapshot_equal ||
        reference.compact_raw_window_crossings != 0u ||
        reference.compact_raw_window_reloads != 0u ||
        reference.compact_raw_window_stops != 0u ||
        reference.compact_raw_window_fast_refills != 0u ||
        reference.compact_raw_window_cache_hits != 0u ||
        refill_off.compact_raw_window_crossings == 0u ||
        refill_off.compact_raw_window_crossings !=
                refill_on.compact_raw_window_crossings ||
        refill_off.compact_raw_window_reloads !=
                refill_off.compact_raw_window_crossings ||
        refill_off.compact_raw_window_stops != 0u ||
        refill_off.compact_raw_window_fast_refills != 0u ||
        refill_off.compact_raw_window_cache_hits != 0u ||
        refill_on.compact_raw_window_reloads !=
                refill_on.compact_raw_window_crossings ||
        refill_on.compact_raw_window_stops != 0u ||
        refill_on.compact_raw_window_fast_refills !=
                refill_on.compact_raw_window_crossings ||
        refill_on.compact_raw_window_cache_hits != 0u ||
        refill_off.compact_raw_fallback_retired -
                refill_on.compact_raw_fallback_retired !=
                refill_on.compact_raw_window_fast_refills ||
        refill_on.compact_raw_retired - refill_off.compact_raw_retired !=
                refill_on.compact_raw_window_fast_refills ||
        cached.compact_raw_window_cache_hits == 0u ||
        cached.compact_raw_window_crossings !=
                refill_on.compact_raw_window_crossings ||
        cached.compact_raw_window_reloads !=
                refill_on.compact_raw_window_reloads ||
        cached.compact_raw_window_stops != 0u ||
        cached.compact_raw_window_fast_refills >=
                refill_on.compact_raw_window_fast_refills ||
        cached.compact_raw_window_fast_refills +
                cached.compact_raw_window_cache_hits !=
                cached.compact_raw_window_crossings ||
        cached.compact_raw_retired != refill_on.compact_raw_retired ||
        cached.compact_raw_fallback_retired !=
                refill_on.compact_raw_fallback_retired) {
        fprintf(stderr,
                "jitbench: SoC compact-raw window oracle failed "
                "off-cross/reload/fast/native/fallback=%" PRIu64 "/%" PRIu64
                "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
                " on=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
                "/%" PRIu64 " cached=%" PRIu64 "/%" PRIu64 "/%" PRIu64
                "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
                " stops=%" PRIu64
                " snapshots(off/on/cached)=%s/%s/%s"
                " cached-snapshot-len=%zu ref-len=%zu first-diff=%zu\n",
                refill_off.compact_raw_window_crossings,
                refill_off.compact_raw_window_reloads,
                refill_off.compact_raw_window_fast_refills,
                refill_off.compact_raw_retired,
                refill_off.compact_raw_fallback_retired,
                refill_on.compact_raw_window_crossings,
                refill_on.compact_raw_window_reloads,
                refill_on.compact_raw_window_fast_refills,
                refill_on.compact_raw_retired,
                refill_on.compact_raw_fallback_retired,
                cached.compact_raw_window_crossings,
                cached.compact_raw_window_reloads,
                cached.compact_raw_window_fast_refills,
                cached.compact_raw_window_cache_hits,
                cached.compact_raw_retired,
                cached.compact_raw_fallback_retired,
                cached.compact_raw_window_stops,
                refill_off_snapshot_equal ? "yes" : "no",
                refill_on_snapshot_equal ? "yes" : "no",
                cached_snapshot_equal ? "yes" : "no",
                cached.snapshot_len, reference.snapshot_len,
                cached_snapshot_first_diff);
        goto done;
    }

    printf("SOC-COMPACT-RAW-WINDOW-ORACLE exact=yes guest-insns=%u "
           "window-bytes=1024 crossings=%" PRIu64
           " fast-refills=%" PRIu64 " cache-hits=%" PRIu64
           " displaced-fallbacks=%" PRIu64
           " sequential-cross=yes branch-cross=yes "
           "lookup-only=yes no-retire-continue=yes repeated-window-cache=yes "
           "same-binary-control=yes "
           "timebase-bounded=yes "
           "serialized-machine=yes runtime-codegen=no\n",
           TOTAL_INSNS, refill_on.compact_raw_window_crossings,
           refill_on.compact_raw_window_fast_refills,
           cached.compact_raw_window_cache_hits,
           refill_on.compact_raw_window_fast_refills);
    ok = true;

done:
    free_soc_run_result(&reference);
    free_soc_run_result(&refill_off);
    free_soc_run_result(&refill_on);
    free_soc_run_result(&cached);
    return ok;
}

/* Repeat the cross-window oracle in privileged mode. Unlike the User path,
 * each continuation must account its completed prefix through the real SoC
 * device/clock boundary before publishing the next FETCH witness. The generic
 * User-mode refill gate stays enabled in both arms; only the privileged gate is
 * changed. The invocation-count inequality is the structural efficiency gate. */
static bool validate_soc_compact_raw_privileged_windows(void) {
    enum { LOOP_INSNS = 259u, LOOP_COUNT = 32u,
           TOTAL_INSNS = LOOP_INSNS * LOOP_COUNT };
    uint32_t program[LOOP_INSNS];
    soc_run_result_t reference = {0};
    soc_run_result_t refill_off = {0};
    soc_run_result_t refill_on = {0};
    bool ok = false;

    for (unsigned i = 0u; i < 256u; i++)
        program[i] = UINT32_C(0xe2800001); /* ADD r0,r0,#1 */
    program[256] = UINT32_C(0xe2844001);   /* admitted ADD at 0x400 */
    program[257] = UINT32_C(0xe0000090);   /* fallback MUL at 0x404 */
    program[258] = UINT32_C(0xeafffefc);   /* branch 0x408 -> 0x000 */

    if (!run_soc_compact_raw_privileged_path(
            program, LOOP_INSNS, TOTAL_INSNS, SOC_ENTRY_REFERENCE,
            &reference) ||
        !run_soc_compact_raw_privileged_path(
            program, LOOP_INSNS, TOTAL_INSNS,
            SOC_ENTRY_COMPACT_RAW_WINDOW_REFILL_OFF, &refill_off) ||
        !run_soc_compact_raw_privileged_path(
            program, LOOP_INSNS, TOTAL_INSNS, SOC_ENTRY_COMPACT_RAW,
            &refill_on) ||
        !reference.snapshot || !refill_off.snapshot || !refill_on.snapshot ||
        reference.snapshot_len != refill_off.snapshot_len ||
        reference.snapshot_len != refill_on.snapshot_len ||
        memcmp(reference.snapshot, refill_off.snapshot,
               reference.snapshot_len) != 0 ||
        memcmp(reference.snapshot, refill_on.snapshot,
               reference.snapshot_len) != 0 ||
        reference.compact_raw_privileged_window_refills != 0u ||
        reference.compact_raw_privileged_boundary_retired != 0u ||
        refill_off.compact_raw_privileged_window_refills != 0u ||
        refill_off.compact_raw_privileged_boundary_retired != 0u ||
        refill_off.compact_raw_window_fast_refills != 0u ||
        refill_on.compact_raw_privileged_window_refills == 0u ||
        refill_on.compact_raw_privileged_window_refills !=
            refill_on.compact_raw_window_fast_refills ||
        refill_on.compact_raw_window_crossings !=
            refill_on.compact_raw_privileged_window_refills ||
        refill_on.compact_raw_window_reloads !=
            refill_on.compact_raw_privileged_window_refills ||
        refill_on.compact_raw_window_stops != 0u ||
        refill_on.compact_raw_privileged_boundary_retired == 0u ||
        refill_on.compact_raw_privileged_boundary_retired >
            refill_on.compact_raw_retired ||
        /* A privileged refill may remove an outer machine entry without
         * changing which path retires the next instruction. Unlike the User
         * callback path, refill count therefore does not equal newly native
         * retirements. Gate the actual contract: every admitted instruction
         * is native, deliberate MUL refusals remain outside the resident
         * callback, and enabled continuation needs fewer outer entries. */
        refill_on.compact_raw_calls >= refill_off.compact_raw_calls ||
        refill_on.compact_raw_fallback_retired != 0u ||
        refill_off.compact_raw_fallback_retired != 0u ||
        refill_on.compact_raw_retired !=
            (uint64_t)TOTAL_INSNS - (uint64_t)LOOP_COUNT ||
        refill_off.compact_raw_retired > refill_on.compact_raw_retired) {
        fprintf(stderr,
                "jitbench: privileged compact-window oracle failed "
                "off-calls/native/fallback=%" PRIu64 "/%" PRIu64
                "/%" PRIu64 " on-calls/native/fallback/refills/boundary=%"
                PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
                "\n",
                refill_off.compact_raw_calls,
                refill_off.compact_raw_retired,
                refill_off.compact_raw_fallback_retired,
                refill_on.compact_raw_calls,
                refill_on.compact_raw_retired,
                refill_on.compact_raw_fallback_retired,
                refill_on.compact_raw_privileged_window_refills,
                refill_on.compact_raw_privileged_boundary_retired);
        goto done;
    }

    printf("SOC-COMPACT-RAW-PRIVILEGED-WINDOW-ORACLE exact=yes "
           "guest-insns=%u window-bytes=1024 refills=%" PRIu64
           " boundary-retired=%" PRIu64 " calls-off=%" PRIu64
           " calls-on=%" PRIu64 " outer-fallbacks-off=%" PRIu64
           " outer-fallbacks-on=%" PRIu64 " all-admitted-native=yes "
           " fewer-outer-entries=yes "
           "user-window-refill=on-both privileged-control=isolated "
           "same-binary-control=yes timebase-bounded=yes device-tick=yes "
           "serialized-machine=yes runtime-codegen=no\n",
           TOTAL_INSNS,
           refill_on.compact_raw_privileged_window_refills,
           refill_on.compact_raw_privileged_boundary_retired,
           refill_off.compact_raw_calls, refill_on.compact_raw_calls,
           (uint64_t)TOTAL_INSNS - refill_off.compact_raw_retired,
           (uint64_t)TOTAL_INSNS - refill_on.compact_raw_retired);
    ok = true;

done:
    free_soc_run_result(&reference);
    free_soc_run_result(&refill_off);
    free_soc_run_result(&refill_on);
    return ok;
}

/* First real-machine gate for the compact live-byte architecture. Both arms
 * use the app-facing s5l8900_run(), an enabled identity-mapped MMU and User
 * mode. The reference uses the exact interpreter tick batcher; the compact
 * arm uses the signed engine's equivalent first-timebase-edge bound. It
 * obtains code only from the CPU's proven live 1 KiB fetch window and compares
 * the complete serialized machine afterward. */
static bool bench_soc_compact_raw(uint64_t requested, unsigned reps) {
    enum { LOOP_INSNS = 16u };
    uint32_t program[A64_STATIC_MAX_INSNS];
    a64_static_block_t shape;
    double *reference_rates = NULL, *compact_rates = NULL;
    uint64_t total;
    uint64_t exact_attempts = 0u, exact_calls = 0u;
    bool ok = false;

    if (requested > UINT64_MAX - (LOOP_INSNS - 1u) ||
        !prepare_product_entry(LOOP_INSNS, program, &shape)) {
        fprintf(stderr, "jitbench: SoC compact-raw shape failed\n");
        return false;
    }
    if (!validate_soc_compact_raw_privileged_prefix() ||
        !validate_soc_compact_raw_thumb_halfword_entry() ||
        !validate_soc_compact_raw_resident() ||
        !validate_soc_compact_raw_windows() ||
        !validate_soc_compact_raw_privileged_windows())
        return false;
    total = ((requested + LOOP_INSNS - 1u) / LOOP_INSNS) * LOOP_INSNS;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    compact_rates = (double *)calloc(reps, sizeof *compact_rates);
    if (!reference_rates || !compact_rates) {
        fprintf(stderr, "jitbench: SoC compact-raw out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t compact = {0};
        const char *order;
        bool ran;

        if ((rep & 1u) == 0u) {
            order = "reference-compact";
            ran = run_soc_compact_raw_path(
                      program, LOOP_INSNS, total, SOC_ENTRY_REFERENCE,
                      &reference) &&
                  run_soc_compact_raw_path(
                      program, LOOP_INSNS, total, SOC_ENTRY_COMPACT_RAW,
                      &compact);
        } else {
            order = "compact-reference";
            ran = run_soc_compact_raw_path(
                      program, LOOP_INSNS, total, SOC_ENTRY_COMPACT_RAW,
                      &compact) &&
                  run_soc_compact_raw_path(
                      program, LOOP_INSNS, total, SOC_ENTRY_REFERENCE,
                      &reference);
        }
        if (!ran || !reference.snapshot || !compact.snapshot ||
            reference.snapshot_len != compact.snapshot_len ||
            memcmp(reference.snapshot, compact.snapshot,
                   reference.snapshot_len) != 0 ||
            reference.compact_raw_attempts != 0u ||
            reference.compact_raw_calls != 0u ||
            reference.compact_raw_retired != 0u ||
            reference.compact_raw_fallback_retired != 0u ||
            compact.compact_raw_retired != total ||
            compact.compact_raw_fallback_retired != 0u ||
            compact.signed_retired != total ||
            compact.compact_raw_calls == 0u ||
            compact.compact_raw_attempts < compact.compact_raw_calls) {
            fprintf(stderr,
                    "jitbench: SoC compact-raw repetition %u failed "
                    "attempts/calls/retired=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 " expected=%" PRIu64 "\n",
                    rep + 1u, compact.compact_raw_attempts,
                    compact.compact_raw_calls,
                    compact.compact_raw_retired, total);
            free_soc_run_result(&reference);
            free_soc_run_result(&compact);
            goto done;
        }
        if (rep == 0u) {
            exact_attempts = compact.compact_raw_attempts;
            exact_calls = compact.compact_raw_calls;
        } else if (compact.compact_raw_attempts != exact_attempts ||
                   compact.compact_raw_calls != exact_calls) {
            fprintf(stderr,
                    "jitbench: SoC compact-raw call accounting drifted\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&compact);
            goto done;
        }
        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        compact_rates[rep] = (double)total / compact.seconds / 1.0e6;
        printf("SOC-COMPACT-RAW-SAMPLE rep=%u order=%s reference=%.3f "
               "compact-raw=%.3f Minsn/s attempts=%" PRIu64
               " calls=%" PRIu64 " native=%" PRIu64
               " fallback=%" PRIu64 "\n",
               rep + 1u, order, reference_rates[rep], compact_rates[rep],
               compact.compact_raw_attempts, compact.compact_raw_calls,
               compact.compact_raw_retired,
               compact.compact_raw_fallback_retired);
        free_soc_run_result(&reference);
        free_soc_run_result(&compact);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(compact_rates, reps, sizeof *compact_rates, cmp_double);
    printf("SOC-COMPACT-RAW-CEILING guest-insns=%" PRIu64
           " reps=%u loop-insns=%u mmu=on live-fetch-window=yes "
           "data-access=absent pre-step-hook=absent timebase-bounded=yes "
           "device-tick=yes serialized-machine=yes runtime-codegen=no "
           "attempts=%" PRIu64 " calls=%" PRIu64
           " reference-median=%.3f compact-raw-median=%.3f speedup=%.3fx "
           "fallback-retired=0\n",
           total, reps, LOOP_INSNS, exact_attempts, exact_calls,
           reference_rates[reps / 2u], compact_rates[reps / 2u],
           compact_rates[reps / 2u] / reference_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(compact_rates);
    return ok;
}

/* The earlier product-entry curve intentionally stops before the SoC. This
 * curve includes that missing product machinery and compares complete machine
 * state. The fourth same-binary arm changes only the total signed-invocation
 * bound from 16 to 256; the machine still clamps it to the first timebase edge.
 * It is still not phone FPS: there is no MMU miss, real firmware mix,
 * framebuffer publication or UIKit in this synthetic loop. */
static bool bench_soc_entry(unsigned length, uint64_t requested,
                            unsigned reps) {
    uint32_t program[A64_STATIC_MAX_INSNS];
    a64_static_block_t shape;
    double *reference_rates = NULL;
    double *signed_rates = NULL;
    double *graph_rates = NULL;
    double *extended_rates = NULL;
    uint64_t total;
    uint64_t signed_chains = 0u;
    uint64_t graph_chains = 0u;
    uint64_t extended_chains = 0u;
    bool ok = false;

    if (requested > UINT64_MAX - (uint64_t)(length - 1u) ||
        !prepare_product_entry(length, program, &shape)) {
        fprintf(stderr, "jitbench: SoC entry shape failed at length %u\n",
                length);
        return false;
    }
    total = ((requested + length - 1u) / length) * length;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    signed_rates = (double *)calloc(reps, sizeof *signed_rates);
    graph_rates = (double *)calloc(reps, sizeof *graph_rates);
    extended_rates = (double *)calloc(reps, sizeof *extended_rates);
    if (!reference_rates || !signed_rates || !graph_rates ||
        !extended_rates) {
        fprintf(stderr, "jitbench: SoC entry out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t signed_result = {0};
        soc_run_result_t graph_result = {0};
        soc_run_result_t extended_result = {0};
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "reference-signed-graph16-graph256";
            ran = run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_REFERENCE,
                                     0u,
                                     &reference) &&
                  run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_SIGNED, 0u,
                                     &signed_result) &&
                  run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_GRAPH, 0u,
                                     &graph_result) &&
                  run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED,
                                     0u,
                                     &extended_result);
        } else if (rep % 3u == 1u) {
            order = "signed-graph16-graph256-reference";
            ran = run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_SIGNED, 0u,
                                     &signed_result) &&
                  run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_GRAPH, 0u,
                                     &graph_result) &&
                  run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED,
                                     0u,
                                     &extended_result) &&
                  run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_REFERENCE, 0u,
                                     &reference);
        } else {
            order = "graph16-graph256-reference-signed";
            ran = run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_GRAPH, 0u,
                                     &graph_result) &&
                  run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED,
                                     0u,
                                     &extended_result) &&
                  run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_REFERENCE, 0u,
                                     &reference) &&
                  run_soc_entry_path(program, length, total,
                                     SOC_ENTRY_SIGNED, 0u,
                                     &signed_result);
        }
        if (!ran || !reference.snapshot || !signed_result.snapshot ||
            !graph_result.snapshot || !extended_result.snapshot ||
            reference.snapshot_len != signed_result.snapshot_len ||
            reference.snapshot_len != graph_result.snapshot_len ||
            reference.snapshot_len != extended_result.snapshot_len ||
            memcmp(reference.snapshot, signed_result.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, graph_result.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, extended_result.snapshot,
                   reference.snapshot_len) != 0 ||
            signed_result.signed_retired != total ||
            graph_result.signed_retired != total ||
            extended_result.signed_retired != total ||
            signed_result.dwrite_hits != 0u ||
            signed_result.dwrite_misses != 0u ||
            graph_result.dwrite_hits != 0u ||
            graph_result.dwrite_misses != 0u ||
            extended_result.dwrite_hits != 0u ||
            extended_result.dwrite_misses != 0u) {
            fprintf(stderr,
                    "jitbench: SoC entry length %u repetition %u failed "
                    "exact machine equality\n",
                    length, rep + 1u);
            free_soc_run_result(&reference);
            free_soc_run_result(&signed_result);
            free_soc_run_result(&graph_result);
            free_soc_run_result(&extended_result);
            goto done;
        }
        if (rep == 0u) {
            signed_chains = signed_result.signed_chains;
            graph_chains = graph_result.graph_chains;
            extended_chains = extended_result.graph_chains;
        } else if (signed_chains != signed_result.signed_chains ||
                   graph_chains != graph_result.graph_chains ||
                   extended_chains != extended_result.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC entry length %u chain counts changed "
                    "across repetitions\n", length);
            free_soc_run_result(&reference);
            free_soc_run_result(&signed_result);
            free_soc_run_result(&graph_result);
            free_soc_run_result(&extended_result);
            goto done;
        }
        /* A fixed graph node cannot manufacture the callback path's decoded
         * bounded prefix when the timer/caller remainder is shorter than its
         * cached block. Stopping there is exact, but legitimately lowers the
         * graph chain count. Preserve all stable counters as performance
         * diagnostics; full four-way snapshot equality remains the gate. */
        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        signed_rates[rep] = (double)total / signed_result.seconds / 1.0e6;
        graph_rates[rep] = (double)total / graph_result.seconds / 1.0e6;
        extended_rates[rep] =
            (double)total / extended_result.seconds / 1.0e6;
        printf("SOC-ENTRY-SAMPLE length=%u rep=%u order=%s "
               "reference=%.3f signed=%.3f graph16=%.3f graph256=%.3f "
               "Minsn/s signed-chains=%" PRIu64
               " graph-chains=%" PRIu64 " extended-graph-chains=%" PRIu64
               " exact-snapshot=yes\n",
               length, rep + 1u, order, reference_rates[rep],
               signed_rates[rep], graph_rates[rep], extended_rates[rep],
               signed_result.signed_chains,
               graph_result.graph_chains, extended_result.graph_chains);
        free_soc_run_result(&reference);
        free_soc_run_result(&signed_result);
        free_soc_run_result(&graph_result);
        free_soc_run_result(&extended_result);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(signed_rates, reps, sizeof *signed_rates, cmp_double);
    qsort(graph_rates, reps, sizeof *graph_rates, cmp_double);
    qsort(extended_rates, reps, sizeof *extended_rates, cmp_double);
    printf("SOC-ENTRY-CURVE length=%u uops=%u guest-insns=%" PRIu64
           " reps=%u chain-limit=16 extended-limit=256 run-api=yes "
           "cache-lookup=yes block-witness=yes "
           "entry-gates=yes timer-boundaries=yes device-tick=yes "
           "head-cache=warm mmu=off exact-snapshot=yes signed-retired=%" PRIu64
           " signed-chains=%" PRIu64 " graph-chains=%" PRIu64
           " extended-graph-chains=%" PRIu64
           " reference-median=%.3f signed-median=%.3f "
           "graph-median=%.3f extended-graph-median=%.3f speedup=%.3fx "
           "graph-speedup=%.3fx extended-graph-speedup=%.3fx "
           "graph-over-signed=%.3fx extended-over-graph=%.3fx\n",
           length, shape.uop_count, total, reps, total, signed_chains,
           graph_chains, extended_chains,
           reference_rates[reps / 2u], signed_rates[reps / 2u],
           graph_rates[reps / 2u], extended_rates[reps / 2u],
           signed_rates[reps / 2u] / reference_rates[reps / 2u],
           graph_rates[reps / 2u] / reference_rates[reps / 2u],
           extended_rates[reps / 2u] / reference_rates[reps / 2u],
           graph_rates[reps / 2u] / signed_rates[reps / 2u],
           extended_rates[reps / 2u] / graph_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(signed_rates);
    free(graph_rates);
    free(extended_rates);
    return ok;
}

/* Measure the first signed-store tranche at the actual SoC entry point. The
 * graph-off and graph-on arms are the same binary and differ only in the
 * machine's explicit direct-RAM-write consent. Four terminal stores per loop
 * make the expected signed-retirement gap exact and independently auditable.
 * This deliberately store-heavy loop is an architectural A/B, not a claim
 * about the restored firmware's instruction mix or physical-device FPS. */
static bool bench_soc_store(uint64_t requested, unsigned reps) {
    const unsigned length =
        (unsigned)(sizeof A32_SOC_STORES / sizeof A32_SOC_STORES[0]);
    const uint64_t stores_per_loop = 4u;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *on_rates = NULL;
    uint64_t total;
    uint64_t expected_stores;
    uint64_t expected_off_retired;
    uint64_t off_chains = 0u;
    uint64_t on_chains = 0u;
    bool ok = false;

    if (length != 16u ||
        requested > UINT64_MAX - (uint64_t)(length - 1u)) {
        fprintf(stderr, "jitbench: SoC store shape failed\n");
        return false;
    }
    total = ((requested + length - 1u) / length) * length;
    expected_stores = (total / length) * stores_per_loop;
    expected_off_retired = total - expected_stores;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    if (!reference_rates || !off_rates || !on_rates) {
        fprintf(stderr, "jitbench: SoC store out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "reference-off-on";
            ran = run_soc_entry_path(A32_SOC_STORES, length, total,
                                     SOC_ENTRY_REFERENCE, DATA_BASE,
                                     &reference) &&
                  run_soc_entry_path(A32_SOC_STORES, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED, DATA_BASE,
                                     &off) &&
                  run_soc_entry_path(A32_SOC_STORES, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED_WRITES,
                                     DATA_BASE, &on);
        } else if (rep % 3u == 1u) {
            order = "off-on-reference";
            ran = run_soc_entry_path(A32_SOC_STORES, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED, DATA_BASE,
                                     &off) &&
                  run_soc_entry_path(A32_SOC_STORES, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED_WRITES,
                                     DATA_BASE, &on) &&
                  run_soc_entry_path(A32_SOC_STORES, length, total,
                                     SOC_ENTRY_REFERENCE, DATA_BASE,
                                     &reference);
        } else {
            order = "on-reference-off";
            ran = run_soc_entry_path(A32_SOC_STORES, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED_WRITES,
                                     DATA_BASE, &on) &&
                  run_soc_entry_path(A32_SOC_STORES, length, total,
                                     SOC_ENTRY_REFERENCE, DATA_BASE,
                                     &reference) &&
                  run_soc_entry_path(A32_SOC_STORES, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED, DATA_BASE,
                                     &off);
        }
        if (!ran || !reference.snapshot || !off.snapshot || !on.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != on.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, on.snapshot,
                   reference.snapshot_len) != 0 ||
            off.signed_retired != expected_off_retired ||
            off.dwrite_hits != 0u || off.dwrite_misses != 0u ||
            on.signed_retired != total ||
            on.dwrite_hits != expected_stores ||
            on.dwrite_misses != 0u) {
            fprintf(stderr,
                    "jitbench: SoC store repetition %u failed exact A/B "
                    "off-retired=%" PRIu64 " on-retired=%" PRIu64
                    " hits=%" PRIu64 " misses=%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    on.dwrite_hits, on.dwrite_misses);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            off_chains = off.graph_chains;
            on_chains = on.graph_chains;
        } else if (off_chains != off.graph_chains ||
                   on_chains != on.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC store chain counts changed across "
                    "repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        printf("SOC-STORE-SAMPLE rep=%u order=%s reference=%.3f "
               "graph-off=%.3f graph-on=%.3f Minsn/s "
               "off-retired=%" PRIu64 " on-retired=%" PRIu64
               " dwrite-hits=%" PRIu64 " exact-snapshot=yes\n",
               rep + 1u, order, reference_rates[rep], off_rates[rep],
               on_rates[rep], off.signed_retired, on.signed_retired,
               on.dwrite_hits);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    printf("SOC-STORE-CURVE length=%u stores=%" PRIu64
           " guest-insns=%" PRIu64 " reps=%u chain-limit=256 "
           "run-api=yes cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm mmu=off "
           "exact-snapshot=yes off-signed-retired=%" PRIu64
           " on-signed-retired=%" PRIu64 " dwrite-hits=%" PRIu64
           " dwrite-misses=0 off-graph-chains=%" PRIu64
           " on-graph-chains=%" PRIu64
           " reference-median=%.3f off-median=%.3f on-median=%.3f "
           "off-speedup=%.3fx on-speedup=%.3fx on-over-off=%.3fx\n",
           length, stores_per_loop, total, reps, expected_off_retired,
           total, expected_stores, off_chains, on_chains,
           reference_rates[reps / 2u], off_rates[reps / 2u],
           on_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(on_rates);
    return ok;
}

/* Measure only the redundant zero-return entry after a positive graph call
 * reaches an unchanged cached-negative instruction. The off/on machines use
 * identical signed handlers, graph data, timer boundaries and interpreter
 * work; the switch changes only whether the known-losing second probe is made.
 * Adjacent order alternates to reduce host drift. Complete snapshots, signed
 * retirement, chain counts and the bypass counter must all agree exactly. */
static bool bench_soc_known_negative_boundary(uint64_t requested,
                                               unsigned reps) {
    const unsigned length = (unsigned)(sizeof A32_SOC_NEGATIVE_BOUNDARY /
                                       sizeof A32_SOC_NEGATIVE_BOUNDARY[0]);
    double *off_rates = NULL;
    double *on_rates = NULL;
    double *paired_ratios = NULL;
    uint64_t total;
    uint64_t loops;
    uint64_t expected_signed;
    uint64_t off_chains = 0u;
    uint64_t on_chains = 0u;
    uint64_t bypasses = 0u;
    unsigned paired_wins = 0u;
    bool ok = false;

    if (length != 10u || requested > UINT64_MAX - (uint64_t)(length - 1u)) {
        fprintf(stderr, "jitbench: known-negative boundary shape failed\n");
        return false;
    }
    total = ((requested + length - 1u) / length) * length;
    loops = total / length;
    expected_signed = total - loops;
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    paired_ratios = (double *)calloc(reps, sizeof *paired_ratios);
    if (!off_rates || !on_rates || !paired_ratios) {
        fprintf(stderr, "jitbench: known-negative boundary out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;
        if ((rep & 1u) == 0u) {
            order = "off-on";
            ran = run_soc_entry_path(
                      A32_SOC_NEGATIVE_BOUNDARY, length, total,
                      SOC_ENTRY_GRAPH_EXTENDED_KNOWN_NEGATIVE_BYPASS_OFF,
                      0u, &off) &&
                  run_soc_entry_path(A32_SOC_NEGATIVE_BOUNDARY, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED, 0u, &on);
        } else {
            order = "on-off";
            ran = run_soc_entry_path(A32_SOC_NEGATIVE_BOUNDARY, length, total,
                                     SOC_ENTRY_GRAPH_EXTENDED, 0u, &on) &&
                  run_soc_entry_path(
                      A32_SOC_NEGATIVE_BOUNDARY, length, total,
                      SOC_ENTRY_GRAPH_EXTENDED_KNOWN_NEGATIVE_BYPASS_OFF,
                      0u, &off);
        }
        if (!ran || !off.snapshot || !on.snapshot ||
            off.snapshot_len != on.snapshot_len ||
            memcmp(off.snapshot, on.snapshot, off.snapshot_len) != 0 ||
            off.signed_retired != expected_signed ||
            on.signed_retired != expected_signed ||
            off.graph_chains == 0u ||
            off.graph_chains != on.graph_chains ||
            off.known_negative_bypasses != 0u ||
            !on.known_negative_bypasses ||
            on.known_negative_bypasses > loops ||
            off.fetch_refill_attempts != 0u ||
            on.fetch_refill_attempts != 0u ||
            off.dread_hits != 0u || off.dread_misses != 0u ||
            on.dread_hits != 0u || on.dread_misses != 0u ||
            off.dwrite_hits != 0u || off.dwrite_misses != 0u ||
            on.dwrite_hits != 0u || on.dwrite_misses != 0u) {
            fprintf(stderr,
                    "jitbench: known-negative boundary repetition %u failed "
                    "exact A/B retired=%" PRIu64 "/%" PRIu64
                    " chains=%" PRIu64 "/%" PRIu64
                    " bypasses=%" PRIu64 "/%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    off.graph_chains, on.graph_chains,
                    off.known_negative_bypasses,
                    on.known_negative_bypasses);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            off_chains = off.graph_chains;
            on_chains = on.graph_chains;
            bypasses = on.known_negative_bypasses;
        } else if (off_chains != off.graph_chains ||
                   on_chains != on.graph_chains ||
                   bypasses != on.known_negative_bypasses) {
            fprintf(stderr,
                    "jitbench: known-negative boundary counters changed "
                    "across repetitions\n");
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        paired_ratios[rep] = on_rates[rep] / off_rates[rep];
        if (paired_ratios[rep] > 1.0) paired_wins++;
        printf("SOC-KNOWN-NEGATIVE-SAMPLE rep=%u order=%s off=%.3f "
               "on=%.3f Minsn/s on-over-off=%.3fx bypasses=%" PRIu64
               " exact-snapshot=yes\n",
               rep + 1u, order, off_rates[rep], on_rates[rep],
               paired_ratios[rep], on.known_negative_bypasses);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    qsort(paired_ratios, reps, sizeof *paired_ratios, cmp_double);
    printf("SOC-KNOWN-NEGATIVE-CURVE length=%u unsupported-per-loop=1 "
           "guest-insns=%" PRIu64 " reps=%u same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm mmu=off "
           "not-phone-fps=yes exact-snapshot=yes signed-retired=%" PRIu64
           " bypasses=%" PRIu64 "/%" PRIu64
           " off-graph-chains=%" PRIu64 " on-graph-chains=%" PRIu64
           " off-median=%.3f on-median=%.3f on-over-off=%.3fx "
           "paired-median=%.3fx paired-min=%.3fx paired-max=%.3fx "
           "paired-wins=%u/%u\n",
           length, total, reps, expected_signed, bypasses, loops,
           off_chains, on_chains, off_rates[reps / 2u],
           on_rates[reps / 2u], on_rates[reps / 2u] / off_rates[reps / 2u],
           paired_ratios[reps / 2u], paired_ratios[0u],
           paired_ratios[reps - 1u], paired_wins, reps);
    ok = true;

done:
    free(off_rates);
    free(on_rates);
    free(paired_ratios);
    return ok;
}

/* Isolate only the lookup-only fetch refill. Four terminal A32 branches hop
 * between distinct 1 KiB virtual blocks under one MMU section. Warmup leaves
 * all four exact FETCH translations in the software TLB. The off arm keeps
 * the complete signed engine but reproduces the former fetch-cache refusal,
 * so every branch is literal. The adaptive arm probes a call, learns that it
 * retired only one instruction, and then skips fifteen repeats before
 * re-probing. Attempts plus skips must account for every branch, while every
 * actual hit retires exactly one signed instruction. TLB hit accounting and
 * complete snapshots remain byte exact. This intentionally pathological 100%
 * boundary mix is not firmware timing or phone FPS. */
static bool bench_soc_fetch_refill(uint64_t requested, unsigned reps) {
    const uint64_t loop_insns = 4u;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *on_rates = NULL;
    uint64_t total;
    uint64_t adaptive_retired = 0u;
    uint64_t adaptive_attempts = 0u;
    uint64_t adaptive_hits = 0u;
    uint64_t adaptive_skips = 0u;
    bool ok = false;

    if (requested > UINT64_MAX - (loop_insns - 1u)) {
        fprintf(stderr, "jitbench: SoC fetch-refill shape failed\n");
        return false;
    }
    total = ((requested + loop_insns - 1u) / loop_insns) * loop_insns;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    if (!reference_rates || !off_rates || !on_rates) {
        fprintf(stderr, "jitbench: SoC fetch-refill out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "reference-off-on";
            ran = run_soc_fetch_refill_path(
                      total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_fetch_refill_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF,
                      &off) &&
                  run_soc_fetch_refill_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &on);
        } else if (rep % 3u == 1u) {
            order = "off-on-reference";
            ran = run_soc_fetch_refill_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF,
                      &off) &&
                  run_soc_fetch_refill_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &on) &&
                  run_soc_fetch_refill_path(
                      total, SOC_ENTRY_REFERENCE, &reference);
        } else {
            order = "on-reference-off";
            ran = run_soc_fetch_refill_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &on) &&
                  run_soc_fetch_refill_path(
                      total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_fetch_refill_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF,
                      &off);
        }
        if (!ran || !reference.snapshot || !off.snapshot || !on.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != on.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, on.snapshot,
                   reference.snapshot_len) != 0 ||
            reference.signed_retired != 0u || off.signed_retired != 0u ||
            on.signed_retired != on.fetch_refill_hits ||
            reference.fetch_refill_attempts != 0u ||
            reference.fetch_refill_hits != 0u ||
            reference.fetch_refill_skips != 0u ||
            off.fetch_refill_attempts != 0u || off.fetch_refill_hits != 0u ||
            off.fetch_refill_skips != 0u ||
            on.fetch_refill_attempts == 0u || on.fetch_refill_skips == 0u ||
            on.fetch_refill_attempts + on.fetch_refill_skips != total ||
            on.fetch_refill_hits != on.fetch_refill_attempts ||
            reference.dread_hits != 0u || reference.dread_misses != 0u ||
            off.dread_hits != 0u || off.dread_misses != 0u ||
            on.dread_hits != 0u || on.dread_misses != 0u ||
            reference.dwrite_hits != 0u ||
            reference.dwrite_misses != 0u || off.dwrite_hits != 0u ||
            off.dwrite_misses != 0u || on.dwrite_hits != 0u ||
            on.dwrite_misses != 0u || on.graph_chains != 0u) {
            fprintf(stderr,
                    "jitbench: SoC fetch-refill repetition %u failed exact "
                    "A/B off-retired=%" PRIu64 " on-retired=%" PRIu64
                    " attempts/hits/skips=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    on.fetch_refill_attempts, on.fetch_refill_hits,
                    on.fetch_refill_skips);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            adaptive_retired = on.signed_retired;
            adaptive_attempts = on.fetch_refill_attempts;
            adaptive_hits = on.fetch_refill_hits;
            adaptive_skips = on.fetch_refill_skips;
        } else if (adaptive_retired != on.signed_retired ||
                   adaptive_attempts != on.fetch_refill_attempts ||
                   adaptive_hits != on.fetch_refill_hits ||
                   adaptive_skips != on.fetch_refill_skips) {
            fprintf(stderr,
                    "jitbench: SoC fetch-refill adaptive accounting changed "
                    "across repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        printf("SOC-FETCH-REFILL-SAMPLE rep=%u order=%s reference=%.3f "
               "refill-off=%.3f refill-adaptive=%.3f Minsn/s "
               "off-retired=%" PRIu64 " adaptive-retired=%" PRIu64
               " adaptive-refills=%" PRIu64 " adaptive-skips=%" PRIu64
               " exact-snapshot=yes\n",
               rep + 1u, order, reference_rates[rep], off_rates[rep],
               on_rates[rep], off.signed_retired, on.signed_retired,
               on.fetch_refill_hits, on.fetch_refill_skips);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    printf("SOC-FETCH-REFILL-CURVE blocks=4 loop-insns=%" PRIu64
           " guest-insns=%" PRIu64
           " reps=%u chain-limit=256 same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm "
           "mmu=section tlb=warm exact-snapshot=yes "
           "off-signed-retired=0 adaptive-signed-retired=%" PRIu64
           " adaptive-refill-attempts=%" PRIu64
           " adaptive-refill-hits=%" PRIu64
           " adaptive-refill-skips=%" PRIu64
           " refill-accounting=%" PRIu64 "/%" PRIu64 " "
           "reference-median=%.3f off-median=%.3f adaptive-median=%.3f "
           "off-speedup=%.3fx adaptive-speedup=%.3fx "
           "adaptive-over-off=%.3fx\n",
           loop_insns, total, reps, adaptive_retired,
           adaptive_attempts, adaptive_hits, adaptive_skips,
           adaptive_attempts + adaptive_skips, total,
           reference_rates[reps / 2u], off_rates[reps / 2u],
           on_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(on_rates);
    return ok;
}

/* Exercise a controlled refill-call length without embedding firmware bytes
 * or pretending this synthetic loop is phone timing. Three of twenty
 * cross-block calls contain only their branch (15.000%, versus the observed
 * 15.342%); the other seventeen contain long_insns instructions. With refill
 * off, arm_step() owns only the first instruction in every new block and the
 * signed engine can retire the remainder. Adaptive refill learns the three
 * true single-instruction calls, periodically probes them, and keeps the
 * proven multi-instruction calls. The default ten-instruction shape matches
 * the observed 9.463-instruction multi-call mean; the break-even sweep varies
 * only that controlled length. */
static bool bench_soc_fetch_refill_mix_length(
        uint64_t requested, unsigned reps, unsigned long_insns) {
    if (long_insns < 2u || long_insns > FETCH_REFILL_MIX_MAX_LONG_INSNS) {
        fprintf(stderr, "jitbench: invalid fetch-refill mix length\n");
        return false;
    }
    const uint64_t loop_insns = FETCH_REFILL_MIX_SINGLE_BLOCKS +
        FETCH_REFILL_MIX_LONG_BLOCKS * (uint64_t)long_insns;
    const uint64_t off_signed_per_loop =
        FETCH_REFILL_MIX_LONG_BLOCKS *
        (uint64_t)(long_insns - 1u);
    const uint64_t adaptive_multi_signed_per_loop =
        FETCH_REFILL_MIX_LONG_BLOCKS * (uint64_t)long_insns;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *on_rates = NULL;
    double *paired_ratios = NULL;
    uint64_t total;
    uint64_t loops;
    uint64_t expected_off_signed;
    uint64_t expected_refills;
    uint64_t expected_multi_refills;
    uint64_t expected_multi_signed;
    uint64_t maximum_off_graph_chains;
    uint64_t maximum_adaptive_graph_chains;
    uint64_t adaptive_retired = 0u;
    uint64_t adaptive_attempts = 0u;
    uint64_t adaptive_hits = 0u;
    uint64_t adaptive_skips = 0u;
    uint64_t off_graph_chains = 0u;
    uint64_t adaptive_graph_chains = 0u;
    unsigned paired_wins = 0u;
    bool ok = false;

    if (requested > UINT64_MAX - (loop_insns - 1u)) {
        fprintf(stderr, "jitbench: SoC fetch-refill mix shape failed\n");
        return false;
    }
    total = ((requested + loop_insns - 1u) / loop_insns) * loop_insns;
    loops = total / loop_insns;
    expected_off_signed = loops * off_signed_per_loop;
    expected_refills = loops * FETCH_REFILL_MIX_BLOCKS;
    expected_multi_refills = loops * FETCH_REFILL_MIX_LONG_BLOCKS;
    expected_multi_signed = loops * adaptive_multi_signed_per_loop;
    maximum_off_graph_chains = loops * FETCH_REFILL_MIX_LONG_BLOCKS *
        (((long_insns - 1u) + A64_STATIC_MAX_INSNS - 1u) /
             A64_STATIC_MAX_INSNS -
         1u);
    maximum_adaptive_graph_chains =
        loops * FETCH_REFILL_MIX_LONG_BLOCKS *
        ((long_insns + A64_STATIC_MAX_INSNS - 1u) /
             A64_STATIC_MAX_INSNS -
         1u);
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    paired_ratios = (double *)calloc(reps, sizeof *paired_ratios);
    if (!reference_rates || !off_rates || !on_rates || !paired_ratios) {
        fprintf(stderr, "jitbench: SoC fetch-refill mix out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;
        if (rep % 3u == 0u) {
            order = "reference-off-on";
            ran = run_soc_fetch_refill_mix_length_path(
                      total, long_insns, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_fetch_refill_mix_length_path(
                      total, long_insns,
                      SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF, &off) &&
                  run_soc_fetch_refill_mix_length_path(
                      total, long_insns, SOC_ENTRY_GRAPH_EXTENDED, &on);
        } else if (rep % 3u == 1u) {
            order = "off-on-reference";
            ran = run_soc_fetch_refill_mix_length_path(
                      total, long_insns,
                      SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF, &off) &&
                  run_soc_fetch_refill_mix_length_path(
                      total, long_insns, SOC_ENTRY_GRAPH_EXTENDED, &on) &&
                  run_soc_fetch_refill_mix_length_path(
                      total, long_insns, SOC_ENTRY_REFERENCE, &reference);
        } else {
            order = "on-reference-off";
            ran = run_soc_fetch_refill_mix_length_path(
                      total, long_insns, SOC_ENTRY_GRAPH_EXTENDED, &on) &&
                  run_soc_fetch_refill_mix_length_path(
                      total, long_insns, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_fetch_refill_mix_length_path(
                      total, long_insns,
                      SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF, &off);
        }
        if (!ran || !reference.snapshot || !off.snapshot || !on.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != on.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, on.snapshot,
                   reference.snapshot_len) != 0 ||
            reference.signed_retired != 0u ||
            off.signed_retired != expected_off_signed ||
            reference.fetch_refill_attempts != 0u ||
            reference.fetch_refill_hits != 0u ||
            reference.fetch_refill_skips != 0u ||
            off.fetch_refill_attempts != 0u || off.fetch_refill_hits != 0u ||
            off.fetch_refill_skips != 0u ||
            on.fetch_refill_attempts < expected_multi_refills ||
            on.fetch_refill_attempts > expected_refills ||
            on.fetch_refill_hits != on.fetch_refill_attempts ||
            on.fetch_refill_attempts + on.fetch_refill_skips !=
                expected_refills ||
            on.signed_retired != expected_multi_signed +
                (on.fetch_refill_attempts - expected_multi_refills) ||
            reference.dread_hits != 0u || reference.dread_misses != 0u ||
            off.dread_hits != 0u || off.dread_misses != 0u ||
            on.dread_hits != 0u || on.dread_misses != 0u ||
            reference.dwrite_hits != 0u ||
            reference.dwrite_misses != 0u || off.dwrite_hits != 0u ||
            off.dwrite_misses != 0u || on.dwrite_hits != 0u ||
            on.dwrite_misses != 0u ||
            (long_insns <= A64_STATIC_MAX_INSNS
                 ? off.graph_chains != 0u || on.graph_chains != 0u
                 : off.graph_chains == 0u || on.graph_chains == 0u ||
                       off.graph_chains > maximum_off_graph_chains ||
                       on.graph_chains > maximum_adaptive_graph_chains)) {
            fprintf(stderr,
                    "jitbench: SoC fetch-refill mix repetition %u failed "
                    "exact A/B off-retired=%" PRIu64
                    " adaptive-retired=%" PRIu64
                    " attempts/hits/skips=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 " decisions=%" PRIu64
                    " graph off=%" PRIu64 "/%" PRIu64
                    " adaptive=%" PRIu64 "/%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    on.fetch_refill_attempts, on.fetch_refill_hits,
                    on.fetch_refill_skips, expected_refills,
                    off.graph_chains, maximum_off_graph_chains,
                    on.graph_chains, maximum_adaptive_graph_chains);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            adaptive_retired = on.signed_retired;
            adaptive_attempts = on.fetch_refill_attempts;
            adaptive_hits = on.fetch_refill_hits;
            adaptive_skips = on.fetch_refill_skips;
            off_graph_chains = off.graph_chains;
            adaptive_graph_chains = on.graph_chains;
        } else if (adaptive_retired != on.signed_retired ||
                   adaptive_attempts != on.fetch_refill_attempts ||
                   adaptive_hits != on.fetch_refill_hits ||
                   adaptive_skips != on.fetch_refill_skips ||
                   off_graph_chains != off.graph_chains ||
                   adaptive_graph_chains != on.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC fetch-refill mix adaptive accounting "
                    "changed across repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        paired_ratios[rep] = on_rates[rep] / off_rates[rep];
        if (paired_ratios[rep] > 1.0) paired_wins++;
        printf("SOC-FETCH-REFILL-MIX-SAMPLE long-insns=%u rep=%u order=%s "
               "reference=%.3f refill-off=%.3f refill-adaptive=%.3f "
               "Minsn/s off-retired=%" PRIu64
               " adaptive-retired=%" PRIu64
               " adaptive-refills=%" PRIu64
               " adaptive-skips=%" PRIu64
               " paired-adaptive-over-off=%.3fx "
               "exact-snapshot=yes\n",
               long_insns, rep + 1u, order, reference_rates[rep],
               off_rates[rep],
               on_rates[rep], off.signed_retired, on.signed_retired,
               on.fetch_refill_hits, on.fetch_refill_skips,
               paired_ratios[rep]);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    qsort(paired_ratios, reps, sizeof *paired_ratios, cmp_double);
    printf("SOC-FETCH-REFILL-MIX-CURVE blocks=20 single-blocks=3 "
           "long-blocks=17 long-insns=%u loop-insns=%" PRIu64
           " guest-insns=%" PRIu64
           " reps=%u single-call-share=15.000%% "
           "observed-single-share=15.342%% "
           "observed-multi-mean=9.463 same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm "
           "mmu=section tlb=warm exact-snapshot=yes "
           "off-signed-retired=%" PRIu64
           " adaptive-signed-retired=%" PRIu64
           " adaptive-refill-attempts=%" PRIu64
           " adaptive-refill-hits=%" PRIu64
           " adaptive-refill-skips=%" PRIu64
           " refill-accounting=%" PRIu64 "/%" PRIu64 " "
           "reference-median=%.3f off-median=%.3f adaptive-median=%.3f "
           "off-speedup=%.3fx adaptive-speedup=%.3fx "
           "adaptive-over-off=%.3fx "
           "paired-adaptive-over-off-median=%.3fx paired-min=%.3fx "
           "paired-max=%.3fx paired-wins=%u/%u\n",
           long_insns, loop_insns, total, reps, expected_off_signed,
           adaptive_retired,
           adaptive_attempts, adaptive_hits, adaptive_skips,
           adaptive_attempts + adaptive_skips, expected_refills,
           reference_rates[reps / 2u],
           off_rates[reps / 2u], on_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u],
           paired_ratios[reps / 2u], paired_ratios[0u],
           paired_ratios[reps - 1u], paired_wins, reps);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(on_rates);
    free(paired_ratios);
    return ok;
}

static bool bench_soc_fetch_refill_mix(uint64_t requested, unsigned reps) {
    return bench_soc_fetch_refill_mix_length(
        requested, reps, FETCH_REFILL_MIX_LONG_INSNS);
}

/* Decisive default-policy timing uses adjacent pairs, not three-arm rotation.
 * The reference path is already covered by the exact mix benchmark; inserting
 * it between adaptive and off in one third of repetitions only adds elapsed
 * drift. Alternate off->adaptive and adaptive->off, retain exact snapshots and
 * all policy accounting, and report both the ratio of medians and the median
 * of paired ratios. This remains hosted synthetic timing, not phone FPS. */
static bool bench_soc_fetch_refill_paired(uint64_t requested,
                                          unsigned reps) {
    const unsigned long_insns = FETCH_REFILL_MIX_LONG_INSNS;
    const uint64_t loop_insns = FETCH_REFILL_MIX_LOOP_INSNS;
    const uint64_t off_signed_per_loop =
        FETCH_REFILL_MIX_LONG_BLOCKS *
        (FETCH_REFILL_MIX_LONG_INSNS - 1u);
    const uint64_t adaptive_multi_signed_per_loop =
        FETCH_REFILL_MIX_LONG_BLOCKS * FETCH_REFILL_MIX_LONG_INSNS;
    double *off_rates = NULL;
    double *on_rates = NULL;
    double *paired_ratios = NULL;
    uint64_t total;
    uint64_t loops;
    uint64_t expected_off_signed;
    uint64_t expected_refills;
    uint64_t expected_multi_refills;
    uint64_t expected_multi_signed;
    uint64_t adaptive_retired = 0u;
    uint64_t adaptive_attempts = 0u;
    uint64_t adaptive_hits = 0u;
    uint64_t adaptive_skips = 0u;
    unsigned paired_wins = 0u;
    bool ok = false;

    if (requested > UINT64_MAX - (loop_insns - 1u)) {
        fprintf(stderr, "jitbench: paired fetch-refill shape failed\n");
        return false;
    }
    total = ((requested + loop_insns - 1u) / loop_insns) * loop_insns;
    loops = total / loop_insns;
    expected_off_signed = loops * off_signed_per_loop;
    expected_refills = loops * FETCH_REFILL_MIX_BLOCKS;
    expected_multi_refills = loops * FETCH_REFILL_MIX_LONG_BLOCKS;
    expected_multi_signed = loops * adaptive_multi_signed_per_loop;
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    paired_ratios = (double *)calloc(reps, sizeof *paired_ratios);
    if (!off_rates || !on_rates || !paired_ratios) {
        fprintf(stderr, "jitbench: paired fetch-refill out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;
        if ((rep & 1u) == 0u) {
            order = "off-on";
            ran = run_soc_fetch_refill_mix_length_path(
                      total, long_insns,
                      SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF, &off) &&
                  run_soc_fetch_refill_mix_length_path(
                      total, long_insns, SOC_ENTRY_GRAPH_EXTENDED, &on);
        } else {
            order = "on-off";
            ran = run_soc_fetch_refill_mix_length_path(
                      total, long_insns, SOC_ENTRY_GRAPH_EXTENDED, &on) &&
                  run_soc_fetch_refill_mix_length_path(
                      total, long_insns,
                      SOC_ENTRY_GRAPH_EXTENDED_FETCH_REFILL_OFF, &off);
        }
        if (!ran || !off.snapshot || !on.snapshot ||
            off.snapshot_len != on.snapshot_len ||
            memcmp(off.snapshot, on.snapshot, off.snapshot_len) != 0 ||
            off.signed_retired != expected_off_signed ||
            off.fetch_refill_attempts != 0u || off.fetch_refill_hits != 0u ||
            off.fetch_refill_skips != 0u ||
            on.fetch_refill_attempts < expected_multi_refills ||
            on.fetch_refill_attempts > expected_refills ||
            on.fetch_refill_hits != on.fetch_refill_attempts ||
            on.fetch_refill_attempts + on.fetch_refill_skips !=
                expected_refills ||
            on.signed_retired != expected_multi_signed +
                (on.fetch_refill_attempts - expected_multi_refills) ||
            off.dread_hits != 0u || off.dread_misses != 0u ||
            on.dread_hits != 0u || on.dread_misses != 0u ||
            off.dwrite_hits != 0u || off.dwrite_misses != 0u ||
            on.dwrite_hits != 0u || on.dwrite_misses != 0u ||
            off.graph_chains != 0u || on.graph_chains != 0u) {
            fprintf(stderr,
                    "jitbench: paired fetch-refill repetition %u failed "
                    "exact A/B off-retired=%" PRIu64
                    " adaptive-retired=%" PRIu64
                    " attempts/hits/skips=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 " decisions=%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    on.fetch_refill_attempts, on.fetch_refill_hits,
                    on.fetch_refill_skips, expected_refills);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            adaptive_retired = on.signed_retired;
            adaptive_attempts = on.fetch_refill_attempts;
            adaptive_hits = on.fetch_refill_hits;
            adaptive_skips = on.fetch_refill_skips;
        } else if (adaptive_retired != on.signed_retired ||
                   adaptive_attempts != on.fetch_refill_attempts ||
                   adaptive_hits != on.fetch_refill_hits ||
                   adaptive_skips != on.fetch_refill_skips) {
            fprintf(stderr,
                    "jitbench: paired fetch-refill accounting changed "
                    "across repetitions\n");
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        paired_ratios[rep] = on_rates[rep] / off_rates[rep];
        if (paired_ratios[rep] > 1.0) paired_wins++;
        printf("SOC-FETCH-REFILL-PAIRED-SAMPLE long-insns=%u rep=%u "
               "order=%s refill-off=%.3f refill-adaptive=%.3f Minsn/s "
               "off-retired=%" PRIu64 " adaptive-retired=%" PRIu64
               " adaptive-refills=%" PRIu64
               " adaptive-skips=%" PRIu64
               " paired-adaptive-over-off=%.3fx exact-snapshot=yes\n",
               long_insns, rep + 1u, order, off_rates[rep], on_rates[rep],
               off.signed_retired, on.signed_retired,
               on.fetch_refill_hits, on.fetch_refill_skips,
               paired_ratios[rep]);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    qsort(paired_ratios, reps, sizeof *paired_ratios, cmp_double);
    printf("SOC-FETCH-REFILL-PAIRED-CURVE blocks=20 single-blocks=3 "
           "long-blocks=17 long-insns=10 loop-insns=%" PRIu64
           " guest-insns=%" PRIu64 " reps=%u "
           "order=alternating-adjacent exact-snapshot=yes "
           "off-signed-retired=%" PRIu64
           " adaptive-signed-retired=%" PRIu64
           " adaptive-refill-attempts=%" PRIu64
           " adaptive-refill-hits=%" PRIu64
           " adaptive-refill-skips=%" PRIu64
           " refill-accounting=%" PRIu64 "/%" PRIu64 " "
           "off-median=%.3f adaptive-median=%.3f "
           "adaptive-over-off=%.3fx "
           "paired-adaptive-over-off-median=%.3fx paired-min=%.3fx "
           "paired-max=%.3fx paired-wins=%u/%u\n",
           loop_insns, total, reps, expected_off_signed,
           adaptive_retired, adaptive_attempts, adaptive_hits,
           adaptive_skips, adaptive_attempts + adaptive_skips,
           expected_refills, off_rates[reps / 2u], on_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u],
           paired_ratios[reps / 2u], paired_ratios[0u],
           paired_ratios[reps - 1u], paired_wins, reps);
    ok = true;

done:
    free(off_rates);
    free(on_rates);
    free(paired_ratios);
    return ok;
}

static bool bench_soc_fetch_refill_break_even(uint64_t requested,
                                              unsigned reps) {
    static const unsigned LENGTHS[] = {
        2u, 3u, 4u, 6u, 8u, 10u, 16u, 32u, 64u
    };
    for (size_t i = 0u; i < sizeof LENGTHS / sizeof LENGTHS[0]; i++) {
        if (!bench_soc_fetch_refill_mix_length(
                requested, reps, LENGTHS[i]))
            return false;
    }
    return true;
}

/* Isolate the register-indirect tranche with a same-binary feature A/B. Both
 * signed arms retain every older handler, graph lookup, 256-instruction bound
 * and machine gate. The off arm reproduces the old longest exact prefix and
 * therefore signs each ALU instruction but returns to arm_step() for BX/BLX;
 * the on arm keeps all four two-instruction heads in signed text. This loop is
 * intentionally 50% indirect branches and is not a firmware-mix or phone-FPS
 * claim. Exact three-way snapshots and exact retirement counts are the gate. */
static bool bench_soc_indirect(uint64_t requested, unsigned reps) {
    const uint64_t loop_insns = 8u;
    const uint64_t indirect_per_loop = 4u;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *on_rates = NULL;
    uint64_t total;
    uint64_t expected_indirect;
    uint64_t expected_off_retired;
    uint64_t off_chains = 0u;
    uint64_t on_chains = 0u;
    bool ok = false;

    if (requested > UINT64_MAX - (loop_insns - 1u)) {
        fprintf(stderr, "jitbench: SoC indirect shape failed\n");
        return false;
    }
    total = ((requested + loop_insns - 1u) / loop_insns) * loop_insns;
    expected_indirect = (total / loop_insns) * indirect_per_loop;
    expected_off_retired = total - expected_indirect;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    if (!reference_rates || !off_rates || !on_rates) {
        fprintf(stderr, "jitbench: SoC indirect out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "reference-off-on";
            ran = run_soc_indirect_path(total, SOC_ENTRY_REFERENCE,
                                        &reference) &&
                  run_soc_indirect_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_INDIRECT_OFF, &off) &&
                  run_soc_indirect_path(total, SOC_ENTRY_GRAPH_EXTENDED,
                                        &on);
        } else if (rep % 3u == 1u) {
            order = "off-on-reference";
            ran = run_soc_indirect_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_INDIRECT_OFF, &off) &&
                  run_soc_indirect_path(total, SOC_ENTRY_GRAPH_EXTENDED,
                                        &on) &&
                  run_soc_indirect_path(total, SOC_ENTRY_REFERENCE,
                                        &reference);
        } else {
            order = "on-reference-off";
            ran = run_soc_indirect_path(total, SOC_ENTRY_GRAPH_EXTENDED,
                                        &on) &&
                  run_soc_indirect_path(total, SOC_ENTRY_REFERENCE,
                                        &reference) &&
                  run_soc_indirect_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_INDIRECT_OFF, &off);
        }
        if (!ran || !reference.snapshot || !off.snapshot || !on.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != on.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, on.snapshot,
                   reference.snapshot_len) != 0 ||
            off.signed_retired != expected_off_retired ||
            on.signed_retired != total || off.graph_chains != 0u ||
            on.graph_chains == 0u || off.dwrite_hits != 0u ||
            off.dwrite_misses != 0u || on.dwrite_hits != 0u ||
            on.dwrite_misses != 0u) {
            fprintf(stderr,
                    "jitbench: SoC indirect repetition %u failed exact A/B "
                    "off-retired=%" PRIu64 " on-retired=%" PRIu64
                    " off-chains=%" PRIu64 " on-chains=%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    off.graph_chains, on.graph_chains);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            off_chains = off.graph_chains;
            on_chains = on.graph_chains;
        } else if (off_chains != off.graph_chains ||
                   on_chains != on.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC indirect chain counts changed across "
                    "repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        printf("SOC-INDIRECT-SAMPLE rep=%u order=%s reference=%.3f "
               "graph-off=%.3f graph-on=%.3f Minsn/s "
               "off-retired=%" PRIu64 " on-retired=%" PRIu64
               " on-graph-chains=%" PRIu64 " exact-snapshot=yes\n",
               rep + 1u, order, reference_rates[rep], off_rates[rep],
               on_rates[rep], off.signed_retired, on.signed_retired,
               on.graph_chains);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    printf("SOC-INDIRECT-CURVE heads=4 loop-insns=%" PRIu64
           " indirect=%" PRIu64 " guest-insns=%" PRIu64
           " reps=%u chain-limit=256 same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm mmu=off "
           "exact-snapshot=yes off-signed-retired=%" PRIu64
           " on-signed-retired=%" PRIu64 " off-graph-chains=%" PRIu64
           " on-graph-chains=%" PRIu64
           " reference-median=%.3f off-median=%.3f on-median=%.3f "
           "off-speedup=%.3fx on-speedup=%.3fx on-over-off=%.3fx\n",
           loop_insns, indirect_per_loop, total, reps,
           expected_off_retired, total, off_chains, on_chains,
           reference_rates[reps / 2u], off_rates[reps / 2u],
           on_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(on_rates);
    return ok;
}

/* Isolate the Thumb conditional-branch tranche with the same three-way exact
 * snapshot gate as the indirect benchmark. Four branch records per fixed
 * sixteen-instruction loop exercise both condition outcomes. Their taken
 * target deliberately equals fallthrough so every arm retires the same loop;
 * this measures product-path overhead, not restored-firmware mix or phone FPS. */
static bool bench_soc_thumb_conditional(uint64_t requested, unsigned reps) {
    const uint64_t loop_insns = 16u;
    const uint64_t branches_per_loop = 4u;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *on_rates = NULL;
    uint64_t total;
    uint64_t expected_branches;
    uint64_t expected_off_retired;
    uint64_t off_chains = 0u;
    uint64_t on_chains = 0u;
    bool ok = false;

    if (requested > UINT64_MAX - (loop_insns - 1u)) {
        fprintf(stderr, "jitbench: SoC Thumb conditional shape failed\n");
        return false;
    }
    total = ((requested + loop_insns - 1u) / loop_insns) * loop_insns;
    expected_branches = (total / loop_insns) * branches_per_loop;
    expected_off_retired = total - expected_branches;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    if (!reference_rates || !off_rates || !on_rates) {
        fprintf(stderr, "jitbench: SoC Thumb conditional out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "reference-off-on";
            ran = run_soc_thumb_conditional_path(
                      total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_thumb_conditional_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_THUMB_CONDITIONAL_OFF,
                      &off) &&
                  run_soc_thumb_conditional_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &on);
        } else if (rep % 3u == 1u) {
            order = "off-on-reference";
            ran = run_soc_thumb_conditional_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_THUMB_CONDITIONAL_OFF,
                      &off) &&
                  run_soc_thumb_conditional_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &on) &&
                  run_soc_thumb_conditional_path(
                      total, SOC_ENTRY_REFERENCE, &reference);
        } else {
            order = "on-reference-off";
            ran = run_soc_thumb_conditional_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &on) &&
                  run_soc_thumb_conditional_path(
                      total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_thumb_conditional_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_THUMB_CONDITIONAL_OFF,
                      &off);
        }
        if (!ran || !reference.snapshot || !off.snapshot || !on.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != on.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, on.snapshot,
                   reference.snapshot_len) != 0 ||
            off.signed_retired != expected_off_retired ||
            on.signed_retired != total || on.graph_chains == 0u ||
            reference.dwrite_hits != 0u || off.dwrite_hits != 0u ||
            on.dwrite_hits != 0u || reference.dwrite_misses != 0u ||
            off.dwrite_misses != 0u || on.dwrite_misses != 0u) {
            fprintf(stderr,
                    "jitbench: SoC Thumb conditional repetition %u failed "
                    "exact A/B off-retired=%" PRIu64
                    " on-retired=%" PRIu64 " off-chains=%" PRIu64
                    " on-chains=%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    off.graph_chains, on.graph_chains);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            off_chains = off.graph_chains;
            on_chains = on.graph_chains;
        } else if (off_chains != off.graph_chains ||
                   on_chains != on.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC Thumb conditional chain counts changed "
                    "across repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        printf("SOC-THUMB-COND-SAMPLE rep=%u order=%s reference=%.3f "
               "branch-off=%.3f branch-on=%.3f Minsn/s "
               "off-retired=%" PRIu64 " on-retired=%" PRIu64
               " on-graph-chains=%" PRIu64 " exact-snapshot=yes\n",
               rep + 1u, order, reference_rates[rep], off_rates[rep],
               on_rates[rep], off.signed_retired, on.signed_retired,
               on.graph_chains);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    printf("SOC-THUMB-COND-CURVE length=%" PRIu64
           " branches=%" PRIu64 " guest-insns=%" PRIu64
           " reps=%u chain-limit=256 same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm mmu=off "
           "taken=yes fallthrough=yes exact-snapshot=yes "
           "off-signed-retired=%" PRIu64
           " on-signed-retired=%" PRIu64
           " off-graph-chains=%" PRIu64
           " on-graph-chains=%" PRIu64
           " reference-median=%.3f off-median=%.3f on-median=%.3f "
           "off-speedup=%.3fx on-speedup=%.3fx on-over-off=%.3fx\n",
           loop_insns, branches_per_loop, total, reps,
           expected_off_retired, total, off_chains, on_chains,
           reference_rates[reps / 2u], off_rates[reps / 2u],
           on_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(on_rates);
    return ok;
}

/* Isolate the VSTR tranche with a same-binary feature A/B. Both signed arms
 * opt into direct RAM writes and retain every other handler, graph lookup,
 * 256-instruction bound and machine gate. The off arm falls back only for the
 * four VSTR instructions; the on arm keeps them in signed text. Six written
 * words per loop independently audit the two signed arms. The interpreter
 * reference deliberately has no direct-write consent and therefore no DWRITE
 * diagnostics. This 25%-VSTR loop is intentionally synthetic and is not
 * restored-firmware timing or a physical-device FPS claim. */
static bool bench_soc_vstr(uint64_t requested, unsigned reps) {
    const unsigned length =
        (unsigned)(sizeof A32_SOC_VSTR / sizeof A32_SOC_VSTR[0]);
    const uint64_t vstr_per_loop = 4u;
    const uint64_t dwrite_words_per_loop = 6u;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *on_rates = NULL;
    uint64_t total;
    uint64_t expected_vstr;
    uint64_t expected_dwrite_hits;
    uint64_t expected_off_retired;
    uint64_t off_chains = 0u;
    uint64_t on_chains = 0u;
    bool ok = false;

    if (length != 16u ||
        requested > UINT64_MAX - (uint64_t)(length - 1u)) {
        fprintf(stderr, "jitbench: SoC VSTR shape failed\n");
        return false;
    }
    total = ((requested + length - 1u) / length) * length;
    expected_vstr = (total / length) * vstr_per_loop;
    expected_dwrite_hits =
        (total / length) * dwrite_words_per_loop;
    expected_off_retired = total - expected_vstr;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    if (!reference_rates || !off_rates || !on_rates) {
        fprintf(stderr, "jitbench: SoC VSTR out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "reference-off-on";
            ran = run_soc_vstr_path(total, SOC_ENTRY_REFERENCE,
                                    &reference) &&
                  run_soc_vstr_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_VSTR_OFF, &off) &&
                  run_soc_vstr_path(total,
                                    SOC_ENTRY_GRAPH_EXTENDED_WRITES, &on);
        } else if (rep % 3u == 1u) {
            order = "off-on-reference";
            ran = run_soc_vstr_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_VSTR_OFF, &off) &&
                  run_soc_vstr_path(total,
                                    SOC_ENTRY_GRAPH_EXTENDED_WRITES, &on) &&
                  run_soc_vstr_path(total, SOC_ENTRY_REFERENCE,
                                    &reference);
        } else {
            order = "on-reference-off";
            ran = run_soc_vstr_path(total,
                                    SOC_ENTRY_GRAPH_EXTENDED_WRITES, &on) &&
                  run_soc_vstr_path(total, SOC_ENTRY_REFERENCE,
                                    &reference) &&
                  run_soc_vstr_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_VSTR_OFF, &off);
        }
        if (!ran || !reference.snapshot || !off.snapshot || !on.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != on.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, on.snapshot,
                   reference.snapshot_len) != 0 ||
            off.signed_retired != expected_off_retired ||
            on.signed_retired != total ||
            reference.dwrite_hits != 0u ||
            off.dwrite_hits != expected_dwrite_hits ||
            on.dwrite_hits != expected_dwrite_hits ||
            reference.dwrite_misses != 0u || off.dwrite_misses != 0u ||
            on.dwrite_misses != 0u || on.graph_chains == 0u) {
            fprintf(stderr,
                    "jitbench: SoC VSTR repetition %u failed exact A/B "
                    "off-retired=%" PRIu64 " on-retired=%" PRIu64
                    " reference-hits=%" PRIu64 " off-hits=%" PRIu64
                    " on-hits=%" PRIu64 " misses=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    reference.dwrite_hits, off.dwrite_hits,
                    on.dwrite_hits, reference.dwrite_misses,
                    off.dwrite_misses, on.dwrite_misses);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            off_chains = off.graph_chains;
            on_chains = on.graph_chains;
        } else if (off_chains != off.graph_chains ||
                   on_chains != on.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC VSTR chain counts changed across "
                    "repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        printf("SOC-VSTR-SAMPLE rep=%u order=%s reference=%.3f "
               "vstr-off=%.3f vstr-on=%.3f Minsn/s "
               "off-retired=%" PRIu64 " on-retired=%" PRIu64
               " dwrite-hits=%" PRIu64 " exact-snapshot=yes\n",
               rep + 1u, order, reference_rates[rep], off_rates[rep],
               on_rates[rep], off.signed_retired, on.signed_retired,
               on.dwrite_hits);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    printf("SOC-VSTR-CURVE length=%u vstr=%" PRIu64
           " dwrite-words=%" PRIu64 " guest-insns=%" PRIu64
           " reps=%u chain-limit=256 same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm mmu=off "
           "exact-snapshot=yes off-signed-retired=%" PRIu64
           " on-signed-retired=%" PRIu64 " dwrite-hits=%" PRIu64
           " dwrite-misses=0 off-graph-chains=%" PRIu64
           " on-graph-chains=%" PRIu64
           " reference-median=%.3f off-median=%.3f on-median=%.3f "
           "off-speedup=%.3fx on-speedup=%.3fx on-over-off=%.3fx\n",
           length, vstr_per_loop, dwrite_words_per_loop, total, reps,
           expected_off_retired, total, expected_dwrite_hits,
           off_chains, on_chains,
           reference_rates[reps / 2u], off_rates[reps / 2u],
           on_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(on_rates);
    return ok;
}

/* Isolate ordinary one-block STM with the same three-way snapshot gate as
 * VSTR. Both signed arms retain direct-write consent and every older feature;
 * the off arm changes only STM admission. Four block stores issue twelve
 * write32 calls per loop across IA/IB/DA/DB. This synthetic 25% STM mix is a
 * native-contract measurement, not firmware timing or phone FPS. */
static bool bench_soc_stm(uint64_t requested, unsigned reps) {
    const unsigned length =
        (unsigned)(sizeof A32_SOC_STM / sizeof A32_SOC_STM[0]);
    const uint64_t stm_per_loop = 4u;
    const uint64_t dwrite_words_per_loop = 12u;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *on_rates = NULL;
    uint64_t total;
    uint64_t expected_stm;
    uint64_t expected_dwrite_hits;
    uint64_t expected_off_retired;
    uint64_t off_chains = 0u;
    uint64_t on_chains = 0u;
    bool ok = false;

    if (length != 16u ||
        requested > UINT64_MAX - (uint64_t)(length - 1u)) {
        fprintf(stderr, "jitbench: SoC STM shape failed\n");
        return false;
    }
    total = ((requested + length - 1u) / length) * length;
    expected_stm = (total / length) * stm_per_loop;
    expected_dwrite_hits =
        (total / length) * dwrite_words_per_loop;
    expected_off_retired = total - expected_stm;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    if (!reference_rates || !off_rates || !on_rates) {
        fprintf(stderr, "jitbench: SoC STM out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "reference-off-on";
            ran = run_soc_stm_path(total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_stm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_STM_OFF, &off) &&
                  run_soc_stm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_WRITES, &on);
        } else if (rep % 3u == 1u) {
            order = "off-on-reference";
            ran = run_soc_stm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_STM_OFF, &off) &&
                  run_soc_stm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_WRITES, &on) &&
                  run_soc_stm_path(total, SOC_ENTRY_REFERENCE, &reference);
        } else {
            order = "on-reference-off";
            ran = run_soc_stm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_WRITES, &on) &&
                  run_soc_stm_path(total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_stm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_STM_OFF, &off);
        }
        if (!ran || !reference.snapshot || !off.snapshot || !on.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != on.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, on.snapshot,
                   reference.snapshot_len) != 0 ||
            off.signed_retired != expected_off_retired ||
            on.signed_retired != total ||
            reference.dwrite_hits != 0u ||
            off.dwrite_hits != expected_dwrite_hits ||
            on.dwrite_hits != expected_dwrite_hits ||
            reference.dwrite_misses != 0u || off.dwrite_misses != 0u ||
            on.dwrite_misses != 0u || on.graph_chains == 0u) {
            fprintf(stderr,
                    "jitbench: SoC STM repetition %u failed exact A/B "
                    "off-retired=%" PRIu64 " on-retired=%" PRIu64
                    " reference-hits=%" PRIu64 " off-hits=%" PRIu64
                    " on-hits=%" PRIu64 " misses=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    reference.dwrite_hits, off.dwrite_hits,
                    on.dwrite_hits, reference.dwrite_misses,
                    off.dwrite_misses, on.dwrite_misses);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            off_chains = off.graph_chains;
            on_chains = on.graph_chains;
        } else if (off_chains != off.graph_chains ||
                   on_chains != on.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC STM chain counts changed across "
                    "repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        printf("SOC-STM-SAMPLE rep=%u order=%s reference=%.3f "
               "stm-off=%.3f stm-on=%.3f Minsn/s "
               "off-retired=%" PRIu64 " on-retired=%" PRIu64
               " dwrite-hits=%" PRIu64 " exact-snapshot=yes\n",
               rep + 1u, order, reference_rates[rep], off_rates[rep],
               on_rates[rep], off.signed_retired, on.signed_retired,
               on.dwrite_hits);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    printf("SOC-STM-CURVE length=%u stm=%" PRIu64
           " dwrite-words=%" PRIu64 " guest-insns=%" PRIu64
           " reps=%u chain-limit=256 same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm mmu=off "
           "exact-snapshot=yes off-signed-retired=%" PRIu64
           " on-signed-retired=%" PRIu64 " dwrite-hits=%" PRIu64
           " dwrite-misses=0 off-graph-chains=%" PRIu64
           " on-graph-chains=%" PRIu64
           " reference-median=%.3f off-median=%.3f on-median=%.3f "
           "off-speedup=%.3fx on-speedup=%.3fx on-over-off=%.3fx\n",
           length, stm_per_loop, dwrite_words_per_loop, total, reps,
           expected_off_retired, total, expected_dwrite_hits,
           off_chains, on_chains,
           reference_rates[reps / 2u], off_rates[reps / 2u],
           on_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(on_rates);
    return ok;
}

/* Isolate ordinary aligned one-block LDM with the same three-way exact
 * snapshot gate as STM. Both signed arms retain every older capability; the
 * off arm changes only LDM admission. Four block loads issue twelve read32
 * calls per loop across IA/IB/DA/DB. This synthetic 25% LDM mix is native
 * contract evidence, not restored-firmware timing or phone FPS. */
static bool bench_soc_ldm(uint64_t requested, unsigned reps) {
    const unsigned length =
        (unsigned)(sizeof A32_SOC_LDM / sizeof A32_SOC_LDM[0]);
    const uint64_t ldm_per_loop = 4u;
    const uint64_t dread_words_per_loop = 12u;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *on_rates = NULL;
    uint64_t total;
    uint64_t expected_ldm;
    uint64_t expected_dread_hits;
    uint64_t expected_off_retired;
    uint64_t off_chains = 0u;
    uint64_t on_chains = 0u;
    bool ok = false;

    if (length != 16u ||
        requested > UINT64_MAX - (uint64_t)(length - 1u)) {
        fprintf(stderr, "jitbench: SoC LDM shape failed\n");
        return false;
    }
    total = ((requested + length - 1u) / length) * length;
    expected_ldm = (total / length) * ldm_per_loop;
    expected_dread_hits =
        (total / length) * dread_words_per_loop;
    expected_off_retired = total - expected_ldm;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    if (!reference_rates || !off_rates || !on_rates) {
        fprintf(stderr, "jitbench: SoC LDM out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "reference-off-on";
            ran = run_soc_ldm_path(total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_ldm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_LDM_OFF, &off) &&
                  run_soc_ldm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &on);
        } else if (rep % 3u == 1u) {
            order = "off-on-reference";
            ran = run_soc_ldm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_LDM_OFF, &off) &&
                  run_soc_ldm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &on) &&
                  run_soc_ldm_path(total, SOC_ENTRY_REFERENCE, &reference);
        } else {
            order = "on-reference-off";
            ran = run_soc_ldm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &on) &&
                  run_soc_ldm_path(total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_ldm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_LDM_OFF, &off);
        }
        if (!ran || !reference.snapshot || !off.snapshot || !on.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != on.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, on.snapshot,
                   reference.snapshot_len) != 0 ||
            off.signed_retired != expected_off_retired ||
            on.signed_retired != total ||
            reference.dread_hits != expected_dread_hits ||
            off.dread_hits != expected_dread_hits ||
            on.dread_hits != expected_dread_hits ||
            reference.dread_misses != 0u || off.dread_misses != 0u ||
            on.dread_misses != 0u || on.graph_chains == 0u) {
            fprintf(stderr,
                    "jitbench: SoC LDM repetition %u failed exact A/B "
                    "off-retired=%" PRIu64 " on-retired=%" PRIu64
                    " hits=%" PRIu64 "/%" PRIu64 "/%" PRIu64
                    " misses=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    reference.dread_hits, off.dread_hits, on.dread_hits,
                    reference.dread_misses, off.dread_misses,
                    on.dread_misses);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            off_chains = off.graph_chains;
            on_chains = on.graph_chains;
        } else if (off_chains != off.graph_chains ||
                   on_chains != on.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC LDM chain counts changed across "
                    "repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        printf("SOC-LDM-SAMPLE rep=%u order=%s reference=%.3f "
               "ldm-off=%.3f ldm-on=%.3f Minsn/s "
               "off-retired=%" PRIu64 " on-retired=%" PRIu64
               " dread-hits=%" PRIu64 " exact-snapshot=yes\n",
               rep + 1u, order, reference_rates[rep], off_rates[rep],
               on_rates[rep], off.signed_retired, on.signed_retired,
               on.dread_hits);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    printf("SOC-LDM-CURVE length=%u ldm=%" PRIu64
           " dread-words=%" PRIu64 " guest-insns=%" PRIu64
           " reps=%u chain-limit=256 same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm mmu=off "
           "exact-snapshot=yes off-signed-retired=%" PRIu64
           " on-signed-retired=%" PRIu64 " dread-hits=%" PRIu64
           " dread-misses=0 off-graph-chains=%" PRIu64
           " on-graph-chains=%" PRIu64
           " reference-median=%.3f off-median=%.3f on-median=%.3f "
           "off-speedup=%.3fx on-speedup=%.3fx on-over-off=%.3fx\n",
           length, ldm_per_loop, dread_words_per_loop, total, reps,
           expected_off_retired, total, expected_dread_hits,
           off_chains, on_chains,
           reference_rates[reps / 2u], off_rates[reps / 2u],
           on_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(on_rates);
    return ok;
}

/* Isolate transactional one-block VSTM with the same three-way exact snapshot
 * gate as VSTR and STM. Both signed arms retain every older capability and
 * direct-write consent; the off arm changes only VSTM admission. Four VSTM
 * instructions issue sixteen write32 calls per loop across all three valid
 * address/writeback modes. This is a native-contract measurement, not restored
 * firmware timing and not physical-device FPS. */
static bool bench_soc_vstm(uint64_t requested, unsigned reps) {
    const unsigned length =
        (unsigned)(sizeof A32_SOC_VSTM / sizeof A32_SOC_VSTM[0]);
    const uint64_t vstm_per_loop = 4u;
    const uint64_t dwrite_words_per_loop = 16u;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *on_rates = NULL;
    uint64_t total;
    uint64_t expected_vstm;
    uint64_t expected_dwrite_hits;
    uint64_t expected_off_retired;
    uint64_t off_chains = 0u;
    uint64_t on_chains = 0u;
    bool ok = false;

    if (length != 16u ||
        requested > UINT64_MAX - (uint64_t)(length - 1u)) {
        fprintf(stderr, "jitbench: SoC VSTM shape failed\n");
        return false;
    }
    total = ((requested + length - 1u) / length) * length;
    expected_vstm = (total / length) * vstm_per_loop;
    expected_dwrite_hits =
        (total / length) * dwrite_words_per_loop;
    expected_off_retired = total - expected_vstm;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    on_rates = (double *)calloc(reps, sizeof *on_rates);
    if (!reference_rates || !off_rates || !on_rates) {
        fprintf(stderr, "jitbench: SoC VSTM out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t on = {0};
        const char *order;
        bool ran;

        if (rep % 3u == 0u) {
            order = "reference-off-on";
            ran = run_soc_vstm_path(total, SOC_ENTRY_REFERENCE,
                                     &reference) &&
                  run_soc_vstm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_VSTM_OFF, &off) &&
                  run_soc_vstm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_WRITES, &on);
        } else if (rep % 3u == 1u) {
            order = "off-on-reference";
            ran = run_soc_vstm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_VSTM_OFF, &off) &&
                  run_soc_vstm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_WRITES, &on) &&
                  run_soc_vstm_path(total, SOC_ENTRY_REFERENCE, &reference);
        } else {
            order = "on-reference-off";
            ran = run_soc_vstm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_WRITES, &on) &&
                  run_soc_vstm_path(total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_vstm_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED_VSTM_OFF, &off);
        }
        if (!ran || !reference.snapshot || !off.snapshot || !on.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != on.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, on.snapshot,
                   reference.snapshot_len) != 0 ||
            off.signed_retired != expected_off_retired ||
            on.signed_retired != total ||
            reference.dwrite_hits != 0u ||
            off.dwrite_hits != expected_dwrite_hits ||
            on.dwrite_hits != expected_dwrite_hits ||
            reference.dwrite_misses != 0u || off.dwrite_misses != 0u ||
            on.dwrite_misses != 0u || on.graph_chains == 0u) {
            fprintf(stderr,
                    "jitbench: SoC VSTM repetition %u failed exact A/B "
                    "off-retired=%" PRIu64 " on-retired=%" PRIu64
                    " reference-hits=%" PRIu64 " off-hits=%" PRIu64
                    " on-hits=%" PRIu64 " misses=%" PRIu64 "/%" PRIu64
                    "/%" PRIu64 "\n",
                    rep + 1u, off.signed_retired, on.signed_retired,
                    reference.dwrite_hits, off.dwrite_hits,
                    on.dwrite_hits, reference.dwrite_misses,
                    off.dwrite_misses, on.dwrite_misses);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }
        if (rep == 0u) {
            off_chains = off.graph_chains;
            on_chains = on.graph_chains;
        } else if (off_chains != off.graph_chains ||
                   on_chains != on.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC VSTM chain counts changed across "
                    "repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&on);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        on_rates[rep] = (double)total / on.seconds / 1.0e6;
        printf("SOC-VSTM-SAMPLE rep=%u order=%s reference=%.3f "
               "vstm-off=%.3f vstm-on=%.3f Minsn/s "
               "off-retired=%" PRIu64 " on-retired=%" PRIu64
               " dwrite-hits=%" PRIu64 " exact-snapshot=yes\n",
               rep + 1u, order, reference_rates[rep], off_rates[rep],
               on_rates[rep], off.signed_retired, on.signed_retired,
               on.dwrite_hits);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&on);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(on_rates, reps, sizeof *on_rates, cmp_double);
    printf("SOC-VSTM-CURVE length=%u vstm=%" PRIu64
           " dwrite-words=%" PRIu64 " guest-insns=%" PRIu64
           " reps=%u chain-limit=256 same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm mmu=off "
           "exact-snapshot=yes off-signed-retired=%" PRIu64
           " on-signed-retired=%" PRIu64 " dwrite-hits=%" PRIu64
           " dwrite-misses=0 off-graph-chains=%" PRIu64
           " on-graph-chains=%" PRIu64
           " reference-median=%.3f off-median=%.3f on-median=%.3f "
           "off-speedup=%.3fx on-speedup=%.3fx on-over-off=%.3fx\n",
           length, vstm_per_loop, dwrite_words_per_loop, total, reps,
           expected_off_retired, total, expected_dwrite_hits,
           off_chains, on_chains,
           reference_rates[reps / 2u], off_rates[reps / 2u],
           on_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / reference_rates[reps / 2u],
           on_rates[reps / 2u] / off_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(on_rates);
    return ok;
}

/* Isolate the guarded arithmetic tranche with a four-way exact snapshot gate.
 * All arms are the same binary. The unbatched and batched arms retire the same
 * signed records and differ only in host FP-state preservation cadence. This
 * 93.75% VFP loop is a handler-overhead measurement, not a firmware mix or
 * phone-FPS result. */
static bool bench_soc_vfp_arithmetic(uint64_t requested, unsigned reps) {
    const unsigned length =
        (unsigned)(sizeof A32_SOC_VFP_ARITH /
                   sizeof A32_SOC_VFP_ARITH[0]);
    const uint64_t arithmetic_per_loop = 15u;
    double *reference_rates = NULL;
    double *off_rates = NULL;
    double *unbatched_rates = NULL;
    double *batched_rates = NULL;
    uint64_t total;
    uint64_t expected_arithmetic;
    uint64_t expected_off_retired;
    uint64_t off_chains = 0u;
    uint64_t unbatched_chains = 0u;
    uint64_t batched_chains = 0u;
    bool ok = false;

    if (length != 16u ||
        requested > UINT64_MAX - (uint64_t)(length - 1u)) {
        fprintf(stderr, "jitbench: SoC VFP arithmetic shape failed\n");
        return false;
    }
    total = ((requested + length - 1u) / length) * length;
    expected_arithmetic = (total / length) * arithmetic_per_loop;
    expected_off_retired = total - expected_arithmetic;
    reference_rates = (double *)calloc(reps, sizeof *reference_rates);
    off_rates = (double *)calloc(reps, sizeof *off_rates);
    unbatched_rates = (double *)calloc(reps, sizeof *unbatched_rates);
    batched_rates = (double *)calloc(reps, sizeof *batched_rates);
    if (!reference_rates || !off_rates || !unbatched_rates ||
        !batched_rates) {
        fprintf(stderr, "jitbench: SoC VFP arithmetic out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t off = {0};
        soc_run_result_t unbatched = {0};
        soc_run_result_t batched = {0};
        const char *order;
        bool ran;

        if (rep % 4u == 0u) {
            order = "reference-off-unbatched-batched";
            ran = run_soc_vfp_arithmetic_path(
                      total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_vfp_arithmetic_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_OFF, &off) &&
                  run_soc_vfp_arithmetic_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_UNBATCHED,
                      &unbatched) &&
                  run_soc_vfp_arithmetic_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &batched);
        } else if (rep % 4u == 1u) {
            order = "off-reference-batched-unbatched";
            ran = run_soc_vfp_arithmetic_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_OFF, &off) &&
                  run_soc_vfp_arithmetic_path(
                      total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_vfp_arithmetic_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &batched) &&
                  run_soc_vfp_arithmetic_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_UNBATCHED,
                      &unbatched);
        } else if (rep % 4u == 2u) {
            order = "unbatched-batched-reference-off";
            ran = run_soc_vfp_arithmetic_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_UNBATCHED,
                      &unbatched) &&
                  run_soc_vfp_arithmetic_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &batched) &&
                  run_soc_vfp_arithmetic_path(
                      total, SOC_ENTRY_REFERENCE, &reference) &&
                  run_soc_vfp_arithmetic_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_OFF, &off);
        } else {
            order = "batched-unbatched-off-reference";
            ran = run_soc_vfp_arithmetic_path(
                      total, SOC_ENTRY_GRAPH_EXTENDED, &batched) &&
                  run_soc_vfp_arithmetic_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_UNBATCHED,
                      &unbatched) &&
                  run_soc_vfp_arithmetic_path(
                      total,
                      SOC_ENTRY_GRAPH_EXTENDED_VFP_ARITHMETIC_OFF, &off) &&
                  run_soc_vfp_arithmetic_path(
                      total, SOC_ENTRY_REFERENCE, &reference);
        }
        if (!ran || !reference.snapshot || !off.snapshot ||
            !unbatched.snapshot || !batched.snapshot ||
            reference.snapshot_len != off.snapshot_len ||
            reference.snapshot_len != unbatched.snapshot_len ||
            reference.snapshot_len != batched.snapshot_len ||
            memcmp(reference.snapshot, off.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, unbatched.snapshot,
                   reference.snapshot_len) != 0 ||
            memcmp(reference.snapshot, batched.snapshot,
                   reference.snapshot_len) != 0 ||
            off.signed_retired != expected_off_retired ||
            unbatched.signed_retired != total ||
            batched.signed_retired != total ||
            reference.dread_hits != 0u || off.dread_hits != 0u ||
            unbatched.dread_hits != 0u || batched.dread_hits != 0u ||
            reference.dread_misses != 0u || off.dread_misses != 0u ||
            unbatched.dread_misses != 0u || batched.dread_misses != 0u ||
            reference.dwrite_hits != 0u || off.dwrite_hits != 0u ||
            unbatched.dwrite_hits != 0u || batched.dwrite_hits != 0u ||
            reference.dwrite_misses != 0u || off.dwrite_misses != 0u ||
            unbatched.dwrite_misses != 0u ||
            batched.dwrite_misses != 0u ||
            unbatched.graph_chains == 0u ||
            unbatched.graph_chains != batched.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC VFP arithmetic repetition %u failed "
                    "exact A/B off-retired=%" PRIu64
                    " unbatched-retired=%" PRIu64
                    " batched-retired=%" PRIu64
                    " unbatched-graph=%" PRIu64
                    " batched-graph=%" PRIu64 "\n",
                    rep + 1u, off.signed_retired,
                    unbatched.signed_retired, batched.signed_retired,
                    unbatched.graph_chains, batched.graph_chains);
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&unbatched);
            free_soc_run_result(&batched);
            goto done;
        }
        if (rep == 0u) {
            off_chains = off.graph_chains;
            unbatched_chains = unbatched.graph_chains;
            batched_chains = batched.graph_chains;
        } else if (off_chains != off.graph_chains ||
                   unbatched_chains != unbatched.graph_chains ||
                   batched_chains != batched.graph_chains) {
            fprintf(stderr,
                    "jitbench: SoC VFP arithmetic chain counts changed "
                    "across repetitions\n");
            free_soc_run_result(&reference);
            free_soc_run_result(&off);
            free_soc_run_result(&unbatched);
            free_soc_run_result(&batched);
            goto done;
        }

        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        off_rates[rep] = (double)total / off.seconds / 1.0e6;
        unbatched_rates[rep] =
            (double)total / unbatched.seconds / 1.0e6;
        batched_rates[rep] = (double)total / batched.seconds / 1.0e6;
        printf("SOC-VFP-ARITH-SAMPLE rep=%u order=%s reference=%.3f "
               "arithmetic-off=%.3f arithmetic-unbatched=%.3f "
               "arithmetic-batched=%.3f Minsn/s "
               "off-retired=%" PRIu64
               " unbatched-retired=%" PRIu64
               " batched-retired=%" PRIu64
               " exact-snapshot=yes\n",
               rep + 1u, order, reference_rates[rep], off_rates[rep],
               unbatched_rates[rep], batched_rates[rep], off.signed_retired,
               unbatched.signed_retired, batched.signed_retired);
        free_soc_run_result(&reference);
        free_soc_run_result(&off);
        free_soc_run_result(&unbatched);
        free_soc_run_result(&batched);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(off_rates, reps, sizeof *off_rates, cmp_double);
    qsort(unbatched_rates, reps, sizeof *unbatched_rates, cmp_double);
    qsort(batched_rates, reps, sizeof *batched_rates, cmp_double);
    printf("SOC-VFP-ARITH-CURVE length=%u arithmetic=%" PRIu64
           " guest-insns=%" PRIu64
           " reps=%u chain-limit=256 same-binary=yes run-api=yes "
           "cache-lookup=yes block-witness=yes entry-gates=yes "
           "timer-boundaries=yes device-tick=yes head-cache=warm mmu=off "
           "exact-snapshot=yes off-signed-retired=%" PRIu64
           " unbatched-signed-retired=%" PRIu64
           " batched-signed-retired=%" PRIu64
           " off-graph-chains=%" PRIu64
           " unbatched-graph-chains=%" PRIu64
           " batched-graph-chains=%" PRIu64
           " reference-median=%.3f off-median=%.3f "
           "unbatched-median=%.3f batched-median=%.3f "
           "off-speedup=%.3fx unbatched-speedup=%.3fx "
           "batched-speedup=%.3fx batched-over-off=%.3fx "
           "session-speedup=%.3fx\n",
           length, arithmetic_per_loop, total, reps,
           expected_off_retired, total, total, off_chains,
           unbatched_chains, batched_chains,
           reference_rates[reps / 2u], off_rates[reps / 2u],
           unbatched_rates[reps / 2u], batched_rates[reps / 2u],
           off_rates[reps / 2u] / reference_rates[reps / 2u],
           unbatched_rates[reps / 2u] / reference_rates[reps / 2u],
           batched_rates[reps / 2u] / reference_rates[reps / 2u],
           batched_rates[reps / 2u] / off_rates[reps / 2u],
           batched_rates[reps / 2u] / unbatched_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(off_rates);
    free(unbatched_rates);
    free(batched_rates);
    return ok;
}

static bool bench_one(const bench_case_t *bc, uint64_t requested,
                      unsigned reps) {
    jit_buf_t arena;
    jit_block_t block;
    a64_static_block_t static_block;
    arm_cpu_t translate_cpu;
    uint32_t *code;
    double *interp_rates = NULL, *static_rates = NULL, *native_rates = NULL;
    uint64_t blocks = (requested + bc->insns - 1u) / bc->insns;
    uint64_t total = blocks * bc->insns;
    bool ok = false;
    unsigned rep;

    memset(&arena, 0, sizeof arena);
    memset(&block, 0, sizeof block);
    memset(&static_block, 0, sizeof static_block);
    if (!a64_static_decode(bc->program, bc->insns, bc->thumb,
                           &static_block)) {
        fprintf(stderr, "jitbench: static decode failed for %s\n", bc->name);
        return false;
    }
    if (!jit_buf_alloc(&arena, 1u << 20)) {
        fprintf(stderr, "jitbench: executable arena unavailable for %s\n", bc->name);
        return false;
    }
    code = jit_buf_take(&arena, CODE_WORDS);
    seed_cpu(&translate_cpu, bc);
    if (!code || !jit_buf_begin_write(&arena) ||
        !jit_translate(&translate_cpu, 0u, code, CODE_WORDS, &block) ||
        !jit_buf_end_write(&arena) || !jit_block_commit(&arena, &block)) {
        fprintf(stderr, "jitbench: could not translate/commit %s\n", bc->name);
        goto done;
    }
    if (block.insn_count != bc->insns || block.native_count != bc->insns ||
        block.end_reason != JIT_END_BRANCH) {
        fprintf(stderr,
                "jitbench: %s covered %u/%u instructions (native %u, end %d)\n",
                bc->name, block.insn_count, bc->insns, block.native_count,
                (int)block.end_reason);
        goto done;
    }

    interp_rates = (double *)calloc(reps, sizeof *interp_rates);
    static_rates = (double *)calloc(reps, sizeof *static_rates);
    native_rates = (double *)calloc(reps, sizeof *native_rates);
    if (!interp_rates || !static_rates || !native_rates) {
        fprintf(stderr, "jitbench: out of memory\n");
        goto done;
    }

    for (rep = 0; rep < reps; rep++) {
        final_state_t interp, statik, native;
        double interp_s = 0.0, static_s = 0.0, native_s = 0.0;
        bool ran;
        const char *order;

        /* Rotate all three arms to reduce monotonic frequency/thermal drift
         * without pretending a shared CI host is a stable lab. */
        if (rep % 3u == 0u) {
            order = "interp-static-native";
            ran = run_interpreter(bc, total, &interp, &interp_s) &&
                  run_static(bc, &static_block, blocks, &statik, &static_s) &&
                  run_native(bc, &arena, &block, blocks, &native, &native_s);
        } else if (rep % 3u == 1u) {
            order = "static-native-interp";
            ran = run_static(bc, &static_block, blocks, &statik, &static_s) &&
                  run_native(bc, &arena, &block, blocks, &native, &native_s) &&
                  run_interpreter(bc, total, &interp, &interp_s);
        } else {
            order = "native-interp-static";
            ran = run_native(bc, &arena, &block, blocks, &native, &native_s) &&
                  run_interpreter(bc, total, &interp, &interp_s) &&
                  run_static(bc, &static_block, blocks, &statik, &static_s);
        }
        if (!ran || !states_equal(&interp, &native) ||
            !states_equal(&interp, &statik)) {
            fprintf(stderr,
                    "jitbench: %s repetition %u failed state/exit equality\n",
                    bc->name, rep + 1u);
            goto done;
        }
        interp_rates[rep] = (double)total / interp_s / 1.0e6;
        static_rates[rep] = (double)total / static_s / 1.0e6;
        native_rates[rep] = (double)total / native_s / 1.0e6;
        printf("NATIVE-CEILING-SAMPLE case=%s rep=%u order=%s "
               "interpreter=%.3f static-threaded=%.3f "
               "native-block=%.3f Minsn/s\n",
               bc->name, rep + 1u, order, interp_rates[rep],
               static_rates[rep], native_rates[rep]);
    }

    qsort(interp_rates, reps, sizeof *interp_rates, cmp_double);
    qsort(static_rates, reps, sizeof *static_rates, cmp_double);
    qsort(native_rates, reps, sizeof *native_rates, cmp_double);
    printf("NATIVE-CEILING case=%s guest-insns=%" PRIu64
           " block-insns=%u reps=%u interpreter-median=%.3f "
           "static-threaded-median=%.3f static-speedup=%.3fx "
           "native-block-median=%.3f native-speedup=%.3fx\n",
           bc->name, total, bc->insns, reps,
           interp_rates[reps / 2u], static_rates[reps / 2u],
           static_rates[reps / 2u] / interp_rates[reps / 2u],
           native_rates[reps / 2u],
           native_rates[reps / 2u] / interp_rates[reps / 2u]);
    ok = true;

done:
    free(interp_rates);
    free(static_rates);
    free(native_rates);
    if (!jit_buf_free(&arena)) {
        fprintf(stderr, "jitbench: could not release executable arena\n");
        ok = false;
    }
    return ok;
}

static bool parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;
    if (!s || !*s) return false;
    value = strtoull(s, &end, 10);
    if (!end || *end || value == 0u) return false;
    *out = (uint64_t)value;
    return true;
}

static bool parse_unsigned(const char *s, unsigned *out) {
    uint64_t value;
    if (!parse_u64(s, &value) || value > 99u) return false;
    *out = (unsigned)value;
    return true;
}

static bool validate_case_translation(const bench_case_t *bc) {
    arm_cpu_t cpu;
    jit_block_t block;
    a64_static_block_t static_block;
    uint32_t code[CODE_WORDS];

    seed_cpu(&cpu, bc);
    memset(&block, 0, sizeof block);
    memset(&static_block, 0, sizeof static_block);
    if (!jit_translate(&cpu, 0u, code, CODE_WORDS, &block) ||
        block.insn_count != bc->insns || block.native_count != bc->insns ||
        block.end_reason != JIT_END_BRANCH) {
        fprintf(stderr,
                "jitbench: structural translation failed for %s (%u/%u, "
                "native %u, end %d)\n",
                bc->name, block.insn_count, bc->insns, block.native_count,
                (int)block.end_reason);
        return false;
    }
    if (!a64_static_decode(bc->program, bc->insns, bc->thumb,
                           &static_block) ||
        static_block.insn_count != bc->insns ||
        static_block.uop_count != bc->insns ||
        static_block.start_pc != 0u || static_block.exit_pc != 0u ||
        static_block.touches_memory != bc->touches_memory) {
        fprintf(stderr, "jitbench: structural static decode failed for %s\n",
                bc->name);
        return false;
    }
    printf("NATIVE-CEILING-STRUCTURAL case=%s block-insns=%u native=%u "
           "static-uops=%u handlers=%u\n",
           bc->name, block.insn_count, block.native_count,
           static_block.uop_count, A64_STATIC_HANDLER_COUNT);
    return true;
}

int main(int argc, char **argv) {
    uint64_t insns = DEFAULT_INSNS;
    uint64_t entry_insns = 0u;
    uint64_t soc_insns = 0u;
    unsigned reps = DEFAULT_REPS;
    unsigned i;
    bool fetch_refill_mix_only = false;
    bool fetch_refill_break_even_only = false;
    bool fetch_refill_paired_only = false;
    bool known_negative_boundary_only = false;
    bool compact_raw_only = false;
    bool compact_raw_soc_only = false;

    for (i = 1u; i < (unsigned)argc; i++) {
        if (strcmp(argv[i], "--insns") == 0 && i + 1u < (unsigned)argc) {
            if (!parse_u64(argv[++i], &insns)) {
                fprintf(stderr, "jitbench: invalid --insns value\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--reps") == 0 && i + 1u < (unsigned)argc) {
            if (!parse_unsigned(argv[++i], &reps)) {
                fprintf(stderr, "jitbench: invalid --reps value\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--entry-insns") == 0 &&
                   i + 1u < (unsigned)argc) {
            if (!parse_u64(argv[++i], &entry_insns)) {
                fprintf(stderr, "jitbench: invalid --entry-insns value\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--soc-insns") == 0 &&
                   i + 1u < (unsigned)argc) {
            if (!parse_u64(argv[++i], &soc_insns)) {
                fprintf(stderr, "jitbench: invalid --soc-insns value\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--fetch-refill-mix-only") == 0) {
            fetch_refill_mix_only = true;
        } else if (strcmp(argv[i], "--fetch-refill-break-even-only") == 0) {
            fetch_refill_break_even_only = true;
        } else if (strcmp(argv[i], "--fetch-refill-paired-only") == 0) {
            fetch_refill_paired_only = true;
        } else if (strcmp(argv[i], "--known-negative-boundary-only") == 0) {
            known_negative_boundary_only = true;
        } else if (strcmp(argv[i], "--compact-raw-only") == 0) {
            compact_raw_only = true;
        } else if (strcmp(argv[i], "--compact-raw-soc-only") == 0) {
            compact_raw_soc_only = true;
        } else {
            fprintf(stderr, "usage: %s [--insns N] [--entry-insns N] "
                            "[--soc-insns N] [--reps N] "
                            "[--fetch-refill-mix-only] "
                            "[--fetch-refill-break-even-only] "
                            "[--fetch-refill-paired-only] "
                            "[--known-negative-boundary-only] "
                            "[--compact-raw-only] "
                            "[--compact-raw-soc-only]\n", argv[0]);
            return 2;
        }
    }
    if ((unsigned)fetch_refill_mix_only +
            (unsigned)fetch_refill_break_even_only +
            (unsigned)fetch_refill_paired_only +
            (unsigned)known_negative_boundary_only +
            (unsigned)compact_raw_only +
            (unsigned)compact_raw_soc_only > 1u) {
        fprintf(stderr, "jitbench: choose only one focused benchmark mode\n");
        return 2;
    }
    if (!entry_insns) entry_insns = insns;
    if (!soc_insns) soc_insns = entry_insns;

    printf("Apple-arm64 native/static-semantics ceiling benchmark\n");
    printf("NOT PHONE FPS: direct native/static rows use flat RAM and omit "
           "the machine path; the separate SoC rows remain synthetic and "
           "omit firmware/framebuffer/UI work.\n");
    printf("Product-entry rows use predecoded hot blocks and compare full "
           "wrapper validation with a cache-owned decoded contract; both "
           "still exclude SoC cache lookup and device gates.\n");
    printf("Compact-raw rows execute a bounded mixed A32/Thumb subset from "
           "live bytes in one build-time-linked semantic loop, with no "
           "decoded cache, graph or runtime code generation. They are an "
           "exact synthetic architecture gate, not a product path or "
           "phone-FPS claim.\n");
    printf("The compact-raw SoC gate first proves resident exact-interpreter "
           "fallback for MMU-on data and unsupported instructions, then "
           "measures a separate compute-only control. Both cross the real "
           "timebase-bounded run API and require byte-identical serialized "
           "machine state; neither is firmware or phone FPS.\n");
    printf("SoC-entry rows rotate reference, legacy signed, 16-instruction graph "
           "and extended graph paths across separately initialized machines. "
           "The extended arm remains clamped to the first timebase edge. They cross "
           "the real run API, cache/raw witness, gates, timer boundaries and "
           "device ticks with exact serialized-machine comparison; they still "
           "exclude real firmware, framebuffer/UI work and phone FPS.\n");
    printf("The SoC-store row is a same-binary direct-write-consent A/B with "
           "four stores per synthetic loop and the same exact snapshot gate; "
           "it is intentionally not a firmware-mix or phone-FPS claim.\n");
    printf("The SoC-indirect row is a same-binary BX/BLX capability A/B over "
           "four mixed ARM/Thumb heads. Its branch-heavy synthetic speedup is "
           "not a firmware-mix or phone-FPS claim either.\n");
    printf("The SoC-fetch-refill row is a same-binary policy A/B over four "
           "MMU-mapped 1 KiB blocks with already-warm FETCH TLB entries. Its "
           "100%% boundary mix is not firmware timing or phone FPS.\n");
    printf("The SoC-fetch-refill-mix row approximates the restored 15.342%% "
           "single-call share with three single and seventeen ten-instruction "
           "cross-block calls. It remains synthetic, not firmware timing or "
           "phone FPS.\n");
    printf("The optional SoC-fetch-refill break-even sweep holds that call "
           "share fixed and varies the multi-call length from 2 through 64. "
           "It measures a synthetic host-cost crossover, not firmware or "
           "phone FPS.\n");
    printf("The optional paired refill confirmation alternates adjacent "
           "off/on order without an intervening interpreter arm. Exact "
           "snapshots remain mandatory; it is still not phone FPS.\n");
    printf("The known-negative boundary row alternates a same-binary policy "
           "A/B around one cached interpreter-only MUL per ten-instruction "
           "loop. It measures redundant probe cost, not firmware or phone "
           "FPS.\n");
    printf("The SoC-Thumb-conditional row is a same-binary capability A/B "
           "with four terminal condition branches and both outcomes per "
           "synthetic loop. It is not firmware timing or phone FPS.\n");
    printf("The SoC-VSTR row is a same-binary VSTR capability A/B with four "
           "single/double stores per synthetic loop. Its 25%% VSTR mix is "
           "not a firmware-mix or phone-FPS claim either.\n");
    printf("The SoC-STM row is a same-binary ordinary block-store A/B with "
           "four IA/IB/DA/DB transfers and twelve words per synthetic loop. "
           "Its 25%% STM mix is not firmware timing or phone FPS.\n");
    printf("The SoC-LDM row is a same-binary ordinary block-load A/B with "
           "four IA/IB/DA/DB transfers and twelve words per synthetic loop. "
           "Its 25%% LDM mix is not firmware timing or phone FPS.\n");
    printf("The SoC-VSTM row is a separately gated same-binary A/B with four "
           "architectural IA/DB transfers and sixteen words per synthetic "
           "loop. Its 25%% VSTM mix is not firmware timing or phone FPS.\n");
    printf("The SoC-VFP-arithmetic row is a separately gated same-binary A/B "
           "with fifteen scalar operations per synthetic loop. Its 93.75%% "
           "arithmetic mix isolates handler overhead and is not firmware "
           "timing or phone FPS.\n");
    if (!validate_static_shapes()) return 1;
    for (i = 0u; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (!validate_case_translation(&CASES[i])) return 1;
    }
    if (!validate_compact_raw_admission_shapes()) return 1;
    if (!jit_host_can_execute()) {
        printf("SKIP: not an arm64 execution host.\n");
        return 0;
    }
    if (!a64_static_host_available()) {
        fprintf(stderr, "jitbench: arm64 host lacks static handler build\n");
        return 1;
    }
    if (!validate_static_read_oracles()) return 1;
    if (!validate_static_store_oracles()) return 1;
    if (!validate_static_stm_oracles()) return 1;
    if (!validate_static_ldm_oracles()) return 1;
    if (!validate_static_vfp_register_oracles()) return 1;
    if (!validate_static_vfp_arithmetic_oracles()) return 1;
    if (!validate_static_vfp_compare_oracles()) return 1;
    if (!validate_static_vfp_widen_oracles()) return 1;
    if (!validate_static_vfp_narrow_oracles()) return 1;
    if (!validate_static_vfp_read_oracles()) return 1;
    if (!validate_static_vfp_write_oracles()) return 1;
    if (!validate_static_vstm_write_oracles()) return 1;
    if (!validate_static_branch_oracles()) return 1;
    if (!validate_static_thumb_cond_branch_oracles()) return 1;
    if (!validate_static_indirect_branch_oracles()) return 1;
    if (!validate_static_oracles()) return 1;
    if (!validate_compact_raw_oracles()) return 1;

    if (fetch_refill_mix_only)
        return bench_soc_fetch_refill_mix(soc_insns, reps) ? 0 : 1;
    if (fetch_refill_break_even_only)
        return bench_soc_fetch_refill_break_even(soc_insns, reps) ? 0 : 1;
    if (fetch_refill_paired_only)
        return bench_soc_fetch_refill_paired(soc_insns, reps) ? 0 : 1;
    if (known_negative_boundary_only)
        return bench_soc_known_negative_boundary(soc_insns, reps) ? 0 : 1;
    if (compact_raw_soc_only)
        return bench_soc_compact_raw(soc_insns, reps) ? 0 : 1;
    if (compact_raw_only) {
        for (i = 0u; i < sizeof CASES / sizeof CASES[0]; i++) {
            if (!CASES[i].thumb &&
                !bench_compact_raw(&CASES[i], insns, reps))
                return 1;
        }
        return 0;
    }
    for (i = 0u; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (!bench_one(&CASES[i], insns, reps)) return 1;
    }
    for (i = 0u; i < sizeof PRODUCT_ENTRY_LENGTHS /
                         sizeof PRODUCT_ENTRY_LENGTHS[0]; i++) {
        if (!bench_product_entry(PRODUCT_ENTRY_LENGTHS[i], entry_insns,
                                 reps))
            return 1;
    }
    if (!bench_soc_compact_raw(soc_insns, reps)) return 1;
    for (i = 0u; i < sizeof SOC_ENTRY_LENGTHS /
                         sizeof SOC_ENTRY_LENGTHS[0]; i++) {
        if (!bench_soc_entry(SOC_ENTRY_LENGTHS[i], soc_insns, reps))
            return 1;
    }
    if (!bench_soc_store(soc_insns, reps)) return 1;
    if (!bench_soc_fetch_refill(soc_insns, reps)) return 1;
    if (!bench_soc_fetch_refill_mix(soc_insns, reps)) return 1;
    if (!bench_soc_known_negative_boundary(soc_insns, reps)) return 1;
    if (!bench_soc_indirect(soc_insns, reps)) return 1;
    if (!bench_soc_thumb_conditional(soc_insns, reps)) return 1;
    if (!bench_soc_vstr(soc_insns, reps)) return 1;
    if (!bench_soc_stm(soc_insns, reps)) return 1;
    if (!bench_soc_ldm(soc_insns, reps)) return 1;
    if (!bench_soc_vstm(soc_insns, reps)) return 1;
    if (!bench_soc_vfp_arithmetic(soc_insns, reps)) return 1;
    return 0;
}
