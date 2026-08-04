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
    A64S_HANDLER_COUNT = A64S_STR_SP + 8u
};

_Static_assert(A64S_HANDLER_COUNT == A64_STATIC_HANDLER_COUNT,
               "C decoder and generated handler table disagree");

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

static bool decode_arm(uint32_t insn, unsigned index, unsigned insns,
                       uint32_t pc,
                       a64_static_uop_t *out) {
    if ((insn >> 28) != 14u) return false;

    if ((insn & UINT32_C(0x0e000000)) == UINT32_C(0x0a000000)) {
        int32_t displacement;
        uint32_t target;
        if ((insn & (1u << 24)) != 0u || index + 1u != insns)
            return false;
        displacement = (int32_t)(insn << 8) >> 6;
        target = pc + index * 4u + 8u + (uint32_t)displacement;
        out->handler = A64S_END;
        out->immediate = target;
        return true;
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
        out->handler = rr(load ? A64S_LDR : A64S_STR, rd, rn);
        out->immediate = offset;
        return true;
    }

    if ((insn & UINT32_C(0x0c000000)) == 0u) {
        unsigned opcode = (insn >> 21) & 15u;
        unsigned rn = (insn >> 16) & 15u;
        unsigned rd = (insn >> 12) & 15u;
        unsigned base;
        if ((insn & (1u << 20)) != 0u || rd > 7u || rn > 7u)
            return false;
        if ((insn & (1u << 25)) != 0u) {
            uint32_t immediate = ror32(insn & 255u,
                                       2u * ((insn >> 8) & 15u));
            switch (opcode) {
            case 1u:  base = A64S_EOR_IMM; break;
            case 2u:  base = A64S_SUB_IMM; break;
            case 4u:  base = A64S_ADD_IMM; break;
            default: return false;
            }
            out->handler = rr(base, rd, rn);
            out->immediate = immediate;
            return true;
        }

        if ((insn & UINT32_C(0x00000ff0)) != 0u) return false;
        unsigned rm = insn & 15u;
        if (rm > 7u) return false;
        switch (opcode) {
        case 1u:  base = A64S_EOR_RRR; break;
        case 2u:  base = A64S_SUB_RRR; break;
        case 4u:  base = A64S_ADD_RRR; break;
        case 12u: base = A64S_ORR_RRR; break;
        default: return false;
        }
        out->handler = rrr(base, rd, rn, rm);
        out->immediate = 0u;
        return true;
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
    unsigned uop_count;
    uint32_t fallthrough;
    if (!program || !out || !insns || insns > A64_STATIC_MAX_INSNS ||
        (pc & (thumb ? 1u : 3u)) != 0u)
        return false;
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < insns; i++) {
        bool ok;
        if (thumb) {
            const uint8_t *p = bytes + i * 2u;
            ok = decode_thumb(guest_bytes ? read_le16(p) : read_native16(p),
                              i, insns, pc, &out->uops[i]);
        } else {
            const uint8_t *p = bytes + i * 4u;
            ok = decode_arm(guest_bytes ? read_le32(p) : read_native32(p),
                            i, insns, pc, &out->uops[i]);
        }
        if (!ok || out->uops[i].handler >= A64S_HANDLER_COUNT) {
            memset(out, 0, sizeof *out);
            return false;
        }
        if (out->uops[i].handler >= A64S_LDR)
            out->touches_memory = true;
        if (i + 1u != insns && out->uops[i].handler == A64S_END) {
            memset(out, 0, sizeof *out);
            return false;
        }
    }
    uop_count = insns;
    fallthrough = pc + insns * (thumb ? 2u : 4u);
    if (out->uops[insns - 1u].handler != A64S_END) {
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
        (block->uop_count != block->insn_count &&
         block->uop_count != block->insn_count + 1u) ||
        block->uop_count > A64_STATIC_MAX_UOPS ||
        block->uops[block->uop_count - 1u].handler != A64S_END ||
        block->uops[block->uop_count - 1u].immediate != block->exit_pc ||
        cpu->r[15] != block->start_pc ||
        ((cpu->cpsr & ARM_CPSR_T) != 0u) != block->thumb ||
        (blocks > 1u && block->exit_pc != block->start_pc) ||
        !ram_size || (ram_size & (ram_size - 1u)) != 0u ||
        ram_size - 1u > UINT32_MAX)
        return false;
    for (unsigned i = 0; i < block->uop_count - 1u; i++)
        if (block->uops[i].handler == A64S_END ||
            block->uops[i].handler >= A64S_HANDLER_COUNT)
            return false;
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
