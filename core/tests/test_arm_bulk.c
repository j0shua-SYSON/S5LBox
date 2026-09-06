/* Differential and refusal tests for witnessed bulk A32/Thumb execution.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm_bulk.h"
#include <stdio.h>
#include <string.h>
#if defined(S5LBOX_STATIC_A64_ENGINE)
#include "a64_static.h"
#endif

#define RAM_SIZE 16384u
#define CODE 0x100u
#define RETURN 0x400u
#define DATA 0x1000u
static uint8_t ram[RAM_SIZE], before_ram[RAM_SIZE];
static unsigned bus_reads, bus_writes;
static bool split_mapping;
static unsigned checks, failures;

#define CHECK(c, ...) do { checks++; if (!(c)) { \
    if (failures++ < 20u) { printf("%s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); } } } while (0)

static const uint32_t length_code[] = {
    0xe1a0c000u, 0xe2103003u, 0xe3c00003u, 0xe4902004u,
    0x0a000003u, 0xe3530002u, 0xe38220ffu, 0xa3822cffu,
    0xc38228ffu, 0xe3a01001u, 0xe1811401u, 0xe1811801u,
    0xe0423001u, 0xe1c33002u, 0xe1130381u, 0x04902004u,
    0x0afffffau, 0xe2400001u, 0xe31200ffu, 0x02400001u,
    0x13120cffu, 0x02400001u, 0x131208ffu, 0x02400001u,
    0xe040000cu, 0xe12fff1eu,
};
static const uint32_t compare_code[] = {
    0xe2800001u, 0xe28cc001u, 0xe1510000u, 0x115e000cu,
    0x0a000003u, 0xe1d020d0u, 0xe1dc30d0u, 0xe1520003u,
    0x0afffff6u,
};

static uint8_t *mapped(uint32_t a) {
    a &= RAM_SIZE - 1u;
    if (split_mapping && a >= 0x1400u && a < 0x1800u)
        a += 0x1c00u;
    return ram + a;
}
static uint32_t r32(void *ctx, uint32_t a) {
    uint32_t v; (void)ctx; bus_reads++; memcpy(&v, mapped(a), 4); return v;
}
static uint16_t r16(void *ctx, uint32_t a) {
    uint16_t v; (void)ctx; bus_reads++; memcpy(&v, mapped(a), 2); return v;
}
static uint8_t r8(void *ctx, uint32_t a) {
    (void)ctx; bus_reads++; return *mapped(a);
}
static void w32(void *ctx, uint32_t a, uint32_t v) {
    (void)ctx; bus_writes++; memcpy(mapped(a), &v, 4);
}
static void w16(void *ctx, uint32_t a, uint16_t v) {
    (void)ctx; bus_writes++; memcpy(mapped(a), &v, 2);
}
static void w8(void *ctx, uint32_t a, uint8_t v) {
    (void)ctx; bus_writes++; *mapped(a) = v;
}
static uint8_t *host_ram(void *ctx, uint32_t a, uint32_t n) {
    (void)ctx;
    if (!n || (uint64_t)a + n > RAM_SIZE ||
        (a >> 10) != ((a + n - 1u) >> 10)) return NULL;
    return mapped(a);
}
static const arm_bus_t bus = {
    .read32 = r32, .read16 = r16, .read8 = r8,
    .write32 = w32, .write16 = w16, .write8 = w8,
    .host_ram = host_ram,
};

static uint8_t *full_host_ram(void *ctx, uint32_t a, uint32_t n) {
    (void)ctx;
    return !split_mapping && n && (uint64_t)a + n <= RAM_SIZE ? ram + a : NULL;
}
static const arm_bus_t full_bus = {
    .read32 = r32, .read16 = r16, .read8 = r8,
    .write32 = w32, .write16 = w16, .write8 = w8,
    .host_ram = full_host_ram,
};

static void setup(arm_cpu_t *cpu, arm_bulk_memory_t *memory,
                  uint32_t address, unsigned length, uint32_t flags,
                  bool thumb_return) {
    memset(ram, 0xa5, sizeof ram);
    for (unsigned i = 0; i < sizeof length_code / sizeof length_code[0]; i++)
        w32(NULL, CODE + 4u * i, length_code[i]);
    for (unsigned i = 0; i < length; i++)
        *mapped(address + i) = (uint8_t)(1u + (i * 83u + length * 23u) % 255u);
    *mapped(address + length) = 0;
    memset(cpu, 0, sizeof *cpu);
    arm_reset(cpu, &bus);
    cpu->cpsr = ARM_MODE_USR | flags;
    for (unsigned i = 0; i < 15u; i++) cpu->r[i] = 0x72f09300u + i;
    cpu->r[0] = address;
    cpu->r[14] = RETURN | (thumb_return ? 1u : 0u);
    cpu->r[15] = CODE;
    cpu->cycles = 127u;
    *memory = (arm_bulk_memory_t){
        .code = ram + CODE, .code_base = CODE,
        .code_bytes = sizeof length_code,
        .flat_ram = ram, .flat_size = sizeof ram,
    };
    bus_reads = bus_writes = 0u;
}

static unsigned oracle(arm_cpu_t *cpu) {
    unsigned n = 0u;
    while (cpu->r[15] != RETURN && n < 4096u) {
        arm_status_t status = arm_step(cpu);
        CHECK(status == ARM_OK, "oracle status %d at %08x", (int)status, cpu->r[15]);
        if (status != ARM_OK) break;
        n++;
    }
    CHECK(cpu->r[15] == RETURN, "oracle did not return: %08x", cpu->r[15]);
    return n;
}

static void refusal(arm_cpu_t *cpu, const arm_bulk_memory_t *memory,
                     unsigned budget) {
    arm_cpu_t saved = *cpu;
    unsigned reads = bus_reads, writes = bus_writes;
    memcpy(before_ram, ram, sizeof ram);
    unsigned n = arm_bulk_string_try(cpu, memory, budget);
    CHECK(n == 0u, "refusal retired %u", n);
    CHECK(memcmp(&saved, cpu, sizeof saved) == 0, "refusal changed CPU/cache state");
    CHECK(memcmp(before_ram, ram, sizeof ram) == 0, "refusal wrote RAM");
    CHECK(bus_reads == reads && bus_writes == writes, "refusal touched bus");
}

static void differential(arm_cpu_t *cpu, const arm_bulk_memory_t *memory,
                          unsigned length, unsigned extra_budget) {
    arm_cpu_t slow = *cpu, expected = *cpu, initial = *cpu;
    unsigned n = oracle(&slow);
    CHECK(slow.r[0] == length, "oracle length %u != %u", slow.r[0], length);
    CHECK(slow.cycles - initial.cycles == n, "oracle cycle count differs");
    memcpy(expected.r, slow.r, sizeof expected.r);
    expected.cpsr = slow.cpsr;
    if (!memory->flat_ram)
        expected.dread_hits += (length + (initial.r[0] & 3u)) / 4u + 1u;
    unsigned reads = bus_reads, writes = bus_writes;
    memcpy(before_ram, ram, sizeof ram);
    unsigned actual = arm_bulk_string_try(cpu, memory, n + extra_budget);
    CHECK(actual == n, "retired %u != %u, length=%u align=%u cpsr=%08x",
          actual, n, length, initial.r[0] & 3u, initial.cpsr);
    for (unsigned i = 0; i < 16u; i++)
        CHECK(cpu->r[i] == slow.r[i], "r%u=%08x != %08x len=%u align=%u",
              i, cpu->r[i], slow.r[i], length, initial.r[0] & 3u);
    CHECK(cpu->cpsr == slow.cpsr, "cpsr=%08x != %08x len=%u align=%u",
          cpu->cpsr, slow.cpsr, length, initial.r[0] & 3u);
    CHECK(memcmp(cpu, &expected, sizeof expected) == 0, "unintended CPU/cache change");
    CHECK(memcmp(ram, before_ram, sizeof ram) == 0, "success wrote RAM");
    CHECK(bus_reads == reads && bus_writes == writes, "bulk operation touched bus");
    refusal(&initial, memory, n - 1u);
}

static void test_lengths(void) {
    const uint32_t preserved = ARM_CPSR_Q | ARM_CPSR_A | ARM_CPSR_I |
                               ARM_CPSR_F | (0xau << 16);
    for (unsigned alignment = 0u; alignment < 4u; alignment++)
        for (unsigned length = 0u; length <= 256u; length++)
            for (unsigned flags = 0u; flags < 16u; flags++)
                for (unsigned thumb = 0u; thumb < 2u; thumb++) {
                    arm_cpu_t cpu; arm_bulk_memory_t memory;
                    setup(&cpu, &memory, DATA + alignment, length,
                          (flags << 28) | preserved, thumb != 0u);
                    differential(&cpu, &memory, length, length % 11u);
                }
    /* Zero in a word with 0x01 bytes exercises the subtraction borrow mask;
     * preceding zeros must be ignored for an unaligned original pointer. */
    for (unsigned alignment = 0u; alignment < 4u; alignment++) {
        arm_cpu_t cpu; arm_bulk_memory_t memory;
        setup(&cpu, &memory, DATA + alignment, 0u, 0u, false);
        memset(ram + DATA, 0, 4u);
        memset(ram + DATA + alignment + 1u, 1, 4u - alignment - 1u);
        differential(&cpu, &memory, 0u, 0u);
    }
}

