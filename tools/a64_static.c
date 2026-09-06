/* See a64_static.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "a64_static.h"
#include "arm_bulk.h"
#include "vfp.h"
#include "native_pc_histogram.h"
#include "compact_guest_pc_sample.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(S5LBOX_STATIC_A64_NATIVE) && defined(__APPLE__) && \
    (defined(__aarch64__) || defined(__arm64__))
#include <mach/arm/thread_status.h>
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#endif

enum {
    A64S_READ_WORD = 0u,
    A64S_READ_BYTE = 1u,
    A64S_READ_HALF = 2u,
    A64S_READ_SIGNED_BYTE = 3u,
    A64S_READ_SIGNED_HALF = 4u,
    A64S_READ_KIND_COUNT = 5u
};

enum {
    A64S_END = 0u,
    A64S_ADD_RRR = 1u,
    A64S_SUB_RRR = A64S_ADD_RRR + 512u,
    A64S_EOR_RRR = A64S_SUB_RRR + 512u,
    A64S_ORR_RRR = A64S_EOR_RRR + 512u,
    A64S_ADD_IMM = A64S_ORR_RRR + 512u,
    A64S_SUB_IMM = A64S_ADD_IMM + 64u,
    A64S_EOR_IMM = A64S_SUB_IMM + 64u,
    A64S_ADDS_IMM = A64S_EOR_IMM + 64u,
    A64S_SUBS_IMM = A64S_ADDS_IMM + 64u,
    A64S_EORS_RR = A64S_SUBS_IMM + 64u,
    A64S_MULS_RR = A64S_EORS_RR + 64u,
    A64S_LDR = A64S_MULS_RR + 64u,
    A64S_STR = A64S_LDR + 64u,
    A64S_LDR_SP = A64S_STR + 64u,
    A64S_STR_SP = A64S_LDR_SP + 8u,
    A64S_COND = A64S_STR_SP + 8u,
    A64S_DP_IMM = A64S_COND + 14u,
    A64S_SHIFT_IMM = A64S_DP_IMM + 16u * 2u * 15u * 16u,
    A64S_SHIFT_REG = A64S_SHIFT_IMM + 2u * 4u * 16u * 32u,
    A64S_DP_REG = A64S_SHIFT_REG + 2u * 4u * 15u * 15u,
    A64S_ADDR_IMM = A64S_DP_REG + 16u * 2u * 15u * 16u,
    A64S_ADDR_REG = A64S_ADDR_IMM + 2u * 16u,
    A64S_POST_ADDR_IMM = A64S_ADDR_REG + 2u * 16u,
    A64S_POST_ADDR_REG = A64S_POST_ADDR_IMM + 2u * 16u,
    A64S_DIRECT_READ = A64S_POST_ADDR_REG + 2u * 16u,
    A64S_DIRECT_WRITE = A64S_DIRECT_READ + A64S_READ_KIND_COUNT * 15u,
    A64S_DIRECT_WRITE_WB = A64S_DIRECT_WRITE + 3u * 16u,
    A64S_DIRECT_WRITE_WB_UNPRIV = A64S_DIRECT_WRITE_WB + 3u * 16u * 15u,
    A64S_STM_PREFLIGHT = A64S_DIRECT_WRITE_WB_UNPRIV + 3u * 16u * 15u,
    A64S_STM_COMMIT = A64S_STM_PREFLIGHT + 4u * 15u,
    A64S_STM_FINISH = A64S_STM_COMMIT + 16u,
    A64S_STM_FINISH_WB = A64S_STM_FINISH + 1u,
    A64S_LDM_PREFLIGHT = A64S_STM_FINISH_WB + 15u,
    A64S_LDM_COMMIT = A64S_LDM_PREFLIGHT + 4u * 15u,
    A64S_LDM_FINISH = A64S_LDM_COMMIT + 15u,
    A64S_LDM_FINISH_WB = A64S_LDM_FINISH + 1u,
    A64S_VFP_CORE_TO_S = A64S_LDM_FINISH_WB + 15u,
    A64S_VFP_S_TO_CORE = A64S_VFP_CORE_TO_S + 15u,
    A64S_VFP_CORE_TO_PAIR = A64S_VFP_S_TO_CORE + 15u,
    A64S_VFP_PAIR_TO_CORE = A64S_VFP_CORE_TO_PAIR + 15u * 15u,
    A64S_VFP_VMRS_FPSID = A64S_VFP_PAIR_TO_CORE + 15u * 15u,
    A64S_VFP_VMRS_FPSCR = A64S_VFP_VMRS_FPSID + 15u,
    A64S_VFP_VMRS_FPEXC = A64S_VFP_VMRS_FPSCR + 15u,
    A64S_VFP_VMRS_APSR = A64S_VFP_VMRS_FPEXC + 15u,
    A64S_VFP_VMSR_FPSCR = A64S_VFP_VMRS_APSR + 1u,
    A64S_VFP_VMSR_FPEXC = A64S_VFP_VMSR_FPSCR + 15u,
    A64S_VFP_UNARY32 = A64S_VFP_VMSR_FPEXC + 15u,
    A64S_VFP_UNARY64 = A64S_VFP_UNARY32 + 3u,
    A64S_VFP_COMPARE32 = A64S_VFP_UNARY64 + 3u,
    A64S_VFP_COMPARE64 = A64S_VFP_COMPARE32 + 1u,
    A64S_VFP_WIDEN32 = A64S_VFP_COMPARE64 + 1u,
    A64S_VFP_NARROW64 = A64S_VFP_WIDEN32 + 1u,
    A64S_VFP_ARITH32 = A64S_VFP_NARROW64 + 1u,
    A64S_VFP_ARITH64 = A64S_VFP_ARITH32 + 9u,
    A64S_VFP_DIRECT_READ32 = A64S_VFP_ARITH64 + 9u,
    A64S_VFP_DIRECT_READ64 = A64S_VFP_DIRECT_READ32 + 1u,
    A64S_VFP_DIRECT_WRITE32 = A64S_VFP_DIRECT_READ64 + 1u,
    A64S_VFP_DIRECT_WRITE64 = A64S_VFP_DIRECT_WRITE32 + 1u,
    A64S_VSTM_DIRECT_WRITE = A64S_VFP_DIRECT_WRITE64 + 1u,
    A64S_BRANCH_COND = A64S_VSTM_DIRECT_WRITE + 3u * 15u,
    A64S_BRANCH_LINK = A64S_BRANCH_COND + 14u,
    A64S_ARM_BX = A64S_BRANCH_LINK + 15u,
    A64S_ARM_BLX = A64S_ARM_BX + 16u,
    A64S_THUMB_BX = A64S_ARM_BLX + 15u,
    A64S_THUMB_BLX = A64S_THUMB_BX + 16u,
    A64S_HANDLER_COUNT = A64S_THUMB_BLX + 15u
};

enum {
    A64S_CARRY_PRESERVE = 0u,
    A64S_CARRY_CLEAR = 1u,
    A64S_CARRY_SET = 2u
};

_Static_assert(A64S_HANDLER_COUNT == A64_STATIC_HANDLER_COUNT,
               "C decoder and generated handler table disagree");
_Static_assert(sizeof(a64_static_uop_t) == 16u,
               "signed handler record stride changed");
_Static_assert(offsetof(a64_static_uop_t, immediate) == 4u &&
               offsetof(a64_static_uop_t, pc_value) == 8u &&
               offsetof(a64_static_uop_t, metadata) == 12u,
               "signed handler record layout changed");

typedef struct {
    const void *dread;
    uint64_t *hits;
    uint32_t generation;
    uint32_t privilege;
    uint32_t *vfp_s;
    uint32_t *vfp_fpexc;
    uint32_t *vfp_fpscr;
    uint32_t vfp_access;
    uint32_t vfp_fp_session;
    const void *dwrite;
    uint64_t *dwrite_hits;
} a64_static_read_context_t;

_Static_assert(offsetof(arm_cpu_t, r) == 0u,
               "signed handlers require arm_cpu.r at offset zero");
_Static_assert(sizeof(((arm_cpu_t *)0)->dread[0]) == 16u &&
               offsetof(arm_cpu_t, dread[0].host) ==
                   offsetof(arm_cpu_t, dread) &&
               offsetof(arm_cpu_t, dread[0].tag) ==
                   offsetof(arm_cpu_t, dread) + 8u &&
               offsetof(arm_cpu_t, dread[0].gen) ==
                   offsetof(arm_cpu_t, dread) + 12u,
               "signed handlers and data-read cache layout disagree");
_Static_assert(sizeof(((arm_cpu_t *)0)->dwrite[0]) == 16u &&
               offsetof(arm_cpu_t, dwrite[0].host) ==
                   offsetof(arm_cpu_t, dwrite) &&
               offsetof(arm_cpu_t, dwrite[0].tag) ==
                   offsetof(arm_cpu_t, dwrite) + 8u &&
               offsetof(arm_cpu_t, dwrite[0].gen) ==
                   offsetof(arm_cpu_t, dwrite) + 12u,
               "signed handlers and data-write cache layout disagree");
_Static_assert(sizeof(a64_static_read_context_t) == 72u &&
               offsetof(a64_static_read_context_t, dread) == 0u &&
               offsetof(a64_static_read_context_t, hits) == 8u &&
               offsetof(a64_static_read_context_t, generation) == 16u &&
               offsetof(a64_static_read_context_t, privilege) == 20u &&
               offsetof(a64_static_read_context_t, vfp_s) == 24u &&
               offsetof(a64_static_read_context_t, vfp_fpexc) == 32u &&
               offsetof(a64_static_read_context_t, vfp_fpscr) == 40u &&
               offsetof(a64_static_read_context_t, vfp_access) == 48u &&
               offsetof(a64_static_read_context_t, vfp_fp_session) == 52u &&
               offsetof(a64_static_read_context_t, dwrite) == 56u &&
               offsetof(a64_static_read_context_t, dwrite_hits) == 64u,
               "signed memory context layout changed");
_Static_assert(ARM1176_FPSID == UINT32_C(0x410120b4) &&
               ARM_FPEXC_EN == (1u << 30) &&
               ARM_FPSCR_STRIDE == UINT32_C(0x00300000) &&
               ARM_FPSCR_LEN == UINT32_C(0x00070000) &&
               ARM_FPSCR_ENABLES == UINT32_C(0x00009f00) &&
               ARM_FPSCR_DN == (1u << 25) &&
               ARM_FPSCR_FZ == (1u << 24) &&
               ARM_FPSCR_IDC == (1u << 7) &&
               ARM_FPSCR_IOC == (1u << 0) &&
               ARM_FPSCR_WMASK == UINT32_C(0xf3f79f9f),
               "generated VFP constants disagree with the interpreter");

static uint32_t ror32(uint32_t value, unsigned amount) {
    amount &= 31u;
    if (!amount) return value;
    return (value >> amount) | (value << (32u - amount));
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_native16(const uint8_t *p) {
    uint16_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static uint32_t read_native32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static uint32_t rrr(unsigned base, unsigned rd, unsigned rn, unsigned rm) {
    return base + rd * 64u + rn * 8u + rm;
}

static uint32_t rr(unsigned base, unsigned rd, unsigned rn) {
    return base + rd * 8u + rn;
}

static uint32_t dp_imm(unsigned opcode, bool set_flags,
                       unsigned rd, unsigned rn) {
    return A64S_DP_IMM +
           (((opcode * 2u + (set_flags ? 1u : 0u)) * 15u + rd) *
            16u + rn);
}

static uint32_t shift_imm(bool carry, unsigned type, unsigned rm,
                          unsigned amount) {
    return A64S_SHIFT_IMM +
           ((((carry ? 1u : 0u) * 4u + type) * 16u + rm) * 32u +
            amount);
}

static uint32_t shift_reg(bool carry, unsigned type, unsigned rm,
                          unsigned rs) {
    return A64S_SHIFT_REG +
           ((((carry ? 1u : 0u) * 4u + type) * 15u + rm) * 15u + rs);
}

static uint32_t dp_reg(unsigned opcode, bool set_flags,
                       unsigned rd, unsigned rn) {
    return A64S_DP_REG +
           (((opcode * 2u + (set_flags ? 1u : 0u)) * 15u + rd) *
            16u + rn);
}

static uint32_t addr_imm(bool up, unsigned rn) {
    return A64S_ADDR_IMM + (up ? 16u : 0u) + rn;
}

static uint32_t addr_reg(bool up, unsigned rn) {
    return A64S_ADDR_REG + (up ? 16u : 0u) + rn;
}

static uint32_t post_addr_imm(bool up, unsigned rn) {
    return A64S_POST_ADDR_IMM + (up ? 16u : 0u) + rn;
}

static uint32_t post_addr_reg(bool up, unsigned rn) {
    return A64S_POST_ADDR_REG + (up ? 16u : 0u) + rn;
}

static uint32_t direct_read(unsigned kind, unsigned rd) {
    return A64S_DIRECT_READ + kind * 15u + rd;
}

static uint32_t direct_write(unsigned kind, unsigned rd) {
    return A64S_DIRECT_WRITE + kind * 16u + rd;
}

static uint32_t direct_write_wb(unsigned kind, unsigned rd, unsigned rn,
                                bool unprivileged) {
    unsigned base = unprivileged ? A64S_DIRECT_WRITE_WB_UNPRIV
                                 : A64S_DIRECT_WRITE_WB;
    return base + (kind * 16u + rd) * 15u + rn;
}

static uint32_t stm_preflight(bool pre, bool up, unsigned rn) {
    return A64S_STM_PREFLIGHT +
           (((pre ? 1u : 0u) * 2u + (up ? 1u : 0u)) * 15u + rn);
}

static uint32_t stm_commit(unsigned rd) {
    return A64S_STM_COMMIT + rd;
}

static uint32_t stm_finish(bool writeback, unsigned rn) {
    return writeback ? A64S_STM_FINISH_WB + rn : A64S_STM_FINISH;
}

static uint32_t ldm_preflight(bool pre, bool up, unsigned rn) {
    return A64S_LDM_PREFLIGHT +
           (((pre ? 1u : 0u) * 2u + (up ? 1u : 0u)) * 15u + rn);
}

static uint32_t ldm_commit(unsigned rd) {
    return A64S_LDM_COMMIT + rd;
}

static uint32_t ldm_finish(bool writeback, unsigned rn) {
    return writeback ? A64S_LDM_FINISH_WB + rn : A64S_LDM_FINISH;
}

static uint32_t vstm_direct_write(unsigned mode, unsigned rn) {
    return A64S_VSTM_DIRECT_WRITE + mode * 15u + rn;
}

static uint32_t vfp_core_to_s(unsigned rt) {
    return A64S_VFP_CORE_TO_S + rt;
}

static uint32_t vfp_s_to_core(unsigned rt) {
    return A64S_VFP_S_TO_CORE + rt;
}

static uint32_t vfp_core_to_pair(unsigned rt, unsigned rt2) {
    return A64S_VFP_CORE_TO_PAIR + rt * 15u + rt2;
}

static uint32_t vfp_pair_to_core(unsigned rt, unsigned rt2) {
    return A64S_VFP_PAIR_TO_CORE + rt * 15u + rt2;
}

static uint32_t branch_cond(unsigned condition) {
    return A64S_BRANCH_COND + condition;
}

static uint32_t branch_link(unsigned condition) {
    return A64S_BRANCH_LINK + condition;
}

static uint32_t arm_bx(unsigned rm) {
    return A64S_ARM_BX + rm;
}

static uint32_t arm_blx(unsigned rm) {
    return A64S_ARM_BLX + rm;
}

static uint32_t thumb_bx(unsigned rm) {
    return A64S_THUMB_BX + rm;
}

static uint32_t thumb_blx(unsigned rm) {
    return A64S_THUMB_BLX + rm;
}

static bool handler_is_condition(uint32_t handler) {
    return handler >= A64S_COND && handler < A64S_DP_IMM;
}

static bool handler_is_shift(uint32_t handler) {
    return handler >= A64S_SHIFT_IMM && handler < A64S_DP_REG;
}

static bool handler_is_dp_reg(uint32_t handler) {
    return handler >= A64S_DP_REG && handler < A64S_ADDR_IMM;
}

static bool handler_is_addr_imm(uint32_t handler) {
    return (handler >= A64S_ADDR_IMM && handler < A64S_ADDR_REG) ||
           (handler >= A64S_POST_ADDR_IMM &&
            handler < A64S_POST_ADDR_REG);
}

static bool handler_is_addr_reg(uint32_t handler) {
    return (handler >= A64S_ADDR_REG &&
            handler < A64S_POST_ADDR_IMM) ||
           (handler >= A64S_POST_ADDR_REG &&
            handler < A64S_DIRECT_READ);
}

static bool handler_is_direct_read(uint32_t handler) {
    return (handler >= A64S_DIRECT_READ &&
            handler < A64S_DIRECT_WRITE) ||
           (handler >= A64S_LDM_PREFLIGHT &&
            handler < A64S_LDM_COMMIT) ||
           (handler >= A64S_VFP_DIRECT_READ32 &&
            handler <= A64S_VFP_DIRECT_READ64);
}

static bool handler_is_direct_write(uint32_t handler) {
    return (handler >= A64S_DIRECT_WRITE &&
            handler < A64S_STM_PREFLIGHT) ||
           (handler >= A64S_STM_FINISH &&
            handler < A64S_LDM_PREFLIGHT) ||
           (handler >= A64S_VFP_DIRECT_WRITE32 &&
            handler < A64S_BRANCH_COND);
}

static bool handler_is_vfp_direct_write(uint32_t handler) {
    return handler >= A64S_VFP_DIRECT_WRITE32 &&
           handler <= A64S_VFP_DIRECT_WRITE64;
}

static bool handler_is_vstm_direct_write(uint32_t handler) {
    return handler >= A64S_VSTM_DIRECT_WRITE &&
           handler < A64S_BRANCH_COND;
}

static bool handler_is_direct_write_wb(uint32_t handler) {
    return handler >= A64S_DIRECT_WRITE_WB &&
           handler < A64S_STM_PREFLIGHT;
}

static bool handler_is_direct_write_unpriv(uint32_t handler) {
    return handler >= A64S_DIRECT_WRITE_WB_UNPRIV &&
           handler < A64S_STM_PREFLIGHT;
}

static bool handler_is_stm_preflight(uint32_t handler) {
    return handler >= A64S_STM_PREFLIGHT && handler < A64S_STM_COMMIT;
}

static bool handler_is_stm_commit(uint32_t handler) {
    return handler >= A64S_STM_COMMIT && handler < A64S_STM_FINISH;
}

static bool handler_is_stm_finish(uint32_t handler) {
    return handler >= A64S_STM_FINISH && handler < A64S_LDM_PREFLIGHT;
}

static bool handler_is_stm_finish_wb(uint32_t handler) {
    return handler >= A64S_STM_FINISH_WB && handler < A64S_LDM_PREFLIGHT;
}

static bool handler_is_ldm_preflight(uint32_t handler) {
    return handler >= A64S_LDM_PREFLIGHT && handler < A64S_LDM_COMMIT;
}

static bool handler_is_ldm_commit(uint32_t handler) {
    return handler >= A64S_LDM_COMMIT && handler < A64S_LDM_FINISH;
}

static bool handler_is_ldm_finish(uint32_t handler) {
    return handler >= A64S_LDM_FINISH && handler < A64S_VFP_CORE_TO_S;
}

static bool handler_is_ldm_finish_wb(uint32_t handler) {
    return handler >= A64S_LDM_FINISH_WB &&
           handler < A64S_VFP_CORE_TO_S;
}

static bool handler_is_vfp(uint32_t handler) {
    return handler >= A64S_VFP_CORE_TO_S &&
           handler < A64S_BRANCH_COND;
}

static bool handler_is_vfp_arithmetic(uint32_t handler) {
    return handler >= A64S_VFP_ARITH32 &&
           handler < A64S_VFP_DIRECT_READ32;
}

static bool vfp_arithmetic_immediate_valid(uint32_t handler,
                                           uint32_t immediate) {
    unsigned rd = immediate & 255u;
    unsigned rn = (immediate >> 8) & 255u;
    unsigned rm = (immediate >> 16) & 255u;
    if (!handler_is_vfp_arithmetic(handler) || (immediate >> 24) != 0u)
        return false;
    if (handler < A64S_VFP_ARITH64)
        return rd <= 31u && rn <= 31u && rm <= 31u;
    return rd <= 30u && rn <= 30u && rm <= 30u &&
           (rd & 1u) == 0u && (rn & 1u) == 0u && (rm & 1u) == 0u;
}

static bool vstm_immediate_valid(uint32_t immediate) {
    unsigned first = immediate & 63u;
    unsigned words = (immediate >> 8) & 63u;
    return immediate == (first | (words << 8)) && words != 0u &&
           words <= 32u && first < 32u && first + words <= 32u;
}

static bool handler_is_terminal_branch(uint32_t handler) {
    return handler >= A64S_BRANCH_COND && handler < A64S_HANDLER_COUNT;
}

static bool handler_is_conditional_branch(uint32_t handler) {
    return handler >= A64S_BRANCH_COND && handler < A64S_BRANCH_LINK;
}

static bool handler_is_indirect_branch(uint32_t handler) {
    return handler >= A64S_ARM_BX && handler < A64S_HANDLER_COUNT;
}

static bool handler_is_runtime_guarded(uint32_t handler) {
    return handler_is_direct_read(handler) ||
           handler_is_stm_preflight(handler) ||
           (handler_is_direct_write(handler) &&
            !handler_is_stm_finish(handler)) ||
           handler_is_vfp(handler) ||
           handler_is_indirect_branch(handler);
}

static bool arm_dp_encoding(uint32_t insn) {
    return (insn & UINT32_C(0x0c000000)) == 0u &&
           (insn & UINT32_C(0x01900000)) != UINT32_C(0x01000000) &&
           !((insn & UINT32_C(0x0e000000)) == 0u &&
             (insn & UINT32_C(0x00000090)) == UINT32_C(0x00000090));
}

static bool handler_touches_memory(uint32_t handler) {
    return handler >= A64S_LDR && handler < A64S_COND;
}

enum {
    A64S_VFP_MOV = 0u,
    A64S_VFP_ABS = 1u,
    A64S_VFP_NEG = 2u
};

enum {
    A64S_VFP_VMLA = 0u,
    A64S_VFP_VMLS = 1u,
    A64S_VFP_VNMLS = 2u,
    A64S_VFP_VNMLA = 3u,
    A64S_VFP_VMUL = 4u,
    A64S_VFP_VNMUL = 5u,
    A64S_VFP_VADD = 6u,
    A64S_VFP_VSUB = 7u,
    A64S_VFP_VDIV = 8u
};

/* Decode the bounded VFPv2 product subset. Every admitted handler rechecks the
 * live VFP access and mode state before touching guest state, because a decoded
 * block can outlive a thread's lazy-VFP context. */
