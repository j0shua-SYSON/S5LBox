/* See arm_bulk.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm_bulk.h"

/* Complete word-at-a-time length routine, including alignment masking and
 * conditional epilogue. A native call must reproduce caller-clobbered
 * registers and NZCV too, not merely the C ABI's return value. */
static const uint32_t length_words[] = {
    0xe1a0c000u, 0xe2103003u, 0xe3c00003u, 0xe4902004u,
    0x0a000003u, 0xe3530002u, 0xe38220ffu, 0xa3822cffu,
    0xc38228ffu, 0xe3a01001u, 0xe1811401u, 0xe1811801u,
    0xe0423001u, 0xe1c33002u, 0xe1130381u, 0x04902004u,
    0x0afffffau, 0xe2400001u, 0xe31200ffu, 0x02400001u,
    0x13120cffu, 0x02400001u, 0x131208ffu, 0x02400001u,
    0xe040000cu, 0xe12fff1eu,
};

/* Bounded signed-byte range comparison. Match the complete loop, including
 * its back edge and both range-end checks; the surrounding caller/ABI is not
 * assumed. Entry is the first LDRSB (word 5). */
static const uint32_t compare_words[] = {
    0xe2800001u, 0xe28cc001u, 0xe1510000u, 0x115e000cu,
    0x0a000003u, 0xe1d020d0u, 0xe1dc30d0u, 0xe1520003u,
    0x0afffff6u,
};

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool matches(const arm_bulk_memory_t *memory, uint32_t offset,
                     const uint32_t *words, unsigned count) {
    if (offset > memory->code_bytes || count * 4u > memory->code_bytes - offset)
        return false;
    for (unsigned i = 0u; i < count; i++)
        if (read32(memory->code + offset + i * 4u) != words[i]) return false;
    return true;
}

static bool current_translation(const arm_cpu_t *cpu) {
    return cpu->tlb_stamp.sctlr == cpu->cp15.sctlr &&
           cpu->tlb_stamp.ttbr0 == cpu->cp15.ttbr0 &&
           cpu->tlb_stamp.ttbr1 == cpu->cp15.ttbr1 &&
           cpu->tlb_stamp.ttbcr == cpu->cp15.ttbcr &&
           cpu->tlb_stamp.dacr == cpu->cp15.dacr &&
           cpu->tlb_stamp.context_id == cpu->cp15.context_id;
}

/* Only an already proved plain-RAM read can be issued. In particular a
 * missing mapping is not replaced with zero, and the helper cannot consume
 * a device register while deciding whether to decline. */
static const uint8_t *word_at(const arm_cpu_t *cpu,
                               const arm_bulk_memory_t *memory,
                               uint32_t va) {
    if (memory->flat_ram) {
        size_t offset = (size_t)va & (memory->flat_size - 1u);
        if (offset > memory->flat_size - 4u) return NULL;
        return memory->flat_ram + offset;
    }
    unsigned index = (va >> 10) & (ARM_DREAD_ENTRIES - 1u);
    uint32_t block = va & ~UINT32_C(0x3ff);
    if (!memory->data_cache || !cpu->bus || !cpu->bus->host_ram ||
        !cpu->dread[index].host || cpu->dread[index].tag != block ||
        cpu->dread[index].gen != cpu->tlb_gen)
        return NULL;
    return cpu->dread[index].host + (va & UINT32_C(0x3ff));
}

static uint32_t compare_flags(uint32_t cpsr, uint32_t a, uint32_t b) {
    const uint32_t result = a - b;
    cpsr &= ~(ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V);
    cpsr |= result & ARM_CPSR_N;
    if (result == 0u) cpsr |= ARM_CPSR_Z;
    if (a >= b) cpsr |= ARM_CPSR_C;
    if ((a ^ b) & (a ^ result) & UINT32_C(0x80000000)) cpsr |= ARM_CPSR_V;
    return cpsr;
}

static unsigned compare_loop(arm_cpu_t *cpu, const arm_bulk_memory_t *memory,
                              unsigned budget) {
    uint32_t left = cpu->r[0], right = cpu->r[12];
    uint32_t a = cpu->r[2], b = cpu->r[3], flags = cpu->cpsr;
    unsigned retired = 0u, reads = 0u;
    bool done = false;
    while (budget - retired >= 4u) {
        const uint8_t *lp = word_at(cpu, memory, left & ~UINT32_C(3));
        const uint8_t *rp = word_at(cpu, memory, right & ~UINT32_C(3));
        if (!lp || !rp) break;
        uint32_t next_a = lp[left & 3u], next_b = rp[right & 3u];
        if (next_a & 128u) next_a |= UINT32_C(0xffffff00);
        if (next_b & 128u) next_b |= UINT32_C(0xffffff00);
        unsigned cost = next_a == next_b ? 9u : 4u;
        if (budget - retired < cost) break;
        a = next_a; b = next_b;
        flags = compare_flags(flags, a, b);
        reads += 2u;
        retired += cost;
        if (a != b) { done = true; break; }
        left++; right++;
        flags = compare_flags(flags, cpu->r[1], left);
        if (cpu->r[1] != left)
            flags = compare_flags(flags, cpu->r[14], right);
        if (flags & ARM_CPSR_Z) { done = true; break; }
    }
    if (!retired) return 0u;
    cpu->r[0] = left; cpu->r[12] = right;
    cpu->r[2] = a; cpu->r[3] = b; cpu->cpsr = flags;
    if (done) cpu->r[15] += 16u;
    if (!memory->flat_ram) cpu->dread_hits += reads;
    return retired;
}