static void test_refusals(void) {
    arm_cpu_t cpu; arm_bulk_memory_t memory;
    /* Every instruction is part of the proof, including untaken conditionals. */
    for (unsigned byte = 0u; byte < sizeof length_code; byte++) {
        setup(&cpu, &memory, DATA, 12u, 0u, false);
        ram[CODE + byte] ^= 1u;
        refusal(&cpu, &memory, 1000u);
    }
    for (unsigned bytes = 0u; bytes < sizeof length_code; bytes++) {
        setup(&cpu, &memory, DATA, 12u, 0u, false);
        memory.code_bytes = bytes;
        refusal(&cpu, &memory, 1000u);
    }
    for (unsigned budget = 0u; budget < 42u; budget++) {
        setup(&cpu, &memory, DATA, 16u, 0u, false);
        refusal(&cpu, &memory, budget);
    }
    for (unsigned scenario = 0u; scenario < 15u; scenario++) {
        setup(&cpu, &memory, DATA, 16u, 0u, false);
        switch (scenario) {
        case 0: cpu.cpsr |= ARM_CPSR_T; break;
        case 1: cpu.cpsr |= ARM_CPSR_E; break;
        case 2: cpu.cpsr = ARM_MODE_SVC; break;
        case 3: cpu.abort_pending = true; break;
        case 4: cpu.irq_line = true; break;
        case 5: cpu.fiq_line = true; break;
        case 6: cpu.r[15]++; break;
        case 7: cpu.r[14] |= 2u; break;
        case 8: cpu.cp15.sctlr |= ARM_SCTLR_M; break;
        case 9: memory.flat_size = 3u; break;
        case 10: memory.flat_size--; break;
        case 11: memory.code = NULL; break;
        case 12: memory.code_base = 0xfffffffcu; break;
        case 13: memory.flat_ram = NULL; break;
        case 14: memset(ram + DATA, 0x81, 2048u); break;
        }
        refusal(&cpu, &memory, 1000u);
    }
    setup(&cpu, &memory, DATA, 16u, ARM_CPSR_I | ARM_CPSR_F, false);
    cpu.irq_line = cpu.fiq_line = true;
    differential(&cpu, &memory, 16u, 0u);
    CHECK(arm_bulk_string_try(NULL, &memory, 100u) == 0u, "null CPU");
    CHECK(arm_bulk_string_try(&cpu, NULL, 100u) == 0u, "null memory");
}

