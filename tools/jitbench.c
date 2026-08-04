/*
 * S5LBox -- Apple-arm64 native-semantics ceiling benchmark.
 *
 * This is deliberately NOT a product-performance claim and not a JIT
 * dispatcher. It translates one small synthetic block once, then compares
 * repeated interpreter execution with both that already-built block and a
 * firmware-independent static-threaded proof. The proof's 24,617 generic
 * ISA/register handlers are compiled and signed with the executable; runtime
 * decoding creates data records only. The table now has 24,617 handlers,
 * including product-only guarded read-cache and exact VFP register/system
 * transfer paths.
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
#define VFP_WIDEN(dd, sm)                                                  \
    VFP_DP(1, 1, 1, 0, 7, (dd), 0, 1, 1, VFP_SB(sm), VFP_SV(sm))
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
    static const uint32_t CONDITIONAL_BRANCH[] = { 0x1a000000u };
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
    static const uint16_t INVALID_PRODUCT_THUMB[] = {
        0x4487u, /* ADD pc,r0 */
        0x4687u, /* MOV pc,r0 */
        0x4700u, /* BX r0 */
        0x6000u, /* STR r0,[r0] */
        0x8000u, /* STRH r0,[r0] */
        0x9000u, /* STR r0,[sp] */
        0xb401u, /* PUSH {r0} */
        0xd000u, /* conditional branch */
        0xdf00u, /* SVC */
        0xf000u, /* BL first half */
    };
    static const uint32_t INVALID_PRODUCT_VFP[] = {
        UINT32_C(0xee300a00), /* VADD.F32: arithmetic is a later tranche */
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
        VFP_UN_D(7, 1, 0, 1), /* narrowing VCVT still rounds */
        VFP_DP(1, 1, 1, 1, 7, 0, 0, 1, 1, 0, 0), /* widen to d16 */
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
    uint8_t vfp_read_bytes[sizeof VFP_READ_HITS];
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

    for (i = 0u; i < sizeof STATIC_CASES / sizeof STATIC_CASES[0]; i++) {
        const static_case_t *sc = &STATIC_CASES[i];
        memset(&block, 0, sizeof block);
        if (!a64_static_decode_at(sc->program, sc->insns, sc->thumb,
                                  sc->pc, &block) ||
            block.insn_count != sc->insns || block.uop_count != sc->uops ||
            block.start_pc != sc->pc || block.exit_pc != sc->exit_pc ||
            block.thumb != sc->thumb || block.touches_memory ||
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
        a64_static_decode_at(CONDITIONAL_BRANCH, 1u, false, 0u, &block) ||
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

static void oracle_warm_dread(arm_cpu_t *cpu, uint32_t va) {
    const bool priv = (cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    const uint32_t block = va & ~UINT32_C(0x3ff);
    const unsigned slot = oracle_dread_slot(va, priv);
    cpu->dread[slot].host = &g_ram[block];
    cpu->dread[slot].tag = block | (priv ? 1u : 0u);
    cpu->dread[slot].gen = cpu->tlb_gen;
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

static bool static_vfp_states_equal(const arm_cpu_t *a,
                                    const arm_cpu_t *b) {
    return memcmp(a->r, b->r, sizeof a->r) == 0 &&
           a->cpsr == b->cpsr && a->cycles == b->cycles &&
           a->vfp_fpexc == b->vfp_fpexc &&
           a->vfp_fpscr == b->vfp_fpscr &&
           memcmp(a->vfp_s, b->vfp_s, sizeof a->vfp_s) == 0 &&
           a->dread_hits == b->dread_hits &&
           a->dread_misses == b->dread_misses;
}

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
    double seconds;
} soc_run_result_t;

static void free_soc_run_result(soc_run_result_t *result) {
    if (!result) return;
    free(result->snapshot);
    memset(result, 0, sizeof *result);
}

/* Run the exact app-facing machine loop. Setup and the two-loop cache warmup
 * stay outside the timed region. The signed arm still pays the product cache
 * index, raw-byte SMC witness, dynamic gates, timer-boundary splitting and
 * device ticks. A complete machine snapshot is retained for comparison with
 * the interpreter arm; the signed cache and its counter are host diagnostics
 * and deliberately do not enter that architectural byte stream. */
static bool run_soc_entry_path(const uint32_t *program, unsigned length,
                               uint64_t total, bool signed_path,
                               soc_run_result_t *out) {
    s5l8900_t machine = {0};
    arm_status_t status = ARM_OK;
    uint64_t remaining = total;
    uint64_t retired_before;
    uint64_t retired_after;
    double start, end;
    bool initialized = false;
    bool ok = false;

    if (!program || !length || !out) return false;
    memset(out, 0, sizeof *out);
    if (!s5l8900_init(&machine, 0u, RAM_SIZE)) {
        fprintf(stderr, "jitbench: SoC entry machine init failed\n");
        goto done;
    }
    initialized = true;
    s5l8900_load(&machine, 0u, program,
                 (size_t)length * sizeof *program);

    /* Clear reset's dirty-level gate before warming either path. This uses the
     * real 412 MHz:6 MHz board clocks installed by s5l8900_init(). */
    s5l8900_tick(&machine, 0u);
    if (signed_path &&
        !s5l8900_static_a64_set_enabled(&machine, true)) {
        fprintf(stderr, "jitbench: SoC entry signed engine unavailable\n");
        goto done;
    }

    /* The first instruction establishes the fetch pointer. The remainder of
     * the first loop and the second complete loop establish the cache-owned
     * decoded entries while returning PC to zero. */
    if (s5l8900_run(&machine, length * 2u, &status) != length * 2u ||
        status != ARM_OK || machine.cpu.r[15] != 0u) {
        fprintf(stderr,
                "jitbench: SoC entry %s warmup failed status=%d pc=0x%08x\n",
                signed_path ? "signed" : "reference", (int)status,
                machine.cpu.r[15]);
        goto done;
    }

    retired_before = s5l8900_static_a64_retired(&machine);
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
    if (remaining != 0u || status != ARM_OK || end <= start ||
        machine.cpu.r[15] != 0u) {
        fprintf(stderr,
                "jitbench: SoC entry %s run failed remaining=%" PRIu64
                " status=%d pc=0x%08x\n",
                signed_path ? "signed" : "reference", remaining,
                (int)status, machine.cpu.r[15]);
        goto done;
    }
    out->signed_retired = retired_after - retired_before;
    if ((signed_path && out->signed_retired != total) ||
        (!signed_path && out->signed_retired != 0u)) {
        fprintf(stderr,
                "jitbench: SoC entry %s retired signed=%" PRIu64
                " expected=%" PRIu64 "\n",
                signed_path ? "signed" : "reference",
                out->signed_retired, signed_path ? total : UINT64_C(0));
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

/* The earlier product-entry curve intentionally stops before the SoC. This
 * curve includes that missing product machinery and compares complete machine
 * state. It is still not phone FPS: there is no MMU miss, real firmware mix,
 * framebuffer publication or UIKit in this synthetic loop. */
static bool bench_soc_entry(unsigned length, uint64_t requested,
                            unsigned reps) {
    uint32_t program[A64_STATIC_MAX_INSNS];
    a64_static_block_t shape;
    double *reference_rates = NULL;
    double *signed_rates = NULL;
    uint64_t total;
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
    if (!reference_rates || !signed_rates) {
        fprintf(stderr, "jitbench: SoC entry out of memory\n");
        goto done;
    }

    for (unsigned rep = 0u; rep < reps; rep++) {
        soc_run_result_t reference = {0};
        soc_run_result_t signed_result = {0};
        const char *order;
        bool ran;

        if ((rep & 1u) == 0u) {
            order = "reference-signed";
            ran = run_soc_entry_path(program, length, total, false,
                                     &reference) &&
                  run_soc_entry_path(program, length, total, true,
                                     &signed_result);
        } else {
            order = "signed-reference";
            ran = run_soc_entry_path(program, length, total, true,
                                     &signed_result) &&
                  run_soc_entry_path(program, length, total, false,
                                     &reference);
        }
        if (!ran || !reference.snapshot || !signed_result.snapshot ||
            reference.snapshot_len != signed_result.snapshot_len ||
            memcmp(reference.snapshot, signed_result.snapshot,
                   reference.snapshot_len) != 0) {
            fprintf(stderr,
                    "jitbench: SoC entry length %u repetition %u failed "
                    "exact machine equality\n",
                    length, rep + 1u);
            free_soc_run_result(&reference);
            free_soc_run_result(&signed_result);
            goto done;
        }
        reference_rates[rep] = (double)total / reference.seconds / 1.0e6;
        signed_rates[rep] = (double)total / signed_result.seconds / 1.0e6;
        printf("SOC-ENTRY-SAMPLE length=%u rep=%u order=%s "
               "reference=%.3f signed=%.3f Minsn/s exact-snapshot=yes\n",
               length, rep + 1u, order, reference_rates[rep],
               signed_rates[rep]);
        free_soc_run_result(&reference);
        free_soc_run_result(&signed_result);
    }

    qsort(reference_rates, reps, sizeof *reference_rates, cmp_double);
    qsort(signed_rates, reps, sizeof *signed_rates, cmp_double);
    printf("SOC-ENTRY-CURVE length=%u uops=%u guest-insns=%" PRIu64
           " reps=%u run-api=yes cache-lookup=yes raw-witness=yes "
           "entry-gates=yes timer-boundaries=yes device-tick=yes "
           "head-cache=warm mmu=off exact-snapshot=yes signed-retired=%" PRIu64
           " reference-median=%.3f signed-median=%.3f speedup=%.3fx\n",
           length, shape.uop_count, total, reps, total,
           reference_rates[reps / 2u], signed_rates[reps / 2u],
           signed_rates[reps / 2u] / reference_rates[reps / 2u]);
    ok = true;

done:
    free(reference_rates);
    free(signed_rates);
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
        } else {
            fprintf(stderr, "usage: %s [--insns N] [--entry-insns N] "
                            "[--soc-insns N] [--reps N]\n", argv[0]);
            return 2;
        }
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
    printf("SoC-entry rows cross the real run API, cache/raw witness, gates, "
           "timer boundaries and device ticks with exact serialized-machine "
           "comparison; they still exclude real firmware, framebuffer/UI "
           "work and phone FPS.\n");
    if (!validate_static_shapes()) return 1;
    for (i = 0u; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (!validate_case_translation(&CASES[i])) return 1;
    }
    if (!jit_host_can_execute()) {
        printf("SKIP: not an arm64 execution host.\n");
        return 0;
    }
    if (!a64_static_host_available()) {
        fprintf(stderr, "jitbench: arm64 host lacks static handler build\n");
        return 1;
    }
    if (!validate_static_read_oracles()) return 1;
    if (!validate_static_vfp_register_oracles()) return 1;
    if (!validate_static_vfp_compare_oracles()) return 1;
    if (!validate_static_vfp_widen_oracles()) return 1;
    if (!validate_static_vfp_read_oracles()) return 1;
    if (!validate_static_oracles()) return 1;

    for (i = 0u; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (!bench_one(&CASES[i], insns, reps)) return 1;
    }
    for (i = 0u; i < sizeof PRODUCT_ENTRY_LENGTHS /
                         sizeof PRODUCT_ENTRY_LENGTHS[0]; i++) {
        if (!bench_product_entry(PRODUCT_ENTRY_LENGTHS[i], entry_insns,
                                 reps))
            return 1;
    }
    for (i = 0u; i < sizeof SOC_ENTRY_LENGTHS /
                         sizeof SOC_ENTRY_LENGTHS[0]; i++) {
        if (!bench_soc_entry(SOC_ENTRY_LENGTHS[i], soc_insns, reps))
            return 1;
    }
    return 0;
}