/* Resume an arbitrarily long length scan at its loop header. Each admitted
 * iteration exactly includes SUB/BIC/TST/LDR/BEQ; a cold next block, NUL or
 * budget boundary stops before that iteration. The ordinary runner handles
 * the epilogue and faults. This keeps both memory reads and device latency
 * bounded without requiring the complete string to fit in one run slice. */
static unsigned length_loop(arm_cpu_t *cpu, const arm_bulk_memory_t *memory,
                             unsigned budget) {
    if (cpu->r[1] != UINT32_C(0x01010101) || (cpu->r[0] & 3u)) return 0u;
    uint32_t address = cpu->r[0], word = cpu->r[2], scratch = cpu->r[3];
    unsigned count = 0u;
    while (count < budget / 5u) {
        uint32_t next_scratch = (word - UINT32_C(0x01010101)) & ~word;
        if (next_scratch & UINT32_C(0x80808080)) break;
        const uint8_t *source = word_at(cpu, memory, address);
        if (!source) break;
        scratch = next_scratch;
        word = read32(source);
        address += 4u;
        count++;
    }
    if (!count) return 0u;
    cpu->r[0] = address; cpu->r[2] = word; cpu->r[3] = scratch;
    cpu->cpsr = (cpu->cpsr & ~(ARM_CPSR_N | ARM_CPSR_C)) | ARM_CPSR_Z;
    if (!memory->flat_ram) cpu->dread_hits += count;
    return count * 5u;
}

unsigned arm_bulk_string_try(arm_cpu_t *cpu, const arm_bulk_memory_t *memory,
                             unsigned budget) {
    uint32_t offset;
    if (!cpu || !memory || !memory->code || budget < 4u ||
        cpu->arch != ARM_ARCH_V6_ARM1176 ||
        (cpu->cpsr & (ARM_CPSR_MODE_MASK | ARM_CPSR_T | ARM_CPSR_E)) !=
            ARM_MODE_USR || cpu->abort_pending ||
        (cpu->irq_line && !(cpu->cpsr & ARM_CPSR_I)) ||
        (cpu->fiq_line && !(cpu->cpsr & ARM_CPSR_F)) ||
        (cpu->r[15] & 3u) ||
        (uint64_t)memory->code_base + memory->code_bytes >
            UINT64_C(0x100000000))
        return 0u;
    if (memory->flat_ram) {
        if ((cpu->cp15.sctlr & ARM_SCTLR_M) || memory->flat_size < 4u ||
            (memory->flat_size & (memory->flat_size - 1u)) ||
            memory->flat_size - 1u > UINT32_MAX)
            return 0u;
    } else if (!memory->data_cache || !current_translation(cpu)) {
        return 0u;
    }
    offset = cpu->r[15] - memory->code_base;
    if (offset > memory->code_bytes || memory->code_bytes - offset < 4u)
        return 0u;
    uint32_t first = read32(memory->code + offset);
    if (first == compare_words[5]) {
        if (offset < 20u || !matches(memory, offset - 20u, compare_words, 9u))
            return 0u;
        return compare_loop(cpu, memory, budget);
    }
    if (first == length_words[12]) {
        if (offset < 48u || !matches(memory, offset - 48u, length_words, 26u))
            return 0u;
        return length_loop(cpu, memory, budget);
    }
    if (first != length_words[0] || (cpu->r[14] & 3u) == 2u ||
        !matches(memory, offset, length_words, 26u)) return 0u;

    const uint32_t original = cpu->r[0];
    const unsigned alignment = original & 3u;
    const unsigned fixed = 17u + (alignment ? 4u : 0u);
    if (budget < fixed + 5u) return 0u;
    const unsigned max_words = (budget - fixed) / 5u;
    uint32_t address = original & ~UINT32_C(3);
    uint32_t word = 0u, scratch = 0u;
    unsigned words = 0u;
    for (; words < max_words;) {
        const uint8_t *source = word_at(cpu, memory, address);
        if (!source) return 0u;
        word = read32(source);
        if (words == 0u && alignment)
            word |= UINT32_MAX >> (32u - alignment * 8u);
        words++;
        scratch = (word - UINT32_C(0x01010101)) & ~word;
        if (scratch & UINT32_C(0x80808080)) break;
        if (address > UINT32_MAX - 4u) return 0u;
        address += 4u;
    }
    if (!(scratch & UINT32_C(0x80808080))) return 0u;
    unsigned zero = 0u;
    while (((word >> (zero * 8u)) & 255u) != 0u) zero++;
    uint32_t flags = cpu->cpsr & ~(ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C |
                                   ARM_CPSR_T);
    if (alignment) flags &= ~ARM_CPSR_V;
    if (zero < 3u) flags |= ARM_CPSR_Z;
    if (cpu->r[14] & 1u) flags |= ARM_CPSR_T;

    /* No mutation precedes complete instruction, memory and budget proofs. */
    cpu->r[0] = address + zero - original;
    cpu->r[1] = UINT32_C(0x01010101);
    cpu->r[2] = word;
    cpu->r[3] = scratch;
    cpu->r[12] = original;
    cpu->r[15] = cpu->r[14] & ~UINT32_C(1);
    cpu->cpsr = flags;
    if (!memory->flat_ram) cpu->dread_hits += words;
    return fixed + words * 5u;
}