static void cached_setup(arm_cpu_t *cpu, arm_bulk_memory_t *memory,
                          uint32_t address, unsigned length) {
    setup(cpu, memory, address, length, 0u, false);
    memory->flat_ram = NULL;
    memory->data_cache = true;
    for (uint32_t a = address & ~0x3ffu; a <= address + length; a += 1024u)
        CHECK(arm_data_cache_try_refill(cpu, a, ARM_ACCESS_READ, false),
              "could not warm %08x", a);
}

static void test_cached_boundaries(void) {
    arm_cpu_t cpu; arm_bulk_memory_t memory;
    for (unsigned align = 0u; align < 4u; align++) {
        cached_setup(&cpu, &memory, 0x13fcu + align, 83u);
        differential(&cpu, &memory, 83u, 0u);
        split_mapping = true;
        cached_setup(&cpu, &memory, 0x13fcu + align, 83u);
        differential(&cpu, &memory, 83u, 0u);
        split_mapping = false;
    }
    /* Real MMU-on permission witnesses, not only identity-mode cache tags. */
    for (unsigned align = 0u; align < 4u; align++) {
        setup(&cpu, &memory, 0x13fcu + align, 83u, 0u, false);
        w32(NULL, 0u, 0x00000c02u); /* User RW identity section, domain 0. */
        cpu.cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
        cpu.cp15.dacr = 1u;
        memory.flat_ram = NULL;
        memory.data_cache = true;
        for (uint32_t a = 0x1000u; a <= 0x1400u; a += 0x400u) {
            uint32_t pa = UINT32_MAX;
            CHECK(arm_mmu_translate(&cpu, a, ARM_ACCESS_READ, false, &pa) == 0u
                  && pa == a, "MMU permission witness failed");
            CHECK(arm_data_cache_try_refill(&cpu, a, ARM_ACCESS_READ, false),
                  "MMU data cache refill failed");
        }
        differential(&cpu, &memory, 83u, 0u);
    }
    for (unsigned scenario = 0u; scenario < 12u; scenario++) {
        cached_setup(&cpu, &memory, 0x13fcu, 83u);
        const unsigned i = (0x1400u >> 10) & (ARM_DREAD_ENTRIES - 1u);
        arm_bus_t altered_bus = bus;
        switch (scenario) {
        case 0: cpu.dread[i].host = NULL; break;
        case 1: cpu.dread[i].gen++; break;
        case 2: cpu.dread[i].tag ^= 1u; break; /* wrong privilege */
        case 3: cpu.dread[i].tag += 1024u; break;
        case 4: cpu.cp15.sctlr ^= ARM_SCTLR_M; break;
        case 5: cpu.cp15.ttbr0 ^= 0x4000u; break;
        case 6: cpu.cp15.ttbr1 ^= 0x4000u; break;
        case 7: cpu.cp15.ttbcr ^= 1u; break;
        case 8: cpu.cp15.dacr ^= 1u; break;
        case 9: cpu.cp15.context_id ^= 1u; break;
        case 10: memory.data_cache = false; break;
        case 11: altered_bus.host_ram = NULL; cpu.bus = &altered_bus; break;
        }
        refusal(&cpu, &memory, 1000u);
    }
}

static unsigned prefix_differential(arm_cpu_t *cpu,
                                    const arm_bulk_memory_t *memory,
                                    unsigned budget, bool compare) {
    arm_cpu_t slow = *cpu, expected = *cpu;
    unsigned reads = bus_reads, writes = bus_writes;
    memcpy(before_ram, ram, sizeof ram);
    unsigned n = arm_bulk_string_try(cpu, memory, budget);
    CHECK(n <= budget, "prefix exceeded budget: %u > %u", n, budget);
    CHECK(bus_reads == reads && bus_writes == writes, "prefix touched bus");
    CHECK(memcmp(before_ram, ram, sizeof ram) == 0, "prefix wrote RAM");
    for (unsigned i = 0u; i < n; i++)
        CHECK(arm_step(&slow) == ARM_OK, "prefix oracle fault");
    memcpy(expected.r, slow.r, sizeof expected.r);
    expected.cpsr = slow.cpsr;
    if (!memory->flat_ram)
        expected.dread_hits += compare ?
            2u * (n / 9u + (n % 9u != 0u)) : n / 5u;
    for (unsigned i = 0u; i < 16u; i++)
        CHECK(cpu->r[i] == slow.r[i], "prefix r%u=%08x != %08x n=%u", i,
              cpu->r[i], slow.r[i], n);
    CHECK(cpu->cpsr == slow.cpsr, "prefix flags=%08x != %08x n=%u",
          cpu->cpsr, slow.cpsr, n);
    CHECK(memcmp(cpu, &expected, sizeof expected) == 0, "prefix state changed");
    return n;
}