static bool decode_vfp_transfer(uint32_t insn, uint32_t pc_value,
                                bool write_hits, a64_static_uop_t *op,
                                unsigned *written) {
    if (!op || !written) return false;

    /* MCR/MRC: core <-> single/double word and VMRS/VMSR. */
    if ((insn & UINT32_C(0x0f000e10)) == UINT32_C(0x0e000a10)) {
        unsigned opc1 = (insn >> 21) & 7u;
        bool load = (insn & (1u << 20)) != 0u;
        bool cp11 = (insn & (1u << 8)) != 0u;
        unsigned vn = (insn >> 16) & 15u;
        unsigned rt = (insn >> 12) & 15u;

        if (!cp11 && opc1 == 0u) {
            if ((insn & UINT32_C(0x0000006f)) != 0u || rt == 15u)
                return false;
            op->handler = load ? vfp_s_to_core(rt) : vfp_core_to_s(rt);
            op->immediate = vn * 2u + ((insn >> 7) & 1u);
            *written = 1u;
            return true;
        }
        if (cp11 && opc1 <= 1u) {
            if ((insn & (1u << 7)) != 0u ||
                (insn & UINT32_C(0x0000006f)) != 0u || rt == 15u)
                return false;
            op->handler = load ? vfp_s_to_core(rt) : vfp_core_to_s(rt);
            op->immediate = vn * 2u + opc1;
            *written = 1u;
            return true;
        }
        if (cp11 || opc1 != 7u ||
            (insn & UINT32_C(0x000000ef)) != 0u)
            return false;
        if (load) {
            if (vn == 1u && rt == 15u)
                op->handler = A64S_VFP_VMRS_APSR;
            else if (rt == 15u)
                return false;
            else if (vn == 0u)
                op->handler = A64S_VFP_VMRS_FPSID + rt;
            else if (vn == 1u)
                op->handler = A64S_VFP_VMRS_FPSCR + rt;
            else if (vn == 8u)
                op->handler = A64S_VFP_VMRS_FPEXC + rt;
            else
                return false;
        } else {
            if (rt == 15u) return false;
            if (vn == 1u)
                op->handler = A64S_VFP_VMSR_FPSCR + rt;
            else if (vn == 8u)
                op->handler = A64S_VFP_VMSR_FPEXC + rt;
            else
                return false;
        }
        *written = 1u;
        return true;
    }

    /* MCRR/MRRC: a core-register pair <-> one D register or two S regs. */
    if ((insn & UINT32_C(0x0fe00e00)) == UINT32_C(0x0c400a00)) {
        bool load = (insn & (1u << 20)) != 0u;
        bool dbl = (insn & (1u << 8)) != 0u;
        bool m = (insn & (1u << 5)) != 0u;
        unsigned rt2 = (insn >> 16) & 15u;
        unsigned rt = (insn >> 12) & 15u;
        unsigned vm = insn & 15u;
        unsigned first;
        if ((insn & UINT32_C(0x000000c0)) != 0u ||
            rt == 15u || rt2 == 15u)
            return false;
        if (dbl) {
            if (m) return false;
            first = vm * 2u;
        } else {
            first = vm * 2u + (m ? 1u : 0u);
            if (first == 31u) return false;
        }
        op->handler = load ? vfp_pair_to_core(rt, rt2)
                           : vfp_core_to_pair(rt, rt2);
        op->immediate = first;
        *written = 1u;
        return true;
    }

    /* LDC/STC: one VSTR/VLDR or one bounded architectural VSTM. */
    if ((insn & UINT32_C(0x0e000e00)) == UINT32_C(0x0c000a00)) {
        bool pre = (insn & (1u << 24)) != 0u;
        bool up = (insn & (1u << 23)) != 0u;
        bool d = (insn & (1u << 22)) != 0u;
        bool writeback = (insn & (1u << 21)) != 0u;
        bool load = (insn & (1u << 20)) != 0u;
        bool dbl = (insn & (1u << 8)) != 0u;
        unsigned rn = (insn >> 16) & 15u;
        unsigned vd = (insn >> 12) & 15u;
        unsigned imm8 = insn & 255u;

        if (pre && !writeback) {
            if ((!load && !write_hits) || (dbl && d)) return false;
            op[0].handler = addr_imm(up, rn);
            op[0].immediate = imm8 * 4u;
            op[0].pc_value = pc_value;
            if (load)
                op[1].handler = dbl ? A64S_VFP_DIRECT_READ64
                                    : A64S_VFP_DIRECT_READ32;
            else
                op[1].handler = dbl ? A64S_VFP_DIRECT_WRITE64
                                    : A64S_VFP_DIRECT_WRITE32;
            op[1].immediate = dbl ? vd * 2u : vd * 2u + (d ? 1u : 0u);
            *written = 2u;
            return true;
        }

        /* VSTM S/D shares one contiguous vfp_s[] word stream. Keep only the
         * three architectural address forms, reject PC bases, empty lists and
         * deprecated odd-count FSTMX D, and leave every load on arm_step(). */
        unsigned mode;
        unsigned first;
        if (load || !write_hits || rn == 15u || imm8 == 0u)
            return false;
        if (!pre && up)
            mode = writeback ? 1u : 0u;
        else if (pre && !up && writeback)
            mode = 2u;
        else
            return false;
        if (dbl) {
            unsigned count = imm8 / 2u;
            if (d || (imm8 & 1u) != 0u || vd + count > 16u)
                return false;
            first = vd * 2u;
        } else {
            first = vd * 2u + (d ? 1u : 0u);
            if (first + imm8 > 32u) return false;
        }
        op->handler = vstm_direct_write(mode, rn);
        op->immediate = first | (imm8 << 8);
        *written = 1u;
        return true;
    }

    /* CDP arithmetic and "other" groups. Arithmetic is a deliberately narrow
     * runtime contract: the generated handler proves the live RunFast mode,
     * simple operands/results and host exception state before committing. */
    if ((insn & UINT32_C(0x0f000e10)) == UINT32_C(0x0e000a00)) {
        unsigned family = (((insn >> 23) & 1u) << 2) |
                          (((insn >> 21) & 1u) << 1) |
                          ((insn >> 20) & 1u);
        unsigned opc2 = (insn >> 16) & 15u;
        bool top = (insn & (1u << 7)) != 0u;
        bool dbl = (insn & (1u << 8)) != 0u;
        unsigned operation;
        unsigned rd;
        unsigned rn;
        unsigned rm;
        if (family != 7u) {
            bool alt = (insn & (1u << 6)) != 0u;
            switch (family) {
            case 0u:
                operation = alt ? A64S_VFP_VMLS : A64S_VFP_VMLA;
                break;
            case 1u:
                operation = alt ? A64S_VFP_VNMLA : A64S_VFP_VNMLS;
                break;
            case 2u:
                operation = alt ? A64S_VFP_VNMUL : A64S_VFP_VMUL;
                break;
            case 3u:
                operation = alt ? A64S_VFP_VSUB : A64S_VFP_VADD;
                break;
            case 4u:
                if (alt) return false;
                operation = A64S_VFP_VDIV;
                break;
            default:
                return false; /* VFPv4 fused operations on a VFPv2 core. */
            }
            if (dbl) {
                if ((insn & ((1u << 22) | (1u << 7) | (1u << 5))) != 0u)
                    return false;
                rd = ((insn >> 12) & 15u) * 2u;
                rn = ((insn >> 16) & 15u) * 2u;
                rm = (insn & 15u) * 2u;
                op->handler = A64S_VFP_ARITH64 + operation;
            } else {
                rd = ((insn >> 12) & 15u) * 2u +
                     ((insn >> 22) & 1u);
                rn = ((insn >> 16) & 15u) * 2u +
                     ((insn >> 7) & 1u);
                rm = (insn & 15u) * 2u + ((insn >> 5) & 1u);
                op->handler = A64S_VFP_ARITH32 + operation;
            }
            op->immediate = rd | (rn << 8) | (rm << 16);
            *written = 1u;
            return true;
        }
        if ((insn & (1u << 6)) == 0u) return false;
        if (opc2 == 4u || opc2 == 5u) {
            bool zero = opc2 == 5u;
            if (zero && ((insn & 15u) != 0u ||
                         (insn & (1u << 5)) != 0u))
                return false;
            if (dbl) {
                if ((insn & ((1u << 22) | (1u << 5))) != 0u)
                    return false;
                rd = ((insn >> 12) & 15u) * 2u;
                rm = (insn & 15u) * 2u;
                op->handler = A64S_VFP_COMPARE64;
            } else {
                rd = ((insn >> 12) & 15u) * 2u +
                     ((insn >> 22) & 1u);
                rm = (insn & 15u) * 2u + ((insn >> 5) & 1u);
                op->handler = A64S_VFP_COMPARE32;
            }
            op->immediate = rd | (rm << 8) |
                            ((zero ? 1u : 0u) << 16) |
                            ((top ? 1u : 0u) << 17);
            *written = 1u;
            return true;
        }
        if (opc2 == 7u && top) {
            if (dbl) {
                /* VCVT.F32.F64 rounds, so its signed handler owns only the
                 * separately-audited RunFast/simple-value contract. */
                if ((insn & (1u << 5)) != 0u) return false;
                rd = ((insn >> 12) & 15u) * 2u +
                     ((insn >> 22) & 1u);
                rm = (insn & 15u) * 2u;
                op->handler = A64S_VFP_NARROW64;
            } else {
                /* VCVT.F64.F32 is exact for every binary32 value. */
                if ((insn & (1u << 22)) != 0u) return false;
                rd = ((insn >> 12) & 15u) * 2u;
                rm = (insn & 15u) * 2u + ((insn >> 5) & 1u);
                op->handler = A64S_VFP_WIDEN32;
            }
            op->immediate = rd | (rm << 8);
            *written = 1u;
            return true;
        }
        if (opc2 == 0u)
            operation = top ? A64S_VFP_ABS : A64S_VFP_MOV;
        else if (opc2 == 1u && !top)
            operation = A64S_VFP_NEG;
        else
            return false;
        if (dbl) {
            if ((insn & ((1u << 22) | (1u << 5))) != 0u)
                return false;
            rd = ((insn >> 12) & 15u) * 2u;
            rm = (insn & 15u) * 2u;
            op->handler = A64S_VFP_UNARY64 + operation;
        } else {
            rd = ((insn >> 12) & 15u) * 2u + ((insn >> 22) & 1u);
            rm = (insn & 15u) * 2u + ((insn >> 5) & 1u);
            op->handler = A64S_VFP_UNARY32 + operation;
        }
        op->immediate = rd | (rm << 8);
        *written = 1u;
        return true;
    }
    return false;
}

static bool decode_arm(uint32_t insn, unsigned index, unsigned insns,
                       uint32_t pc, bool read_hits, bool write_hits,
                       a64_static_uop_t *out, unsigned *written) {
    unsigned condition = insn >> 28;
    unsigned count = 0u;
    a64_static_uop_t *op = out;
    a64_static_uop_t *guard = NULL;

    if (!written || condition == 15u) return false;
    *written = 0u;

    if ((insn & UINT32_C(0x0e000000)) == UINT32_C(0x0a000000)) {
        int32_t displacement;
        uint32_t target;
        uint32_t fallthrough;
        bool link = (insn & (1u << 24)) != 0u;
        if (index + 1u != insns) return false;
        displacement = (int32_t)(insn << 8) >> 6;
        target = pc + index * 4u + 8u + (uint32_t)displacement;
        fallthrough = pc + index * 4u + 4u;
        /* Unconditional B retains the compact fixed-exit record. Every BL and
         * every conditional B needs both destinations: the generated handler
         * chooses from live NZCV and writes LR only on a taken link. */
        op->handler = !link && condition == 14u
            ? A64S_END
            : (link ? branch_link(condition) : branch_cond(condition));
        op->immediate = target;
        op->pc_value = fallthrough;
        *written = 1u;
        return true;
    }

    if (condition < 14u) {
        guard = op;
        op->handler = A64S_COND + condition;
        op++;
        count++;
    }

    if ((insn & UINT32_C(0x0ffffff0)) == UINT32_C(0x012fff10) ||
        (insn & UINT32_C(0x0ffffff0)) == UINT32_C(0x012fff30)) {
        unsigned rm = insn & 15u;
        bool link = (insn & UINT32_C(0x20)) != 0u;
        if (index + 1u != insns || (link && rm == 15u)) return false;
        op->handler = link ? arm_blx(rm) : arm_bx(rm);
        if (guard) guard->metadata = 1u;
        *written = count + 1u;
        return true;
    }

    if (read_hits && decode_vfp_transfer(
            insn, pc + index * 4u + 8u, write_hits, op, written)) {
        if (guard) guard->metadata = *written;
        *written += count;
        return true;
    }

    if ((insn & UINT32_C(0x0e000000)) == UINT32_C(0x08000000)) {
        const bool pre = (insn & (1u << 24)) != 0u;
        const bool up = (insn & (1u << 23)) != 0u;
        const bool user_bank = (insn & (1u << 22)) != 0u;
        const bool writeback = (insn & (1u << 21)) != 0u;
        const bool load = (insn & (1u << 20)) != 0u;
        const unsigned rn = (insn >> 16) & 15u;
        const uint32_t list = insn & UINT32_C(0xffff);
        unsigned n = 0u;

        /* Both block-transfer families preflight one complete aligned cache
         * block before any architectural mutation. LDM excludes PC and is not
         * terminal; STM remains terminal because a store can rewrite a later
         * cached instruction. User-bank, empty-list and unsafe writeback/base
         * aliases remain literal. */
        if (!read_hits || user_bank || rn == 15u || list == 0u ||
            (writeback && (list & (1u << rn)) != 0u))
            return false;
        if (load && (list & (1u << 15)) != 0u) return false;
        if (!load && (!write_hits || index + 1u != insns)) return false;
        for (unsigned i = 0u; i < 16u; i++)
            if ((list & (1u << i)) != 0u) n++;

        op->handler = load ? ldm_preflight(pre, up, rn)
                           : stm_preflight(pre, up, rn);
        op->immediate = n;
        op++;
        for (unsigned i = 0u; i < (load ? 15u : 16u); i++) {
            if ((list & (1u << i)) == 0u) continue;
            op->handler = load ? ldm_commit(i) : stm_commit(i);
            op++;
        }
        op->handler = load ? ldm_finish(writeback, rn)
                           : stm_finish(writeback, rn);
        op->immediate = n;
        if (guard) guard->metadata = n + 2u;
        *written = count + n + 2u;
        return true;
    }

    if ((insn & UINT32_C(0x0c000000)) == UINT32_C(0x04000000)) {
        unsigned rn = (insn >> 16) & 15u;
        unsigned rd = (insn >> 12) & 15u;
        uint32_t offset = insn & UINT32_C(0x0fff);
        bool load = (insn & (1u << 20)) != 0u;
        bool up = (insn & (1u << 23)) != 0u;

        if (read_hits) {
            bool byte = (insn & (1u << 22)) != 0u;
            bool pre = (insn & (1u << 24)) != 0u;
            bool writeback = !pre || (insn & (1u << 21)) != 0u;
            bool unprivileged = !pre && (insn & (1u << 21)) != 0u;

            if (load) {
                if (!pre || writeback || rd == 15u) return false;
            } else {
                /* Stores deliberately terminate every signed head. On a hit,
                 * the next raw-byte witness therefore observes SMC before any
                 * following cached instruction can execute. */
                if (!write_hits || index + 1u != insns ||
                    (writeback && (rn == 15u || rn == rd)) ||
                    (byte && rd == 15u))
                    return false;
            }
            if ((insn & (1u << 25)) != 0u) {
                unsigned rm = insn & 15u;
                unsigned type = (insn >> 5) & 3u;
                unsigned amount = (insn >> 7) & 31u;
                if ((insn & (1u << 4)) != 0u || rm == 15u)
                    return false;
                op[0].handler = shift_imm(false, type, rm, amount);
                op[0].pc_value = pc + index * 4u + 8u;
                op[1].handler = pre ? addr_reg(up, rn)
                                    : post_addr_reg(up, rn);
                op[1].pc_value = pc + index * 4u + 8u;
                op[2].handler = load ? direct_read(byte, rd) :
                    (writeback ? direct_write_wb(byte, rd, rn,
                                                 unprivileged)
                               : direct_write(byte, rd));
                if (!load && writeback) op[2].immediate = pre ? 0u : 1u;
                if (guard) guard->metadata = 3u;
                *written = count + 3u;
            } else {
                op[0].handler = pre ? addr_imm(up, rn)
                                    : post_addr_imm(up, rn);
                op[0].immediate = offset;
                op[0].pc_value = pc + index * 4u + 8u;
                op[1].handler = load ? direct_read(byte, rd) :
                    (writeback ? direct_write_wb(byte, rd, rn,
                                                 unprivileged)
                               : direct_write(byte, rd));
                if (!load && writeback) op[1].immediate = pre ? 0u : 1u;
                if (guard) guard->metadata = 2u;
                *written = count + 2u;
            }
            return true;
        }

        if ((insn & (1u << 25)) != 0u ||
            (insn & (1u << 24)) == 0u ||
            (insn & (1u << 22)) != 0u ||
            (insn & (1u << 21)) != 0u || rd > 7u || rn > 7u)
            return false;
        if (!up) offset = 0u - offset;
        op->handler = rr(load ? A64S_LDR : A64S_STR, rd, rn);
        op->immediate = offset;
        if (guard) guard->metadata = 1u;
        *written = count + 1u;
        return true;
    }

    if (arm_dp_encoding(insn)) {
        unsigned opcode = (insn >> 21) & 15u;
        bool set_flags = (insn & (1u << 20)) != 0u;
        unsigned rn = (insn >> 16) & 15u;
        unsigned rd = (insn >> 12) & 15u;
        bool writes_result = opcode < 8u || opcode >= 12u;
        unsigned base;

        /* TST/TEQ/CMP/CMN require S. The S==0 encodings are miscellaneous
         * control instructions, which arm_dp_encoding has already excluded. */
        if (!writes_result && !set_flags) return false;
        if (writes_result && rd == 15u) return false;

        if ((insn & (1u << 25)) != 0u) {
            unsigned rotate = 2u * ((insn >> 8) & 15u);
            uint32_t immediate = ror32(insn & 255u, rotate);
            unsigned handler_rd = writes_result ? rd : 0u;
            op->handler = dp_imm(opcode, set_flags, handler_rd, rn);
            op->immediate = immediate;
            op->pc_value = pc + index * 4u + 8u;
            op->metadata = rotate == 0u ? A64S_CARRY_PRESERVE :
                ((immediate >> 31) != 0u ? A64S_CARRY_SET :
                                           A64S_CARRY_CLEAR);
            if (guard) guard->metadata = 1u;
            *written = count + 1u;
            return true;
        }

        {
            unsigned rm = insn & 15u;
            unsigned type = (insn >> 5) & 3u;
            unsigned handler_rd = writes_result ? rd : 0u;
            bool logical = opcode == 0u || opcode == 1u ||
                           opcode == 8u || opcode == 9u ||
                           opcode >= 12u;
            bool needs_carry = logical && (set_flags || !writes_result);

            /* Retain the small one-record fast path proved by r491. */
            base = 0u;
            if (!set_flags && rd <= 7u && rn <= 7u && rm <= 7u &&
                (insn & UINT32_C(0x00000ff0)) == 0u) {
                switch (opcode) {
                case 1u:  base = A64S_EOR_RRR; break;
                case 2u:  base = A64S_SUB_RRR; break;
                case 4u:  base = A64S_ADD_RRR; break;
                case 12u: base = A64S_ORR_RRR; break;
                default: break;
                }
            }
            if (base != 0u) {
                op->handler = rrr(base, rd, rn, rm);
                if (guard) guard->metadata = 1u;
                *written = count + 1u;
                return true;
            }

            if ((insn & (1u << 4)) != 0u) {
                unsigned rs = (insn >> 8) & 15u;
                /* ARMv6 declares every R15 use in a register-specified shift
                 * UNPREDICTABLE; the literal core refuses it as undefined. */
                if (rd == 15u || rn == 15u || rm == 15u || rs == 15u)
                    return false;
                op[0].handler = shift_reg(needs_carry, type, rm, rs);
            } else {
                unsigned amount = (insn >> 7) & 31u;
                op[0].handler = shift_imm(needs_carry, type, rm, amount);
                op[0].pc_value = pc + index * 4u + 8u;
            }
            op[1].handler = dp_reg(opcode, set_flags, handler_rd, rn);
            op[1].pc_value = pc + index * 4u + 8u;
            if (guard) guard->metadata = 2u;
            *written = count + 2u;
            return true;
        }
    }

    return false;
}

