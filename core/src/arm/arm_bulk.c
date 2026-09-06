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

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static const uint8_t *chain_word_at(const arm_cpu_t *cpu,
                                    const arm_bulk_memory_t *memory,
                                    uint32_t address, unsigned *tlb_reads) {
    if (address & 3u) return NULL;
    const uint8_t *p = word_at(cpu, memory, address);
    if (p || !memory->ram_window) return p;
    p = arm_ram_window_tlb_lookup(memory->ram_window, cpu, address,
                                  ARM_ACCESS_READ, false);
    if (!p) return NULL;
    (*tlb_reads)++;
    return p + (address & 1023u);
}

/* Batch complete read-only pointer-search iterations. The first shape is
 * register-parametric: MOV previous,current; LDR current,[walk,next_offset];
 * CMP/BEQ null; LDR value,[current,key_offset]; MOVS walk,current;
 * CMP/BEQ equal; CMP needle,value; BHI header. Only the full back-edge path
 * is admitted. Exits, cold mappings and partial budgets stay literal. */
static unsigned thumb_ordered_chain(arm_cpu_t *cpu,
                                     const arm_bulk_memory_t *memory,
                                     uint32_t offset, unsigned budget) {
    if (memory->code_bytes - offset < 20u || budget < 10u) return 0u;
    uint16_t h[10];
    for (unsigned i = 0u; i < 10u; i++)
        h[i] = read16(memory->code + offset + 2u * i);
    if ((h[0] & 0xff00u) != 0x4600u || (h[1] & 0xfe00u) != 0x5800u ||
        (h[4] & 0xfe00u) != 0x5800u || (h[6] & 0xffc0u) != 0x4280u ||
        (h[3] & 0xff00u) != 0xd000u || (h[7] & 0xff00u) != 0xd000u ||
        h[9] != 0xd8f5u) return 0u;
    unsigned previous = (h[0] & 7u) | ((h[0] >> 4) & 8u);
    unsigned current = h[1] & 7u, walk = (h[1] >> 3) & 7u;
    unsigned next_offset = (h[1] >> 6) & 7u;
    unsigned value = h[4] & 7u, key_offset = (h[4] >> 6) & 7u;
    unsigned needle = (h[6] >> 3) & 7u;
    unsigned roles[] = {previous, current, walk, value, next_offset, key_offset, needle};
    if (previous == 15u || ((h[0] >> 3) & 15u) != current ||
        h[2] != (uint16_t)(0x2800u | current << 8) ||
        ((h[4] >> 3) & 7u) != current ||
        h[5] != (uint16_t)(0x1c00u | current << 3 | walk) ||
        (h[6] & 7u) != value ||
        h[8] != (uint16_t)(0x4280u | value << 3 | needle)) return 0u;
    for (unsigned i = 0u; i < 7u; i++)
        for (unsigned j = 0u; j < i; j++)
            if (roles[i] == roles[j]) return 0u;
    uint32_t cur = cpu->r[current], walker = cpu->r[walk];
    uint32_t prev = cpu->r[previous], key = cpu->r[value];
    const uint32_t wanted = cpu->r[needle];
    unsigned count = 0u, tlb_reads = 0u;
    while (count < budget / 10u) {
        unsigned iteration_reads = 0u;
        const uint8_t *np = chain_word_at(cpu, memory,
            walker + cpu->r[next_offset], &iteration_reads);
        if (!np) break;
        uint32_t next = read32(np);
        if (!next) break;
        const uint8_t *kp = chain_word_at(cpu, memory,
            next + cpu->r[key_offset], &iteration_reads);
        if (!kp) break;
        uint32_t next_key = read32(kp);
        if (wanted <= next_key) break;
        prev = cur; cur = next; walker = next; key = next_key;
        tlb_reads += iteration_reads;
        count++;
    }
    if (!count) return 0u;
    cpu->r[previous] = prev; cpu->r[current] = cur;
    cpu->r[walk] = walker; cpu->r[value] = key;
    cpu->cpsr = compare_flags(cpu->cpsr, wanted, key);
    if (!memory->flat_ram) {
        cpu->dread_hits += 2u * count - tlb_reads;
        cpu->tlb_hits += tlb_reads;
    }
    return 10u * count;
}

