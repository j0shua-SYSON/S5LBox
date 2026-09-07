/* See arm_bulk.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm_bulk.h"
#include <stdlib.h>
#include <string.h>

#define BULK_SEGMENT 32u
#define BULK_PAGES 8u
#define BULK_ENTRIES 256u

typedef struct {
    uint32_t address, reads;
    uint32_t words[8];
    /* Used only while building, in this read-only call; never dereferenced by
     * a cache hit. Validation resolves a fresh live pointer for every page. */
    const uint8_t *source;
    uint8_t bytes[1024];
} bulk_page_t;

typedef struct {
    bool valid, empty_path;
    unsigned kind, pages, retired, loads;
    uint32_t start, offsets[4];
    uint32_t previous, current, value;
    uint32_t low, high, head_low, head_high;
    bulk_page_t page[BULK_PAGES];
} bulk_segment_t;

struct arm_bulk_cache {
    uint64_t hits;
    bulk_segment_t entry[BULK_ENTRIES];
};

arm_bulk_cache_t *arm_bulk_cache_create(void) {
    return calloc(1u, sizeof(arm_bulk_cache_t));
}

void arm_bulk_cache_destroy(arm_bulk_cache_t *cache) { free(cache); }

void arm_bulk_cache_reset(arm_bulk_cache_t *cache) {
    if (!cache) return;
    for (unsigned i = 0u; i < BULK_ENTRIES; i++) cache->entry[i].valid = false;
    cache->hits = 0u;
}

uint64_t arm_bulk_cache_hits(const arm_bulk_cache_t *cache) {
    return cache ? cache->hits : 0u;
}

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
                                    uint32_t address, unsigned *tlb_reads,
                                    unsigned *walk_reads) {
    if (address & 3u) return NULL;
    const uint8_t *p = word_at(cpu, memory, address);
    if (p || !memory->ram_window) return p;
    bool walked = false;
    p = arm_ram_window_read_resolve(memory->ram_window, cpu, address, &walked);
    if (!p) return NULL;
    if (walked) (*walk_reads)++;
    else (*tlb_reads)++;
    return p + (address & 1023u);
}

static bulk_segment_t *segment_slot(const arm_bulk_memory_t *memory,
                                     unsigned kind, uint32_t start,
                                     const uint32_t offsets[4]) {
    if (!memory->cache || (memory->flat_ram && memory->flat_size < 1024u))
        return NULL;
    uint32_t hash = start * UINT32_C(0x9e3779b1) + kind;
    for (unsigned i = 0u; i < 4u; i++)
        hash = (hash ^ offsets[i]) * UINT32_C(0x85ebca6b);
    return &memory->cache->entry[(hash ^ (hash >> 16)) & (BULK_ENTRIES - 1u)];
}

static bool segment_key(const bulk_segment_t *entry, unsigned kind,
                          uint32_t start, const uint32_t offsets[4],
                          unsigned budget) {
    return entry && entry->valid && entry->kind == kind &&
        entry->start == start && entry->retired <= budget &&
        memcmp(entry->offsets, offsets, sizeof entry->offsets) == 0;
}

static void segment_begin(bulk_segment_t *entry, unsigned kind,
                            uint32_t start, const uint32_t offsets[4]) {
    if (!entry) return;
    memset(entry, 0, offsetof(bulk_segment_t, page));
    entry->kind = kind; entry->start = start;
    memcpy(entry->offsets, offsets, sizeof entry->offsets);
    entry->low = entry->head_low = UINT32_MAX;
}

static const uint8_t *segment_word(const arm_cpu_t *cpu,
                                    const arm_bulk_memory_t *memory,
                                    bulk_segment_t *entry, uint32_t address,
                                    unsigned *tlb_reads, unsigned *walk_reads) {
    const uint8_t *p = chain_word_at(cpu, memory, address, tlb_reads, walk_reads);
    if (!p || !entry || entry->pages > BULK_PAGES) return p;
    uint32_t page = address & ~UINT32_C(1023);
    unsigned i = 0u;
    while (i < entry->pages && entry->page[i].address != page) i++;
    if (i == entry->pages) {
        if (entry->pages++ == BULK_PAGES) return p;
        entry->page[i].address = page;
        entry->page[i].source = p - (address & 1023u);
        entry->page[i].reads = 0u;
        memset(entry->page[i].words, 0, sizeof entry->page[i].words);
    }
    unsigned word = (address & 1023u) / 4u;
    entry->page[i].words[word / 32u] |= UINT32_C(1) << (word % 32u);
    entry->page[i].reads++;
    entry->loads++;
    return p;
}

