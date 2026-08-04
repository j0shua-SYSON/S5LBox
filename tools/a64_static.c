/* See a64_static.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "a64_static.h"

#include <limits.h>
#include <string.h>

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
    A64S_LDR = A64S_EORS_RR + 64u,
    A64S_STR = A64S_LDR + 64u,
    A64S_LDR_SP = A64S_STR + 64u,
    A64S_STR_SP = A64S_LDR_SP + 8u,
    A64S_COND = A64S_STR_SP + 8u,
    A64S_DP_IMM = A64S_COND + 14u,
    A64S_HANDLER_COUNT = A64S_DP_IMM + 16u * 2u * 15u * 16u
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

static bool arm_dp_encoding(uint32_t insn) {
    return (insn & UINT32_C(0x0c000000)) == 0u &&
           (insn & UINT32_C(0x01900000)) != UINT32_C(0x01000000) &&
           !((insn & UINT32_C(0x0e000000)) == 0u &&
             (insn & UINT32_C(0x00000090)) == UINT32_C(0x00000090));
}

static bool handler_touches_memory(uint32_t handler) {
    return handler >= A64S_LDR && handler < A64S_COND;
}

static bool decode_arm(uint32_t insn, unsigned index, unsigned insns,
                       uint32_t pc, a64_static_uop_t *out,
                       unsigned *written) {
    unsigned condition = insn >> 28;
    unsigned count = 0u;
    a64_static_uop_t *op = out;

    if (!written || condition == 15u) return false;
    *written = 0u;

    if ((insn & UINT32_C(0x0e000000)) == UINT32_C(0x0a000000)) {
        int32_t displacement;
        uint32_t target;
        /* A conditional branch has two possible block exits. It stays in the
         * literal path until the signed contract carries both destinations. */
        if (condition != 14u || (insn & (1u << 24)) != 0u ||
            index + 1u != insns)
            return false;
        displacement = (int32_t)(insn << 8) >> 6;
        target = pc + index * 4u + 8u + (uint32_t)displacement;
        op->handler = A64S_END;
        op->immediate = target;
        *written = 1u;
        return true;
    }

    if (condition < 14u) {
        op->handler = A64S_COND + condition;
        op++;
        count++;
    }

    if ((insn & UINT32_C(0x0c000000)) == UINT32_C(0x04000000)) {
        unsigned rn = (insn >> 16) & 15u;
        unsigned rd = (insn >> 12) & 15u;
        uint32_t offset = insn & UINT32_C(0x0fff);
        bool load = (insn & (1u << 20)) != 0u;
        if ((insn & (1u << 25)) != 0u ||
            (insn & (1u << 24)) == 0u ||
            (insn & (1u << 22)) != 0u ||
            (insn & (1u << 21)) != 0u || rd > 7u || rn > 7u)
            return false;
        if ((insn & (1u << 23)) == 0u) offset = 0u - offset;
        op->handler = rr(load ? A64S_LDR : A64S_STR, rd, rn);
        op->immediate = offset;
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
            *written = count + 1u;
            return true;
        }

        /* Keep the original no-shift r0-r7 register proof. Broader register
         * forms need an exact barrel-shifter contract and are a later gate. */
        if (set_flags || rd > 7u || rn > 7u ||
            (insn & UINT32_C(0x00000ff0)) != 0u)
            return false;
        {
            unsigned rm = insn & 15u;
            if (rm > 7u) return false;
            switch (opcode) {
            case 1u:  base = A64S_EOR_RRR; break;
            case 2u:  base = A64S_SUB_RRR; break;
            case 4u:  base = A64S_ADD_RRR; break;
            case 12u: base = A64S_ORR_RRR; break;
            default: return false;
            }
            op->handler = rrr(base, rd, rn, rm);
            *written = count + 1u;
            return true;
        }
    }

    return false;
}

static bool decode_thumb(uint16_t insn, unsigned index, unsigned insns,
                         uint32_t pc,
                         a64_static_uop_t *out) {
    if ((insn & UINT16_C(0xf800)) == UINT16_C(0xe000)) {
        int32_t displacement = (int32_t)((uint32_t)(insn & 0x07ffu) << 21) >> 20;
        uint32_t target = pc + index * 2u + 4u + (uint32_t)displacement;
        if (index + 1u != insns) return false;
        out->handler = A64S_END;
        out->immediate = target;
        return true;
    }

    if ((insn & UINT16_C(0xf800)) == UINT16_C(0x3000) ||
        (insn & UINT16_C(0xf800)) == UINT16_C(0x3800)) {
        unsigned rd = (insn >> 8) & 7u;
        bool subtract = (insn & UINT16_C(0x0800)) != 0u;
        out->handler = rr(subtract ? A64S_SUBS_IMM : A64S_ADDS_IMM,
                          rd, rd);
        out->immediate = insn & 255u;
        return true;
    }

    if ((insn & UINT16_C(0xffc0)) == UINT16_C(0x4040)) {
        unsigned rm = (insn >> 3) & 7u;
        unsigned rd = insn & 7u;
        out->handler = rr(A64S_EORS_RR, rd, rm);
        out->immediate = 0u;
        return true;
    }

    if ((insn & UINT16_C(0xf000)) == UINT16_C(0x9000)) {
        unsigned rd = (insn >> 8) & 7u;
        bool load = (insn & UINT16_C(0x0800)) != 0u;
        out->handler = (load ? A64S_LDR_SP : A64S_STR_SP) + rd;
        out->immediate = (uint32_t)(insn & 255u) * 4u;
        return true;
    }

    return false;
}