static void test_length_prefixes(void) {
    for (unsigned length = 0u; length < 512u; length += 7u)
        for (unsigned alignment = 0u; alignment < 4u; alignment++)
            for (unsigned budget = 0u; budget <= 256u; budget++) {
                arm_cpu_t cpu; arm_bulk_memory_t memory;
                setup(&cpu, &memory, DATA + alignment, length,
                      (length % 16u) << 28, false);
                while (cpu.r[15] != CODE + 48u)
                    CHECK(arm_step(&cpu) == ARM_OK, "length prologue fault");
                unsigned n = prefix_differential(&cpu, &memory, budget, false);
                CHECK(n == 5u * (budget / 5u < (length + alignment) / 4u ?
                                budget / 5u : (length + alignment) / 4u),
                      "length prefix unexpectedly declined");
            }
    for (unsigned scenario = 0u; scenario < 3u; scenario++) {
        arm_cpu_t cpu; arm_bulk_memory_t memory;
        cached_setup(&cpu, &memory, DATA, 256u);
        while (cpu.r[15] != CODE + 48u) (void)arm_step(&cpu);
        if (scenario == 0u) cpu.r[1] ^= 1u;
        if (scenario == 1u) cpu.r[0]++;
        if (scenario == 2u) cpu.dread[(DATA >> 10) & 63u].host = NULL;
        refusal(&cpu, &memory, 256u);
    }
    arm_cpu_t cpu; arm_bulk_memory_t memory;
    cached_setup(&cpu, &memory, 0x13f4u, 83u);
    while (cpu.r[15] != CODE + 48u) (void)arm_step(&cpu);
    cpu.dread[(0x1400u >> 10) & 63u].host = NULL;
    CHECK(prefix_differential(&cpu, &memory, 256u, false) == 10u,
          "length cold-page prefix did not stop before the missing load");
}

static void compare_setup(arm_cpu_t *cpu, arm_bulk_memory_t *memory,
                           unsigned left_length, unsigned right_length,
                           unsigned common, uint8_t a, uint8_t b) {
    setup(cpu, memory, DATA, 0u, ARM_CPSR_Q | ARM_CPSR_V, false);
    for (unsigned i = 0u; i < sizeof compare_code / sizeof compare_code[0]; i++)
        w32(NULL, CODE + 4u * i, compare_code[i]);
    memset(ram + DATA, 0x81, 512u);
    memset(ram + 0x2000u, 0x81, 512u);
    ram[DATA + common] = a;
    ram[0x2000u + common] = b;
    cpu->r[0] = DATA;
    cpu->r[1] = DATA + left_length;
    cpu->r[12] = 0x2000u;
    cpu->r[14] = 0x2000u + right_length;
    cpu->r[15] = CODE + 20u;
    memory->code_bytes = sizeof compare_code;
}

static void test_compare_prefixes(void) {
    /* Every signed/unsigned-byte relation, including embedded NULs: these
     * are bounded ranges, not zero-terminated strings. */
    for (unsigned a = 0u; a < 256u; a++)
        for (unsigned b = 0u; b < 256u; b++) {
            arm_cpu_t cpu; arm_bulk_memory_t memory;
            compare_setup(&cpu, &memory, 1u, 1u, 0u, (uint8_t)a, (uint8_t)b);
            CHECK(prefix_differential(&cpu, &memory, 256u, true) ==
                  (a == b ? 9u : 4u), "compare unexpectedly declined");
        }
    for (unsigned length = 1u; length < 130u; length += 3u)
        for (unsigned budget = 0u; budget <= 256u; budget++)
            for (unsigned end = 0u; end < 3u; end++) {
                arm_cpu_t cpu; arm_bulk_memory_t memory;
                compare_setup(&cpu, &memory, length + (end == 0u),
                              length + (end == 1u), length - 1u, 0x81, 0x81);
                unsigned n = prefix_differential(&cpu, &memory, budget, true);
                CHECK(n == 9u * (budget / 9u < length ? budget / 9u : length),
                      "equal-prefix budget/end mismatch");
            }
    for (unsigned byte = 0u; byte < sizeof compare_code; byte++) {
        arm_cpu_t cpu; arm_bulk_memory_t memory;
        compare_setup(&cpu, &memory, 2u, 2u, 1u, 1u, 2u);
        ram[CODE + byte] ^= 1u;
        refusal(&cpu, &memory, 256u);
    }
    arm_cpu_t cpu; arm_bulk_memory_t memory;
    compare_setup(&cpu, &memory, 83u, 83u, 70u, 0xffu, 0u);
    memory.flat_ram = NULL; memory.data_cache = true;
    CHECK(arm_data_cache_try_refill(&cpu, DATA, ARM_ACCESS_READ, false), "left map");
    CHECK(arm_data_cache_try_refill(&cpu, 0x2000u, ARM_ACCESS_READ, false), "right map");
    CHECK(prefix_differential(&cpu, &memory, 256u, true) == 252u, "cached compare");
    cpu.dread[(0x2000u >> 10) & 63u].gen++;
    refusal(&cpu, &memory, 256u);
}

