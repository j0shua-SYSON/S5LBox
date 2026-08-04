/*
 * S5LBox -- Apple-arm64 native-semantics ceiling benchmark.
 *
 * This is deliberately NOT a product-performance claim and not a JIT
 * dispatcher. It translates one small synthetic block once, then compares
 * repeated interpreter execution with both that already-built block and a
 * firmware-independent static-threaded proof. The proof's 24,050 generic
 * ISA/register handlers are compiled and signed with the executable; runtime
 * decoding creates data records only. The table now has 24,050 handlers,
 * including a product-only guarded read-cache path.
 *
 * The answer is only a feasibility bound. There is no device tick, MMIO,
 * interrupt sampling, cache lookup, translation, chaining, framebuffer
 * publication, UIKit, or real iOS instruction mix here. In particular, a
 * positive result cannot be reported as phone FPS or as proof that a complete
 * no-JIT interpreter will have the same speed. The timed rows still cover only
 * four synthetic blocks and flat power-of-two RAM. Separate exactness cases now
 * cover every A32 data-processing opcode, all conditions, immediate and
 * register barrel-shifter edge cases, r8-r14 and the architecturally valid PC
 * reads, but there is still no MMU, fault, timer, IRQ, MMIO, cache, framebuffer
 * or UI path in the ceiling. Its inner repetition also avoids real block
 * lookup. Those omissions are why it is an architecture gate, not an emulator
 * speed claim.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "jit.h"
#include "a64_static.h"

#include <inttypes.h>
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
    /* Deliberately unaligned guest byte stream: ADD r0,r0,#1. */
    static const uint8_t UNALIGNED_A32[] = {
        0xffu, 0x01u, 0x00u, 0x80u, 0xe2u,
    };
    a64_static_block_t block;
    uint8_t read_bytes[sizeof A32_READ_HITS];
    uint8_t thumb_read_bytes[sizeof THUMB_READ_HITS];
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

static int cmp_double(const void *lhs, const void *rhs) {
    double a = *(const double *)lhs, b = *(const double *)rhs;
    return (a > b) - (a < b);
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
        } else {
            fprintf(stderr, "usage: %s [--insns N] [--reps N]\n", argv[0]);
            return 2;
        }
    }

    printf("Apple-arm64 native/static-semantics ceiling benchmark\n");
    printf("NOT PHONE FPS: four synthetic timed blocks; static arm has flat "
           "RAM and no MMU/fault/tick/IRQ/MMIO/cache/framebuffer/UI "
           "path or real block lookup.\n");
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
    if (!validate_static_oracles()) return 1;

    for (i = 0u; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (!bench_one(&CASES[i], insns, reps)) return 1;
    }
    return 0;
}