static void segment_finish(bulk_segment_t *entry, uint32_t previous,
                             uint32_t current, uint32_t value) {
    if (!entry || entry->pages > BULK_PAGES) return;
    entry->previous = previous; entry->current = current; entry->value = value;
    for (unsigned i = 0u; i < entry->pages; i++) {
        memcpy(entry->page[i].bytes, entry->page[i].source, 1024u);
        entry->page[i].source = NULL;
    }
    entry->valid = true;
}

/* Re-reading the witnessed plain-RAM bytes catches ALL writers, including direct
 * native stores, DMA, bridges, restore and aliases. Translation generation
 * alone is not a content witness. A changed mapping is acceptable only when
 * its newly proved contents reproduce every original load. No hashes decide
 * equality, no old host pointer is followed, and no guest cache is published.
 * Most pages compare equal at once. If unrelated data in the page changed,
 * check the exact loaded words before discarding useful search work. A word
 * mask records dependencies, not a hash of their values.
 * Logical load accounting uses each page's live witness classification. */
static bool segment_validate(const arm_cpu_t *cpu,
                               const arm_bulk_memory_t *memory,
                               bulk_segment_t *entry,
                               unsigned *tlb_reads, unsigned *walk_reads) {
    unsigned reads = 0u, walks = 0u;
    for (unsigned i = 0u; i < entry->pages; i++) {
        bulk_page_t *page = &entry->page[i];
        unsigned read = 0u, walk = 0u;
        const uint8_t *p = chain_word_at(cpu, memory, page->address, &read, &walk);
        if (!p) return false;
        if (memcmp(p, page->bytes, 1024u) != 0) {
            for (unsigned group = 0u; group < 8u; group++) {
                uint32_t words = page->words[group];
                while (words) {
                    unsigned bit = 0u;
#if defined(__GNUC__) || defined(__clang__)
                    bit = (unsigned)__builtin_ctz(words);
#else
                    while (!(words & (UINT32_C(1) << bit))) bit++;
#endif
                    unsigned at = (group * 32u + bit) * 4u;
                    if (read32(p + at) != read32(page->bytes + at)) return false;
                    words &= words - 1u;
                }
            }
            memcpy(page->bytes, p, 1024u);
        }
        reads += read * page->reads;
        walks += walk * page->reads;
    }
    *tlb_reads += reads; *walk_reads += walks;
    memory->cache->hits++;
    return true;
}

static void segment_range(uint32_t value, uint32_t *low, uint32_t *high) {
    if (value < *low) *low = value;
    if (value > *high) *high = value;
}

static bool outside(uint32_t value, uint32_t low, uint32_t high) {
    return value < low || value > high;
}

/* Batch complete read-only pointer-search iterations. The first shape is
 * register-parametric: MOV previous,current; LDR current,[walk,next_offset];
 * CMP/BEQ null; LDR value,[current,key_offset]; MOVS walk,current;
 * CMP/BEQ equal; CMP needle,value; BHI header. Only the full back-edge path
 * is admitted. Exits, unproved mappings and partial budgets stay literal. */
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
    unsigned count = 0u, tlb_reads = 0u, walk_reads = 0u;
    const uint32_t offsets[4] = {cpu->r[next_offset], cpu->r[key_offset], 0u, 0u};
    unsigned part = 0u;
    bulk_segment_t *building = NULL;
    while (count < budget / 10u) {
        if (part == 0u) {
            bulk_segment_t *entry = segment_slot(memory, 1u, walker, offsets);
            if (segment_key(entry, 1u, walker, offsets, budget - count * 10u) &&
                wanted > entry->high &&
                segment_validate(cpu, memory, entry, &tlb_reads, &walk_reads)) {
                prev = entry->previous; cur = walker = entry->current;
                key = entry->value; count += BULK_SEGMENT;
                continue;
            }
            building = budget / 10u - count >= BULK_SEGMENT ? entry : NULL;
            segment_begin(building, 1u, walker, offsets);
        }
        unsigned iteration_reads = 0u, iteration_walks = 0u;
        const uint8_t *np = segment_word(cpu, memory, building,
            walker + cpu->r[next_offset], &iteration_reads, &iteration_walks);
        if (!np) break;
        uint32_t next = read32(np);
        if (!next) break;
        const uint8_t *kp = segment_word(cpu, memory, building,
            next + cpu->r[key_offset], &iteration_reads, &iteration_walks);
        if (!kp) break;
        uint32_t next_key = read32(kp);
        if (wanted <= next_key) break;
        prev = cur; cur = next; walker = next; key = next_key;
        tlb_reads += iteration_reads;
        walk_reads += iteration_walks;
        count++;
        if (building) {
            segment_range(key, &building->low, &building->high);
            building->retired += 10u;
        }
        if (++part == BULK_SEGMENT) {
            segment_finish(building, prev, cur, key);
            part = 0u;
        }
    }
    if (!count) return 0u;
    cpu->r[previous] = prev; cpu->r[current] = cur;
    cpu->r[walk] = walker; cpu->r[value] = key;
    cpu->cpsr = compare_flags(cpu->cpsr, wanted, key);
    if (!memory->flat_ram) {
        cpu->dread_hits += 2u * count - tlb_reads - walk_reads;
        cpu->tlb_hits += tlb_reads;
        cpu->tlb_misses += walk_reads;
    }
    return 10u * count;
}