static void chain_setup(arm_cpu_t *cpu, arm_bulk_memory_t *memory,
                         unsigned kind, unsigned rotation, unsigned nodes,
                         uint32_t flags) {
    setup(cpu, memory, DATA, 0u, flags, false);
    cpu->cpsr |= ARM_CPSR_T;
    memory->code_bytes = 64u;
    for (unsigned i = 0u; i < 32u; i++) w16(NULL, CODE + 2u * i, 0x46c0u);
    if (kind == 0u) {
        unsigned prev = 8u + rotation % 7u, cur = (4u + rotation) % 7u;
        unsigned walk = (6u + rotation) % 7u, noff = (1u + rotation) % 7u;
        unsigned value = (3u + rotation) % 7u, koff = (2u + rotation) % 7u;
        unsigned needle = (5u + rotation) % 7u;
        const uint16_t code[] = {
            (uint16_t)(0x4600u | cur << 3 | (prev & 7u) | (prev & 8u) << 4),
            (uint16_t)(0x5800u | noff << 6 | walk << 3 | cur),
            (uint16_t)(0x2800u | cur << 8), 0xd01bu,
            (uint16_t)(0x5800u | koff << 6 | cur << 3 | value),
            (uint16_t)(0x1c00u | cur << 3 | walk),
            (uint16_t)(0x4280u | needle << 3 | value), 0xd017u,
            (uint16_t)(0x4280u | value << 3 | needle), 0xd8f5u,
        };
        for (unsigned i = 0u; i < sizeof code / sizeof code[0]; i++)
            w16(NULL, CODE + 2u * i, code[i]);
        cpu->r[cur] = cpu->r[walk] = DATA;
        cpu->r[noff] = 0u; cpu->r[koff] = 4u;
        cpu->r[needle] = 0x80000000u + nodes;
        for (unsigned i = 0u; i < nodes; i++) {
            w32(NULL, DATA + 16u * i, i + 1u < nodes ? DATA + 16u * (i + 1u) : 0u);
            w32(NULL, DATA + 16u * i + 4u, 0x7fffffffu + i);
        }
    } else {
        const uint16_t code[] = {
            0x5919u, 0x2900u, 0xd01cu, 0x2100u, 0x1c1eu, 0x468bu,
            0x4562u, 0xd017u, 0x595bu, 0x2b00u, 0xd014u, 0x4582u,
            0xd112u, 0x4641u, 0x585au, (uint16_t)(0x9900u | rotation),
            0x6809u, 0x428au, 0xd1ecu,
        };
        for (unsigned i = 0u; i < sizeof code / sizeof code[0]; i++)
            w16(NULL, CODE + 2u * i, code[i]);
        cpu->r[0] = cpu->r[10] = 3u;
        cpu->r[2] = 0x7fffffffu; cpu->r[3] = DATA;
        cpu->r[4] = 4u; cpu->r[5] = 8u; cpu->r[8] = 0u;
        cpu->r[12] = 0x12345678u; cpu->r[13] = 0x3000u;
        w32(NULL, 0x3000u + 4u * rotation, 0x3100u);
        w32(NULL, 0x3100u, 0xfffffff0u);
        for (unsigned i = 0u; i < nodes; i++) {
            w32(NULL, DATA + 16u * i, 0x7fffffffu + i);
            w32(NULL, DATA + 16u * i + 4u, 1u);
            w32(NULL, DATA + 16u * i + 8u, i + 1u < nodes ? DATA + 16u * (i + 1u) : 0u);
        }
    }
    bus_reads = bus_writes = 0u;
}

static unsigned chain_differential(arm_cpu_t *cpu,
                                    const arm_bulk_memory_t *memory,
                                    unsigned budget, unsigned kind) {
    arm_cpu_t slow = *cpu, expected = *cpu;
    unsigned reads = bus_reads, writes = bus_writes;
    memcpy(before_ram, ram, sizeof ram);
    unsigned n = arm_bulk_string_try(cpu, memory, budget);
    unsigned stride = kind ? 19u : 10u;
    CHECK(n <= budget && n % stride == 0u, "chain prefix budget/count");
    CHECK(bus_reads == reads && bus_writes == writes, "chain touched bus");
    CHECK(memcmp(before_ram, ram, sizeof ram) == 0, "chain wrote RAM");
    for (unsigned i = 0u; i < n; i++)
        CHECK(arm_step(&slow) == ARM_OK, "chain oracle fault");
    memcpy(expected.r, slow.r, sizeof expected.r);
    expected.cpsr = slow.cpsr;
    if (!memory->flat_ram) {
        uint64_t tlb_reads = cpu->tlb_hits - expected.tlb_hits;
        unsigned loads = n / stride * (kind ? 5u : 2u);
        CHECK(tlb_reads <= loads && (memory->ram_window || !tlb_reads),
              "chain READ witness accounting");
        expected.dread_hits += loads - tlb_reads;
        expected.tlb_hits += tlb_reads;
    }
    CHECK(memcmp(cpu, &expected, sizeof expected) == 0,
          "chain state differs kind=%u n=%u cpsr=%08x/%08x",
          kind, n, cpu->cpsr, slow.cpsr);
    return n;
}