static void thumb_emit_dp_reg(a64_static_uop_t *out, unsigned opcode,
                              bool set_flags, unsigned rd, unsigned rn,
                              unsigned rm, bool needs_carry,
                              uint32_t pc_value) {
    out[0].handler = shift_imm(needs_carry, 0u, rm, 0u);
    out[0].pc_value = pc_value;
    out[1].handler = dp_reg(opcode, set_flags, rd, rn);
    out[1].pc_value = pc_value;
}

static void thumb_emit_shift(a64_static_uop_t *out, bool register_shift,
                             unsigned type, unsigned rd, unsigned source,
                             unsigned amount, uint32_t pc_value) {
    out[0].handler = register_shift
        ? shift_reg(true, type, rd, amount)
        : shift_imm(true, type, source, amount);
    out[0].pc_value = pc_value;
    out[1].handler = dp_reg(13u, true, rd, 0u); /* MOVS Rd, shifter result */
    out[1].pc_value = pc_value;
}

static void thumb_emit_read_imm(a64_static_uop_t *out, unsigned kind,
                                unsigned rd, unsigned rn, uint32_t offset,
                                uint32_t pc_value) {
    out[0].handler = addr_imm(true, rn);
    out[0].immediate = offset;
    out[0].pc_value = pc_value;
    out[1].handler = direct_read(kind, rd);
}

static void thumb_emit_read_reg(a64_static_uop_t *out, unsigned kind,
                                unsigned rd, unsigned rn, unsigned rm,
                                uint32_t pc_value) {
    out[0].handler = shift_imm(false, 0u, rm, 0u);
    out[0].pc_value = pc_value;
    out[1].handler = addr_reg(true, rn);
    out[1].pc_value = pc_value;
    out[2].handler = direct_read(kind, rd);
}

static void thumb_emit_write_imm(a64_static_uop_t *out, unsigned kind,
                                 unsigned rd, unsigned rn, uint32_t offset,
                                 uint32_t pc_value) {
    out[0].handler = addr_imm(true, rn);
    out[0].immediate = offset;
    out[0].pc_value = pc_value;
    out[1].handler = direct_write(kind, rd);
}

static void thumb_emit_write_reg(a64_static_uop_t *out, unsigned kind,
                                 unsigned rd, unsigned rn, unsigned rm,
                                 uint32_t pc_value) {
    out[0].handler = shift_imm(false, 0u, rm, 0u);
    out[0].pc_value = pc_value;
    out[1].handler = addr_reg(true, rn);
    out[1].pc_value = pc_value;
    out[2].handler = direct_write(kind, rd);
}

static bool decode_thumb(uint16_t insn, unsigned index, unsigned insns,
                         uint32_t pc, bool read_hits, bool write_hits,
                         a64_static_uop_t *out, unsigned *written) {
    uint32_t pc_value = pc + index * 2u + 4u;
    if (!written) return false;
    *written = 0u;

    if ((insn & UINT16_C(0xf000)) == UINT16_C(0xd000) &&
        ((insn >> 8) & 15u) < 14u) {
        unsigned condition = (insn >> 8) & 15u;
        int32_t displacement =
            (int32_t)((uint32_t)(insn & 0x00ffu) << 24) >> 23;
        uint32_t target = pc_value + (uint32_t)displacement;
        if (index + 1u != insns) return false;
        out->handler = branch_cond(condition);
        out->immediate = target;
        out->pc_value = pc + index * 2u + 2u;
        *written = 1u;
        return true;
    }

    if ((insn & UINT16_C(0xf800)) == UINT16_C(0xe000)) {
        int32_t displacement = (int32_t)((uint32_t)(insn & 0x07ffu) << 21) >> 20;
        uint32_t target = pc + index * 2u + 4u + (uint32_t)displacement;
        if (index + 1u != insns) return false;
        out->handler = A64S_END;
        out->immediate = target;
        *written = 1u;
        return true;
    }

    if ((insn & UINT16_C(0xff00)) == UINT16_C(0x4700)) {
        unsigned rm = (insn >> 3) & 15u;
        bool link = (insn & (1u << 7)) != 0u;
        if (index + 1u != insns || (link && rm == 15u)) return false;
        out->handler = link ? thumb_blx(rm) : thumb_bx(rm);
        *written = 1u;
        return true;
    }

    if ((insn & UINT16_C(0xf800)) < UINT16_C(0x1800)) {
        unsigned type = (insn >> 11) & 3u;
        unsigned amount = (insn >> 6) & 31u;
        unsigned source = (insn >> 3) & 7u;
        unsigned rd = insn & 7u;
        thumb_emit_shift(out, false, type, rd, source, amount, pc_value);
        *written = 2u;
        return true;
    }

    if ((insn & UINT16_C(0xf800)) == UINT16_C(0x1800)) {
        unsigned rd = insn & 7u;
        unsigned rn = (insn >> 3) & 7u;
        unsigned operand = (insn >> 6) & 7u;
        unsigned opcode = (insn & (1u << 9)) != 0u ? 2u : 4u;
        if (insn & (1u << 10)) {
            out->handler = dp_imm(opcode, true, rd, rn);
            out->immediate = operand;
            out->pc_value = pc_value;
            *written = 1u;
        } else {
            thumb_emit_dp_reg(out, opcode, true, rd, rn, operand,
                              false, pc_value);
            *written = 2u;
        }
        return true;
    }

    if ((insn & UINT16_C(0xe000)) == UINT16_C(0x2000)) {
        unsigned rd = (insn >> 8) & 7u;
        unsigned operation = (insn >> 11) & 3u;
        if (operation >= 2u) {
            out->handler = rr(operation == 3u ? A64S_SUBS_IMM
                                              : A64S_ADDS_IMM,
                              rd, rd);
        } else {
            unsigned opcode = operation == 0u ? 13u : 10u;
            unsigned handler_rd = operation == 0u ? rd : 0u;
            unsigned rn = operation == 0u ? 0u : rd;
            out->handler = dp_imm(opcode, true, handler_rd, rn);
            out->metadata = A64S_CARRY_PRESERVE;
        }
        out->immediate = insn & 255u;
        out->pc_value = pc_value;
        *written = 1u;
        return true;
    }

    if ((insn & UINT16_C(0xfc00)) == UINT16_C(0x4000)) {
        unsigned rm = (insn >> 3) & 7u;
        unsigned rd = insn & 7u;
        unsigned opcode = (insn >> 6) & 15u;
        switch (opcode) {
        case 1u: /* EOR: retain the compact r491 handler. */
            out->handler = rr(A64S_EORS_RR, rd, rm);
            *written = 1u;
            return true;
        case 2u: case 3u: case 4u: case 7u:
            thumb_emit_shift(out, true,
                             opcode == 2u ? 0u :
                             opcode == 3u ? 1u :
                             opcode == 4u ? 2u : 3u,
                             rd, rd, rm, pc_value);
            *written = 2u;
            return true;
        case 9u: /* NEG Rd, Rm is RSBS Rd,Rm,#0. */
            out->handler = dp_imm(3u, true, rd, rm);
            out->immediate = 0u;
            out->pc_value = pc_value;
            *written = 1u;
            return true;
        case 13u:
            out->handler = rr(A64S_MULS_RR, rd, rm);
            *written = 1u;
            return true;
        default: {
            unsigned arm_opcode;
            bool needs_carry = false;
            switch (opcode) {
            case 0u:  arm_opcode = 0u;  needs_carry = true; break; /* AND */
            case 5u:  arm_opcode = 5u;  break;                    /* ADC */
            case 6u:  arm_opcode = 6u;  break;                    /* SBC */
            case 8u:  arm_opcode = 8u;  needs_carry = true; break; /* TST */
            case 10u: arm_opcode = 10u; break;                    /* CMP */
            case 11u: arm_opcode = 11u; break;                    /* CMN */
            case 12u: arm_opcode = 12u; needs_carry = true; break; /* ORR */
            case 14u: arm_opcode = 14u; needs_carry = true; break; /* BIC */
            case 15u: arm_opcode = 15u; needs_carry = true; break; /* MVN */
            default: return false;
            }
            bool writes_result = arm_opcode < 8u || arm_opcode >= 12u;
            unsigned handler_rd = writes_result ? rd : 0u;
            unsigned rn = arm_opcode == 15u ? 0u : rd;
            thumb_emit_dp_reg(out, arm_opcode, true, handler_rd, rn,
                              rm, needs_carry, pc_value);
            *written = 2u;
            return true;
        }
        }
    }

    if ((insn & UINT16_C(0xfc00)) == UINT16_C(0x4400)) {
        unsigned operation = (insn >> 8) & 3u;
        unsigned rd = (insn & 7u) | ((insn >> 4) & 8u);
        unsigned rm = ((insn >> 3) & 7u) | ((insn >> 3) & 8u);
        if (operation == 3u || (operation != 1u && rd == 15u))
            return false;
        unsigned opcode = operation == 0u ? 4u :
                          operation == 1u ? 10u : 13u;
        bool writes_result = operation != 1u;
        unsigned handler_rd = writes_result ? rd : 0u;
        unsigned rn = operation == 2u ? 0u : rd;
        thumb_emit_dp_reg(out, opcode, operation == 1u,
                          handler_rd, rn, rm, false, pc_value);
        *written = 2u;
        return true;
    }

    if ((insn & UINT16_C(0xf800)) == UINT16_C(0x4800)) {
        unsigned rd = (insn >> 8) & 7u;
        if (!read_hits) return false;
        thumb_emit_read_imm(out, A64S_READ_WORD, rd, 15u,
                            (uint32_t)(insn & 255u) * 4u,
                            pc_value & ~UINT32_C(3));
        *written = 2u;
        return true;
    }

    if ((insn & UINT16_C(0xf000)) == UINT16_C(0x5000)) {
        unsigned rd = insn & 7u;
        unsigned rn = (insn >> 3) & 7u;
        unsigned rm = (insn >> 6) & 7u;
        unsigned kind;
        unsigned operation = (insn >> 9) & 7u;
        if (!read_hits) return false;
        if (operation <= 2u) {
            if (!write_hits || index + 1u != insns) return false;
            kind = operation == 0u ? A64S_READ_WORD :
                   operation == 1u ? A64S_READ_HALF : A64S_READ_BYTE;
            thumb_emit_write_reg(out, kind, rd, rn, rm, pc_value);
        } else {
            switch (operation) {
            case 3u: kind = A64S_READ_SIGNED_BYTE; break;
            case 4u: kind = A64S_READ_WORD; break;
            case 5u: kind = A64S_READ_HALF; break;
            case 6u: kind = A64S_READ_BYTE; break;
            case 7u: kind = A64S_READ_SIGNED_HALF; break;
            default: return false;
            }
            thumb_emit_read_reg(out, kind, rd, rn, rm, pc_value);
        }
        *written = 3u;
        return true;
    }

    if ((insn & UINT16_C(0xe000)) == UINT16_C(0x6000)) {
        unsigned rd = insn & 7u;
        unsigned rn = (insn >> 3) & 7u;
        unsigned offset = (insn >> 6) & 31u;
        bool byte = (insn & (1u << 12)) != 0u;
        bool load = (insn & (1u << 11)) != 0u;
        if (!read_hits || (!load &&
            (!write_hits || index + 1u != insns))) return false;
        if (load)
            thumb_emit_read_imm(out,
                byte ? A64S_READ_BYTE : A64S_READ_WORD,
                rd, rn, byte ? offset : offset * 4u, pc_value);
        else
            thumb_emit_write_imm(out,
                byte ? A64S_READ_BYTE : A64S_READ_WORD,
                rd, rn, byte ? offset : offset * 4u, pc_value);
        *written = 2u;
        return true;
    }

    if ((insn & UINT16_C(0xf000)) == UINT16_C(0x8000)) {
        unsigned rd = insn & 7u;
        unsigned rn = (insn >> 3) & 7u;
        unsigned offset = ((insn >> 6) & 31u) * 2u;
        bool load = (insn & (1u << 11)) != 0u;
        if (!read_hits || (!load &&
            (!write_hits || index + 1u != insns))) return false;
        if (load)
            thumb_emit_read_imm(out, A64S_READ_HALF, rd, rn, offset,
                                pc_value);
        else
            thumb_emit_write_imm(out, A64S_READ_HALF, rd, rn, offset,
                                 pc_value);
        *written = 2u;
        return true;
    }

    if ((insn & UINT16_C(0xf000)) == UINT16_C(0x9000)) {
        unsigned rd = (insn >> 8) & 7u;
        bool load = (insn & UINT16_C(0x0800)) != 0u;
        if (read_hits) {
            if (!load && (!write_hits || index + 1u != insns)) return false;
            if (load)
                thumb_emit_read_imm(out, A64S_READ_WORD, rd, 13u,
                                    (uint32_t)(insn & 255u) * 4u, pc_value);
            else
                thumb_emit_write_imm(out, A64S_READ_WORD, rd, 13u,
                                     (uint32_t)(insn & 255u) * 4u,
                                     pc_value);
            *written = 2u;
            return true;
        }
        out->handler = (load ? A64S_LDR_SP : A64S_STR_SP) + rd;
        out->immediate = (uint32_t)(insn & 255u) * 4u;
        out->pc_value = pc_value;
        *written = 1u;
        return true;
    }

    if ((insn & UINT16_C(0xf000)) == UINT16_C(0xa000)) {
        unsigned rd = (insn >> 8) & 7u;
        bool sp = (insn & (1u << 11)) != 0u;
        out->handler = dp_imm(4u, false, rd, sp ? 13u : 15u);
        out->immediate = (uint32_t)(insn & 255u) * 4u;
        out->pc_value = sp ? pc_value : (pc_value & ~UINT32_C(3));
        *written = 1u;
        return true;
    }

    if ((insn & UINT16_C(0xff00)) == UINT16_C(0xb000)) {
        bool subtract = (insn & (1u << 7)) != 0u;
        out->handler = dp_imm(subtract ? 2u : 4u, false, 13u, 13u);
        out->immediate = (uint32_t)(insn & 127u) * 4u;
        out->pc_value = pc_value;
        *written = 1u;
        return true;
    }

    return false;
}

static bool decode_program_at(const void *program, unsigned insns, bool thumb,
                              uint32_t pc, bool guest_bytes, bool read_hits,
                              bool write_hits,
                              a64_static_block_t *out) {
    const uint8_t *bytes = (const uint8_t *)program;
    unsigned uop_count = 0u;
    uint32_t fallthrough;
    if (!program || !out || !insns || insns > A64_STATIC_MAX_INSNS ||
        (pc & (thumb ? 1u : 3u)) != 0u)
        return false;
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < insns; i++) {
        /* Decode out of line so a large block transfer cannot overrun the
         * final slots while a longer candidate is being shortened. One
         * instruction needs at most guard + preflight + sixteen commits +
         * finish records. */
        a64_static_uop_t decoded[19u] = {{0u, 0u, 0u, 0u}};
        bool ok;
        unsigned added = 0u;
        if (thumb) {
            const uint8_t *p = bytes + i * 2u;
            ok = decode_thumb(guest_bytes ? read_le16(p) : read_native16(p),
                              i, insns, pc, read_hits, write_hits,
                              decoded, &added);
            if (ok && read_hits)
                for (unsigned j = 0u; j < added; j++)
                    if (handler_touches_memory(
                            decoded[j].handler))
                        ok = false;
        } else {
            const uint8_t *p = bytes + i * 4u;
            ok = decode_arm(guest_bytes ? read_le32(p) : read_native32(p),
                            i, insns, pc, read_hits, write_hits,
                            decoded, &added);
        }
        if (!ok || !added || added > 19u ||
            added > A64_STATIC_MAX_UOPS - uop_count) {
            memset(out, 0, sizeof *out);
            return false;
        }
        memcpy(&out->uops[uop_count], decoded,
               added * sizeof out->uops[0]);
        for (unsigned j = 0u; j < added; j++) {
            uint32_t handler = out->uops[uop_count + j].handler;
            if (handler >= A64S_HANDLER_COUNT ||
                (i + 1u != insns && handler == A64S_END)) {
                memset(out, 0, sizeof *out);
                return false;
            }
            if (handler_is_terminal_branch(handler)) {
                if (i + 1u != insns || out->dynamic_exit) {
                    memset(out, 0, sizeof *out);
                    return false;
                }
                out->dynamic_exit = true;
                if (thumb && handler_is_conditional_branch(handler))
                    out->thumb_conditional_exit = true;
                if (handler_is_indirect_branch(handler))
                    out->indirect_exit = true;
            }
            if (handler_touches_memory(handler))
                out->touches_memory = true;
            if (handler_is_runtime_guarded(handler)) {
                out->runtime_guards = true;
                out->uops[uop_count + j].pc_value =
                    pc + i * (thumb ? 2u : 4u);
                out->uops[uop_count + j].metadata =
                    ((i + 1u) << 8) | (insns - i);
            }
            if (handler_is_direct_read(handler)) {
                out->touches_memory = true;
                out->direct_reads = true;
                if (handler_is_ldm_preflight(handler))
                    out->ldm_direct_reads = true;
            }
            if (handler_is_direct_write(handler)) {
                if (i + 1u != insns) {
                    memset(out, 0, sizeof *out);
                    return false;
                }
                out->touches_memory = true;
                out->direct_writes = true;
                if (handler_is_vfp_direct_write(handler))
                    out->vfp_direct_writes = true;
                if (handler_is_stm_finish(handler))
                    out->stm_direct_writes = true;
                if (handler_is_vstm_direct_write(handler))
                    out->vstm_direct_writes = true;
            }
            if (handler_is_vfp(handler)) out->vfp = true;
            if (handler_is_vfp_arithmetic(handler))
                out->vfp_arithmetic = true;
        }
        uop_count += added;
    }
    fallthrough = pc + insns * (thumb ? 2u : 4u);
    if (out->uops[uop_count - 1u].handler != A64S_END) {
        if (uop_count == A64_STATIC_MAX_UOPS) {
            memset(out, 0, sizeof *out);
            return false;
        }
        out->uops[uop_count].handler = A64S_END;
        out->uops[uop_count].immediate = fallthrough;
        uop_count++;
    }
    out->insn_count = insns;
    out->uop_count = uop_count;
    out->start_pc = pc;
    out->exit_pc = out->uops[uop_count - 1u].immediate;
    out->thumb = thumb;
    return true;
}