/* The empty-payload detour can rejoin this read-only search, but only after
 * proving both links nonzero. Witness the actual target and its return edge;
 * an arbitrary BEQ destination is not evidence for this alternate path. */
static bool thumb_filtered_empty_path(const arm_bulk_memory_t *memory,
                                       uint32_t offset) {
    int64_t target = (int64_t)offset + 8 +
        (int8_t)read16(memory->code + offset + 4u) * 2;
    if (target < 0 || (uint64_t)target > memory->code_bytes ||
        memory->code_bytes - (uint32_t)target < 14u) return false;
    const uint8_t *p = memory->code + (uint32_t)target;
    return read16(p) == 0x4649u && read16(p + 2u) == 0x5859u &&
           read16(p + 4u) == 0x2900u &&
           (read16(p + 6u) & 0xff00u) == 0xd000u &&
           read16(p + 8u) == 0x5959u && read16(p + 10u) == 0x2900u &&
           (read16(p + 12u) & 0xff00u) == 0xd100u &&
           target + 16 + (int8_t)read16(p + 12u) * 2 == (int64_t)offset + 6;
}

/* A second read-only chain includes two depth paths and an optional empty-
 * payload detour. Every taken internal edge is matched to witnessed code.
 * Exits which can descend, unlink, write or return remain ordinary execution.
 * Each complete iteration has its own original instruction and load count. */