static void test_thumb_chains(void) {
    for (unsigned kind = 0u; kind < 2u; kind++) {
        unsigned stride = kind ? 19u : 10u;
        for (unsigned rotation = 0u; rotation < 7u; rotation++)
            for (unsigned flags = 0u; flags < 16u; flags++)
                for (unsigned budget = 0u; budget < 257u; budget++) {
                    arm_cpu_t cpu; arm_bulk_memory_t memory;
                    chain_setup(&cpu, &memory, kind, rotation, 17u,
                                flags << 28 | ARM_CPSR_Q | ARM_CPSR_A);
                    unsigned count = budget / stride;
                    if (count > 16u) count = 16u;
                    CHECK(chain_differential(&cpu, &memory, budget, kind) == count * stride,
                          "chain did not batch expected prefix");
                }
        for (unsigned half = 0u; half < stride; half++) {
            arm_cpu_t cpu; arm_bulk_memory_t memory;
            chain_setup(&cpu, &memory, kind, 0u, 17u, 0u);
            ram[CODE + 2u * half + 1u] ^= 0x80u;
            refusal(&cpu, &memory, 256u);
        }
        for (unsigned bytes = 0u; bytes < 2u * stride; bytes++) {
            arm_cpu_t cpu; arm_bulk_memory_t memory;
            chain_setup(&cpu, &memory, kind, 0u, 17u, 0u);
            memory.code_bytes = bytes;
            refusal(&cpu, &memory, 256u);
        }
        for (unsigned scenario = 0u; scenario < 10u; scenario++) {
            arm_cpu_t cpu; arm_bulk_memory_t memory;
            chain_setup(&cpu, &memory, kind, 0u, 17u, 0u);
            if (scenario == 0u) cpu.cpsr ^= ARM_CPSR_T;
            if (scenario == 1u) cpu.r[15]++;
            if (scenario == 2u) cpu.cpsr |= ARM_CPSR_E;
            if (scenario == 3u) cpu.irq_line = true;
            if (scenario == 4u) cpu.abort_pending = true;
            if (scenario == 5u) cpu.r[kind ? 3u : 6u]++;
            if (scenario == 6u) cpu.r[kind ? 8u : 2u]++;
            if (scenario == 7u) {
                if (kind) cpu.r[10]++;
                else cpu.r[5] = 0u;
            }
            if (scenario == 8u) w32(NULL, DATA + (kind ? 4u : 0u), 0u);
            if (scenario == 9u) {
                if (kind) cpu.r[12] = cpu.r[2];
                else w32(NULL, DATA + 16u + 4u, cpu.r[5]);
            }
            refusal(&cpu, &memory, 256u);
        }
        arm_cpu_t cpu; arm_bulk_memory_t memory;
        chain_setup(&cpu, &memory, kind, 0u, 128u, 0u);
        memory.flat_ram = NULL; memory.data_cache = true;
        CHECK(arm_data_cache_try_refill(&cpu, DATA, ARM_ACCESS_READ, false), "chain data map");
        CHECK(arm_data_cache_try_refill(&cpu, 0x3000u, ARM_ACCESS_READ, false), "chain stack map");
        CHECK(chain_differential(&cpu, &memory, 4096u, kind) == 63u * stride,
              "chain crossed a cold page or did not keep its warm prefix");
        refusal(&cpu, &memory, 4096u);
        CHECK(arm_data_cache_try_refill(&cpu, 0x1400u, ARM_ACCESS_READ, false), "chain next map");
        CHECK(chain_differential(&cpu, &memory, 4096u, kind) == 64u * stride,
              "chain did not resume across a newly proved page");
        chain_setup(&cpu, &memory, kind, 0u, 17u, 0u);
        memory.flat_ram = NULL; memory.data_cache = true;
        (void)arm_data_cache_try_refill(&cpu, DATA, ARM_ACCESS_READ, false);
        (void)arm_data_cache_try_refill(&cpu, 0x3000u, ARM_ACCESS_READ, false);
        cpu.dread[(DATA >> 10) & 63u].gen++;
        refusal(&cpu, &memory, 256u);
    }
}

/* Alternate nodes between VAs 64KiB apart, mapped to distinct physical pages.
 * They collide in DREAD but not the larger, already permission-checked TLB.
 * Neither the bulk executor nor its refusal path may fill a software cache. */
static void chain_tlb_setup(arm_cpu_t *cpu, arm_bulk_memory_t *memory,
                             arm_ram_window_t *window, unsigned kind) {
    chain_setup(cpu, memory, kind, 0u, 128u, ARM_CPSR_Q | ARM_CPSR_V);
    for (unsigned i = 0u; i < 128u; i++) {
        uint32_t pa = DATA + (i & 1u) * 0x1000u + (i >> 1) * 16u;
        uint32_t next = i == 127u ? 0u :
            DATA + ((i + 1u) & 1u) * 0x10000u + ((i + 1u) >> 1) * 16u;
        w32(NULL, pa, kind ? 0x7fffffffu + i : next);
        w32(NULL, pa + 4u, kind ? 1u : 0x7fffffffu + i);
        if (kind) w32(NULL, pa + 8u, next);
    }
    memset(ram + 0x800u, 0, 1024u);
    w32(NULL, 0u, 0x801u); /* Coarse table; User RW small pages. */
    w32(NULL, 0x800u, 0x32u);
    w32(NULL, 0x804u, 0x1032u);
    w32(NULL, 0x844u, 0x2032u);
    w32(NULL, 0x80cu, 0x3032u);
    cpu->bus = &full_bus;
    cpu->cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP;
    cpu->cp15.dacr = 1u;
    memory->flat_ram = NULL; memory->data_cache = true;
    static const uint32_t va[] = {DATA, 0x11000u, 0x3000u};
    for (unsigned i = 0u; i < sizeof va / sizeof va[0]; i++) {
        uint32_t pa = UINT32_MAX;
        CHECK(arm_mmu_translate(cpu, va[i], ARM_ACCESS_READ, false, &pa) == 0u,
              "chain READ translation");
        CHECK(pa == DATA + i * 0x1000u, "nonidentity chain translation");
    }
    uint32_t pa = UINT32_MAX;
    CHECK(arm_mmu_translate(cpu, CODE, ARM_ACCESS_FETCH, false, &pa) == 0u &&
              pa == CODE, "chain FETCH witness");
    CHECK(arm_ram_window_capture(window, cpu, 0u, RAM_SIZE), "chain RAM capability");
    memory->ram_window = window;
    bus_reads = bus_writes = 0u;
}