bool a64_static_decode_at(const void *program, unsigned insns, bool thumb,
                          uint32_t pc, a64_static_block_t *out) {
    return decode_program_at(program, insns, thumb, pc, false, false, false,
                             out);
}

bool a64_static_decode_bytes_at(const uint8_t *program, unsigned insns,
                                bool thumb, uint32_t pc,
                                a64_static_block_t *out) {
    return decode_program_at(program, insns, thumb, pc, true, false, false,
                             out);
}

bool a64_static_decode_read_hits_bytes_at(const uint8_t *program,
                                          unsigned insns, bool thumb,
                                          uint32_t pc,
                                          a64_static_block_t *out) {
    return decode_program_at(program, insns, thumb, pc, true, true, false,
                             out);
}

bool a64_static_decode_memory_hits_bytes_at(const uint8_t *program,
                                            unsigned insns, bool thumb,
                                            uint32_t pc,
                                            a64_static_block_t *out) {
    return decode_program_at(program, insns, thumb, pc, true, true, true,
                             out);
}

bool a64_static_decode(const void *program, unsigned insns, bool thumb,
                       a64_static_block_t *out) {
    return a64_static_decode_at(program, insns, thumb, 0u, out);
}

bool a64_static_host_available(void) {
#if defined(S5LBOX_STATIC_A64_NATIVE)
    return true;
#else
    return false;
#endif
}