static unsigned thumb_filtered_chain(arm_cpu_t *cpu,
                                      const arm_bulk_memory_t *memory,
                                      uint32_t offset, unsigned budget) {
    static const uint16_t shape[] = {
        0x5919u, 0x2900u, 0xd000u, 0x2100u, 0x1c1eu, 0x468bu,
        0x4562u, 0xd000u, 0x595bu, 0x2b00u, 0xd000u, 0x4582u,
        0xd100u, 0x4641u, 0x585au, 0x9900u, 0x6809u, 0x428au, 0xd1ecu,
    };
    const bool same_depth = cpu->r[10] == cpu->r[0];
    const unsigned stride = same_depth ? 19u : 15u;
    if (memory->code_bytes - offset < sizeof shape || budget < stride) return 0u;
    for (unsigned i = 0u; i < sizeof shape / sizeof shape[0]; i++) {
        unsigned mask = (i == 2u || i == 7u || i == 10u || i == 12u || i == 15u)
            ? 0xff00u : 0xffffu;
        if ((read16(memory->code + offset + 2u * i) & mask) != shape[i])
            return 0u;
    }
    if (!same_depth && (offset < 4u ||
        read16(memory->code + offset + 24u) != 0xd1f0u ||
        read16(memory->code + offset - 4u) != 0x4641u ||
        read16(memory->code + offset - 2u) != 0x585au)) return 0u;
    unsigned invariant_reads = 0u, invariant_walks = 0u;
    uint32_t wanted = 0u;
    if (same_depth) {
        uint32_t stack_offset = (read16(memory->code + offset + 30u) & 255u) * 4u;
        const uint8_t *slot = chain_word_at(cpu, memory,
            cpu->r[13] + stack_offset, &invariant_reads, &invariant_walks);
        if (!slot) return 0u;
        const uint8_t *target = chain_word_at(cpu, memory, read32(slot),
                                             &invariant_reads, &invariant_walks);
        if (!target) return 0u;
        wanted = read32(target);
    }
    uint32_t current = cpu->r[3], value = cpu->r[2], previous = cpu->r[6];
    unsigned retired = 0u, loads = 0u, tlb_reads = 0u, walk_reads = 0u;
    bool empty_path_witnessed = false;
    const uint32_t offsets[4] = {cpu->r[4], cpu->r[5], cpu->r[8], cpu->r[9]};
    const unsigned kind = same_depth ? 2u : 3u;
    unsigned part = 0u;
    bulk_segment_t *building = NULL;
    while (budget - retired >= stride) {
        if (part == 0u) {
            bulk_segment_t *entry = segment_slot(memory, kind, current, offsets);
            if (segment_key(entry, kind, current, offsets, budget - retired) &&
                value != cpu->r[12] &&
                outside(cpu->r[12], entry->head_low, entry->head_high) &&
                (!same_depth || outside(wanted, entry->low, entry->high)) &&
                (!entry->empty_path || thumb_filtered_empty_path(memory, offset)) &&
                segment_validate(cpu, memory, entry, &tlb_reads, &walk_reads)) {
                previous = entry->previous; current = entry->current;
                value = entry->value; retired += entry->retired;
                loads += entry->loads + (same_depth ? 2u * BULK_SEGMENT : 0u);
                tlb_reads += invariant_reads * BULK_SEGMENT;
                walk_reads += invariant_walks * BULK_SEGMENT;
                continue;
            }
            building = (budget - retired) / stride >= BULK_SEGMENT ? entry : NULL;
            segment_begin(building, kind, current, offsets);
        }
        unsigned cost = stride, iteration_loads = same_depth ? 5u : 3u;
        unsigned iteration_reads = invariant_reads;
        unsigned iteration_walks = invariant_walks;
        const uint8_t *payload = segment_word(cpu, memory, building,
            current + cpu->r[4], &iteration_reads, &iteration_walks);
        if (!payload) break;
        if (!read32(payload)) {
            cost += 7u;
            if (budget - retired < cost) break;
            if (!empty_path_witnessed) {
                if (!thumb_filtered_empty_path(memory, offset)) break;
                empty_path_witnessed = true;
            }
            const uint8_t *child = segment_word(cpu, memory, building,
                current + cpu->r[9], &iteration_reads, &iteration_walks);
            if (!child || !read32(child)) break;
            const uint8_t *sibling = segment_word(cpu, memory, building,
                current + cpu->r[5], &iteration_reads, &iteration_walks);
            if (!sibling || !read32(sibling)) break;
            iteration_loads += 2u;
            if (building) building->empty_path = true;
        }
        if (value == cpu->r[12]) break;
        const uint8_t *link = segment_word(cpu, memory, building,
            current + cpu->r[5], &iteration_reads, &iteration_walks);
        if (!link) break;
        uint32_t next = read32(link);
        if (!next) break;
        const uint8_t *key = segment_word(cpu, memory, building,
            next + cpu->r[8], &iteration_reads, &iteration_walks);
        if (!key) break;
        uint32_t next_value = read32(key);
        if (same_depth && next_value == wanted) break;
        if (building) {
            segment_range(next_value, &building->low, &building->high);
            if (part) segment_range(value, &building->head_low, &building->head_high);
            building->retired += cost;
        }
        previous = current; current = next; value = next_value;
        tlb_reads += iteration_reads;
        walk_reads += iteration_walks;
        loads += iteration_loads;
        retired += cost;
        if (++part == BULK_SEGMENT) {
            segment_finish(building, previous, current, value);
            part = 0u;
        }
    }
    if (!retired) return 0u;
    cpu->r[1] = same_depth ? wanted : cpu->r[8];
    cpu->r[2] = value; cpu->r[3] = current;
    cpu->r[6] = previous; cpu->r[11] = 0u;
    cpu->cpsr = same_depth ? compare_flags(cpu->cpsr, value, wanted)
                          : compare_flags(cpu->cpsr, cpu->r[10], cpu->r[0]);
    if (!memory->flat_ram) {
        cpu->dread_hits += loads - tlb_reads - walk_reads;
        cpu->tlb_hits += tlb_reads;
        cpu->tlb_misses += walk_reads;
    }
    return retired;
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