static void test_thumb_chain_tlb(void) {
    for (unsigned kind = 0u; kind < 2u; kind++) {
        unsigned stride = kind ? 19u : 10u;
        arm_cpu_t cpu; arm_bulk_memory_t memory; arm_ram_window_t window;
        chain_tlb_setup(&cpu, &memory, &window, kind);
        CHECK(arm_data_cache_try_refill(&cpu, DATA, ARM_ACCESS_READ, false),
              "warm colliding DREAD page");
        CHECK(((DATA >> 10) & (ARM_DREAD_ENTRIES - 1u)) ==
                  ((0x11000u >> 10) & (ARM_DREAD_ENTRIES - 1u)), "DREAD collision");
        memory.ram_window = NULL;
        refusal(&cpu, &memory, 4096u);
        memset(cpu.dread, 0, sizeof cpu.dread);
        memory.ram_window = &window;
        uint64_t hits = cpu.tlb_hits, dread = cpu.dread_hits;
        CHECK(chain_differential(&cpu, &memory, 4096u, kind) == 127u * stride,
              "TLB-backed chain did not batch across colliding DREAD pages");
        CHECK(cpu.tlb_hits - hits == 127u * (kind ? 5u : 2u) &&
                  cpu.dread_hits == dread, "READ TLB grants mislabeled as DREAD hits");
        for (unsigned budget = 0u; budget <= 256u; budget++) {
            chain_tlb_setup(&cpu, &memory, &window, kind);
            CHECK(chain_differential(&cpu, &memory, budget, kind) ==
                      budget / stride * stride, "TLB chain partial budget");
        }
        chain_tlb_setup(&cpu, &memory, &window, kind);
        w32(NULL, DATA + 30u * 16u + (kind ? 8u : 0u), 0x21000u);
        CHECK(chain_differential(&cpu, &memory, 4096u, kind) == 60u * stride,
              "TLB chain did not stop before an unproved later page");
        refusal(&cpu, &memory, 4096u);
        for (unsigned scenario = 0u; scenario < 10u; scenario++) {
            chain_tlb_setup(&cpu, &memory, &window, kind);
            unsigned slot = (DATA >> 10) & (ARM_TLB_ENTRIES - 1u);
            arm_bus_t altered = full_bus;
            switch (scenario) {
            case 0: cpu.tlb[slot].gen++; break;
            case 1: cpu.tlb[slot].tag ^= 1u; break; /* Privileged-only grant. */
            case 2: cpu.tlb[slot].fsr = 13u; break;
            case 3: cpu.tlb[slot].pa = RAM_SIZE; break; /* Outside RAM. */
            case 4: cpu.cp15.ttbr0 ^= 0x4000u; break;
            case 5: cpu.cp15.context_id++; break;
            case 6: altered.read32 = NULL; cpu.bus = &altered; break;
            case 7: window.read_host = NULL; break;
            case 8: window.bytes = 1024u; break;
            case 9: cpu.tlb[slot].tag ^= 2u; break; /* Wrong access kind. */
            }
            refusal(&cpu, &memory, 4096u);
        }
    }
}

#if defined(S5LBOX_STATIC_A64_ENGINE)
typedef struct {
    arm_cpu_t *cpu;
    const arm_bulk_memory_t *memory;
} native_context_t;

static a64_compact_raw_fallback_result_t native_fallback(
        void *opaque, a64_compact_raw_code_window_t *next,
        const a64_compact_raw_data_miss_t *miss) {
    native_context_t *context = opaque;
    arm_cpu_t *cpu = context->cpu;
    const arm_bulk_memory_t *memory = context->memory;
    (void)miss;
    if (cpu->r[15] - memory->code_base >= memory->code_bytes)
        return A64_COMPACT_RAW_FALLBACK_NO_RETIRE;
    arm_status_t status = arm_step(cpu);
    CHECK(status == ARM_OK, "native fallback fault");
    if (status != ARM_OK) return A64_COMPACT_RAW_FALLBACK_NO_RETIRE;
    if (cpu->r[15] - memory->code_base >= memory->code_bytes)
        return A64_COMPACT_RAW_FALLBACK_RETIRE_STOP;
    next->code = memory->code;
    next->code_base = memory->code_base;
    next->code_bytes = memory->code_bytes;
    return A64_COMPACT_RAW_FALLBACK_RETIRE_CONTINUE;
}

static uint64_t native_differential(arm_cpu_t *cpu,
                                    const arm_bulk_memory_t *memory,
                                    unsigned budget, bool enabled) {
    arm_cpu_t slow = *cpu;
    native_context_t context = {cpu, memory};
    unsigned n = 0u, native = 0u, fallback = 0u;
    a64_compact_bulk_stats_t stats = {0};
    memcpy(before_ram, ram, sizeof ram);
    const a64_compact_raw_options_t options = {
        .bulk_enabled = enabled, .bulk_ram_window = memory->ram_window,
    };
    bool ok = a64_compact_raw_run_code_window_resident_options(cpu, memory->code,
        memory->code_base, memory->code_bytes, budget, native_fallback, &context,
        &options, NULL, &stats, NULL, &n, &native, &fallback);
    CHECK(ok, "native bulk wrapper refused");
    CHECK(n <= budget && native + fallback == n, "native retirement partition");
    CHECK(stats.retired <= native, "bulk retirement is not a native subset");
    if (enabled && memory->ram_window)
        CHECK(stats.calls == 1u && stats.retired == budget && n == budget,
              "colliding-page native loop did not run as one complete bulk batch");
    CHECK(enabled || (stats.calls == 0u && stats.retired == 0u), "disabled bulk ran");
    CHECK((stats.calls == 0u) == (stats.retired == 0u), "bulk counter mismatch");
    CHECK(memcmp(ram, before_ram, sizeof ram) == 0, "native bulk wrote RAM");
    for (unsigned i = 0u; i < n; i++)
        CHECK(arm_step(&slow) == ARM_OK, "native reference fault");
    CHECK(cpu->cycles == slow.cycles, "native cycle accounting mismatch");
    for (unsigned i = 0u; i < 16u; i++)
        CHECK(cpu->r[i] == slow.r[i], "native r%u=%08x != %08x", i,
              cpu->r[i], slow.r[i]);
    CHECK(cpu->cpsr == slow.cpsr, "native CPSR mismatch");
    CHECK(memcmp(cpu, &slow, offsetof(arm_cpu_t, tlb)) == 0,
          "native architectural state mismatch");
    return stats.calls;
}