static a64_compact_raw_admission_t compact_raw_classify_thumb(
        const arm_cpu_t *cpu, uint16_t insn) {
    unsigned top = insn >> 12;

    if ((insn & UINT16_C(0xf000)) == UINT16_C(0xd000)) {
        unsigned condition = (insn >> 8) & 15u;
        if (condition >= 14u) return A64_COMPACT_RAW_REJECT_THUMB;
        return arm_cond_passed(cpu, condition)
            ? A64_COMPACT_RAW_ADMIT_EXECUTE
            : A64_COMPACT_RAW_ADMIT_CONDITION_SKIP;
    }
    /* Thumb-1 long BL/BLX is represented as two separately retired
     * halfwords.  Both halves, plus the 0xe8xx BLX suffix, are exact live
     * semantics in the compact loop. */
    if (top >= 14u)
        return A64_COMPACT_RAW_ADMIT_EXECUTE;

    if ((insn & UINT16_C(0xff00)) == UINT16_C(0x4700)) {
        unsigned rm = (insn >> 3) & 15u;
        bool link = (insn & (1u << 7)) != 0u;
        uint32_t target;
        if (link && rm == 15u) return A64_COMPACT_RAW_REJECT_THUMB;
        target = rm == 15u ? cpu->r[15] + 4u : cpu->r[rm];
        return (target & 3u) == 2u
            ? A64_COMPACT_RAW_REJECT_THUMB
            : A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if ((insn & UINT16_C(0xf800)) < UINT16_C(0x1800) ||
        (insn & UINT16_C(0xf800)) == UINT16_C(0x1800) ||
        (insn & UINT16_C(0xe000)) == UINT16_C(0x2000) ||
        (insn & UINT16_C(0xfc00)) == UINT16_C(0x4000))
        return A64_COMPACT_RAW_ADMIT_EXECUTE;

    if ((insn & UINT16_C(0xfc00)) == UINT16_C(0x4400)) {
        unsigned operation = (insn >> 8) & 3u;
        unsigned rd = (insn & 7u) | ((insn >> 4) & 8u);
        if (operation == 3u || (operation != 1u && rd == 15u))
            return A64_COMPACT_RAW_REJECT_THUMB;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if ((insn & UINT16_C(0xf800)) == UINT16_C(0x4800))
        return A64_COMPACT_RAW_ADMIT_EXECUTE;

    if (top == 5u) {
        unsigned rb = (insn >> 3) & 7u;
        unsigned ro = (insn >> 6) & 7u;
        unsigned operation = (insn >> 9) & 7u;
        uint32_t address = cpu->r[rb] + cpu->r[ro];
        unsigned width = operation == 0u || operation == 4u ? 4u :
                         operation == 2u || operation == 3u ||
                         operation == 6u ? 1u : 2u;
        return width > 1u && (address & (width - 1u)) != 0u
            ? A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT
            : A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if (top == 6u || top == 7u) {
        unsigned rb = (insn >> 3) & 7u;
        unsigned offset = (insn >> 6) & 31u;
        bool byte = (insn & (1u << 12)) != 0u;
        uint32_t address = cpu->r[rb] + (byte ? offset : offset * 4u);
        return !byte && (address & 3u) != 0u
            ? A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT
            : A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if (top == 8u) {
        unsigned rb = (insn >> 3) & 7u;
        uint32_t address = cpu->r[rb] + ((uint32_t)((insn >> 6) & 31u) * 2u);
        return (address & 1u) != 0u
            ? A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT
            : A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if (top == 9u) {
        uint32_t address = cpu->r[13] + ((uint32_t)(insn & 255u) * 4u);
        return (address & 3u) != 0u
            ? A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT
            : A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if ((insn & UINT16_C(0xf600)) == UINT16_C(0xb400)) {
        const bool load = (insn & (1u << 11)) != 0u;
        const bool extra = (insn & (1u << 8)) != 0u;
        const uint32_t list = insn & UINT16_C(0xff);
        unsigned words = extra ? 1u : 0u;
        uint32_t start;

        if (list == 0u && !extra) return A64_COMPACT_RAW_REJECT_THUMB;
        for (unsigned reg = 0u; reg < 8u; reg++)
            if ((list & (UINT32_C(1) << reg)) != 0u) words++;
        start = load ? cpu->r[13] : cpu->r[13] - words * 4u;
        if ((start & 3u) != 0u ||
            (start & UINT32_C(0x3ff)) + words * 4u > 1024u)
            return A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if ((insn & UINT16_C(0xff00)) == UINT16_C(0xb200))
        return A64_COMPACT_RAW_ADMIT_EXECUTE;

    if (top == 12u) {
        const bool load = (insn & (1u << 11)) != 0u;
        const unsigned rb = (insn >> 8) & 7u;
        const uint32_t list = insn & UINT16_C(0xff);
        unsigned words = 0u;
        uint32_t start = cpu->r[rb];

        if (list == 0u) return A64_COMPACT_RAW_REJECT_THUMB;
        if (!load && (list & (UINT32_C(1) << rb)) != 0u) {
            const uint32_t lower = rb == 0u
                ? 0u : (UINT32_C(1) << rb) - 1u;
            if ((list & lower) != 0u)
                return A64_COMPACT_RAW_REJECT_THUMB;
        }
        for (unsigned reg = 0u; reg < 8u; reg++)
            if ((list & (UINT32_C(1) << reg)) != 0u) words++;
        if ((start & 3u) != 0u ||
            (start & UINT32_C(0x3ff)) + words * 4u > 1024u)
            return A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if (top == 10u ||
        (insn & UINT16_C(0xff00)) == UINT16_C(0xb000))
        return A64_COMPACT_RAW_ADMIT_EXECUTE;

    return A64_COMPACT_RAW_REJECT_THUMB;
}

static bool compact_raw_is_vfp_encoding(uint32_t insn) {
    return (insn & UINT32_C(0x0f000e10)) == UINT32_C(0x0e000a10) ||
           (insn & UINT32_C(0x0fe00e00)) == UINT32_C(0x0c400a00) ||
           (insn & UINT32_C(0x0e000e00)) == UINT32_C(0x0c000a00) ||
           (insn & UINT32_C(0x0f000e10)) == UINT32_C(0x0e000a00);
}

/* The resident loop can keep only system-coprocessor operations whose entire
 * architectural effect is local to already-published CPU state. CP14 reports
 * the deliberately absent debug unit as zero in privileged modes. CP15 c7 is
 * cache/barrier maintenance and therefore a no-op in this cacheless model,
 * except for the exact WFI encoding whose platform callback and time advance
 * remain owned by arm_step(). The three software thread-ID registers are
 * ordinary scalar state; MMU/TLB/control registers remain literal. */
static bool compact_raw_system_coprocessor_supported(
        const arm_cpu_t *cpu, uint32_t insn) {
    const bool priv =
        (cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    const bool load = (insn & (UINT32_C(1) << 20)) != 0u;
    const unsigned cp = (insn >> 8) & 15u;
    const unsigned crn = (insn >> 16) & 15u;
    const unsigned opc2 = (insn >> 5) & 7u;
    const unsigned crm = insn & 15u;

    if ((insn & UINT32_C(0x0f000010)) != UINT32_C(0x0e000010))
        return false;
    if (cp == 14u) return priv;
    if (cp != 15u) return false;
    if (crn == 7u) {
        const unsigned opc1 = (insn >> 21) & 7u;
        const bool wfi = !load && opc1 == 0u && opc2 == 4u && crm == 0u;
        return !wfi;
    }
    if (crn != 13u || opc2 < 2u || opc2 > 4u) return false;
    if (priv) return true;
    return crm == 0u &&
           (opc2 == 2u || (opc2 == 3u && load));
}

static bool compact_raw_vfp_simple32(uint32_t value) {
    const uint32_t exponent = (value >> 23) & UINT32_C(0xff);
    return exponent ? exponent != UINT32_C(0xff)
                    : (value << 1) == 0u;
}

static bool compact_raw_vfp_simple64(const uint32_t *words,
                                     unsigned first) {
    const uint64_t value = (uint64_t)words[first] |
                           ((uint64_t)words[first + 1u] << 32);
    const uint64_t exponent = (value >> 52) & UINT64_C(0x7ff);
    return exponent ? exponent != UINT64_C(0x7ff)
                    : (value << 1) == 0u;
}

static a64_compact_raw_admission_t compact_raw_classify_vfp(
        const arm_cpu_t *cpu, uint32_t insn) {
    a64_static_uop_t ops[2];
    uint32_t handler;
    unsigned written = 0u;
    const bool priv =
        (cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    const bool enabled = (cpu->vfp_fpexc & ARM_FPEXC_EN) != 0u;

    memset(ops, 0, sizeof ops);
    if (!decode_vfp_transfer(insn, cpu->r[15] + 8u, true, ops, &written) ||
        !written || written > 2u)
        return A64_COMPACT_RAW_REJECT_VFP;
    handler = ops[written - 1u].handler;
    if (!handler_is_vfp(handler) || !vfp_cpacr_permits(cpu))
        return A64_COMPACT_RAW_REJECT_VFP;

    if (handler >= A64S_VFP_VMRS_FPSID &&
        handler < A64S_VFP_VMRS_FPSCR) {
        if (!enabled && !priv) return A64_COMPACT_RAW_REJECT_VFP;
    } else if ((handler >= A64S_VFP_VMRS_FPEXC &&
                handler < A64S_VFP_VMRS_APSR) ||
               (handler >= A64S_VFP_VMSR_FPEXC &&
                handler < A64S_VFP_UNARY32)) {
        if (!priv) return A64_COMPACT_RAW_REJECT_VFP;
    } else if (!enabled) {
        return A64_COMPACT_RAW_REJECT_VFP;
    }

    /* The live loop owns exact bitwise/register, compare, widening, scalar
     * arithmetic and witnessed memory semantics. Arithmetic admits only the
     * replay-proven RunFast mode plus signed-zero/finite-normal inputs; the
     * native loop separately validates intermediate/results and restores its
     * lazy host-FP session before every exit or callback. */
    if (handler >= A64S_VFP_UNARY32 &&
        handler < A64S_VFP_COMPARE32) {
        return (cpu->vfp_fpscr &
                (ARM_FPSCR_STRIDE | ARM_FPSCR_LEN)) == 0u
            ? A64_COMPACT_RAW_ADMIT_EXECUTE
            : A64_COMPACT_RAW_REJECT_VFP;
    }
    if (handler == A64S_VFP_NARROW64) {
        const uint32_t immediate = ops[written - 1u].immediate;
        const unsigned rm = (immediate >> 8) & 255u;
        if ((cpu->vfp_fpscr & UINT32_C(0x03c79f00)) !=
                UINT32_C(0x03000000) ||
            (cpu->vfp_fpscr & UINT32_C(0x9f)) != UINT32_C(0x10) ||
            !compact_raw_vfp_simple64(cpu->vfp_s, rm))
            return A64_COMPACT_RAW_REJECT_VFP;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }
    if (handler >= A64S_VFP_COMPARE32 &&
        handler < A64S_VFP_ARITH32) {
        return (cpu->vfp_fpscr &
                (ARM_FPSCR_LEN | ARM_FPSCR_ENABLES)) == 0u
            ? A64_COMPACT_RAW_ADMIT_EXECUTE
            : A64_COMPACT_RAW_REJECT_VFP;
    }
    if (handler_is_vfp_arithmetic(handler)) {
        const uint32_t immediate = ops[written - 1u].immediate;
        const unsigned rd = immediate & 255u;
        const unsigned rn = (immediate >> 8) & 255u;
        const unsigned rm = (immediate >> 16) & 255u;
        const bool dbl = handler >= A64S_VFP_ARITH64;
        const unsigned operation = dbl
            ? handler - A64S_VFP_ARITH64
            : handler - A64S_VFP_ARITH32;

        if ((cpu->vfp_fpscr & UINT32_C(0x03c79f00)) !=
                UINT32_C(0x03000000) ||
            (cpu->vfp_fpscr & UINT32_C(0x9f)) != UINT32_C(0x10))
            return A64_COMPACT_RAW_REJECT_VFP;
        if (dbl) {
            if (!compact_raw_vfp_simple64(cpu->vfp_s, rn) ||
                !compact_raw_vfp_simple64(cpu->vfp_s, rm) ||
                (operation < 4u &&
                 !compact_raw_vfp_simple64(cpu->vfp_s, rd)))
                return A64_COMPACT_RAW_REJECT_VFP;
        } else if (!compact_raw_vfp_simple32(cpu->vfp_s[rn]) ||
                   !compact_raw_vfp_simple32(cpu->vfp_s[rm]) ||
                   (operation < 4u &&
                    !compact_raw_vfp_simple32(cpu->vfp_s[rd]))) {
            return A64_COMPACT_RAW_REJECT_VFP;
        }
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }
    if ((handler >= A64S_VFP_DIRECT_READ32 &&
         handler <= A64S_VFP_DIRECT_WRITE64) ||
        handler_is_vstm_direct_write(handler)) {
        const bool pre = (insn & (1u << 24)) != 0u;
        const bool up = (insn & (1u << 23)) != 0u;
        const bool writeback = (insn & (1u << 21)) != 0u;
        const unsigned rn = (insn >> 16) & 15u;
        const unsigned words = insn & 255u;
        uint32_t base = rn == 15u ? cpu->r[15] + 8u : cpu->r[rn];
        uint32_t address;

        if (pre && !writeback) {
            address = up ? base + words * 4u : base - words * 4u;
            return (address & 3u) == 0u
                ? A64_COMPACT_RAW_ADMIT_EXECUTE
                : A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT;
        }
        address = pre ? base - words * 4u : base;
        if ((address & 3u) != 0u ||
            (address & UINT32_C(0x3ff)) + words * 4u > 1024u)
            return A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }
    return A64_COMPACT_RAW_ADMIT_EXECUTE;
}

a64_compact_raw_admission_t a64_compact_raw_classify_instruction(
        const arm_cpu_t *cpu, uint32_t insn, bool thumb) {
    unsigned condition;

    if (!cpu) return A64_COMPACT_RAW_REJECT_CLASS;
    if (thumb) return compact_raw_classify_thumb(cpu, (uint16_t)insn);

    condition = insn >> 28;
    if (condition == 15u) return A64_COMPACT_RAW_REJECT_NV;
    if (condition != 14u && !arm_cond_passed(cpu, condition))
        return A64_COMPACT_RAW_ADMIT_CONDITION_SKIP;

    /* B/BL is selected before the broad bits[27:26] data-processing class in
     * the generated loop. */
    if (((insn >> 25) & 7u) == 5u)
        return A64_COMPACT_RAW_ADMIT_EXECUTE;

    if ((insn & UINT32_C(0x0ffffff0)) == UINT32_C(0x012fff10) ||
        (insn & UINT32_C(0x0ffffff0)) == UINT32_C(0x012fff30)) {
        const bool link = (insn & UINT32_C(0x20)) != 0u;
        const unsigned rm = insn & 15u;
        const uint32_t target = rm == 15u ? cpu->r[15] + 8u
                                          : cpu->r[rm];
        if ((link && rm == 15u) || (target & 3u) == 2u)
            return A64_COMPACT_RAW_REJECT_CLASS;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if ((insn & UINT32_C(0x0e000000)) == UINT32_C(0x08000000)) {
        const bool pre = (insn & (1u << 24)) != 0u;
        const bool up = (insn & (1u << 23)) != 0u;
        const bool user_bank = (insn & (1u << 22)) != 0u;
        const bool writeback = (insn & (1u << 21)) != 0u;
        const bool load = (insn & (1u << 20)) != 0u;
        const unsigned rn = (insn >> 16) & 15u;
        const uint32_t list = insn & UINT32_C(0xffff);
        unsigned words = 0u;
        uint32_t base;
        uint32_t start;

        if (user_bank || rn == 15u || list == 0u)
            return A64_COMPACT_RAW_REJECT_MEMORY_FORM;
        if (writeback && (list & (UINT32_C(1) << rn)) != 0u) {
            const uint32_t lower = rn == 0u
                ? 0u : (UINT32_C(1) << rn) - 1u;
            if (load || (list & lower) != 0u)
                return A64_COMPACT_RAW_REJECT_MEMORY_FORM;
        }
        for (unsigned reg = 0u; reg < 16u; reg++)
            if ((list & (UINT32_C(1) << reg)) != 0u) words++;
        base = cpu->r[rn];
        if (up)
            start = pre ? base + 4u : base;
        else
            start = pre ? base - words * 4u
                        : base - words * 4u + 4u;
        if ((start & 3u) != 0u ||
            (start & UINT32_C(0x3ff)) + words * 4u > 1024u)
            return A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if (compact_raw_system_coprocessor_supported(cpu, insn))
        return A64_COMPACT_RAW_ADMIT_EXECUTE;

    if (compact_raw_is_vfp_encoding(insn))
        return compact_raw_classify_vfp(cpu, insn);

    if ((insn & UINT32_C(0x0fc000f0)) == UINT32_C(0x00000090) ||
        (insn & UINT32_C(0x0f8000f0)) == UINT32_C(0x00800090)) {
        const bool wide = (insn & (1u << 23)) != 0u;
        const unsigned hi = (insn >> 16) & 15u;
        const unsigned lo = (insn >> 12) & 15u;
        const unsigned rs = (insn >> 8) & 15u;
        const unsigned rm = insn & 15u;

        /* The word forms swap Rd/Rn relative to ordinary DP. Keep PC in
         * any field literal, even the unused accumulator field of MUL. */
        if (hi == 15u || lo == 15u || rs == 15u || rm == 15u)
            return A64_COMPACT_RAW_REJECT_DP_PC;
        if (wide && hi == lo)
            return A64_COMPACT_RAW_REJECT_DP_REGISTER_SHIFT;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if ((insn & UINT32_C(0x0e000090)) == UINT32_C(0x00000090) &&
        (insn & UINT32_C(0x60)) != 0u) {
        const bool pre = (insn & (1u << 24)) != 0u;
        const bool up = (insn & (1u << 23)) != 0u;
        const bool immediate = (insn & (1u << 22)) != 0u;
        const bool write = (insn & (1u << 21)) != 0u;
        const bool load = (insn & (1u << 20)) != 0u;
        const unsigned rn = (insn >> 16) & 15u;
        const unsigned rd = (insn >> 12) & 15u;
        const unsigned sh = (insn >> 5) & 3u;
        uint32_t offset, base, address;

        /* LDRD/STRD are still literal; L is not their load/store selector. */
        if (!load && sh != 1u)
            return A64_COMPACT_RAW_REJECT_MEMORY_FORM;
        if (rd == 15u || ((!pre || write) && rn == 15u))
            return A64_COMPACT_RAW_REJECT_MEMORY_PC;
        if ((!pre && write) || ((!pre || write) && rn == rd))
            return A64_COMPACT_RAW_REJECT_MEMORY_FORM;
        if (immediate) {
            offset = ((insn >> 4) & UINT32_C(0xf0)) | (insn & 15u);
        } else {
            if ((insn & UINT32_C(0xf00)) != 0u)
                return A64_COMPACT_RAW_REJECT_MEMORY_FORM;
            if ((insn & 15u) == 15u)
                return A64_COMPACT_RAW_REJECT_MEMORY_PC;
            offset = cpu->r[insn & 15u];
        }
        base = rn == 15u ? cpu->r[15] + 8u : cpu->r[rn];
        address = pre ? (up ? base + offset : base - offset) : base;
        if (sh != 2u && (address & 1u) != 0u)
            return A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if (((insn >> 26) & 3u) == 0u) {
        unsigned opcode = (insn >> 21) & 15u;
        unsigned rn = (insn >> 16) & 15u;
        unsigned rd = (insn >> 12) & 15u;

        if (rn == 15u || rd == 15u)
            return A64_COMPACT_RAW_REJECT_DP_PC;
        if ((insn & (1u << 25)) == 0u) {
            if (insn & (1u << 4)) {
                const unsigned rs = (insn >> 8) & 15u;

                /* A true register-specified data-processing shift has bit 7
                 * clear.  The bit7+bit4 space belongs to multiply, extra
                 * load/store and synchronization encodings.  ARMv6 also
                 * forbids PC in Rs just as it does in Rn/Rd/Rm. */
                if ((insn & (1u << 7)) != 0u)
                    return A64_COMPACT_RAW_REJECT_DP_REGISTER_SHIFT;
                if (rs == 15u)
                    return A64_COMPACT_RAW_REJECT_DP_PC;
            }
            if ((insn & 15u) == 15u)
                return A64_COMPACT_RAW_REJECT_DP_RM_PC;
        }
        if ((insn & (1u << 20)) == 0u && opcode >= 8u && opcode <= 11u)
            return A64_COMPACT_RAW_REJECT_DP_TEST_WITHOUT_S;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    if (((insn >> 26) & 3u) == 1u) {
        const bool indexed = (insn & (1u << 25)) != 0u;
        const bool pre = (insn & (1u << 24)) != 0u;
        const bool up = (insn & (1u << 23)) != 0u;
        const bool byte = (insn & (1u << 22)) != 0u;
        const bool write = (insn & (1u << 21)) != 0u;
        const bool load = (insn & (1u << 20)) != 0u;
        const bool writeback = !pre || write;
        const unsigned rn = (insn >> 16) & 15u;
        const unsigned rd = (insn >> 12) & 15u;
        uint32_t offset;
        uint32_t base;
        uint32_t address;

        if (writeback && (rn == 15u || rn == rd))
            return (rn == 15u || rd == 15u)
                ? A64_COMPACT_RAW_REJECT_MEMORY_PC
                : A64_COMPACT_RAW_REJECT_MEMORY_FORM;
        if (byte && rd == 15u)
            return A64_COMPACT_RAW_REJECT_MEMORY_PC;
        if (load && !pre && write && rd == 15u)
            return A64_COMPACT_RAW_REJECT_MEMORY_PC;
        if (!indexed) {
            offset = insn & UINT32_C(0xfff);
        } else {
            const unsigned rm = insn & 15u;
            const unsigned type = (insn >> 5) & 3u;
            const unsigned amount = (insn >> 7) & 31u;
            uint32_t value;

            if ((insn & (1u << 4)) != 0u)
                return A64_COMPACT_RAW_REJECT_MEMORY_FORM;
            if (rm == 15u)
                return A64_COMPACT_RAW_REJECT_MEMORY_PC;
            value = cpu->r[rm];
            switch (type) {
            case 0u:
                offset = amount ? value << amount : value;
                break;
            case 1u:
                offset = amount ? value >> amount : 0u;
                break;
            case 2u:
                offset = amount
                    ? (uint32_t)((int32_t)value >> amount)
                    : ((value & UINT32_C(0x80000000)) ? UINT32_MAX : 0u);
                break;
            default:
                offset = amount ? ror32(value, amount)
                    : ((cpu->cpsr & ARM_CPSR_C) ? UINT32_C(0x80000000)
                                                : 0u) |
                      (value >> 1);
                break;
            }
        }
        base = rn == 15u ? cpu->r[15] + 8u : cpu->r[rn];
        address = pre ? (up ? base + offset : base - offset) : base;
        if (!byte && (address & 3u) != 0u)
            return A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT;
        return A64_COMPACT_RAW_ADMIT_EXECUTE;
    }

    return A64_COMPACT_RAW_REJECT_CLASS;
}

#if defined(S5LBOX_STATIC_A64_NATIVE)
enum { A64_COMPACT_RAW_WINDOW_CACHE_ENTRIES = 8u };

typedef struct {
    uint8_t *flat_ram;
    uint64_t flat_mask;
    const void *dread;
    const void *dwrite;
    uint64_t *dread_hits;
    uint64_t *dwrite_hits;
    a64_compact_raw_fallback_fn fallback;
    void *fallback_opaque;
    uint64_t native_retired;
    uint64_t fallback_retired;
    uint32_t tlb_gen;
    uint32_t priv_tag;
    a64_compact_raw_code_window_t next_window;
    uint32_t *vfp_s;
    uint32_t *vfp_fpexc;
    uint32_t *vfp_fpscr;
    uint32_t vfp_access;
    uint32_t vfp_host_active;
    uint64_t vfp_host_fpcr;
    uint64_t vfp_host_fpsr;
    arm_cp15_t *cp15;
    uint64_t window_cache_hits;
    uint32_t window_cache_next;
    uint32_t window_cache_enabled;
    a64_compact_raw_code_window_t current_window;
    a64_compact_raw_code_window_t
        window_cache[A64_COMPACT_RAW_WINDOW_CACHE_ENTRIES];
    a64_compact_raw_data_miss_t data_miss;
    arm_cpu_t *bulk_cpu;
    a64_compact_bulk_stats_t bulk_stats;
    const arm_ram_window_t *ram_window;
    const void *tlb;
    uint64_t *tlb_hits;
    void *fetch_cache;
    uint32_t *owner_fetch_block;
    a64_compact_tlb_stats_t tlb_stats;
    arm_cpu_t *tlb_cpu;
    arm_cp15_t tlb_cp15;
    a64_compact_raw_fallback_fn guarded_fallback;
    void *guarded_opaque;
    arm_ram_map_t *ram_map;
    a64_compact_ram_map_stats_t ram_map_stats;
} a64_compact_raw_context_t;

_Static_assert(sizeof(void *) == 8u,
               "compact raw native context requires AArch64 pointers");
_Static_assert(sizeof(a64_compact_raw_code_window_t) == 16u &&
                   offsetof(a64_compact_raw_code_window_t, code) == 0u &&
                   offsetof(a64_compact_raw_code_window_t, code_base) == 8u &&
                   offsetof(a64_compact_raw_code_window_t, code_bytes) == 12u,
               "compact raw code-window layout drifted");
_Static_assert(offsetof(a64_compact_raw_context_t, flat_ram) == 0u &&
                   offsetof(a64_compact_raw_context_t, flat_mask) == 8u &&
                   offsetof(a64_compact_raw_context_t, dread) == 16u &&
                   offsetof(a64_compact_raw_context_t, dwrite) == 24u &&
                   offsetof(a64_compact_raw_context_t, dread_hits) == 32u &&
                   offsetof(a64_compact_raw_context_t, dwrite_hits) == 40u &&
                   offsetof(a64_compact_raw_context_t, fallback) == 48u &&
                   offsetof(a64_compact_raw_context_t, fallback_opaque) == 56u &&
                   offsetof(a64_compact_raw_context_t, native_retired) == 64u &&
                   offsetof(a64_compact_raw_context_t, fallback_retired) == 72u &&
                   offsetof(a64_compact_raw_context_t, tlb_gen) == 80u &&
                   offsetof(a64_compact_raw_context_t, priv_tag) == 84u &&
                   offsetof(a64_compact_raw_context_t, next_window) == 88u &&
                   offsetof(a64_compact_raw_context_t, vfp_s) == 104u &&
                   offsetof(a64_compact_raw_context_t, vfp_fpexc) == 112u &&
                   offsetof(a64_compact_raw_context_t, vfp_fpscr) == 120u &&
                   offsetof(a64_compact_raw_context_t, vfp_access) == 128u &&
                   offsetof(a64_compact_raw_context_t, vfp_host_active) == 132u &&
                   offsetof(a64_compact_raw_context_t, vfp_host_fpcr) == 136u &&
                   offsetof(a64_compact_raw_context_t, vfp_host_fpsr) == 144u &&
                   offsetof(a64_compact_raw_context_t, cp15) == 152u &&
                   offsetof(a64_compact_raw_context_t,
                            window_cache_hits) == 160u &&
                   offsetof(a64_compact_raw_context_t,
                            window_cache_next) == 168u &&
                   offsetof(a64_compact_raw_context_t,
                            window_cache_enabled) == 172u &&
                   offsetof(a64_compact_raw_context_t,
                            current_window) == 176u &&
                   offsetof(a64_compact_raw_context_t,
                            window_cache) == 192u &&
                   offsetof(a64_compact_raw_context_t,
                            data_miss) == 320u &&
                   offsetof(a64_compact_raw_context_t, bulk_cpu) == 344u &&
                   offsetof(a64_compact_raw_context_t, bulk_stats) == 352u &&
                   offsetof(a64_compact_raw_context_t, ram_window) == 368u &&
                   offsetof(a64_compact_raw_context_t, tlb) == 376u &&
                   offsetof(a64_compact_raw_context_t, tlb_hits) == 384u &&
                   offsetof(a64_compact_raw_context_t, fetch_cache) == 392u &&
                   offsetof(a64_compact_raw_context_t,
                            owner_fetch_block) == 400u &&
                   offsetof(a64_compact_raw_context_t, tlb_stats) == 408u &&
                   offsetof(a64_compact_raw_context_t, ram_map) == 520u &&
                   offsetof(a64_compact_raw_context_t, ram_map_stats) == 528u &&
                   sizeof(a64_compact_raw_context_t) == 552u,
               "compact raw native context layout drifted");
_Static_assert(ARM_RAM_MAP_ENTRIES == 4096u &&
                   sizeof(arm_ram_map_entry_t) == 16u &&
                   offsetof(arm_ram_map_entry_t, host) == 0u &&
                   offsetof(arm_ram_map_entry_t, key) == 8u &&
                   offsetof(arm_ram_map_t, entries) == 0u &&
                   ARM_ACCESS_READ == 0 && ARM_ACCESS_WRITE == 1 &&
                   ARM_ACCESS_FETCH == 2 &&
                   offsetof(a64_compact_ram_map_stats_t, read) == 0u &&
                   offsetof(a64_compact_ram_map_stats_t, write) == 8u &&
                   offsetof(a64_compact_ram_map_stats_t, fetch) == 16u &&
                   sizeof(a64_compact_ram_map_stats_t) == 24u,
               "persistent native RAM map layout drifted");
_Static_assert(offsetof(arm_ram_window_t, read_host) == 104u &&
                   offsetof(arm_ram_window_t, write_host) == 112u &&
                   offsetof(arm_ram_window_t, base) == 120u &&
                   offsetof(arm_ram_window_t, bytes) == 124u &&
                   sizeof(arm_ram_window_t) == 128u,
               "compact raw RAM capability layout drifted");
_Static_assert(ARM_TLB_ENTRIES == 4096u && ARM_DREAD_ENTRIES == 64u &&
                   sizeof(((arm_cpu_t *)0)->tlb[0]) == 16u &&
                   offsetof(arm_cpu_t, tlb[0].tag) -
                       offsetof(arm_cpu_t, tlb[0]) == 4u &&
                   offsetof(arm_cpu_t, tlb[0].pa) -
                       offsetof(arm_cpu_t, tlb[0]) == 8u &&
                   offsetof(arm_cpu_t, tlb[0].fsr) -
                       offsetof(arm_cpu_t, tlb[0]) == 12u &&
                   offsetof(arm_cpu_t, fetch_blk) -
                       offsetof(arm_cpu_t, fetch_host) == 8u &&
                   offsetof(arm_cpu_t, fetch_gen) -
                       offsetof(arm_cpu_t, fetch_host) == 12u &&
                   offsetof(arm_cpu_t, fetch_priv) -
                       offsetof(arm_cpu_t, fetch_host) == 16u,
               "compact raw TLB/FETCH layout drifted");
_Static_assert(offsetof(arm_cp15_t, tpidrurw) == 52u &&
                   offsetof(arm_cp15_t, tpidruro) == 56u &&
                   offsetof(arm_cp15_t, tpidrprw) == 60u &&
                   sizeof(arm_cp15_t) == 64u,
               "compact raw CP15 thread-ID layout drifted");

extern int a64_static_execute(uint32_t *regs, uint32_t *cpsr,
                              uint64_t *cycles,
                              const a64_static_uop_t *uops,
                              uint64_t blocks, uint8_t *ram,
                              uint64_t ram_mask, uint64_t block_insns,
                              const a64_static_read_context_t *read_context,
                              void *chain_context);
extern uint32_t a64_compact_raw_execute(uint32_t *regs, uint32_t *cpsr,
                                        uint64_t *cycles,
                                        const uint8_t *code,
                                        uint32_t code_base,
                                        uint32_t code_bytes,
                                        uint32_t max_insns,
                                        a64_compact_raw_context_t *context);

/* Called only from the three candidate instruction shapes in signed text.
 * The helper sees the live PC and full FETCH witness; it neither observes nor
 * commits the runner's pending cycles. Its returned exact prefix is retired
 * by that runner through the same budget/device boundary as ordinary code. */
unsigned a64_compact_raw_bulk_try(a64_compact_raw_context_t *context,
                                  const uint8_t *code, uint32_t code_base,
                                  uint32_t code_bytes, unsigned budget,
                                  uint32_t pc) {
    if (!context->bulk_cpu) return 0u;
    arm_bulk_memory_t memory = {
        .code = code, .code_base = code_base, .code_bytes = code_bytes,
        .data_cache = context->dread != NULL,
    };
    context->bulk_cpu->r[15] = pc;
    unsigned n = arm_bulk_string_try(context->bulk_cpu, &memory, budget);
    if (n) {
        context->bulk_stats.calls++;
        context->bulk_stats.retired += n;
    }
    return n;
}

/* Native User instructions cannot change translation control or install bus
 * observers. An interpreter callback can. Check once at that mutation
 * boundary, not on every native load: a rejected continuation retains its
 * exact retirement result and returns to the machine-owned device boundary. */
static a64_compact_raw_fallback_result_t compact_tlb_guarded_fallback(
        void *opaque, a64_compact_raw_code_window_t *next_window,
        const a64_compact_raw_data_miss_t *data_miss) {
    a64_compact_raw_context_t *context = opaque;
    a64_compact_raw_fallback_result_t result = context->guarded_fallback(
        context->guarded_opaque, next_window, data_miss);
    if (result != A64_COMPACT_RAW_FALLBACK_RETIRE_CONTINUE &&
        result != A64_COMPACT_RAW_FALLBACK_NO_RETIRE_CONTINUE) return result;
    const arm_cpu_t *cpu = context->tlb_cpu;
    const arm_cp15_t *saved = &context->tlb_cp15;
    if ((!context->ram_map || arm_ram_map_current(context->ram_map, cpu)) &&
        arm_ram_window_current(context->ram_window, cpu) &&
        cpu->tlb_gen == context->tlb_gen &&
        cpu->cp15.sctlr == saved->sctlr &&
        cpu->cp15.ttbr0 == saved->ttbr0 &&
        cpu->cp15.ttbr1 == saved->ttbr1 &&
        cpu->cp15.ttbcr == saved->ttbcr &&
        cpu->cp15.dacr == saved->dacr &&
        cpu->cp15.context_id == saved->context_id &&
        cpu->cp15.cpacr == saved->cpacr &&
        cpu->tlb_stamp.sctlr == saved->sctlr &&
        cpu->tlb_stamp.ttbr0 == saved->ttbr0 &&
        cpu->tlb_stamp.ttbr1 == saved->ttbr1 &&
        cpu->tlb_stamp.ttbcr == saved->ttbcr &&
        cpu->tlb_stamp.dacr == saved->dacr &&
        cpu->tlb_stamp.context_id == saved->context_id &&
        (cpu->cpsr & (ARM_CPSR_MODE_MASK | ARM_CPSR_E)) == ARM_MODE_USR &&
        !cpu->abort_pending &&
        !(cpu->fiq_line && !(cpu->cpsr & ARM_CPSR_F)) &&
        !(cpu->irq_line && !(cpu->cpsr & ARM_CPSR_I))) return result;
    memset(next_window, 0, sizeof *next_window);
    return result == A64_COMPACT_RAW_FALLBACK_RETIRE_CONTINUE
        ? A64_COMPACT_RAW_FALLBACK_RETIRE_STOP
        : A64_COMPACT_RAW_FALLBACK_NO_RETIRE;
}
#endif

#if defined(S5LBOX_STATIC_A64_NATIVE) && defined(__APPLE__) && \
    (defined(__aarch64__) || defined(__arm64__))
/* These symbols alias existing instruction addresses in the generated signed
 * text. They are objects only for address arithmetic here; no data is read
 * through them and no executable page is ever modified. */
extern const unsigned char a64_compact_raw_profile_entry[];
extern const unsigned char a64_compact_raw_profile_dp[];
extern const unsigned char a64_compact_raw_profile_memory[];
extern const unsigned char a64_compact_raw_profile_block_control[];
extern const unsigned char a64_compact_raw_profile_system[];
extern const unsigned char a64_compact_raw_profile_vfp[];
extern const unsigned char a64_compact_raw_profile_thumb_decode[];
extern const unsigned char a64_compact_raw_profile_thumb_low_alu[];
extern const unsigned char a64_compact_raw_profile_thumb_alu_high[];
extern const unsigned char a64_compact_raw_profile_thumb_memory_form[];
extern const unsigned char a64_compact_raw_profile_thumb_misc[];
extern const unsigned char a64_compact_raw_profile_thumb_branch[];
extern const unsigned char a64_compact_raw_profile_thumb_memory_access[];
extern const unsigned char a64_compact_raw_profile_thumb_condition[];
extern const unsigned char a64_compact_raw_profile_a32_condition[];
extern const unsigned char a64_compact_raw_profile_retire[];
extern const unsigned char a64_compact_raw_profile_fallback[];
extern const unsigned char a64_compact_raw_profile_exit[];
extern const unsigned char a64_compact_raw_profile_end[];
extern const unsigned char a64_compact_raw_profile_guest_live_begin[];
extern const unsigned char a64_compact_raw_profile_guest_live_end[];

static const unsigned char *const g_compact_profile_boundary[] = {
    a64_compact_raw_profile_entry,
    a64_compact_raw_profile_dp,
    a64_compact_raw_profile_memory,
    a64_compact_raw_profile_block_control,
    a64_compact_raw_profile_system,
    a64_compact_raw_profile_vfp,
    a64_compact_raw_profile_thumb_decode,
    a64_compact_raw_profile_thumb_low_alu,
    a64_compact_raw_profile_thumb_alu_high,
    a64_compact_raw_profile_thumb_memory_form,
    a64_compact_raw_profile_thumb_misc,
    a64_compact_raw_profile_thumb_branch,
    a64_compact_raw_profile_thumb_memory_access,
    a64_compact_raw_profile_thumb_condition,
    a64_compact_raw_profile_a32_condition,
    a64_compact_raw_profile_retire,
    a64_compact_raw_profile_fallback,
    a64_compact_raw_profile_exit,
    a64_compact_raw_profile_end,
};

_Static_assert(sizeof g_compact_profile_boundary /
                       sizeof g_compact_profile_boundary[0] ==
                   A64_COMPACT_RAW_PC_PROFILE_REGION_COUNT + 1u,
               "compact PC-profile boundary count drifted");

static atomic_bool g_compact_profile_enabled = ATOMIC_VAR_INIT(false);
static atomic_bool g_compact_profile_active = ATOMIC_VAR_INIT(false);
static atomic_uint g_compact_profile_generation = ATOMIC_VAR_INIT(0u);
static atomic_uint g_compact_profile_target_thread =
    ATOMIC_VAR_INIT(MACH_PORT_NULL);
static pthread_mutex_t g_compact_profile_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_compact_profile_sampler_started;
static uint64_t g_compact_profile_polls;
static uint64_t g_compact_profile_not_running;
static uint64_t g_compact_profile_state_failures;
static uint64_t g_compact_profile_target_races;
static uint64_t g_compact_profile_samples;
static uint64_t g_compact_profile_outside;
static uint64_t
    g_compact_profile_region[A64_COMPACT_RAW_PC_PROFILE_REGION_COUNT];

_Static_assert(sizeof(uintptr_t) == sizeof(uint64_t),
               "compact PC-profile storage requires 64-bit pointers");

/* Only the marker-created sampler thread writes these counters and the
 * snapshot path reads them under g_compact_profile_lock. No code touches the
 * storage unless the explicit profile marker is enabled, so marker-free runs
 * still pay no initialization, thread, timer, signal, or ranking work. */
static native_pc_histogram_t g_compact_profile_outside_histogram;
static native_pc_histogram_t g_compact_profile_guest_histogram;
static native_pc_histogram_t g_compact_profile_host_histogram;
static uint64_t g_compact_profile_guest_unavailable;

static void compact_profile_increment(uint64_t *value) {
    if (*value != UINT64_MAX) (*value)++;
}

static void compact_profile_reset_locked(void) {
    g_compact_profile_polls = 0;
    g_compact_profile_not_running = 0;
    g_compact_profile_state_failures = 0;
    g_compact_profile_target_races = 0;
    g_compact_profile_samples = 0;
    g_compact_profile_outside = 0;
    for (unsigned i = 0u;
         i < (unsigned)A64_COMPACT_RAW_PC_PROFILE_REGION_COUNT; i++)
        g_compact_profile_region[i] = 0;
    native_pc_histogram_reset(&g_compact_profile_outside_histogram);
    native_pc_histogram_reset(&g_compact_profile_guest_histogram);
    native_pc_histogram_reset(&g_compact_profile_host_histogram);
    g_compact_profile_guest_unavailable = 0u;
}

static void compact_profile_capture_outside_pc_locked(uintptr_t pc) {
    (void)native_pc_histogram_note(&g_compact_profile_outside_histogram, pc);
}

static bool compact_profile_hot_before(
        uint64_t samples, uintptr_t pc,
        const a64_compact_raw_pc_profile_hot_t *current) {
    return samples > current->samples ||
           (samples == current->samples && samples != 0u &&
            pc < current->pc);
}

static void compact_profile_rank(
        const native_pc_histogram_t *histogram,
        a64_compact_raw_pc_profile_hot_t *hot, unsigned hot_count) {
    for (unsigned i = 0u; i < NATIVE_PC_HISTOGRAM_CAPACITY; i++) {
        const native_pc_histogram_bucket_t *bucket =
            &histogram->bucket[i];
        uint64_t samples = bucket->samples;
        uintptr_t pc = bucket->pc;
        if (!samples) continue;
        for (unsigned rank = 0u;
             rank < hot_count; rank++) {
            if (!compact_profile_hot_before(
                    samples, pc, &hot[rank]))
                continue;
            for (unsigned move = hot_count - 1u;
                 move > rank; move--)
                hot[move] = hot[move - 1u];
            hot[rank].pc = pc;
            hot[rank].samples = samples;
            break;
        }
    }
}

static bool compact_profile_layout_valid(void) {
    uintptr_t previous = (uintptr_t)g_compact_profile_boundary[0];
    if (!previous) return false;
    for (unsigned i = 1u;
         i <= (unsigned)A64_COMPACT_RAW_PC_PROFILE_REGION_COUNT; i++) {
        uintptr_t current = (uintptr_t)g_compact_profile_boundary[i];
        if (current <= previous) return false;
        previous = current;
    }
    return (uintptr_t)a64_compact_raw_profile_guest_live_begin >
               (uintptr_t)a64_compact_raw_profile_entry &&
           (uintptr_t)a64_compact_raw_profile_guest_live_begin <
               (uintptr_t)a64_compact_raw_profile_dp &&
           (uintptr_t)a64_compact_raw_profile_guest_live_end >
               (uintptr_t)a64_compact_raw_profile_exit &&
           (uintptr_t)a64_compact_raw_profile_guest_live_end <
               (uintptr_t)a64_compact_raw_profile_end;
}

static void compact_profile_sample_pc_locked(uintptr_t pc) {
    compact_profile_increment(&g_compact_profile_samples);
    (void)native_pc_histogram_note(&g_compact_profile_host_histogram, pc);

    const uintptr_t begin =
        (uintptr_t)g_compact_profile_boundary[0];
    const uintptr_t end = (uintptr_t)g_compact_profile_boundary[
        A64_COMPACT_RAW_PC_PROFILE_REGION_COUNT];
    if (pc < begin || pc >= end) {
        compact_profile_increment(&g_compact_profile_outside);
        compact_profile_capture_outside_pc_locked(pc);
        return;
    }
    for (unsigned i = 0u;
         i < (unsigned)A64_COMPACT_RAW_PC_PROFILE_REGION_COUNT; i++) {
        if (pc < (uintptr_t)g_compact_profile_boundary[i + 1u]) {
            compact_profile_increment(&g_compact_profile_region[i]);
            return;
        }
    }
    compact_profile_increment(&g_compact_profile_outside);
}

typedef enum {
    COMPACT_PROFILE_SAMPLE_CAPTURED = 0,
    COMPACT_PROFILE_SAMPLE_NOT_RUNNING,
    COMPACT_PROFILE_SAMPLE_STATE_FAILURE,
    COMPACT_PROFILE_SAMPLE_TARGET_RACE
} compact_profile_sample_result_t;

static bool compact_profile_target_matches(mach_port_t target) {
    return atomic_load_explicit(&g_compact_profile_active,
                                memory_order_acquire) &&
           (mach_port_t)atomic_load_explicit(
               &g_compact_profile_target_thread,
               memory_order_acquire) == target;
}

static void compact_profile_record_poll(
        mach_port_t target, unsigned generation,
        compact_profile_sample_result_t result,
        uintptr_t pc, uint64_t x26) {
    (void)pthread_mutex_lock(&g_compact_profile_lock);
    compact_profile_increment(&g_compact_profile_polls);
    if (result == COMPACT_PROFILE_SAMPLE_CAPTURED &&
        (!compact_profile_target_matches(target) ||
         generation != atomic_load_explicit(&g_compact_profile_generation,
                                             memory_order_acquire)))
        result = COMPACT_PROFILE_SAMPLE_TARGET_RACE;
    switch (result) {
    case COMPACT_PROFILE_SAMPLE_CAPTURED:
        compact_profile_sample_pc_locked(pc);
        {
            uint32_t guest_pc;
            if (compact_guest_pc_sample(
                    pc, (uintptr_t)a64_compact_raw_profile_guest_live_begin,
                    (uintptr_t)a64_compact_raw_profile_guest_live_end,
                    x26, &guest_pc))
                (void)native_pc_histogram_note(
                    &g_compact_profile_guest_histogram, guest_pc);
            else
                compact_profile_increment(
                    &g_compact_profile_guest_unavailable);
        }
        break;
    case COMPACT_PROFILE_SAMPLE_NOT_RUNNING:
        compact_profile_increment(&g_compact_profile_not_running);
        break;
    case COMPACT_PROFILE_SAMPLE_STATE_FAILURE:
        compact_profile_increment(&g_compact_profile_state_failures);
        break;
    case COMPACT_PROFILE_SAMPLE_TARGET_RACE:
        compact_profile_increment(&g_compact_profile_target_races);
        break;
    }
    (void)pthread_mutex_unlock(&g_compact_profile_lock);
}

static kern_return_t compact_profile_thread_basic_info(
        mach_port_t target, thread_basic_info_data_t *info) {
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    memset(info, 0, sizeof *info);
    return thread_info(target, THREAD_BASIC_INFO,
                       (thread_info_t)info, &count);
}

static void *compact_profile_sampler_main(void *opaque) {
    (void)opaque;
    const struct timespec interval = {0, 2000000L};
    for (;;) {
        (void)nanosleep(&interval, NULL);
        if (!atomic_load_explicit(&g_compact_profile_enabled,
                                  memory_order_acquire) ||
            !atomic_load_explicit(&g_compact_profile_active,
                                  memory_order_acquire))
            continue;

        mach_port_t target = (mach_port_t)atomic_load_explicit(
            &g_compact_profile_target_thread, memory_order_acquire);
        if (target == MACH_PORT_NULL) continue;
        unsigned generation = atomic_load_explicit(
            &g_compact_profile_generation, memory_order_acquire);

        compact_profile_sample_result_t result =
            COMPACT_PROFILE_SAMPLE_STATE_FAILURE;
        uintptr_t pc = 0u;
        uint64_t x26 = 0u;
        thread_basic_info_data_t before;
        if (compact_profile_thread_basic_info(target, &before) !=
                KERN_SUCCESS) {
            result = COMPACT_PROFILE_SAMPLE_STATE_FAILURE;
        } else if (before.run_state != TH_STATE_RUNNING) {
            result = COMPACT_PROFILE_SAMPLE_NOT_RUNNING;
        } else {
            arm_thread_state64_t state;
            mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
            memset(&state, 0, sizeof state);
            if (thread_get_state(target, ARM_THREAD_STATE64,
                                 (thread_state_t)&state, &count) !=
                    KERN_SUCCESS || count != ARM_THREAD_STATE64_COUNT) {
                result = COMPACT_PROFILE_SAMPLE_STATE_FAILURE;
            } else {
                thread_basic_info_data_t after;
                if (compact_profile_thread_basic_info(target, &after) !=
                        KERN_SUCCESS) {
                    result = COMPACT_PROFILE_SAMPLE_STATE_FAILURE;
                } else if (after.run_state != TH_STATE_RUNNING) {
                    result = COMPACT_PROFILE_SAMPLE_NOT_RUNNING;
                } else if (!compact_profile_target_matches(target)) {
                    result = COMPACT_PROFILE_SAMPLE_TARGET_RACE;
                } else {
                    pc = (uintptr_t)arm_thread_state64_get_pc(state);
                    x26 = state.__x[26];
                    result = COMPACT_PROFILE_SAMPLE_CAPTURED;
                }
            }
        }
        compact_profile_record_poll(target, generation, result, pc, x26);
    }
    return NULL;
}

bool a64_compact_raw_pc_profile_enable(void) {
    pthread_t sampler;
    pthread_attr_t attributes;

    if (!compact_profile_layout_valid()) return false;
    atomic_store_explicit(&g_compact_profile_enabled, false,
                          memory_order_release);
    /* An in-flight observation of the previous run must not enter counters
     * that this enable operation resets, even when the same pthread is used. */
    (void)atomic_fetch_add_explicit(&g_compact_profile_generation, 1u,
                                    memory_order_acq_rel);
    atomic_store_explicit(&g_compact_profile_active, false,
                          memory_order_release);
    atomic_store_explicit(&g_compact_profile_target_thread, MACH_PORT_NULL,
                          memory_order_release);

    (void)pthread_mutex_lock(&g_compact_profile_lock);
    compact_profile_reset_locked();
    if (!g_compact_profile_sampler_started) {
        if (pthread_attr_init(&attributes) != 0) {
            (void)pthread_mutex_unlock(&g_compact_profile_lock);
            return false;
        }
        if (pthread_attr_setdetachstate(
                &attributes, PTHREAD_CREATE_DETACHED) != 0) {
            (void)pthread_attr_destroy(&attributes);
            (void)pthread_mutex_unlock(&g_compact_profile_lock);
            return false;
        }
        int create_result = pthread_create(
            &sampler, &attributes, compact_profile_sampler_main, NULL);
        (void)pthread_attr_destroy(&attributes);
        if (create_result != 0) {
            (void)pthread_mutex_unlock(&g_compact_profile_lock);
            return false;
        }
        g_compact_profile_sampler_started = true;
    }
    atomic_store_explicit(&g_compact_profile_enabled, true,
                          memory_order_release);
    (void)pthread_mutex_unlock(&g_compact_profile_lock);
    return true;
}

void a64_compact_raw_pc_profile_slice_begin(void) {
    if (!atomic_load_explicit(&g_compact_profile_enabled,
                              memory_order_acquire))
        return;
    mach_port_t target = pthread_mach_thread_np(pthread_self());
    if (target == MACH_PORT_NULL) return;
    atomic_store_explicit(&g_compact_profile_target_thread,
                          (unsigned)target, memory_order_release);
    atomic_store_explicit(&g_compact_profile_active, true,
                          memory_order_release);
}

void a64_compact_raw_pc_profile_slice_end(void) {
    atomic_store_explicit(&g_compact_profile_active, false,
                          memory_order_release);
    atomic_store_explicit(&g_compact_profile_target_thread, MACH_PORT_NULL,
                          memory_order_release);
}

void a64_compact_raw_pc_profile_snapshot(
        a64_compact_raw_pc_profile_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->enabled = atomic_load_explicit(&g_compact_profile_enabled,
                                        memory_order_acquire);
    (void)pthread_mutex_lock(&g_compact_profile_lock);
    out->polls = g_compact_profile_polls;
    out->not_running = g_compact_profile_not_running;
    out->state_failures = g_compact_profile_state_failures;
    out->target_races = g_compact_profile_target_races;
    out->samples = g_compact_profile_samples;
    out->outside = g_compact_profile_outside;
    for (unsigned i = 0u;
         i < (unsigned)A64_COMPACT_RAW_PC_PROFILE_REGION_COUNT; i++)
        out->region[i] = (uint64_t)g_compact_profile_region[i];
    out->reference_pc =
        (uintptr_t)g_compact_profile_boundary[0];
    out->outside_pc_captured = g_compact_profile_outside_histogram.captured;
    out->outside_pc_dropped = g_compact_profile_outside_histogram.dropped;
    compact_profile_rank(&g_compact_profile_outside_histogram,
                         out->outside_hot,
                         A64_COMPACT_RAW_PC_PROFILE_HOT_COUNT);
    out->guest_pc_captured = g_compact_profile_guest_histogram.captured;
    out->guest_pc_dropped = g_compact_profile_guest_histogram.dropped;
    out->guest_pc_unavailable = g_compact_profile_guest_unavailable;
    compact_profile_rank(&g_compact_profile_guest_histogram, out->guest_hot,
                         A64_COMPACT_RAW_PC_PROFILE_GUEST_HOT_COUNT);
    (void)pthread_mutex_unlock(&g_compact_profile_lock);
}
static bool compact_profile_histogram_visit(
        const native_pc_histogram_t *source,
        bool (*visit)(void *, uint64_t, uint64_t, uint64_t), void *opaque,
        uint64_t *captured, uint64_t *dropped) {
    if (captured) *captured = 0u;
    if (dropped) *dropped = 0u;
    if (!visit || !captured || !dropped ||
        !atomic_load_explicit(&g_compact_profile_enabled, memory_order_acquire))
        return false;
    native_pc_histogram_t *copy = malloc(sizeof *copy);
    if (!copy) return false;
    (void)pthread_mutex_lock(&g_compact_profile_lock);
    memcpy(copy, source, sizeof *copy);
    (void)pthread_mutex_unlock(&g_compact_profile_lock);
    bool ok = native_pc_histogram_visit(copy, visit, opaque);
    if (ok) {
        *captured = copy->captured;
        *dropped = copy->dropped;
    }
    free(copy);
    return ok;
}
bool a64_compact_raw_guest_pc_profile_visit(
        bool (*visit)(void *, uint64_t, uint64_t, uint64_t), void *opaque,
        uint64_t *captured, uint64_t *dropped) {
    return compact_profile_histogram_visit(&g_compact_profile_guest_histogram,
                                          visit, opaque, captured, dropped);
}
bool a64_compact_raw_host_pc_profile_visit(
        bool (*visit)(void *, uint64_t, uint64_t, uint64_t), void *opaque,
        uint64_t *captured, uint64_t *dropped) {
    return compact_profile_histogram_visit(&g_compact_profile_host_histogram,
                                          visit, opaque, captured, dropped);
}
#else
bool a64_compact_raw_pc_profile_enable(void) { return false; }
void a64_compact_raw_pc_profile_slice_begin(void) {}
void a64_compact_raw_pc_profile_slice_end(void) {}
void a64_compact_raw_pc_profile_snapshot(
        a64_compact_raw_pc_profile_t *out) {
    if (out) memset(out, 0, sizeof *out);
}
bool a64_compact_raw_guest_pc_profile_visit(
        bool (*visit)(void *, uint64_t, uint64_t, uint64_t), void *opaque,
        uint64_t *captured, uint64_t *dropped) {
    (void)visit; (void)opaque;
    if (captured) *captured = 0u;
    if (dropped) *dropped = 0u;
    return false;
}
bool a64_compact_raw_host_pc_profile_visit(
        bool (*visit)(void *, uint64_t, uint64_t, uint64_t), void *opaque,
        uint64_t *captured, uint64_t *dropped) {
    (void)visit; (void)opaque;
    if (captured) *captured = 0u;
    if (dropped) *dropped = 0u;
    return false;
}
#endif

bool a64_compact_raw_run(arm_cpu_t *cpu, const uint8_t *code,
                         uint32_t code_base, uint32_t code_bytes,
                         unsigned max_insns, uint8_t *ram, size_t ram_size,
                         unsigned *completed) {
    uint64_t code_end;

    if (!completed) return false;
    *completed = 0u;
    code_end = (uint64_t)code_base + code_bytes;
    if (!cpu || !code || !ram || !max_insns || code_bytes < 4u ||
        (code_base & 3u) != 0u || (code_bytes & 3u) != 0u ||
        code_end > (uint64_t)UINT32_MAX + 1u ||
        (cpu->r[15] & ((cpu->cpsr & ARM_CPSR_T) ? 1u : 3u)) != 0u ||
        (cpu->cpsr & ARM_CPSR_E) != 0u ||
        (cpu->cp15.sctlr & ARM_SCTLR_M) != 0u || cpu->irq_line ||
        cpu->fiq_line || cpu->abort_pending || ram_size < 4u ||
        (ram_size & (ram_size - 1u)) != 0u ||
        ram_size - 1u > UINT32_MAX)
        return false;
#if defined(S5LBOX_STATIC_A64_NATIVE)
    {
        a64_compact_raw_context_t context;
        memset(&context, 0, sizeof context);
        context.flat_ram = ram;
        context.flat_mask = (uint64_t)ram_size - 1u;
        context.vfp_s = cpu->vfp_s;
        context.vfp_fpexc = &cpu->vfp_fpexc;
        context.vfp_fpscr = &cpu->vfp_fpscr;
        context.vfp_access = vfp_cpacr_permits(cpu) ? 1u : 0u;
        context.cp15 = &cpu->cp15;
        context.priv_tag =
            (cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR ? 1u : 0u;
        uint32_t result = a64_compact_raw_execute(
            cpu->r, &cpu->cpsr, &cpu->cycles, code, code_base, code_bytes,
            max_insns, &context);
        if (result > max_insns || context.vfp_host_active != 0u)
            return false;
        *completed = result;
        return true;
    }
#else
    (void)code_end;
    return false;
#endif
}

bool a64_compact_raw_run_code_window(arm_cpu_t *cpu, const uint8_t *code,
                                     uint32_t code_base,
                                     uint32_t code_bytes,
                                     unsigned max_insns,
                                     unsigned *completed) {
    unsigned native_completed = 0u;
    unsigned fallback_completed = 0u;

    return a64_compact_raw_run_code_window_resident(
        cpu, code, code_base, code_bytes, max_insns, NULL, NULL, completed,
        &native_completed, &fallback_completed);
}

bool a64_compact_raw_run_code_window_resident(
        arm_cpu_t *cpu, const uint8_t *code, uint32_t code_base,
        uint32_t code_bytes, unsigned max_insns,
        a64_compact_raw_fallback_fn fallback, void *fallback_opaque,
        unsigned *completed, unsigned *native_completed,
        unsigned *fallback_completed) {
    return a64_compact_raw_run_code_window_resident_cached(
        cpu, code, code_base, code_bytes, max_insns, fallback,
        fallback_opaque, false, NULL, completed, native_completed,
        fallback_completed);
}

bool a64_compact_raw_run_code_window_resident_cached(
        arm_cpu_t *cpu, const uint8_t *code, uint32_t code_base,
        uint32_t code_bytes, unsigned max_insns,
        a64_compact_raw_fallback_fn fallback, void *fallback_opaque,
        bool window_cache_enabled, uint64_t *window_cache_hits,
        unsigned *completed, unsigned *native_completed,
        unsigned *fallback_completed) {
    return a64_compact_raw_run_code_window_resident_bulk(
        cpu, code, code_base, code_bytes, max_insns, fallback, fallback_opaque,
        window_cache_enabled, window_cache_hits, false, NULL, completed,
        native_completed, fallback_completed);
}

bool a64_compact_raw_run_code_window_resident_bulk(
        arm_cpu_t *cpu, const uint8_t *code, uint32_t code_base,
        uint32_t code_bytes, unsigned max_insns,
        a64_compact_raw_fallback_fn fallback, void *fallback_opaque,
        bool window_cache_enabled, uint64_t *window_cache_hits,
        bool bulk_enabled, a64_compact_bulk_stats_t *bulk_stats,
        unsigned *completed, unsigned *native_completed,
        unsigned *fallback_completed) {
    const a64_compact_raw_options_t options = {
        .window_cache_enabled = window_cache_enabled,
        .bulk_enabled = bulk_enabled,
    };
    return a64_compact_raw_run_code_window_resident_options(
        cpu, code, code_base, code_bytes, max_insns, fallback, fallback_opaque,
        &options, window_cache_hits, bulk_stats, NULL, completed,
        native_completed, fallback_completed);
}

bool a64_compact_raw_run_code_window_resident_options(
        arm_cpu_t *cpu, const uint8_t *code, uint32_t code_base,
        uint32_t code_bytes, unsigned max_insns,
        a64_compact_raw_fallback_fn fallback, void *fallback_opaque,
        const a64_compact_raw_options_t *options,
        uint64_t *window_cache_hits, a64_compact_bulk_stats_t *bulk_stats,
        a64_compact_tlb_stats_t *tlb_stats,
        unsigned *completed, unsigned *native_completed,
        unsigned *fallback_completed) {
    uint64_t code_end;

    if (!completed || !native_completed || !fallback_completed) return false;
    *completed = 0u;
    *native_completed = 0u;
    *fallback_completed = 0u;
    if (window_cache_hits) *window_cache_hits = 0u;
    if (bulk_stats) memset(bulk_stats, 0, sizeof *bulk_stats);
    if (tlb_stats) memset(tlb_stats, 0, sizeof *tlb_stats);
    if (options && options->ram_map_stats)
        memset(options->ram_map_stats, 0, sizeof *options->ram_map_stats);
    code_end = (uint64_t)code_base + code_bytes;
    if (!cpu || !code || !max_insns || code_bytes < 4u ||
        (code_base & 3u) != 0u || (code_bytes & 3u) != 0u ||
        code_end > (uint64_t)UINT32_MAX + 1u ||
        (cpu->r[15] & ((cpu->cpsr & ARM_CPSR_T) ? 1u : 3u)) != 0u ||
        (cpu->cpsr & ARM_CPSR_E) != 0u ||
        !arm_mode_is_valid(cpu->cpsr) || cpu->abort_pending ||
        (cpu->fiq_line && !(cpu->cpsr & ARM_CPSR_F)) ||
        (cpu->irq_line && !(cpu->cpsr & ARM_CPSR_I)))
        return false;
#if defined(S5LBOX_STATIC_A64_NATIVE)
    {
        const bool priv = (cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
        a64_compact_raw_context_t context;
        memset(&context, 0, sizeof context);
        if (fallback) {
            context.dread = cpu->dread;
            /* A cached translation is not write authority. Expose DWRITE only
             * while the frontend's separate observer-bypass consent is live;
             * its revocation API also clears every derived entry. */
            context.dwrite =
                cpu->bus && cpu->bus->host_ram_write ? cpu->dwrite : NULL;
            context.dread_hits = &cpu->dread_hits;
            context.dwrite_hits = &cpu->dwrite_hits;
        }
        context.fallback = fallback;
        context.fallback_opaque = fallback_opaque;
        context.bulk_cpu = options && options->bulk_enabled && fallback && !priv
            ? cpu : NULL;
        context.tlb_gen = cpu->tlb_gen;
        context.priv_tag = priv ? 1u : 0u;
        context.vfp_s = cpu->vfp_s;
        context.vfp_fpexc = &cpu->vfp_fpexc;
        context.vfp_fpscr = &cpu->vfp_fpscr;
        context.vfp_access = vfp_cpacr_permits(cpu) ? 1u : 0u;
        context.cp15 = &cpu->cp15;
        context.window_cache_enabled =
            options && options->window_cache_enabled && fallback && !priv &&
            (code_base & UINT32_C(0x3ff)) == 0u &&
            code_bytes == UINT32_C(0x400) ? 1u : 0u;
        context.window_cache_next = 1u;
        context.current_window.code = code;
        context.current_window.code_base = code_base;
        context.current_window.code_bytes = code_bytes;
        context.window_cache[0] = context.current_window;
        if (options && options->ram_window && fallback && !priv &&
            code_bytes == 1024u && (code_base & 1023u) == 0u &&
            arm_ram_window_tlb_lookup(options->ram_window, cpu, code_base,
                                      ARM_ACCESS_FETCH, false) == code) {
            context.ram_window = options->ram_window;
            context.tlb = cpu->tlb;
            context.tlb_hits = &cpu->tlb_hits;
            context.fetch_cache = &cpu->fetch_host;
            context.owner_fetch_block = options->owner_fetch_block;
            context.tlb_cpu = cpu;
            context.tlb_cp15 = cpu->cp15;
            context.guarded_fallback = fallback;
            context.guarded_opaque = fallback_opaque;
            context.fallback = compact_tlb_guarded_fallback;
            context.fallback_opaque = &context;
            if (options->ram_map &&
                arm_ram_map_prepare(options->ram_map, options->ram_window, cpu))
                context.ram_map = options->ram_map;
            /* Do not combine two independent window-reuse experiments. */
            context.window_cache_enabled = 0u;
        }
        uint32_t result = a64_compact_raw_execute(
            cpu->r, &cpu->cpsr, &cpu->cycles, code, code_base, code_bytes,
            max_insns, &context);
        if (result > max_insns || context.vfp_host_active != 0u ||
            context.native_retired > result ||
            context.fallback_retired > result ||
            context.native_retired + context.fallback_retired != result ||
            context.native_retired > UINT_MAX ||
            context.fallback_retired > UINT_MAX)
            return false;
        /* Every cached switch replaces a successful refill to a different
         * FETCH block. With the MMU on, that exact refill consumed an already
         * proved TLB entry and incremented this serialized diagnostic counter.
         * Preserve its meaning even though native text avoided the C call. */
        if ((cpu->cp15.sctlr & ARM_SCTLR_M) != 0u)
            cpu->tlb_hits += context.window_cache_hits;
        *completed = result;
        *native_completed = (unsigned)context.native_retired;
        *fallback_completed = (unsigned)context.fallback_retired;
        if (window_cache_hits)
            *window_cache_hits = context.window_cache_hits;
        if (bulk_stats) *bulk_stats = context.bulk_stats;
        if (tlb_stats) *tlb_stats = context.tlb_stats;
        if (options && options->ram_map_stats)
            *options->ram_map_stats = context.ram_map_stats;
        if (context.window_cache_enabled && context.current_window.code &&
            cpu->tlb_gen == context.tlb_gen &&
            ((cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR) == priv &&
            cpu->r[15] - context.current_window.code_base <
                context.current_window.code_bytes) {
            /* This is derived FETCH state, not architectural state. Restore
             * the exact window selected inside signed text so the next outer
             * entry does not pay to rediscover the same witness. */
            cpu->fetch_host = (uint8_t *)context.current_window.code;
            cpu->fetch_blk = context.current_window.code_base;
            cpu->fetch_gen = context.tlb_gen;
            cpu->fetch_priv = priv;
        }
        return true;
    }
#else
    (void)code_end;
    (void)fallback;
    (void)fallback_opaque;
    (void)options;
    (void)window_cache_hits;
    return false;
#endif
}

typedef struct {
    const a64_static_uop_t *uops;
    uint64_t insns;
} a64_static_chain_step_t;

typedef struct {
    a64_static_chain_step_t step;
    a64_static_chain_next_fn next;
    void *opaque;
    uint64_t *cycles;
    uint64_t ram_mask;
    uint32_t budget;
    uint32_t completed;
    uint32_t blocks;
    uint32_t final_pc;
    bool thumb;
    const a64_static_graph_node_t *graph_nodes;
    const uint8_t *graph_fetch_host;
    uint32_t graph_fetch_block;
    uint32_t graph_fetch_gen;
    uint32_t graph_fetch_priv;
    bool memory_hits;
} a64_static_chain_context_t;

/* These offsets are consumed by the build-time assembly generator. Keep the
 * static assertions beside the C layout so an ABI drift is a build failure,
 * not silent guest-state corruption. */
_Static_assert(offsetof(a64_static_chain_context_t, step) == 0u,
               "chain step ABI offset");
_Static_assert(offsetof(a64_static_chain_context_t, ram_mask) == 40u,
               "chain RAM-mask ABI offset");
_Static_assert(offsetof(a64_static_chain_context_t, final_pc) == 60u,
               "chain final-PC ABI offset");
_Static_assert(offsetof(a64_static_chain_context_t, thumb) == 64u,
               "chain Thumb ABI offset");
_Static_assert(offsetof(a64_static_chain_context_t, graph_nodes) == 72u,
               "chain graph-node ABI offset");
_Static_assert(offsetof(a64_static_chain_context_t, graph_fetch_host) == 80u,
               "chain graph-fetch-host ABI offset");
_Static_assert(offsetof(a64_static_chain_context_t, graph_fetch_block) == 88u,
               "chain graph-fetch-block ABI offset");
_Static_assert(offsetof(a64_static_chain_context_t, graph_fetch_gen) == 92u,
               "chain graph-generation ABI offset");
_Static_assert(offsetof(a64_static_chain_context_t, graph_fetch_priv) == 96u,
               "chain graph-privilege ABI offset");
_Static_assert(offsetof(a64_static_chain_context_t, memory_hits) == 100u,
               "chain memory-hit ABI offset");
_Static_assert(sizeof(a64_static_graph_node_t) == 128u,
               "graph node ABI size");
_Static_assert(offsetof(a64_static_graph_node_t, fetch_host) == 8u,
               "graph fetch-host ABI offset");
_Static_assert(offsetof(a64_static_graph_node_t, uops) == 16u,
               "graph uops ABI offset");
_Static_assert(offsetof(a64_static_graph_node_t, pc) == 24u,
               "graph PC ABI offset");
_Static_assert(offsetof(a64_static_graph_node_t, fetch_gen) == 28u,
               "graph generation ABI offset");
_Static_assert(offsetof(a64_static_graph_node_t, insn_count) == 32u,
               "graph instruction-count ABI offset");
_Static_assert(offsetof(a64_static_graph_node_t, raw_len) == 36u,
               "graph raw-length ABI offset");
_Static_assert(offsetof(a64_static_graph_node_t, raw) == 40u,
               "graph raw-witness ABI offset");
_Static_assert(offsetof(a64_static_graph_node_t, fetch_priv) == 104u &&
               offsetof(a64_static_graph_node_t, thumb) == 105u &&
               offsetof(a64_static_graph_node_t, valid) == 106u &&
               offsetof(a64_static_graph_node_t, supported) == 107u,
               "graph flag ABI offsets");

typedef enum {
    A64S_RUN_FLAT,
    A64S_RUN_READ_HITS,
    A64S_RUN_MEMORY_HITS
} a64s_run_kind_t;

static unsigned stm_semantic_span(const a64_static_block_t *block,
                                  unsigned i, unsigned end) {
    const a64_static_uop_t *preflight;
    const a64_static_uop_t *finish;
    uint32_t register_mask = 0u;
    unsigned n;
    unsigned rn;
    unsigned previous = 0u;

    if (!block || i >= end ||
        !handler_is_stm_preflight(block->uops[i].handler))
        return 0u;
    preflight = &block->uops[i];
    n = preflight->immediate;
    rn = (preflight->handler - A64S_STM_PREFLIGHT) % 15u;
    if (!n || n > 16u || i + n + 1u >= end) return 0u;

    for (unsigned j = 0u; j < n; j++) {
        const a64_static_uop_t *commit = &block->uops[i + 1u + j];
        unsigned reg;
        if (!handler_is_stm_commit(commit->handler) || commit->immediate != 0u ||
            commit->pc_value != 0u || commit->metadata != 0u)
            return 0u;
        reg = commit->handler - A64S_STM_COMMIT;
        if (j != 0u && reg <= previous) return 0u;
        previous = reg;
        register_mask |= 1u << reg;
    }

    finish = &block->uops[i + n + 1u];
    if (!handler_is_stm_finish(finish->handler) || finish->immediate != n ||
        finish->pc_value != 0u || finish->metadata != 0u)
        return 0u;
    if (handler_is_stm_finish_wb(finish->handler) &&
        (finish->handler - A64S_STM_FINISH_WB != rn ||
         (register_mask & (1u << rn)) != 0u))
        return 0u;
    return n + 2u;
}

static unsigned ldm_semantic_span(const a64_static_block_t *block,
                                  unsigned i, unsigned end) {
    const a64_static_uop_t *preflight;
    const a64_static_uop_t *finish;
    uint32_t register_mask = 0u;
    unsigned n;
    unsigned rn;
    unsigned previous = 0u;

    if (!block || i >= end ||
        !handler_is_ldm_preflight(block->uops[i].handler))
        return 0u;
    preflight = &block->uops[i];
    n = preflight->immediate;
    rn = (preflight->handler - A64S_LDM_PREFLIGHT) % 15u;
    if (!n || n > 15u || i + n + 1u >= end) return 0u;

    for (unsigned j = 0u; j < n; j++) {
        const a64_static_uop_t *commit = &block->uops[i + 1u + j];
        unsigned reg;
        if (!handler_is_ldm_commit(commit->handler) ||
            commit->immediate != 0u || commit->pc_value != 0u ||
            commit->metadata != 0u)
            return 0u;
        reg = commit->handler - A64S_LDM_COMMIT;
        if (j != 0u && reg <= previous) return 0u;
        previous = reg;
        register_mask |= 1u << reg;
    }

    finish = &block->uops[i + n + 1u];
    if (!handler_is_ldm_finish(finish->handler) || finish->immediate != n ||
        finish->pc_value != 0u || finish->metadata != 0u)
        return 0u;
    if (handler_is_ldm_finish_wb(finish->handler) &&
        (finish->handler - A64S_LDM_FINISH_WB != rn ||
         (register_mask & (1u << rn)) != 0u))
        return 0u;
    return n + 2u;
}

static unsigned semantic_span(const a64_static_block_t *block, unsigned i,
                              unsigned end) {
    uint32_t handler;
    if (i >= end) return 0u;
    handler = block->uops[i].handler;
    if (handler_is_terminal_branch(handler)) return 1u;
    if (handler_is_condition(handler) || handler == A64S_END)
        return 0u;
    if (handler_is_stm_preflight(handler))
        return stm_semantic_span(block, i, end);
    if (handler_is_ldm_preflight(handler))
        return ldm_semantic_span(block, i, end);
    if (handler_is_shift(handler)) {
        if (i + 1u < end && handler_is_dp_reg(block->uops[i + 1u].handler))
            return 2u;
        if (i + 2u < end &&
            handler_is_addr_reg(block->uops[i + 1u].handler) &&
            (handler_is_direct_read(block->uops[i + 2u].handler) ||
             handler_is_direct_write(block->uops[i + 2u].handler)))
            return 3u;
        return 0u;
    }
    if (handler_is_addr_imm(handler)) {
        return i + 1u < end &&
               (handler_is_direct_read(block->uops[i + 1u].handler) ||
                handler_is_direct_write(block->uops[i + 1u].handler))
                   ? 2u : 0u;
    }
    /* VSTM folds address generation, the transactional DWRITE proof and the
     * complete VFP word stream into one terminal record. Unlike ordinary
     * direct writes, it is therefore a complete semantic instruction by
     * itself rather than the second half of an address/write pair. */
    if (handler_is_vstm_direct_write(handler)) return 1u;
    if (handler_is_dp_reg(handler) || handler_is_addr_reg(handler) ||
        handler_is_direct_read(handler) ||
        handler_is_direct_write(handler) || handler_is_stm_commit(handler) ||
        handler_is_ldm_commit(handler) || handler_is_ldm_finish(handler))
        return 0u;
    if (handler_is_vfp(handler)) return 1u;
    return 1u;
}

static bool terminal_branch_shape_valid(const a64_static_block_t *block,
                                        uint32_t handler,
                                        const a64_static_uop_t *op) {
    uint32_t width;
    uint32_t instruction_pc;
    if (!block || !op || !block->insn_count) return false;
    width = block->thumb ? 2u : 4u;
    instruction_pc = block->start_pc + (block->insn_count - 1u) * width;
    if (handler_is_indirect_branch(handler)) {
        return op->immediate == 0u && op->pc_value == instruction_pc &&
               op->metadata == (block->insn_count << 8 | 1u) &&
               block->exit_pc == block->start_pc + block->insn_count * width;
    }
    if (block->thumb && handler_is_conditional_branch(handler)) {
        return (op->immediate & 1u) == 0u &&
               op->pc_value == block->exit_pc && op->metadata == 0u &&
               block->exit_pc == block->start_pc + block->insn_count * 2u;
    }
    return !block->thumb && (op->immediate & 3u) == 0u &&
           op->pc_value == block->exit_pc && op->metadata == 0u &&
           block->exit_pc == block->start_pc + block->insn_count * 4u;
}

static bool validate_run(const arm_cpu_t *cpu,
                         const a64_static_block_t *block, uint64_t blocks,
                         const uint8_t *ram, size_t ram_size,
                         a64s_run_kind_t kind) {
    unsigned i = 0u;
    unsigned semantic_insns = 0u;
    unsigned end;
    bool saw_flat_memory = false;
    bool saw_direct_read = false;
    bool saw_direct_write = false;
    bool saw_runtime_guard = false;
    bool saw_vfp = false;
    bool saw_vfp_arithmetic = false;
    bool saw_vfp_direct_write = false;
    bool saw_stm_direct_write = false;
    bool saw_ldm_direct_read = false;
    bool saw_vstm_direct_write = false;
    bool saw_dynamic_exit = false;
    bool saw_indirect_exit = false;
    bool saw_thumb_conditional_exit = false;

    if (!cpu || !block || !blocks || !ram ||
        !block->insn_count || block->insn_count > A64_STATIC_MAX_INSNS ||
        block->uop_count < block->insn_count ||
        block->uop_count > A64_STATIC_MAX_UOPS ||
        block->uops[block->uop_count - 1u].handler != A64S_END ||
        block->uops[block->uop_count - 1u].immediate != block->exit_pc ||
        cpu->r[15] != block->start_pc ||
        ((cpu->cpsr & ARM_CPSR_T) != 0u) != block->thumb ||
        (blocks > 1u &&
         (block->dynamic_exit || block->exit_pc != block->start_pc)) ||
        (kind != A64S_RUN_FLAT && blocks != 1u) ||
        !ram_size || (ram_size & (ram_size - 1u)) != 0u ||
        ram_size - 1u > UINT32_MAX)
        return false;

    end = block->uop_count - 1u;
    for (unsigned j = 0u; j < end; j++) {
        uint32_t handler = block->uops[j].handler;
        if (handler == A64S_END || handler >= A64S_HANDLER_COUNT)
            return false;
        if (handler_touches_memory(handler)) saw_flat_memory = true;
        if (handler_is_runtime_guarded(handler)) {
            uint32_t metadata = block->uops[j].metadata;
            unsigned remaining = metadata & 255u;
            unsigned completed_plus_one = (metadata >> 8) & 255u;
            unsigned completed;
            saw_runtime_guard = true;
            if ((metadata >> 16) != 0u || !remaining ||
                !completed_plus_one)
                return false;
            completed = completed_plus_one - 1u;
            if (completed >= block->insn_count ||
                remaining != block->insn_count - completed ||
                block->uops[j].pc_value !=
                    block->start_pc + completed * (block->thumb ? 2u : 4u))
                return false;
        }
        if (handler_is_direct_read(handler)) {
            saw_direct_read = true;
            if (handler_is_ldm_preflight(handler))
                saw_ldm_direct_read = true;
        }
        if (handler_is_direct_write(handler)) {
            const uint32_t immediate = block->uops[j].immediate;
            const bool vfp_write = handler_is_vfp_direct_write(handler);
            const bool stm_write = handler_is_stm_finish(handler);
            const bool vstm_write = handler_is_vstm_direct_write(handler);
            if (saw_direct_write || j + 1u != end ||
                 (stm_write
                      ? immediate == 0u || immediate > 16u
                      : vstm_write
                      ? !vstm_immediate_valid(immediate)
                      : vfp_write
                     ? (handler == A64S_VFP_DIRECT_WRITE32
                            ? immediate > 31u
                            : immediate > 30u || (immediate & 1u) != 0u)
                     : (handler_is_direct_write_wb(handler)
                            ? immediate > 1u
                            : immediate != 0u)) ||
                (handler_is_direct_write_unpriv(handler) &&
                 immediate != 1u))
                return false;
            saw_direct_write = true;
            if (vfp_write) saw_vfp_direct_write = true;
            if (stm_write) saw_stm_direct_write = true;
            if (vstm_write) saw_vstm_direct_write = true;
        }
        if (handler_is_vfp(handler)) saw_vfp = true;
        if (handler_is_vfp_arithmetic(handler)) {
            if (!vfp_arithmetic_immediate_valid(
                    handler, block->uops[j].immediate))
                return false;
            saw_vfp_arithmetic = true;
        }
        if (handler_is_terminal_branch(handler)) {
            if (saw_dynamic_exit || j + 1u != end ||
                !terminal_branch_shape_valid(block, handler,
                                             &block->uops[j]))
                return false;
            saw_dynamic_exit = true;
            if (block->thumb && handler_is_conditional_branch(handler))
                saw_thumb_conditional_exit = true;
            if (handler_is_indirect_branch(handler))
                saw_indirect_exit = true;
        }
        if (handler >= A64S_DP_IMM && handler < A64S_SHIFT_IMM &&
            block->uops[j].metadata > A64S_CARRY_SET)
            return false;
    }

    if (block->touches_memory !=
            (saw_flat_memory || saw_direct_read || saw_direct_write) ||
        block->direct_reads != saw_direct_read ||
        block->direct_writes != saw_direct_write ||
        block->runtime_guards != saw_runtime_guard ||
        block->vfp != saw_vfp ||
        block->vfp_arithmetic != saw_vfp_arithmetic ||
        block->vfp_direct_writes != saw_vfp_direct_write ||
        block->stm_direct_writes != saw_stm_direct_write ||
        block->ldm_direct_reads != saw_ldm_direct_read ||
        block->vstm_direct_writes != saw_vstm_direct_write ||
        block->dynamic_exit != saw_dynamic_exit ||
        block->indirect_exit != saw_indirect_exit ||
        block->thumb_conditional_exit != saw_thumb_conditional_exit ||
        (kind == A64S_RUN_FLAT && (saw_direct_read || saw_direct_write)) ||
        (kind == A64S_RUN_FLAT && saw_vfp) ||
        (kind == A64S_RUN_READ_HITS &&
         (saw_flat_memory || saw_direct_write)) ||
        (kind == A64S_RUN_MEMORY_HITS && saw_flat_memory))
        return false;

    while (i < end) {
        unsigned span;
        if (handler_is_condition(block->uops[i].handler)) {
            span = semantic_span(block, i + 1u, end);
            if (!span || block->uops[i].metadata != span)
                return false;
            i += span + 1u;
        } else {
            span = semantic_span(block, i, end);
            if (!span) return false;
            i += span;
        }
        semantic_insns++;
    }
    if (i != end ||
        (semantic_insns != block->insn_count &&
         semantic_insns + 1u != block->insn_count))
        return false;
    return true;
}

bool a64_static_run(arm_cpu_t *cpu, const a64_static_block_t *block,
                    uint64_t blocks, uint8_t *ram, size_t ram_size) {
    if (!validate_run(cpu, block, blocks, ram, ram_size, A64S_RUN_FLAT))
        return false;
#if defined(S5LBOX_STATIC_A64_NATIVE)
    return a64_static_execute(cpu->r, &cpu->cpsr, &cpu->cycles,
                              block->uops, blocks, ram,
                              (uint64_t)ram_size - 1u,
                              block->insn_count, NULL, NULL) == 0;
#else
    (void)blocks;
    return false;
#endif
}

static bool execute_memory_hits(arm_cpu_t *cpu,
                                const a64_static_block_t *block,
                                uint8_t *ram, size_t ram_size,
                                bool vfp_fp_session,
                                unsigned *completed) {
#if defined(S5LBOX_STATIC_A64_NATIVE)
    a64_static_read_context_t context = {
        cpu->dread,
        &cpu->dread_hits,
        cpu->tlb_gen,
        (cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR ? 1u : 0u,
        cpu->vfp_s,
        &cpu->vfp_fpexc,
        &cpu->vfp_fpscr,
        vfp_cpacr_permits(cpu) ? 1u : 0u,
        vfp_fp_session ? 1u : 0u,
        cpu->bus && cpu->bus->host_ram_write ? cpu->dwrite : NULL,
        &cpu->dwrite_hits
    };
    int result = a64_static_execute(cpu->r, &cpu->cpsr, &cpu->cycles,
                                    block->uops, 1u, ram,
                                    (uint64_t)ram_size - 1u,
                                    block->insn_count, &context, NULL);
    if (result < 0 || (unsigned)result > block->insn_count)
        return false;
    *completed = result == 0 ? block->insn_count : (unsigned)result - 1u;
    return true;
#else
    (void)cpu;
    (void)block;
    (void)ram;
    (void)ram_size;
    (void)vfp_fp_session;
    (void)completed;
    return false;
#endif
}

bool a64_static_run_read_hits(arm_cpu_t *cpu,
                              const a64_static_block_t *block,
                              uint8_t *ram, size_t ram_size,
                              unsigned *completed) {
    if (!completed ||
        !validate_run(cpu, block, 1u, ram, ram_size,
                      A64S_RUN_READ_HITS))
        return false;
    return execute_memory_hits(cpu, block, ram, ram_size, true, completed);
}

bool a64_static_run_memory_hits(arm_cpu_t *cpu,
                                const a64_static_block_t *block,
                                uint8_t *ram, size_t ram_size,
                                unsigned *completed) {
    if (!completed ||
        !validate_run(cpu, block, 1u, ram, ram_size,
                      A64S_RUN_MEMORY_HITS))
        return false;
    return execute_memory_hits(cpu, block, ram, ram_size, true, completed);
}

static bool validate_decoded_hits_at(const a64_static_block_t *block,
                                     uint32_t pc, bool thumb,
                                     unsigned remaining,
                                     bool memory_hits) {
    bool terminal_dynamic;
    bool terminal_indirect;
    bool terminal_thumb_conditional;
    bool terminal_write;
    bool terminal_vfp_write;
    bool terminal_stm_write;
    bool terminal_vstm_write;
    if (!block || !remaining || !block->insn_count ||
        block->insn_count > remaining ||
        block->insn_count > A64_STATIC_MAX_INSNS || !block->uop_count ||
        block->uop_count > A64_STATIC_MAX_UOPS ||
        block->uops[block->uop_count - 1u].handler != A64S_END ||
        block->uops[block->uop_count - 1u].immediate != block->exit_pc ||
        pc != block->start_pc || thumb != block->thumb ||
        (!memory_hits && block->direct_writes) ||
        (block->touches_memory &&
         !(block->direct_reads || block->direct_writes)))
        return false;
    terminal_write = block->uop_count > 1u &&
        handler_is_direct_write(
            block->uops[block->uop_count - 2u].handler);
    terminal_vfp_write = terminal_write &&
        handler_is_vfp_direct_write(
            block->uops[block->uop_count - 2u].handler);
    terminal_stm_write = terminal_write &&
        handler_is_stm_finish(
            block->uops[block->uop_count - 2u].handler);
    terminal_vstm_write = terminal_write &&
        handler_is_vstm_direct_write(
            block->uops[block->uop_count - 2u].handler);
    if (block->direct_writes != terminal_write ||
        block->vfp_direct_writes != terminal_vfp_write ||
        block->stm_direct_writes != terminal_stm_write ||
        block->vstm_direct_writes != terminal_vstm_write)
        return false;
    terminal_dynamic = block->uop_count > 1u &&
        handler_is_terminal_branch(
            block->uops[block->uop_count - 2u].handler);
    terminal_indirect = terminal_dynamic &&
        handler_is_indirect_branch(
            block->uops[block->uop_count - 2u].handler);
    terminal_thumb_conditional = terminal_dynamic && block->thumb &&
        handler_is_conditional_branch(
            block->uops[block->uop_count - 2u].handler);
    if (block->dynamic_exit != terminal_dynamic ||
        block->indirect_exit != terminal_indirect ||
        block->thumb_conditional_exit != terminal_thumb_conditional ||
        (terminal_dynamic &&
         !terminal_branch_shape_valid(
             block, block->uops[block->uop_count - 2u].handler,
             &block->uops[block->uop_count - 2u])))
        return false;
    return true;
}

bool a64_static_run_read_hits_decoded(arm_cpu_t *cpu,
                                      const a64_static_block_t *block,
                                      uint8_t *ram, size_t ram_size,
                                      unsigned *completed) {
    if (!cpu || !ram || !completed || !ram_size ||
        (ram_size & (ram_size - 1u)) != 0u ||
        ram_size - 1u > UINT32_MAX ||
        !validate_decoded_hits_at(
            block, cpu->r[15], (cpu->cpsr & ARM_CPSR_T) != 0u,
            A64_STATIC_MAX_INSNS, false))
        return false;
    return execute_memory_hits(cpu, block, ram, ram_size, true, completed);
}

bool a64_static_run_memory_hits_decoded(arm_cpu_t *cpu,
                                        const a64_static_block_t *block,
                                        uint8_t *ram, size_t ram_size,
                                        bool vfp_fp_session,
                                        unsigned *completed) {
    if (!cpu || !ram || !completed || !ram_size ||
        (ram_size & (ram_size - 1u)) != 0u ||
        ram_size - 1u > UINT32_MAX ||
        !validate_decoded_hits_at(
            block, cpu->r[15], (cpu->cpsr & ARM_CPSR_T) != 0u,
            A64_STATIC_MAX_INSNS, true))
        return false;
    return execute_memory_hits(cpu, block, ram, ram_size,
                               vfp_fp_session, completed);
}

#if defined(S5LBOX_STATIC_A64_NATIVE)
/* Called only from the generated signed function while guest r0-r7/SP remain
 * pinned in AAPCS64 callee-saved registers. The callback may inspect and update
 * host decode-cache state, but it cannot see those deliberately unflushed guest
 * registers and therefore must be limited to head validation/selection. */
const a64_static_chain_step_t *a64_static_chain_advance(
    a64_static_chain_context_t *context, uint32_t pc) {
    const a64_static_block_t *next;
    unsigned current;
    unsigned remaining;

    if (!context) return NULL;
    current = (unsigned)context->step.insns;
    context->final_pc = pc;
    if (!current || context->completed > context->budget ||
        current > context->budget - context->completed)
        return NULL;
    context->completed += current;
    context->blocks++;
    remaining = context->budget - context->completed;
    if (!remaining || !context->next) return NULL;

    next = context->next(context->opaque, pc, remaining);
    if (!validate_decoded_hits_at(next, pc, context->thumb, remaining,
                                  context->memory_hits))
        return NULL;
    context->step.uops = next->uops;
    context->step.insns = next->insn_count;
    *context->cycles += next->insn_count;
    return &context->step;
}

void a64_static_chain_partial(a64_static_chain_context_t *context,
                              uint32_t completed, uint32_t pc) {
    if (!context) return;
    context->final_pc = pc;
    if (completed > context->step.insns ||
        completed > context->budget - context->completed)
        return;
    context->completed += completed;
    if (completed) context->blocks++;
}
#endif

static bool run_hits_chain(arm_cpu_t *cpu,
                           const a64_static_block_t *first,
                           uint8_t *ram, size_t ram_size,
                           unsigned budget,
                           a64_static_chain_next_fn next,
                           void *opaque,
                           const a64_static_graph_node_t *graph_nodes,
                           bool memory_hits,
                           bool vfp_fp_session,
                           unsigned *completed, unsigned *blocks) {
    bool thumb;
    bool priv;
    if (!completed || !blocks) return false;
    *completed = 0u;
    *blocks = 0u;
    if (!cpu) return false;
    thumb = (cpu->cpsr & ARM_CPSR_T) != 0u;
    priv = (cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
    if (!ram || !budget ||
        budget > A64_STATIC_MAX_CHAIN_INSNS || !ram_size ||
        (ram_size & (ram_size - 1u)) != 0u ||
        ram_size - 1u > UINT32_MAX ||
        !validate_decoded_hits_at(
            first, cpu->r[15], thumb, budget, memory_hits) ||
        (graph_nodes &&
         (!cpu->fetch_host ||
          cpu->fetch_blk != (cpu->r[15] & ~UINT32_C(0x3ff)) ||
          cpu->fetch_gen != cpu->tlb_gen || cpu->fetch_priv != priv)))
        return false;
#if defined(S5LBOX_STATIC_A64_NATIVE)
    a64_static_read_context_t read_context = {
        cpu->dread,
        &cpu->dread_hits,
        cpu->tlb_gen,
        (cpu->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR ? 1u : 0u,
        cpu->vfp_s,
        &cpu->vfp_fpexc,
        &cpu->vfp_fpscr,
        vfp_cpacr_permits(cpu) ? 1u : 0u,
        vfp_fp_session ? 1u : 0u,
        cpu->bus && cpu->bus->host_ram_write ? cpu->dwrite : NULL,
        &cpu->dwrite_hits
    };
    a64_static_chain_context_t context = {
        .step = { first->uops, first->insn_count },
        .next = next,
        .opaque = opaque,
        .cycles = &cpu->cycles,
        .ram_mask = (uint64_t)ram_size - 1u,
        .budget = budget,
        .final_pc = cpu->r[15],
        .thumb = thumb,
        .graph_nodes = graph_nodes,
        .graph_fetch_host = cpu->fetch_host,
        .graph_fetch_block = cpu->fetch_blk,
        .graph_fetch_gen = cpu->fetch_gen,
        .graph_fetch_priv = priv ? 1u : 0u,
        .memory_hits = memory_hits
    };
    int result = a64_static_execute(cpu->r, &cpu->cpsr, &cpu->cycles,
                                    first->uops, 1u, ram,
                                    (uint64_t)ram_size - 1u,
                                    first->insn_count, &read_context, &context);
    if (result < 0) return false;
    *completed = context.completed;
    *blocks = context.blocks;
    return true;
#else
    (void)first;
    (void)next;
    (void)opaque;
    (void)graph_nodes;
    (void)memory_hits;
    (void)vfp_fp_session;
    return false;
#endif
}

bool a64_static_run_read_hits_chain(arm_cpu_t *cpu,
                                    const a64_static_block_t *first,
                                    uint8_t *ram, size_t ram_size,
                                    unsigned budget,
                                    a64_static_chain_next_fn next,
                                    void *opaque, unsigned *completed,
                                    unsigned *blocks) {
    return run_hits_chain(cpu, first, ram, ram_size, budget, next, opaque,
                          NULL, false, true, completed, blocks);
}

bool a64_static_run_memory_hits_chain(arm_cpu_t *cpu,
                                      const a64_static_block_t *first,
                                      uint8_t *ram, size_t ram_size,
                                      unsigned budget,
                                      a64_static_chain_next_fn next,
                                      void *opaque, bool vfp_fp_session,
                                      unsigned *completed,
                                      unsigned *blocks) {
    return run_hits_chain(cpu, first, ram, ram_size, budget, next, opaque,
                          NULL, true, vfp_fp_session, completed, blocks);
}

bool a64_static_run_read_hits_graph(
    arm_cpu_t *cpu, const a64_static_block_t *first,
    uint8_t *ram, size_t ram_size, unsigned budget,
    const a64_static_graph_node_t nodes[A64_STATIC_GRAPH_SLOTS],
    unsigned *completed, unsigned *blocks) {
    if (!nodes) return false;
    return run_hits_chain(cpu, first, ram, ram_size, budget, NULL, NULL,
                          nodes, false, true, completed, blocks);
}

bool a64_static_run_memory_hits_graph(
    arm_cpu_t *cpu, const a64_static_block_t *first,
    uint8_t *ram, size_t ram_size, unsigned budget,
    const a64_static_graph_node_t nodes[A64_STATIC_GRAPH_SLOTS],
    bool vfp_fp_session, unsigned *completed, unsigned *blocks) {
    if (!nodes) return false;
    return run_hits_chain(cpu, first, ram, ram_size, budget, NULL, NULL,
                          nodes, true, vfp_fp_session, completed, blocks);
}