static bool decode_program_at(const void *program, unsigned insns, bool thumb,
                              uint32_t pc, bool guest_bytes,
                              a64_static_block_t *out) {
    const uint8_t *bytes = (const uint8_t *)program;
    unsigned uop_count = 0u;
    uint32_t fallthrough;
    if (!program || !out || !insns || insns > A64_STATIC_MAX_INSNS ||
        (pc & (thumb ? 1u : 3u)) != 0u)
        return false;
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < insns; i++) {
        bool ok;
        unsigned added = 1u;
        if (thumb) {
            const uint8_t *p = bytes + i * 2u;
            ok = decode_thumb(guest_bytes ? read_le16(p) : read_native16(p),
                              i, insns, pc, &out->uops[uop_count]);
        } else {
            const uint8_t *p = bytes + i * 4u;
            ok = decode_arm(guest_bytes ? read_le32(p) : read_native32(p),
                            i, insns, pc, &out->uops[uop_count], &added);
        }
        if (!ok || !added || added > A64_STATIC_MAX_UOPS - uop_count) {
            memset(out, 0, sizeof *out);
            return false;
        }
        for (unsigned j = 0u; j < added; j++) {
            uint32_t handler = out->uops[uop_count + j].handler;
            if (handler >= A64S_HANDLER_COUNT ||
                (i + 1u != insns && handler == A64S_END)) {
                memset(out, 0, sizeof *out);
                return false;
            }
            if (handler_touches_memory(handler))
                out->touches_memory = true;
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
    return decode_program_at(program, insns, thumb, pc, false, out);
}

bool a64_static_decode_bytes_at(const uint8_t *program, unsigned insns,
                                bool thumb, uint32_t pc,
                                a64_static_block_t *out) {
    return decode_program_at(program, insns, thumb, pc, true, out);
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

#if defined(S5LBOX_STATIC_A64_NATIVE)
extern int a64_static_execute(uint32_t *regs, uint32_t *cpsr,
                              uint64_t *cycles,
                              const a64_static_uop_t *uops,
                              uint64_t blocks, uint8_t *ram,
                              uint64_t ram_mask, uint64_t block_insns);
#endif

bool a64_static_run(arm_cpu_t *cpu, const a64_static_block_t *block,
                    uint64_t blocks, uint8_t *ram, size_t ram_size) {
    if (!cpu || !block || !blocks || !ram ||
        !block->insn_count || block->insn_count > A64_STATIC_MAX_INSNS ||
        block->uop_count < block->insn_count ||
        block->uop_count > block->insn_count * 2u + 1u ||
        block->uop_count > A64_STATIC_MAX_UOPS ||
        block->uops[block->uop_count - 1u].handler != A64S_END ||
        block->uops[block->uop_count - 1u].immediate != block->exit_pc ||
        cpu->r[15] != block->start_pc ||
        ((cpu->cpsr & ARM_CPSR_T) != 0u) != block->thumb ||
        (blocks > 1u && block->exit_pc != block->start_pc) ||
        !ram_size || (ram_size & (ram_size - 1u)) != 0u ||
        ram_size - 1u > UINT32_MAX)
        return false;
    for (unsigned i = 0; i < block->uop_count - 1u; i++) {
        uint32_t handler = block->uops[i].handler;
        if (handler == A64S_END || handler >= A64S_HANDLER_COUNT)
            return false;
        if (handler >= A64S_COND && handler < A64S_DP_IMM) {
            uint32_t next_handler;
            if (i + 1u >= block->uop_count - 1u) return false;
            next_handler = block->uops[i + 1u].handler;
            if (next_handler == A64S_END ||
                (next_handler >= A64S_COND && next_handler < A64S_DP_IMM))
                return false;
        }
        if (handler >= A64S_DP_IMM && block->uops[i].metadata >
                                         A64S_CARRY_SET)
            return false;
    }
#if defined(S5LBOX_STATIC_A64_NATIVE)
    return a64_static_execute(cpu->r, &cpu->cpsr, &cpu->cycles,
                              block->uops, blocks, ram,
                              (uint64_t)ram_size - 1u,
                              block->insn_count) == 0;
#else
    (void)blocks;
    return false;
#endif
}