static void test_native_integration(void) {
    if (!a64_static_host_available()) {
        printf("arm_bulk native integration: SKIP (host has no signed A64 runner)\n");
        return;
    }
    uint64_t calls = 0u;
    static const unsigned lengths[] = {0u, 1u, 7u, 31u, 127u, 511u};
    static const unsigned budgets[] = {1u, 5u, 16u, 64u, 256u};
    for (unsigned length = 0u; length < sizeof lengths / sizeof lengths[0]; length++)
        for (unsigned align = 0u; align < 4u; align++)
            for (unsigned flags = 0u; flags < 16u; flags++)
                for (unsigned b = 0u; b < sizeof budgets / sizeof budgets[0]; b++)
                    for (unsigned enabled = 0u; enabled < 2u; enabled++) {
                        arm_cpu_t cpu; arm_bulk_memory_t memory;
                        cached_setup(&cpu, &memory, DATA + align, lengths[length]);
                        cpu.cpsr |= flags << 28;
                        cpu.r[14] |= flags & 1u;
                        calls += native_differential(&cpu, &memory, budgets[b],
                                                     enabled != 0u);
                    }
    for (unsigned n = 1u; n <= 64u; n++)
        for (unsigned enabled = 0u; enabled < 2u; enabled++) {
            arm_cpu_t cpu; arm_bulk_memory_t memory;
            compare_setup(&cpu, &memory, n, n + 1u, n - 1u, 255u, (uint8_t)n);
            memory.flat_ram = NULL; memory.data_cache = true;
            (void)arm_data_cache_try_refill(&cpu, DATA, ARM_ACCESS_READ, false);
            (void)arm_data_cache_try_refill(&cpu, 0x2000u, ARM_ACCESS_READ, false);
            calls += native_differential(&cpu, &memory, 256u, enabled != 0u);
        }
    /* A candidate instruction with a changed body must resume its ordinary
     * decode operands unchanged, including incoming NZCV. */
    for (unsigned flags = 0u; flags < 16u; flags++) {
        arm_cpu_t cpu; arm_bulk_memory_t memory;
        cached_setup(&cpu, &memory, DATA, 16u);
        cpu.cpsr |= flags << 28;
        w32(NULL, CODE + 17u * 4u, 0xe2400002u); /* SUB r0,r0,#2 */
        CHECK(native_differential(&cpu, &memory, 256u, true) == 0u,
              "changed length body was bulk-executed");
    }
    uint64_t thumb_calls = 0u;
    for (unsigned kind = 0u; kind < 2u; kind++) {
        uint64_t kind_calls = 0u;
        for (unsigned flags = 0u; flags < 16u; flags++)
            for (unsigned enabled = 0u; enabled < 2u; enabled++) {
                arm_cpu_t cpu; arm_bulk_memory_t memory;
                chain_setup(&cpu, &memory, kind, flags % 7u, 17u, flags << 28);
                memory.flat_ram = NULL; memory.data_cache = true;
                (void)arm_data_cache_try_refill(&cpu, DATA, ARM_ACCESS_READ, false);
                (void)arm_data_cache_try_refill(&cpu, 0x3000u, ARM_ACCESS_READ, false);
                kind_calls += native_differential(&cpu, &memory, 256u, enabled != 0u);
            }
        CHECK(kind_calls > 0u, "native Thumb chain kind %u never executed", kind);
        thumb_calls += kind_calls;
    }
    CHECK(thumb_calls > 0u, "native Thumb chain integration never executed");
    calls += thumb_calls;
    for (unsigned kind = 0u; kind < 2u; kind++)
        for (unsigned enabled = 0u; enabled < 2u; enabled++) {
            arm_cpu_t cpu; arm_bulk_memory_t memory; arm_ram_window_t window;
            chain_tlb_setup(&cpu, &memory, &window, kind);
            calls += native_differential(&cpu, &memory, (kind ? 19u : 10u) * 60u,
                                          enabled != 0u);
        }
    CHECK(calls > 0u, "native bulk integration never executed");
    printf("arm_bulk native integration: %llu bulk calls\n", (unsigned long long)calls);
}
#else
static void test_native_integration(void) {
    printf("arm_bulk native integration: SKIP (static engine disabled)\n");
}
#endif

int main(void) {
    test_lengths();
    test_refusals();
    test_cached_boundaries();
    test_length_prefixes();
    test_compare_prefixes();
    test_thumb_chains();
    test_thumb_chain_tlb();
    test_native_integration();
    printf("arm_bulk: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