/* A second read-only chain shape includes a depth comparison and a target
 * loaded through an invariant stack slot. Match every instruction in the
 * cycle, including the final backward branch. The other branch destinations
 * are irrelevant only because each admitted iteration proves them untaken. */
static unsigned thumb_filtered_chain(arm_cpu_t *cpu,
                                      const arm_bulk_memory_t *memory,
                                      uint32_t offset, unsigned budget) {
    static const uint16_t shape[] = {
        0x5919u, 0x2900u, 0xd000u, 0x2100u, 0x1c1eu, 0x468bu,
        0x4562u, 0xd000u, 0x595bu, 0x2b00u, 0xd000u, 0x4582u,
        0xd100u, 0x4641u, 0x585au, 0x9900u, 0x6809u, 0x428au, 0xd1ecu,
    };
    if (memory->code_bytes - offset < sizeof shape || budget < 19u ||
        cpu->r[10] != cpu->r[0]) return 0u;
    for (unsigned i = 0u; i < sizeof shape / sizeof shape[0]; i++) {
        unsigned mask = (i == 2u || i == 7u || i == 10u || i == 12u || i == 15u)
            ? 0xff00u : 0xffffu;
        if ((read16(memory->code + offset + 2u * i) & mask) != shape[i])
            return 0u;
    }
    uint32_t stack_offset = (read16(memory->code + offset + 30u) & 255u) * 4u;
    unsigned invariant_reads = 0u;
    const uint8_t *slot = chain_word_at(cpu, memory,
        cpu->r[13] + stack_offset, &invariant_reads);
    if (!slot) return 0u;
    const uint8_t *target = chain_word_at(cpu, memory, read32(slot), &invariant_reads);
    if (!target) return 0u;
    const uint32_t wanted = read32(target);
    uint32_t current = cpu->r[3], value = cpu->r[2], previous = cpu->r[6];
    unsigned count = 0u, tlb_reads = 0u;
    while (count < budget / 19u) {
        unsigned iteration_reads = invariant_reads;
        const uint8_t *payload = chain_word_at(cpu, memory,
            current + cpu->r[4], &iteration_reads);
        if (!payload || !read32(payload) || value == cpu->r[12]) break;
        const uint8_t *link = chain_word_at(cpu, memory,
            current + cpu->r[5], &iteration_reads);
        if (!link) break;
        uint32_t next = read32(link);
        if (!next) break;
        const uint8_t *key = chain_word_at(cpu, memory,
            next + cpu->r[8], &iteration_reads);
        if (!key) break;
        uint32_t next_value = read32(key);
        if (next_value == wanted) break;
        previous = current; current = next; value = next_value;
        tlb_reads += iteration_reads;
        count++;
    }
    if (!count) return 0u;
    cpu->r[1] = wanted; cpu->r[2] = value; cpu->r[3] = current;
    cpu->r[6] = previous; cpu->r[11] = 0u;
    cpu->cpsr = compare_flags(cpu->cpsr, value, wanted);
    if (!memory->flat_ram) {
        cpu->dread_hits += 5u * count - tlb_reads;
        cpu->tlb_hits += tlb_reads;
    }
    return 19u * count;
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
        (cpu->cpsr & (ARM_CPSR_MODE_MASK | ARM_CPSR_E)) !=
            ARM_MODE_USR || cpu->abort_pending ||
        (cpu->irq_line && !(cpu->cpsr & ARM_CPSR_I)) ||
        (cpu->fiq_line && !(cpu->cpsr & ARM_CPSR_F)) ||
        (cpu->r[15] & ((cpu->cpsr & ARM_CPSR_T) ? 1u : 3u)) ||
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
    if (cpu->cpsr & ARM_CPSR_T) {
        unsigned count = thumb_ordered_chain(cpu, memory, offset, budget);
        return count ? count : thumb_filtered_chain(cpu, memory, offset, budget);
    }
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
