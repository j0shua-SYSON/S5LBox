/* Exact TLB-to-RAM continuation: portable proof and real AArch64 execution.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm.h"
#include "arm_ram_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(S5LBOX_STATIC_A64_ENGINE)
#include "a64_static.h"
#include "soc.h"
#include "snapshot.h"
#endif

#define BASE UINT32_C(0x10000000)
#define VA UINT32_C(0x70000000)
#define SIZE 65536u
#define CODE (VA + 0x8000u)
#define NEXT (VA + 0x8400u)
#define DATA (VA + 0xc000u)
static uint8_t ram[SIZE], saved_ram[SIZE], expected_ram[SIZE];
static arm_cpu_t cpu, before, reference;
static arm_bus_t bus;
static unsigned checks, failures, reads, writes, grants, observed_writes;
static bool refuse_range;
static arm_ram_map_t ram_map;
#define CHECK(c, ...) do { checks++; if (!(c)) { \
    if (failures++ < 30u) { printf("%s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint8_t *host(void *ctx, uint32_t pa, uint32_t bytes) {
    (void)ctx;
    grants++;
    if (refuse_range || !bytes || pa < BASE ||
        (uint64_t)pa + bytes > (uint64_t)BASE + SIZE) return NULL;
    return ram + (pa - BASE);
}
static uint32_t read32(void *ctx, uint32_t pa) {
    uint32_t value = 0; (void)ctx; reads++;
    if (pa >= BASE && (uint64_t)pa + 4u <= (uint64_t)BASE + SIZE)
        memcpy(&value, ram + pa - BASE, 4u);
    return value;
}
static uint16_t read16(void *ctx, uint32_t pa) {
    uint16_t value = 0; (void)ctx; reads++;
    if (pa >= BASE && (uint64_t)pa + 2u <= (uint64_t)BASE + SIZE)
        memcpy(&value, ram + pa - BASE, 2u);
    return value;
}
static uint8_t read8(void *ctx, uint32_t pa) {
    (void)ctx; reads++;
    return pa >= BASE && pa - BASE < SIZE ? ram[pa - BASE] : 0u;
}
static void write32(void *ctx, uint32_t pa, uint32_t value) {
    (void)ctx; writes++;
    if (pa >= BASE && (uint64_t)pa + 4u <= (uint64_t)BASE + SIZE)
        memcpy(ram + pa - BASE, &value, 4u);
}
static void write16(void *ctx, uint32_t pa, uint16_t value) {
    (void)ctx; writes++;
    if (pa >= BASE && (uint64_t)pa + 2u <= (uint64_t)BASE + SIZE)
        memcpy(ram + pa - BASE, &value, 2u);
}
static void write8(void *ctx, uint32_t pa, uint8_t value) {
    (void)ctx; writes++;
    if (pa >= BASE && pa - BASE < SIZE) ram[pa - BASE] = value;
}
static void observed_write(void *ctx, uint32_t pa, uint32_t value) {
    observed_writes++;
    write32(ctx, pa, value);
}
static void insn(uint32_t va, uint32_t value) {
    memcpy(ram + (va - VA), &value, 4u);
}
static void half(uint32_t va, uint16_t value) {
    memcpy(ram + (va - VA), &value, 2u);
}
static unsigned slot(uint32_t va, arm_access_t access) {
    return ((va >> 10) + (unsigned)access * 1024u) & 4095u;
}
static void prime(uint32_t va, arm_access_t access, bool priv, uint32_t pa) {
    unsigned i = slot(va, access);
    cpu.tlb[i].gen = cpu.tlb_gen;
    cpu.tlb[i].tag = ((va >> 10) << 3) | ((unsigned)access << 1) | priv;
    cpu.tlb[i].pa = pa;
    cpu.tlb[i].fsr = 0;
}
static void setup(void) {
    memset(ram, 0, sizeof ram);
    refuse_range = false;
    bus = (arm_bus_t){
        .read32 = read32, .read16 = read16, .read8 = read8,
        .write32 = write32, .write16 = write16, .write8 = write8,
        .host_ram = host, .host_ram_write = host,
    };
    arm_reset(&cpu, &bus);
    cpu.cpsr = ARM_MODE_USR;
    cpu.r[15] = CODE;
    cpu.cp15.sctlr |= ARM_SCTLR_M;
    cpu.cp15.ttbr0 = BASE;
    cpu.cp15.dacr = 1u;
    /* Real legacy section descriptor, full User permissions, domain zero. */
    write32(NULL, BASE + (VA >> 20) * 4u, BASE | 0xc02u);
    uint32_t pa = 0;
    CHECK(arm_mmu_translate(&cpu, CODE, ARM_ACCESS_FETCH, false, &pa) == 0u &&
              pa == BASE + 0x8000u, "actual MMU section setup");
    CHECK(arm_fetch_cache_try_refill(&cpu, CODE, false), "initial FETCH");
    reads = writes = grants = 0;
}

static void test_capability(void) {
    arm_ram_window_t w;
    setup();
    CHECK(!arm_ram_window_capture(NULL, &cpu, BASE, SIZE), "null result");
    CHECK(!arm_ram_window_capture(&w, NULL, BASE, SIZE), "null CPU");
    CHECK(!w.read_host, "refusal clears old permission");
    CHECK(!arm_ram_window_capture(&w, &cpu, BASE, 1023u), "short range");
    CHECK(!arm_ram_window_capture(&w, &cpu, 0xfffffc00u, 2048u), "PA wrap");
    refuse_range = true;
    CHECK(!arm_ram_window_capture(&w, &cpu, BASE, SIZE), "bus range refusal");
    refuse_range = false;
    CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "whole RAM grant");
    CHECK(w.read_host == ram && w.write_host == ram, "separate grants");
    unsigned count = grants;
    CHECK(arm_ram_window_current(&w, &cpu), "live capability");
    CHECK(grants == count, "revalidation does not call bus");
    bus.write32 = observed_write;
    CHECK(!arm_ram_window_current(&w, &cpu), "changed write observer");
    bus.write32 = write32;
    bus.host_ram_write = NULL;
    CHECK(!arm_ram_window_current(&w, &cpu), "revoked write consent");
    CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE) && !w.write_host,
          "read grant without write consent");
    CHECK(arm_ram_window_current(&w, &cpu), "read-only capability current");
    arm_bus_t other = bus;
    cpu.bus = &other;
    CHECK(!arm_ram_window_current(&w, &cpu), "bus replacement");
    cpu.bus = &bus;
    bus.ctx = &w;
    CHECK(!arm_ram_window_current(&w, &cpu), "bus context replacement");
}

static void test_persistent_map(void) {
    arm_ram_window_t w;
    setup();
    arm_ram_map_reset(&ram_map);
    arm_ram_map_reset(NULL);
    CHECK(!arm_ram_map_prepare(NULL, &w, &cpu), "null map");
    CHECK(!arm_ram_map_prepare(&ram_map, NULL, &cpu), "null capability");
    CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "map RAM grant");
    CHECK(!arm_ram_map_prepare(&ram_map, &w, NULL), "null CPU");
    CHECK(arm_ram_map_prepare(&ram_map, &w, &cpu), "map prepare");
    CHECK(arm_ram_map_current(&ram_map, &cpu), "map current");
    CHECK(!arm_ram_map_lookup(&ram_map, &cpu, CODE, ARM_ACCESS_FETCH),
          "empty map cannot grant VA zero or any other address");

    unsigned previous_grants = grants;
    for (unsigned access = 0; access < 3u; access++) {
        for (unsigned block = 0; block < 64u; block++) {
            uint32_t va = VA + block * 1024u;
            uint32_t pa = BASE + (63u - block) * 1024u;
            prime(va, (arm_access_t)access, false, pa);
            before = cpu;
            CHECK(arm_ram_map_publish(&ram_map, &cpu, va,
                      (arm_access_t)access), "publish exact User permission");
            for (unsigned offset = 0; offset < 1024u; offset += 31u)
                CHECK(arm_ram_map_lookup(&ram_map, &cpu, va + offset,
                          (arm_access_t)access) == ram + pa - BASE,
                      "persistent map matches all bytes in 1KiB grant");
            CHECK(!memcmp(&cpu, &before, sizeof cpu), "map does not alter CPU");
        }
    }
    CHECK(reads == 0u && writes == 0u && grants == previous_grants,
          "map never walks, calls bus, or requests another RAM grant");
    CHECK(!arm_ram_map_publish(&ram_map, &cpu, DATA, (arm_access_t)-1) &&
              !arm_ram_map_lookup(&ram_map, &cpu, DATA, (arm_access_t)-1),
          "invalid access rejected");
    prime(DATA, ARM_ACCESS_READ, false, BASE + 0xc000u);
    CHECK(arm_ram_map_publish(&ram_map, &cpu, DATA, ARM_ACCESS_READ), "seed read");
    unsigned index = slot(DATA, ARM_ACCESS_READ);
    memset(&cpu.tlb[index], 0, sizeof cpu.tlb[index]);
    CHECK(!arm_ram_window_tlb_lookup(&w, &cpu, DATA, ARM_ACCESS_READ, false) &&
              arm_ram_map_lookup(&ram_map, &cpu, DATA, ARM_ACCESS_READ) ==
                  ram + 0xc000u,
          "TLB eviction does not revoke an independent proved RAM mapping");

    /* Deliberate map collisions cannot answer for the evicted address. */
    uint32_t alias = DATA + UINT32_C(0x400000);
    while (arm_ram_map_slot(alias, ARM_ACCESS_READ) !=
               arm_ram_map_slot(DATA, ARM_ACCESS_READ)) alias += 1024u;
    prime(alias, ARM_ACCESS_READ, false, BASE + 0xc400u);
    CHECK(arm_ram_map_publish(&ram_map, &cpu, alias, ARM_ACCESS_READ), "alias fill");
    CHECK(!arm_ram_map_lookup(&ram_map, &cpu, DATA, ARM_ACCESS_READ) &&
              arm_ram_map_lookup(&ram_map, &cpu, alias, ARM_ACCESS_READ) ==
                  ram + 0xc400u, "full key separates direct-map aliases");

    arm_mmu_tlb_flush(&cpu);
    CHECK(!arm_ram_map_current(&ram_map, &cpu), "flush ends old lease");
    CHECK(arm_ram_map_prepare(&ram_map, &w, &cpu) &&
              !arm_ram_map_lookup(&ram_map, &cpu, alias, ARM_ACCESS_READ),
          "new generation cannot consume old keys");
    for (unsigned access = 0; access < 3u; access++) {
        prime(DATA, (arm_access_t)access, true, BASE + 0xc000u);
        CHECK(!arm_ram_map_publish(&ram_map, &cpu, DATA, (arm_access_t)access),
              "privileged TLB entry cannot grant User mapping");
        prime(DATA, (arm_access_t)access, false, BASE + 0xc000u);
        unsigned i = slot(DATA, (arm_access_t)access);
        cpu.tlb[i].fsr = 15u;
        CHECK(!arm_ram_map_publish(&ram_map, &cpu, DATA, (arm_access_t)access),
              "faulting TLB entry cannot be published");
        cpu.tlb[i].fsr = 0;
        cpu.tlb[i].pa = BASE + SIZE;
        CHECK(!arm_ram_map_publish(&ram_map, &cpu, DATA, (arm_access_t)access),
              "non-RAM range cannot be published");
        cpu.tlb[i].pa = BASE + 1u;
        CHECK(!arm_ram_map_publish(&ram_map, &cpu, DATA, (arm_access_t)access),
              "unaligned physical block cannot be published");
    }

    prime(DATA, ARM_ACCESS_WRITE, false, BASE + 0xc000u);
    CHECK(arm_ram_map_publish(&ram_map, &cpu, DATA, ARM_ACCESS_WRITE), "seed write");
    bus.host_ram_write = NULL;
    CHECK(!arm_ram_map_lookup(&ram_map, &cpu, DATA, ARM_ACCESS_WRITE) &&
              !arm_ram_map_prepare(&ram_map, &w, &cpu) && !ram_map.bound,
          "revoked write observer grant invalidates lease");
    CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE) &&
              arm_ram_map_prepare(&ram_map, &w, &cpu), "rebind read-only RAM");
    CHECK(!arm_ram_map_lookup(&ram_map, &cpu, DATA, ARM_ACCESS_WRITE) &&
              !arm_ram_map_publish(&ram_map, &cpu, DATA, ARM_ACCESS_WRITE),
          "read-only rebind does not resurrect old write pointer");
    prime(DATA, ARM_ACCESS_READ, false, BASE + 0xc000u);
    CHECK(arm_ram_map_publish(&ram_map, &cpu, DATA, ARM_ACCESS_READ),
          "read-only grant still supports reads");
    cpu.cpsr = ARM_MODE_SVC;
    CHECK(!arm_ram_map_lookup(&ram_map, &cpu, DATA, ARM_ACCESS_READ),
          "User mapping never crosses into kernel execution");
    cpu.cpsr = ARM_MODE_USR;
    cpu.irq_line = true;
    CHECK(!arm_ram_map_current(&ram_map, &cpu), "pending IRQ stops lease");
    cpu.irq_line = false;
    cpu.cp15.ttbr0 ^= 0x4000u;
    CHECK(!arm_ram_map_lookup(&ram_map, &cpu, DATA, ARM_ACCESS_READ) &&
              !arm_ram_map_prepare(&ram_map, &w, &cpu), "unannounced TTBR change");
    cpu.cp15.ttbr0 ^= 0x4000u;
    CHECK(arm_ram_map_prepare(&ram_map, &w, &cpu) &&
              !arm_ram_map_lookup(&ram_map, &cpu, DATA, ARM_ACCESS_READ),
          "failed prepare requires a new permission witness");

    CHECK(arm_ram_map_publish(&ram_map, &cpu, DATA, ARM_ACCESS_READ), "wrap seed");
    cpu.tlb_flushes += UINT64_C(1) << 32;
    CHECK(!arm_ram_map_current(&ram_map, &cpu) &&
              arm_ram_map_prepare(&ram_map, &w, &cpu) &&
              !arm_ram_map_lookup(&ram_map, &cpu, DATA, ARM_ACCESS_READ),
          "full generation lap cannot resurrect identical old key");
    cpu.tlb_gen = UINT32_MAX;
    CHECK(arm_ram_map_prepare(&ram_map, &w, &cpu), "last generation");
    prime(DATA, ARM_ACCESS_READ, false, BASE + 0xc000u);
    CHECK(arm_ram_map_publish(&ram_map, &cpu, DATA, ARM_ACCESS_READ), "last seed");
    arm_mmu_tlb_flush(&cpu);
    CHECK(arm_ram_map_prepare(&ram_map, &w, &cpu) &&
              !arm_ram_map_lookup(&ram_map, &cpu, DATA, ARM_ACCESS_READ),
          "actual generation wrap clears persistent witnesses");
    reference = cpu;
    CHECK(!arm_ram_map_lookup(&ram_map, &reference, DATA, ARM_ACCESS_READ) &&
              arm_ram_map_prepare(&ram_map, &w, &reference) &&
              !arm_ram_map_lookup(&ram_map, &reference, DATA, ARM_ACCESS_READ),
          "different CPU owner never inherits a cache grant");
    arm_ram_map_reset(&ram_map);
    CHECK(!arm_ram_map_current(&ram_map, &reference), "reset revokes all pointers");
    puts("PERSISTENT-RAM-MAP portable permission/lifetime checks executed");
}

static void test_exact_lookup(void) {
    arm_ram_window_t w;
    setup();
    CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "capture");
    for (unsigned access = 0; access < 3u; access++) {
        for (unsigned priv = 0; priv < 2u; priv++) {
            for (unsigned block = 0; block < 64u; block++) {
                uint32_t va = VA + block * 1024u;
                uint32_t pa = BASE + (63u - block) * 1024u;
                prime(va, (arm_access_t)access, priv != 0u, pa);
                unsigned i = slot(va, (arm_access_t)access);
                before = cpu;
                for (unsigned offset = 0; offset < 1024u; offset += 31u)
                    CHECK(arm_ram_window_tlb_lookup(&w, &cpu, va + offset,
                              (arm_access_t)access, priv != 0u) == ram + pa - BASE,
                          "exact block/access/privilege mapping");
                CHECK(!memcmp(&cpu, &before, sizeof cpu), "no CPU mutation");
                CHECK(!arm_ram_window_tlb_lookup(&w, &cpu, va,
                          (arm_access_t)access, priv == 0u), "wrong privilege");
                cpu.tlb[i].gen--;
                CHECK(!arm_ram_window_tlb_lookup(&w, &cpu, va,
                          (arm_access_t)access, priv != 0u), "stale entry");
                cpu.tlb[i].gen++;
                cpu.tlb[i].fsr = 15u;
                CHECK(!arm_ram_window_tlb_lookup(&w, &cpu, va,
                          (arm_access_t)access, priv != 0u), "cached fault");
                cpu.tlb[i].fsr = 0;
                cpu.tlb[i].pa = BASE - 1024u;
                CHECK(!arm_ram_window_tlb_lookup(&w, &cpu, va,
                          (arm_access_t)access, priv != 0u), "below RAM");
                cpu.tlb[i].pa = BASE + SIZE;
                CHECK(!arm_ram_window_tlb_lookup(&w, &cpu, va,
                          (arm_access_t)access, priv != 0u), "past RAM");
                cpu.tlb[i].pa = pa + 1u;
                CHECK(!arm_ram_window_tlb_lookup(&w, &cpu, va,
                          (arm_access_t)access, priv != 0u), "unaligned PA");
            }
        }
    }
    prime(DATA, ARM_ACCESS_READ, false, BASE + 0xc000u);
    uint32_t *controls[] = { &cpu.cp15.sctlr, &cpu.cp15.ttbr0,
        &cpu.cp15.ttbr1, &cpu.cp15.ttbcr, &cpu.cp15.dacr, &cpu.cp15.context_id };
    for (unsigned i = 0; i < sizeof controls / sizeof controls[0]; i++) {
        *controls[i] ^= 1u;
        CHECK(!arm_ram_window_tlb_lookup(&w, &cpu, DATA, ARM_ACCESS_READ, false),
              "unsignaled CP15 change %u", i);
        *controls[i] ^= 1u;
    }
    CHECK(!arm_ram_window_tlb_lookup(&w, &cpu, DATA, (arm_access_t)-1, false),
          "invalid access");
    cpu.tlb_gen = UINT32_MAX;
    prime(DATA, ARM_ACCESS_READ, false, BASE + 0xc000u);
    arm_mmu_tlb_flush(&cpu);
    CHECK(cpu.tlb_gen == 1u && !arm_ram_window_tlb_lookup(&w, &cpu, DATA,
              ARM_ACCESS_READ, false), "generation wrap clears witnesses");
    CHECK(reads == 0u && writes == 0u, "lookup never touches bus");
}

#if defined(S5LBOX_STATIC_A64_ENGINE)
typedef struct {
    unsigned calls, mutate;
    bool retire;
    a64_compact_raw_data_miss_t miss;
} fallback_t;
static a64_compact_raw_fallback_result_t fallback(
        void *opaque, a64_compact_raw_code_window_t *next,
        const a64_compact_raw_data_miss_t *miss) {
    fallback_t *f = opaque;
    f->calls++;
    if (miss) f->miss = *miss;
    if (!f->mutate) return A64_COMPACT_RAW_FALLBACK_NO_RETIRE;
    if (f->retire) { cpu.r[15] += 4u; cpu.cycles++; }
    switch (f->mutate) {
    case 1: arm_mmu_tlb_flush(&cpu); break;
    case 2: cpu.cp15.ttbr0 ^= 0x4000u; break;
    case 3: cpu.cp15.dacr ^= 1u; break;
    case 4: cpu.cp15.context_id++; break;
    case 5: bus.host_ram_write = NULL; break;
    case 6: bus.write32 = observed_write; break;
    case 7: cpu.cpsr = ARM_MODE_SVC; break;
    case 8: cpu.irq_line = true; break;
    case 9: cpu.fiq_line = true; break;
    case 10: cpu.abort_pending = true; break;
    case 11: cpu.cp15.sctlr ^= ARM_SCTLR_M; break;
    case 12: cpu.cp15.ttbr1 ^= 0x4000u; break;
    case 13: cpu.cp15.ttbcr ^= 1u; break;
    case 14: bus.ctx = f; break;
    case 15: cpu.cp15.cpacr ^= 1u; break;
    case 16: cpu.cpsr |= ARM_CPSR_E; break;
    case 17: cpu.tlb_stamp.sctlr ^= 1u; break;
    case 18: cpu.tlb_stamp.ttbr0 ^= 1u; break;
    case 19: cpu.tlb_stamp.ttbr1 ^= 1u; break;
    case 20: cpu.tlb_stamp.ttbcr ^= 1u; break;
    case 21: cpu.tlb_stamp.dacr ^= 1u; break;
    case 22: cpu.tlb_stamp.context_id ^= 1u; break;
    default: break;
    }
    next->code = ram + 0x8000u;
    next->code_base = CODE;
    next->code_bytes = 1024u;
    return f->retire ? A64_COMPACT_RAW_FALLBACK_RETIRE_CONTINUE
                     : A64_COMPACT_RAW_FALLBACK_NO_RETIRE_CONTINUE;
}

static void native_program(bool thumb) {
    if (thumb) {
        half(CODE, 0x6808u);     /* ldr r0, [r1] */
        half(CODE + 2u, 0x6010u); /* str r0, [r2] */
        half(CODE + 4u, 0x4718u); /* bx r3 */
        half(NEXT, 0x3001u);     /* adds r0, #1 */
        half(NEXT + 2u, 0x4720u); /* bx r4 */
    } else {
        insn(CODE, 0xe5910000u); /* ldr r0, [r1] */
        insn(CODE + 4u, 0xe5820000u); /* str r0, [r2] */
        insn(CODE + 8u, 0xe12fff13u); /* bx r3 */
        insn(NEXT, 0xe2800001u); /* add r0, r0, #1 */
        insn(NEXT + 4u, 0xe12fff14u); /* bx r4 */
    }
    insn(DATA, 0x13579bdfu);
    cpu.r[1] = DATA;
    cpu.r[2] = DATA + 1024u;
    cpu.r[3] = NEXT | (thumb ? 1u : 0u);
    cpu.r[4] = CODE | (thumb ? 1u : 0u);
    if (thumb) cpu.cpsr |= ARM_CPSR_T;
    prime(NEXT, ARM_ACCESS_FETCH, false, BASE + 0x8400u);
    prime(DATA, ARM_ACCESS_READ, false, BASE + 0xc000u);
    prime(DATA + 1024u, ARM_ACCESS_WRITE, false, BASE + 0xc400u);
}

static void test_native(void) {
    if (!a64_static_host_available()) {
        puts("NATIVE-TLB-REFILL execution unavailable on this host");
        return;
    }
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
        for (unsigned budget = 1; budget <= 1031u; budget += 17u) {
            arm_ram_window_t w;
            setup();
            native_program(thumb != 0u);
            CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "capture");
            reference = cpu;
            memcpy(saved_ram, ram, SIZE);
            for (unsigned i = 0; i < budget; i++)
                CHECK(arm_step(&reference) == ARM_OK, "interpreter reference");
            /* Reference and native share the same bus but execute separately. */
            memcpy(ram, saved_ram, SIZE);
            fallback_t f = {0};
            unsigned total = 0, native = 0, slow = 0;
            uint32_t owner_block = CODE;
            a64_compact_tlb_stats_t stats = {0};
            a64_compact_raw_options_t options = {
                .ram_window = &w, .owner_fetch_block = &owner_block,
            };
            CHECK(arm_ram_window_tlb_lookup(&w, &cpu, CODE,
                      ARM_ACCESS_FETCH, false) == ram + 0x8000u,
                  "native entry FETCH proof thumb=%u budget=%u", thumb, budget);
            CHECK(arm_ram_window_tlb_lookup(&w, &cpu, DATA,
                      ARM_ACCESS_READ, false) == ram + 0xc000u,
                  "native entry data proof thumb=%u budget=%u", thumb, budget);
            reads = writes = 0;
            CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
                      ram + 0x8000u, CODE, 1024u, budget, fallback, &f,
                      &options, NULL, NULL, &stats, &total, &native, &slow),
                  "native execution");
            if (budget == 1u || budget == 18u)
                printf("TLB-LOOP thumb=%u budget=%u total=%u native=%u slow=%u "
                       "calls=%u pc=%08x r1=%08x miss=%u/%08x/%u gen=%u "
                       "refills=%llu/%llu/%llu\n", thumb, budget, total,
                       native, slow, f.calls, cpu.r[15], cpu.r[1], f.miss.valid,
                       f.miss.va, f.miss.access, f.miss.tlb_gen,
                       (unsigned long long)stats.fetch,
                       (unsigned long long)stats.read,
                       (unsigned long long)stats.write);
            CHECK(total == budget && native == budget && slow == 0u &&
                      f.calls == 0u, "exact budget with no C fallbacks");
            CHECK(!memcmp(cpu.r, reference.r, sizeof cpu.r) &&
                      cpu.cpsr == reference.cpsr && cpu.cycles == reference.cycles,
                  "native/interpreter architectural equality");
            CHECK(stats.read == 1u && stats.write == (budget > 1u ? 1u : 0u),
                  "native access-specific refill counters");
            CHECK(budget < 4u || stats.fetch > 0u, "native FETCH refill ran");
            CHECK(cpu.fetch_blk == owner_block &&
                      cpu.fetch_gen == cpu.tlb_gen && !cpu.fetch_priv,
                  "CPU and owner FETCH state published together");
            CHECK(reads == 0u && writes == 0u, "no native MMIO/bus calls");
            CHECK(cpu.dread_hits == reference.dread_hits + stats.read &&
                      cpu.dwrite_hits == reference.dwrite_hits + stats.write &&
                      cpu.tlb_hits == reference.tlb_hits,
                  "refill is one TLB hit followed by one data hit");
            uint32_t stored = 0;
            memcpy(&stored, ram + 0xc400u, 4u);
            CHECK(budget < 2u || stored == 0x13579bdfu, "store result");
        }
    }
    for (unsigned persistent = 0; persistent < 2u; persistent++) {
    for (unsigned mutate = 1; mutate <= 22u; mutate++) {
        for (unsigned retire = 0; retire < 2u; retire++) {
            setup();
            insn(CODE, 0xe1a0000fu); /* MOV r0, PC: exact interpreter fallback */
            insn(CODE + 4u, 0xe3a00007u);
            arm_ram_window_t w;
            CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "capture");
            arm_ram_map_reset(&ram_map);
            if (persistent) {
                CHECK(arm_ram_map_prepare(&ram_map, &w, &cpu) &&
                          arm_ram_map_publish(&ram_map, &cpu, CODE, ARM_ACCESS_FETCH),
                      "warm mapping before callback revocation");
            }
            a64_compact_raw_options_t options = {
                .ram_window = &w, .ram_map = persistent ? &ram_map : NULL,
            };
            fallback_t f = { .mutate = mutate, .retire = retire != 0u };
            unsigned total, native, slow;
            uint64_t cycles = cpu.cycles;
            CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
                      ram + 0x8000u, CODE, 1024u, 20u, fallback, &f, &options,
                      NULL, NULL, NULL, &total, &native, &slow), "guarded call");
            CHECK(f.calls == 1u && total == retire && native == 0u &&
                      slow == retire && cpu.cycles == cycles + retire &&
                      cpu.r[0] == 0u, "context mutation %u stops exactly", mutate);
        }
    }
    }
    puts("NATIVE-TLB-REFILL real execution and mutation-boundary checks complete");
}

static void test_native_persistent_map(void) {
    if (!a64_static_host_available()) return;
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
        setup();
        native_program(thumb != 0u);
        arm_ram_window_t w;
        CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "persistent capture");
        arm_ram_map_reset(&ram_map);
        for (unsigned phase = 0; phase < 2u; phase++) {
            if (phase) {
                /* Independent derived grants must survive both a native call
                 * boundary and eviction from the small first-level caches
                 * and raw TLB. The actual page tables still grant each VA. */
                memset(cpu.dread, 0, sizeof cpu.dread);
                memset(cpu.dwrite, 0, sizeof cpu.dwrite);
                memset(&cpu.tlb[slot(NEXT, ARM_ACCESS_FETCH)], 0,
                       sizeof cpu.tlb[0]);
                memset(&cpu.tlb[slot(DATA, ARM_ACCESS_READ)], 0,
                       sizeof cpu.tlb[0]);
                memset(&cpu.tlb[slot(DATA+1024u, ARM_ACCESS_WRITE)], 0,
                       sizeof cpu.tlb[0]);
            }
            reference = cpu;
            memcpy(saved_ram, ram, SIZE);
            for (unsigned i = 0; i < 510u; i++)
                CHECK(arm_step(&reference) == ARM_OK, "persistent literal oracle");
            memcpy(expected_ram, ram, SIZE);
            memcpy(ram, saved_ram, SIZE);
            reads = writes = 0;
            fallback_t f = {0};
            a64_compact_tlb_stats_t raw = {0};
            a64_compact_ram_map_stats_t mapped = {0};
            a64_compact_raw_options_t options = {
                .ram_window = &w, .ram_map = &ram_map, .ram_map_stats = &mapped,
            };
            unsigned total, native, slow;
            CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
                      ram + 0x8000u, CODE, 1024u, 510u, fallback, &f, &options,
                      NULL, NULL, &raw, &total, &native, &slow), "persistent native");
            CHECK(total == 510u && native == total && !slow && !f.calls,
                  "persistent map keeps exact bounded native execution");
            CHECK(!memcmp(cpu.r, reference.r, sizeof cpu.r) &&
                      cpu.cpsr == reference.cpsr && cpu.cycles == reference.cycles,
                  "persistent native/interpreter architecture matches");
            uint32_t stored;
            memcpy(&stored, ram + 0xc400u, sizeof stored);
            CHECK(stored == 0x13579bdfu && !memcmp(ram, expected_ram, SIZE) &&
                      reads == 0u && writes == 0u,
                  "persistent memory result without bus callbacks");
            CHECK(mapped.fetch > 0u, "native map FETCH hit actually executed");
            if (phase) {
                CHECK(mapped.read == 1u && mapped.write == 1u && !raw.fetch &&
                          !raw.read && !raw.write,
                      "warm persistent map replaces all raw-TLB refills");
            } else {
                CHECK(raw.read == 1u && raw.write == 1u && raw.fetch == 2u &&
                          !mapped.read && !mapped.write,
                      "only proved raw refills publish cold mappings");
            }
        }
    }
    puts("NATIVE-PERSISTENT-RAM-MAP A32/Thumb fetch/read/write hits executed");
}

static void test_native_refusals(void) {
    if (!a64_static_host_available()) return;
    for (unsigned persistent = 0; persistent < 2u; persistent++) {
    for (unsigned access = 0; access < 3u; access++) {
        for (unsigned kind = 0; kind < 9u; kind++) {
            setup();
            native_program(false);
            uint32_t target = access == ARM_ACCESS_FETCH ? NEXT : DATA;
            prime(target, (arm_access_t)access, false,
                  BASE + (target - VA));
            insn(CODE, access == ARM_ACCESS_FETCH ? 0xe12fff13u :
                 access == ARM_ACCESS_WRITE ? 0xe5810000u : 0xe5910000u);
            unsigned i = slot(target, (arm_access_t)access);
            switch (kind) {
            case 0: cpu.tlb[i].gen = 0u; break;
            case 1: cpu.tlb[i].tag ^= 1u; break; /* privileged alias */
            case 2: cpu.tlb[i].tag ^= 2u; break; /* other access kind */
            case 3: cpu.tlb[i].fsr = 15u; break;
            case 4: cpu.tlb[i].pa = BASE - 1024u; break;
            case 5: cpu.tlb[i].pa = BASE + SIZE; break;
            case 6: cpu.tlb[i].pa++; break;
            case 7:
                /* Last bytes fit, but the promised full block does not. */
                break;
            case 8:
                if (access == ARM_ACCESS_WRITE) bus.host_ram_write = NULL;
                else cpu.tlb[i].tag ^= 8u; /* neighboring 1 KiB permission */
                break;
            }
            arm_ram_window_t w;
            CHECK(arm_ram_window_capture(&w, &cpu, BASE,
                      kind == 7u ? 0x8500u : SIZE), "refusal capture");
            /* For the truncated FETCH case the target block is 0x8400. */
            arm_ram_map_reset(&ram_map);
            a64_compact_raw_options_t options = {
                .ram_window = &w, .ram_map = persistent ? &ram_map : NULL,
            };
            a64_compact_tlb_stats_t stats;
            fallback_t f = {0};
            unsigned total, native, slow;
            before = cpu;
            memcpy(saved_ram, ram, SIZE);
            reads = writes = 0;
            CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
                      ram + 0x8000u, CODE, 1024u, 10u, fallback, &f, &options,
                      NULL, NULL, &stats, &total, &native, &slow), "refusal run");
            unsigned expected = access == ARM_ACCESS_FETCH ? 1u : 0u;
            CHECK(f.calls == 1u && total == expected && native == expected &&
                      slow == 0u, "exact refusal access=%u kind=%u", access, kind);
            CHECK(stats.fetch == 0u && stats.read == 0u && stats.write == 0u &&
                      cpu.tlb_hits == before.tlb_hits,
                  "refusal did not publish or count a refill");
            CHECK(!memcmp(ram, saved_ram, SIZE) && reads == 0u && writes == 0u,
                  "refusal leaves memory and bus untouched");
            CHECK(!memcmp(cpu.r, before.r, 15u * sizeof cpu.r[0]) &&
                      cpu.cpsr == before.cpsr && !cpu.abort_pending,
                  "refused instruction did not mutate architecture");
        }
    }
    }
}

static void test_native_memory_families(void) {
    if (!a64_static_host_available()) return;
    static const struct {
        uint32_t opcode;
        bool thumb, write, unsupported;
    } cases[] = {
        {0xe5910000u, false, false, false}, {0xe5d10000u, false, false, false},
        {0xe1d100b0u, false, false, false}, {0xe1d100d0u, false, false, false},
        {0xe1d100f0u, false, false, false}, {0xe5810000u, false, true, false},
        {0xe5c10000u, false, true, false}, {0xe1c100b0u, false, true, false},
        {0xe1c100d0u, false, false, true}, {0xe1c100f0u, false, true, true},
        {0xe8910015u, false, false, false}, {0xe8810015u, false, true, false},
        {0xe4910004u, false, false, false}, {0xe4810004u, false, true, false},
        {0xe4b10004u, false, false, false}, {0xe4a10004u, false, true, false},
        {0xed910a00u, false, false, false}, {0xed810a00u, false, true, false},
        {0xed910b00u, false, false, false}, {0xed810b00u, false, true, false},
        {0x6808u, true, false, false}, {0x7808u, true, false, false},
        {0x8808u, true, false, false}, {0x6008u, true, true, false},
        {0x7008u, true, true, false}, {0x8008u, true, true, false},
        {0xc915u, true, false, false}, {0xc115u, true, true, false},
        {0x5688u, true, false, false}, {0x5e88u, true, false, false},
    };
    static uint8_t expected_ram[SIZE];
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        for (unsigned offset = 0; offset <= 1000u; offset += 100u) {
            setup();
            insn(CODE, cases[i].opcode);
            for (unsigned j = 0xc000u; j < 0xc400u; j++)
                ram[j] = (uint8_t)(j * 39u + i);
            cpu.cpsr |= ARM_CPSR_C | (cases[i].thumb ? ARM_CPSR_T : 0u);
            cpu.r[0] = 0x12345678u;
            cpu.r[1] = DATA + offset;
            cpu.r[2] = 4u;
            cpu.r[4] = 0xabcdef01u;
            cpu.cp15.cpacr = 0x00f00000u;
            cpu.vfp_fpexc = 1u << 30;
            cpu.vfp_s[0] = 0x3f123456u;
            cpu.vfp_s[1] = 0x40234567u;
            arm_access_t access = cases[i].write ? ARM_ACCESS_WRITE : ARM_ACCESS_READ;
            prime(DATA, access, false, BASE + 0xc000u);
            arm_ram_window_t w;
            CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "family capture");
            before = cpu;
            reference = cpu;
            memcpy(saved_ram, ram, SIZE);
            CHECK(arm_step(&reference) == ARM_OK, "family reference %08x", cases[i].opcode);
            memcpy(expected_ram, ram, SIZE);
            memcpy(ram, saved_ram, SIZE);
            a64_compact_raw_options_t options = { .ram_window = &w };
            a64_compact_tlb_stats_t stats;
            fallback_t f = {0};
            unsigned total, native, slow;
            CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
                      ram + 0x8000u, CODE, 1024u, 1u, fallback, &f, &options,
                      NULL, NULL, &stats, &total, &native, &slow), "family run");
            if (cases[i].unsupported) {
                /* Doubleword transfers remain outside the scalar mode-3
                 * family. L is not their load/store selector. */
                CHECK(a64_compact_raw_classify_instruction(&before,
                          cases[i].opcode, false) ==
                              A64_COMPACT_RAW_REJECT_MEMORY_FORM,
                      "doubleword transfer rejection %08x", cases[i].opcode);
                CHECK(total == 0u && native == 0u && slow == 0u && f.calls == 1u &&
                          stats.fetch == 0u && stats.read == 0u && stats.write == 0u &&
                          !memcmp(cpu.r, before.r, sizeof cpu.r) &&
                          cpu.cpsr == before.cpsr && cpu.cycles == before.cycles &&
                          !memcmp(ram, saved_ram, SIZE),
                      "unsupported instruction still falls back untouched %08x",
                      cases[i].opcode);
                continue;
            }
            CHECK(total == 1u && native == 1u && slow == 0u && f.calls == 0u,
                  "family admitted %08x offset %u", cases[i].opcode, offset);
            CHECK(!memcmp(cpu.r, reference.r, sizeof cpu.r) &&
                      cpu.cpsr == reference.cpsr && cpu.cycles == reference.cycles &&
                      !memcmp(cpu.vfp_s, reference.vfp_s, sizeof cpu.vfp_s) &&
                      cpu.vfp_fpscr == reference.vfp_fpscr &&
                      cpu.vfp_fpexc == reference.vfp_fpexc &&
                      !memcmp(ram, expected_ram, SIZE),
                  "family exact state %08x offset %u", cases[i].opcode, offset);
            CHECK(stats.read == (cases[i].write ? 0u : 1u) &&
                      stats.write == (cases[i].write ? 1u : 0u),
                  "family proved native refill %08x", cases[i].opcode);
        }
    }
}

static void test_native_map_revocation(void) {
    if (!a64_static_host_available()) return;
    for (unsigned access = 0; access < 3u; access++) {
        setup();
        native_program(false);
        uint32_t target = access == ARM_ACCESS_FETCH ? NEXT : DATA;
        prime(target, (arm_access_t)access, false, BASE + target - VA);
        insn(CODE, access == ARM_ACCESS_FETCH ? 0xe12fff13u :
             access == ARM_ACCESS_WRITE ? 0xe5810000u : 0xe5910000u);
        arm_ram_window_t w;
        CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "revocation capture");
        arm_ram_map_reset(&ram_map);
        CHECK(arm_ram_map_prepare(&ram_map, &w, &cpu) &&
                  arm_ram_map_publish(&ram_map, &cpu, target, (arm_access_t)access),
              "old generation really has a usable mapping");
        arm_mmu_tlb_flush(&cpu);
        prime(CODE, ARM_ACCESS_FETCH, false, BASE + CODE - VA);
        prime(target, (arm_access_t)access, false, BASE + target - VA);
        cpu.tlb[slot(target, (arm_access_t)access)].fsr = 15u;
        a64_compact_ram_map_stats_t mapped;
        a64_compact_tlb_stats_t raw;
        a64_compact_raw_options_t options = {
            .ram_window = &w, .ram_map = &ram_map, .ram_map_stats = &mapped,
        };
        fallback_t f = {0};
        unsigned total, native, slow;
        memcpy(saved_ram, ram, SIZE);
        reads = writes = 0;
        CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
                  ram + 0x8000u, CODE, 1024u, 10u, fallback, &f, &options,
                  NULL, NULL, &raw, &total, &native, &slow), "revoked native run");
        unsigned prefix = access == ARM_ACCESS_FETCH ? 1u : 0u;
        CHECK(total == prefix && native == prefix && !slow && f.calls == 1u &&
                  !mapped.read && !mapped.write && !mapped.fetch &&
                  !raw.read && !raw.write && !raw.fetch &&
                  !memcmp(saved_ram, ram, SIZE) && !reads && !writes,
              "warm mapping cannot bypass new access fault %u", access);
    }
}

static unsigned arith_extra_native_runs;

/* Classify on every host; compare the actual signed runner where available.
 * A missing memory witness can refuse an otherwise admitted instruction. */
static void compare_arith_extra(uint32_t opcode,
        a64_compact_raw_admission_t admission, bool execute, bool refill) {
    static uint8_t expected_ram[SIZE];
    insn(CODE, opcode);
    CHECK(a64_compact_raw_classify_instruction(&cpu, opcode, false) == admission,
          "arithmetic/extra admission %08x expected %u", opcode, admission);
    if (!a64_static_host_available()) return;
    arm_ram_window_t w;
    CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "extra RAM capture");
    before = cpu;
    reference = cpu;
    memcpy(saved_ram, ram, SIZE);
    if (execute)
        CHECK(arm_step(&reference) == ARM_OK, "extra reference %08x", opcode);
    memcpy(expected_ram, ram, SIZE);
    memcpy(ram, saved_ram, SIZE);
    a64_compact_raw_options_t options = { .ram_window = refill ? &w : NULL };
    a64_compact_tlb_stats_t stats = {0};
    fallback_t f = {0};
    unsigned total = 0, native = 0, slow = 0;
    reads = writes = 0;
    CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
              ram + 0x8000u, CODE, 1024u, 1u, fallback, &f, &options,
              NULL, NULL, &stats, &total, &native, &slow), "extra native run");
    CHECK(total == (unsigned)execute && native == total && !slow &&
              f.calls == (unsigned)!execute,
          "extra native admission %08x executed %u fallback %u", opcode, total, f.calls);
    CHECK(!memcmp(cpu.r, reference.r, sizeof cpu.r) &&
              cpu.cpsr == reference.cpsr && cpu.cycles == reference.cycles &&
              cpu.abort_pending == reference.abort_pending &&
              !memcmp(cpu.vfp_s, reference.vfp_s, sizeof cpu.vfp_s) &&
              cpu.vfp_fpscr == reference.vfp_fpscr &&
              cpu.vfp_fpexc == reference.vfp_fpexc &&
              !memcmp(ram, expected_ram, SIZE),
          "extra exact architectural state %08x refill %u", opcode, refill);
    CHECK(!reads && !writes, "extra native access never enters bus %08x", opcode);
    if (!execute)
        CHECK(!stats.fetch && !stats.read && !stats.write &&
                  cpu.dread_hits == before.dread_hits &&
                  cpu.dwrite_hits == before.dwrite_hits,
              "refusal has no successful access counters %08x", opcode);
    arith_extra_native_runs++;
}

static void test_multiply_families(void) {
    static const uint32_t values[] = {
        0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu,
        0x0000ffffu, 0xffff0000u, 0x13579bdfu,
    };
    for (unsigned kind = 0; kind < 12u; kind++) {
        for (unsigned pair = 0; pair < 64u; pair++) {
            for (unsigned alias = 0; alias < 5u; alias++) {
                setup();
                cpu.cpsr |= (pair & 15u) << 28;
                cpu.cpsr |= ARM_CPSR_Q;
                cpu.r[2] = 0xffffffffu;
                cpu.r[3] = 0x80000000u;
                cpu.r[4] = values[pair >> 3];
                cpu.r[5] = values[pair & 7u];
                cpu.r[6] = pair & 1u ? 0x7fffffffu : 0x80000000u;
                unsigned rm = alias == 1u || alias == 4u ? 2u : 4u;
                unsigned rs = alias == 2u || alias == 4u ? 3u : 5u;
                if (alias == 3u) rs = rm;
                uint32_t opcode;
                if (kind < 4u) {
                    unsigned rn = alias == 4u ? 2u : 6u;
                    opcode = 0xe0020090u | (rn << 12) | (rs << 8) | rm |
                        ((kind & 1u) << 20) | ((kind >> 1) << 21);
                } else {
                    unsigned form = kind - 4u;
                    opcode = 0xe0832090u | (rs << 8) | rm |
                        ((form & 1u) << 20) | (((form >> 1) & 1u) << 21) |
                        ((form >> 2) << 22);
                }
                compare_arith_extra(opcode, A64_COMPACT_RAW_ADMIT_EXECUTE,
                                    true, false);
            }
        }
    }
    static const uint32_t refused[] = {
        0xe00f0594u, 0xe002f594u, 0xe0020f94u, 0xe002059fu,
        0xe0833094u, 0xe08f2594u, 0xe083f594u, 0xe0832f94u, 0xe083259fu,
        0xe1032094u, /* SWP is not a multiply. */
    };
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        setup();
        a64_compact_raw_admission_t a = i == 4u || i == 9u
            ? A64_COMPACT_RAW_REJECT_DP_REGISTER_SHIFT : A64_COMPACT_RAW_REJECT_DP_PC;
        compare_arith_extra(refused[i], a, false, false);
        setup(); /* A failed condition skips even an unsupported encoding. */
        compare_arith_extra(refused[i] & 0x0fffffffu,
                            A64_COMPACT_RAW_ADMIT_CONDITION_SKIP, true, false);
    }
}

static uint32_t extra_opcode(unsigned kind, bool pre, bool up, bool immediate,
        bool writeback, unsigned rn, unsigned rd, unsigned offset) {
    unsigned sh = kind < 3u ? kind + 1u : 1u;
    return 0xe0000090u | ((uint32_t)pre << 24) | ((uint32_t)up << 23) |
        ((uint32_t)immediate << 22) | ((uint32_t)writeback << 21) |
        ((uint32_t)(kind < 3u) << 20) | (rn << 16) | (rd << 12) | (sh << 5) |
        (immediate ? ((offset & 0xf0u) << 4) | (offset & 15u) : offset);
}

static void test_extra_transfer_forms(void) {
    static const unsigned offsets[] = {0u, 1u, 2u, 15u, 16u, 127u, 254u, 255u};
    for (unsigned kind = 0; kind < 4u; kind++) {
        for (unsigned mode = 0; mode < 12u; mode++) {
            bool immediate = (mode & 1u) != 0u;
            bool up = (mode & 2u) != 0u;
            bool pre = mode / 4u != 2u;
            bool writeback = mode / 4u == 1u;
            for (unsigned sample = 0; sample < 32u; sample++) {
                for (unsigned refill = 0; refill < 2u; refill++) {
                    setup();
                    unsigned offset = offsets[sample & 7u];
                    unsigned rd = sample & 16u ? 2u : 0u;
                    uint32_t base = DATA + 256u + ((sample >> 3) & 1u);
                    uint32_t address = pre ? (up ? base + offset : base - offset) : base;
                    cpu.r[0] = 0xface9876u;
                    cpu.r[1] = base;
                    cpu.r[2] = offset;
                    cpu.cpsr |= (sample & 15u) << 28;
                    ram[address - VA] = (uint8_t)(sample * 47u);
                    ram[address - VA + 1u] = (uint8_t)(sample * 71u);
                    arm_access_t access = kind == 3u ? ARM_ACCESS_WRITE : ARM_ACCESS_READ;
                    prime(address, access, false, BASE + ((address - VA) & ~1023u));
                    if (!refill)
                        CHECK(arm_data_cache_try_refill(&cpu, address, access, false),
                              "ordinary data-cache proof");
                    bool aligned = kind == 1u || (address & 1u) == 0u;
                    compare_arith_extra(extra_opcode(kind, pre, up, immediate,
                              writeback, 1u, rd, immediate ? offset : 2u),
                        aligned ? A64_COMPACT_RAW_ADMIT_EXECUTE
                                : A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT,
                        aligned, refill != 0u);
                }
            }
        }
    }
    /* Literal PC bases and the final two bytes of a 1 KiB witness. Include
     * every signed byte value and both signs of a halfword. */
    for (unsigned kind = 0; kind < 4u; kind++) {
        for (unsigned sample = 0; sample < 256u; sample++) {
            setup();
            bool literal = (sample & 1u) != 0u;
            uint32_t address = literal ? CODE + 24u : DATA + 1022u + (kind == 1u);
            cpu.r[0] = 0xffff0000u | sample;
            cpu.r[1] = address;
            ram[address - VA] = (uint8_t)sample;
            ram[address - VA + 1u] = (uint8_t)(sample ^ 0x80u);
            arm_access_t access = kind == 3u ? ARM_ACCESS_WRITE : ARM_ACCESS_READ;
            prime(address, access, false, BASE + ((address - VA) & ~1023u));
            compare_arith_extra(extra_opcode(kind, true, true, true, false,
                      literal ? 15u : 1u, 0u, literal ? 16u : 0u),
                A64_COMPACT_RAW_ADMIT_EXECUTE, true, true);
        }
    }
    static const struct { uint32_t opcode; a64_compact_raw_admission_t reject; } bad[] = {
        {0xe1d1f0b0u, A64_COMPACT_RAW_REJECT_MEMORY_PC},
        {0xe0f100b0u, A64_COMPACT_RAW_REJECT_MEMORY_FORM}, /* P=0,W=1 */
        {0xe1f110b0u, A64_COMPACT_RAW_REJECT_MEMORY_FORM}, /* base/data WB alias */
        {0xe0d110b0u, A64_COMPACT_RAW_REJECT_MEMORY_FORM},
        {0xe1ff00b0u, A64_COMPACT_RAW_REJECT_MEMORY_PC},
        {0xe0df00b0u, A64_COMPACT_RAW_REJECT_MEMORY_PC},
        {0xe19101b2u, A64_COMPACT_RAW_REJECT_MEMORY_FORM}, /* SBZ bits11:8 */
        {0xe19100bfu, A64_COMPACT_RAW_REJECT_MEMORY_PC},
        {0xe1c100d0u, A64_COMPACT_RAW_REJECT_MEMORY_FORM}, /* LDRD */
        {0xe1c100f0u, A64_COMPACT_RAW_REJECT_MEMORY_FORM}, /* STRD */
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        setup();
        cpu.r[1] = DATA;
        compare_arith_extra(bad[i].opcode, bad[i].reject, false, true);
        setup();
        compare_arith_extra(bad[i].opcode & 0x0fffffffu,
                            A64_COMPACT_RAW_ADMIT_CONDITION_SKIP, true, true);
    }
    for (unsigned kind = 0; kind < 4u; kind++) {
        for (unsigned refill = 0; refill < 2u; refill++) {
            setup(); /* No current READ/WRITE mapping; do not page-walk. */
            cpu.r[0] = 0xdeadbeefu;
            cpu.r[1] = DATA;
            compare_arith_extra(extra_opcode(kind, false, true, true, false,
                      1u, 0u, 2u), A64_COMPACT_RAW_ADMIT_EXECUTE, false, refill != 0u);
        }
    }
    if (a64_static_host_available())
        printf("NATIVE-ARITH-EXTRA: %u exact execution/refusal comparisons\n",
               arith_extra_native_runs);
}

static void test_arith_extra_budget_loop(void) {
    if (!a64_static_host_available()) return;
    static uint8_t expected_ram[SIZE];
    for (unsigned priv = 0; priv < 2u; priv++) {
        for (unsigned refill = 0; refill < 2u; refill++) {
            for (unsigned budget = 1; budget <= 131u; budget++) {
                setup();
                if (priv) cpu.cpsr = ARM_MODE_SVC;
                insn(CODE,       0xe1d100b0u); /* LDRH r0,[r1] */
                insn(CODE + 4u,  0xe0224390u); /* MLA r2,r0,r3,r4 */
                insn(CODE + 8u,  0xe1d150d1u); /* LDRSB r5,[r1,#1] */
                insn(CODE + 12u, 0xe0c76095u); /* SMULL r6,r7,r5,r0 */
                insn(CODE + 16u, 0xe1c120b2u); /* STRH r2,[r1,#2] */
                insn(CODE + 20u, 0xe1d180f2u); /* LDRSH r8,[r1,#2] */
                insn(CODE + 24u, 0xe12fff1au); /* BX r10 */
                half(CODE + 64u, 0x1c40u);    /* ADDS r0,r0,#1 */
                half(CODE + 66u, 0x4758u);    /* BX r11 */
                insn(DATA, budget & 1u ? 0x000080ffu : 0xffff7fffu);
                cpu.r[1] = DATA;
                cpu.r[3] = 7u;
                cpu.r[4] = 0xffff1234u;
                cpu.r[10] = CODE + 65u;
                cpu.r[11] = CODE;
                prime(DATA, ARM_ACCESS_READ, priv != 0u, BASE + 0xc000u);
                prime(DATA, ARM_ACCESS_WRITE, priv != 0u, BASE + 0xc000u);
                /* Native TLB refill deliberately remains User-only. A cold
                 * privileged request must refuse without mutation; ordinary
                 * privileged DREAD/DWRITE witnesses still execute natively. */
                if (priv && refill)
                    compare_arith_extra(0xe1d100b0u, A64_COMPACT_RAW_ADMIT_EXECUTE,
                                        false, true);
                if (!refill || priv) {
                    CHECK(arm_data_cache_try_refill(&cpu, DATA, ARM_ACCESS_READ,
                              priv != 0u), "loop READ witness");
                    CHECK(arm_data_cache_try_refill(&cpu, DATA, ARM_ACCESS_WRITE,
                              priv != 0u), "loop WRITE witness");
                }
                arm_ram_window_t w;
                CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "loop capture");
                reference = cpu;
                memcpy(saved_ram, ram, SIZE);
                for (unsigned n = 0; n < budget; n++)
                    CHECK(arm_step(&reference) == ARM_OK, "mixed loop reference");
                memcpy(expected_ram, ram, SIZE);
                memcpy(ram, saved_ram, SIZE);
                fallback_t f = {0};
                a64_compact_raw_options_t options = { .ram_window = refill ? &w : NULL };
                unsigned total, native, slow;
                CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
                          ram + 0x8000u, CODE, 1024u, budget, fallback, &f,
                          &options, NULL, NULL, NULL, &total, &native, &slow),
                      "mixed loop native");
                CHECK(total == budget && native == budget && !slow && !f.calls,
                      "mixed loop no fallback budget %u priv %u refill %u",
                      budget, priv, refill);
                CHECK(!memcmp(cpu.r, reference.r, sizeof cpu.r) &&
                          cpu.cpsr == reference.cpsr && cpu.cycles == reference.cycles &&
                          !memcmp(ram, expected_ram, SIZE),
                      "mixed ARM/Thumb exact prefix budget %u", budget);
            }
        }
    }
}

static void test_native_live_code(void) {
    if (!a64_static_host_available()) return;
    for (unsigned persistent = 0; persistent < 2u; persistent++) {
    for (unsigned different = 0; different < 2u; different++) {
        setup();
        uint32_t target = different ? NEXT : CODE + 8u;
        insn(CODE, 0xe5810000u); /* str r0, [r1] */
        insn(CODE + 4u, 0xe12fff13u); /* bx r3 */
        insn(target, 0xe3a02003u); /* mov r2, #3, overwritten before fetch */
        cpu.r[0] = 0xe3a02009u; /* mov r2, #9 */
        cpu.r[1] = cpu.r[3] = target;
        prime(target, ARM_ACCESS_WRITE, false,
              BASE + ((target - VA) & ~1023u));
        if (different)
            prime(target, ARM_ACCESS_FETCH, false, BASE + 0x8400u);
        arm_ram_window_t w;
        CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "live-code capture");
        arm_ram_map_reset(&ram_map);
        if (persistent) {
            CHECK(arm_ram_map_prepare(&ram_map, &w, &cpu) &&
                      arm_ram_map_publish(&ram_map, &cpu, target, ARM_ACCESS_WRITE) &&
                      arm_ram_map_publish(&ram_map, &cpu, target, ARM_ACCESS_FETCH),
                  "warm read-code/write-data grants before live modification");
        }
        a64_compact_ram_map_stats_t mapped = {0};
        a64_compact_raw_options_t options = {
            .ram_window = &w, .ram_map = persistent ? &ram_map : NULL,
            .ram_map_stats = &mapped,
        };
        a64_compact_tlb_stats_t stats;
        fallback_t f = {0};
        unsigned total, native, slow;
        CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
                  ram + 0x8000u, CODE, 1024u, 3u, fallback, &f, &options,
                  NULL, NULL, &stats, &total, &native, &slow), "live-code run");
        CHECK(total == 3u && native == 3u && slow == 0u && !f.calls &&
                  cpu.r[2] == 9u && stats.write + mapped.write == 1u &&
                  stats.fetch + mapped.fetch == different &&
                  mapped.write == persistent,
              "live guest store visible in %s window", different ? "new" : "same");
    }
    }
}

static void test_native_machine(void) {
    static s5l8900_t fast, literal;
    CHECK(!s5l8900_static_a64_set_compact_tlb_refill(NULL, true), "null machine");
    CHECK(!s5l8900_static_a64_set_compact_tlb_refill(&fast, true), "uninitialized machine");
    bool a = s5l8900_init(&fast, BASE, 1u << 20);
    bool b = s5l8900_init(&literal, BASE, 1u << 20);
    CHECK(a && b, "machine init");
    if (!a || !b) {
        if (a) s5l8900_free(&fast);
        if (b) s5l8900_free(&literal);
        return;
    }
    CHECK(s5l8900_static_a64_compact_tlb_fetch(&fast) == 0u &&
              s5l8900_static_a64_compact_tlb_read(&fast) == 0u &&
              s5l8900_static_a64_compact_tlb_write(&fast) == 0u,
          "native refill default counters are zero");
    if (!a64_static_host_available()) {
        CHECK(!s5l8900_static_a64_set_compact_tlb_refill(&fast, true),
              "unavailable engine refused");
    } else {
        setup();
        native_program(false);
        s5l8900_load(&fast, BASE, ram, SIZE);
        s5l8900_load(&literal, BASE, ram, SIZE);
        fast.cpu = literal.cpu = cpu;
        fast.cpu.bus = &fast.bus;
        literal.cpu.bus = &literal.bus;
        fast.cpu.fetch_host = fast.ram + 0x8000u;
        literal.cpu.fetch_host = literal.ram + 0x8000u;
        CHECK(s5l8900_set_direct_ram_writes(&fast, true) &&
                  s5l8900_set_direct_ram_writes(&literal, true),
              "explicit machine write-observer consent");
        s5l8900_tick(&fast, 0u);
        s5l8900_tick(&literal, 0u);
        CHECK(s5l8900_static_a64_set_enabled(&fast, true) &&
                  s5l8900_static_a64_set_compact_raw(&fast, true) &&
                  s5l8900_static_a64_set_compact_tlb_refill(&fast, true),
              "enable native machine refill");
        CHECK(s5l8900_static_a64_set_enabled(&literal, false), "literal engine");
        arm_status_t fast_status = ARM_OK, literal_status = ARM_OK;
        CHECK(s5l8900_run(&fast, 8192u, &fast_status) == 8192u &&
                  s5l8900_run(&literal, 8192u, &literal_status) == 8192u &&
                  fast_status == ARM_OK && literal_status == ARM_OK,
              "exact machine retirement budget");
        CHECK(!memcmp(fast.cpu.r, literal.cpu.r, sizeof fast.cpu.r) &&
                  fast.cpu.cpsr == literal.cpu.cpsr &&
                  fast.cpu.cycles == literal.cpu.cycles &&
                  !memcmp(fast.ram, literal.ram, fast.ram_size),
              "machine/native memory continuation equals interpreter");
        CHECK(s5l8900_static_a64_compact_tlb_fetch(&fast) > 0u &&
                  s5l8900_static_a64_compact_tlb_read(&fast) == 1u &&
                  s5l8900_static_a64_compact_tlb_write(&fast) == 1u,
              "machine integration really used all three native refills");
        uint64_t fetches = s5l8900_static_a64_compact_tlb_fetch(&fast);
        CHECK(s5l8900_static_a64_set_compact_tlb_refill(&fast, false), "disable");
        CHECK(s5l8900_run(&fast, 8192u, &fast_status) == 8192u &&
                  s5l8900_static_a64_compact_tlb_fetch(&fast) == fetches,
              "same-machine OFF stops new native refills");
    }
    s5l8900_free(&fast);
    s5l8900_free(&literal);
}

static void test_native_map_machine(void) {
    static s5l8900_t fast, literal;
    CHECK(!s5l8900_static_a64_set_compact_ram_map(NULL, true) &&
              !s5l8900_static_a64_set_compact_ram_map(&fast, true),
          "persistent map rejects absent/uninitialized machine");
    bool a = s5l8900_init(&fast, BASE, 1u << 20);
    bool b = s5l8900_init(&literal, BASE, 1u << 20);
    CHECK(a && b, "persistent machine init");
    if (!a || !b) {
        if (a) s5l8900_free(&fast);
        if (b) s5l8900_free(&literal);
        return;
    }
    CHECK(!s5l8900_static_a64_compact_ram_map_fetch(&fast) &&
              !s5l8900_static_a64_compact_ram_map_read(&fast) &&
              !s5l8900_static_a64_compact_ram_map_write(&fast),
          "persistent mapping counters default to zero");
    if (!a64_static_host_available()) {
        CHECK(!s5l8900_static_a64_set_compact_ram_map(&fast, true),
              "unavailable persistent native engine refused");
    } else {
        setup();
        /* Match snapshot_load's reset generation deliberately. The test must
         * exercise lifetime revocation, not accidentally pass due to gen !=. */
        cpu.tlb_gen = 1u;
        prime(CODE, ARM_ACCESS_FETCH, false, BASE + 0x8000u);
        native_program(false);
        s5l8900_load(&fast, BASE, ram, SIZE);
        s5l8900_load(&literal, BASE, ram, SIZE);
        fast.cpu = literal.cpu = cpu;
        fast.cpu.bus = &fast.bus;
        literal.cpu.bus = &literal.bus;
        fast.cpu.fetch_host = fast.ram + 0x8000u;
        literal.cpu.fetch_host = literal.ram + 0x8000u;
        fast.cpu.fetch_gen = literal.cpu.fetch_gen = 1u;
        CHECK(s5l8900_set_direct_ram_writes(&fast, true) &&
                  s5l8900_set_direct_ram_writes(&literal, true), "map write consent");
        s5l8900_tick(&fast, 0u);
        s5l8900_tick(&literal, 0u);
        CHECK(s5l8900_static_a64_set_enabled(&fast, true) &&
                  s5l8900_static_a64_set_compact_raw(&fast, true) &&
                  s5l8900_static_a64_set_compact_ram_map(&fast, true), "enable map");
        CHECK(s5l8900_static_a64_set_enabled(&literal, false), "map literal oracle");
        CHECK(!s5l8900_static_a64_set_compact_tlb_refill(&fast, true) &&
                  !s5l8900_static_a64_set_compact_raw_window_cache(&fast, true) &&
                  !s5l8900_static_a64_set_compact_raw_window_refill(&fast, false) &&
                  s5l8900_static_a64_set_compact_tlb_refill(&fast, false),
              "conflicts refused; disabling other experiment preserves map");
        uint8_t *snapshot = NULL;
        size_t snapshot_size = 0;
        CHECK(snapshot_save_mem(&literal, &snapshot, &snapshot_size) == SNAP_OK,
              "save initial persistent-map machine");
        /* An old, still-valid cached translation can differ from the page
         * table stored in the snapshot. A restore must not keep that grant. */
        fast.cpu.tlb[slot(DATA, ARM_ACCESS_READ)].pa = BASE + 0xe000u;
        uint32_t old_value = 0xdeadbeefu, stored = 0;
        memcpy(fast.ram + 0xe000u, &old_value, sizeof old_value);
        arm_status_t fast_status = ARM_OK, literal_status = ARM_OK;
        CHECK(s5l8900_run(&fast, 510u, &fast_status) == 510u && fast_status == ARM_OK,
              "populate persistent machine mappings");
        memcpy(&stored, fast.ram + 0xc400u, sizeof stored);
        CHECK(stored == old_value &&
                  s5l8900_static_a64_compact_ram_map_fetch(&fast) > 0u,
              "old translation and warm native map were actually used");
        uint64_t reads_before = s5l8900_static_a64_compact_tlb_read(&fast);
        CHECK(snapshot && snapshot_load_mem(&fast, snapshot, snapshot_size) == SNAP_OK,
              "restore into same live machine/map allocation");
        free(snapshot);
        /* Supply only the new initial FETCH witness and current stamp. DATA
         * has to be translated again; no old derived cache may supply it. */
        fast.cpu.tlb_stamp = cpu.tlb_stamp;
        fast.cpu.tlb[slot(CODE, ARM_ACCESS_FETCH)] = cpu.tlb[slot(CODE, ARM_ACCESS_FETCH)];
        fast.cpu.fetch_host = fast.ram + 0x8000u;
        fast.cpu.fetch_blk = CODE;
        fast.cpu.fetch_gen = fast.cpu.tlb_gen;
        fast.cpu.fetch_priv = false;
        CHECK(fast.cpu.tlb_gen == 1u && fast.cpu.tlb_flushes == cpu.tlb_flushes,
              "restored translation generation/flush count intentionally collide");
        CHECK(s5l8900_run(&fast, 8192u, &fast_status) == 8192u &&
                  s5l8900_run(&literal, 8192u, &literal_status) == 8192u &&
                  fast_status == ARM_OK && literal_status == ARM_OK,
              "restored map exact machine retirement");
        CHECK(!memcmp(fast.cpu.r, literal.cpu.r, sizeof fast.cpu.r) &&
                  fast.cpu.cpsr == literal.cpu.cpsr &&
                  fast.cpu.cycles == literal.cpu.cycles &&
                  !memcmp(fast.ram, literal.ram, fast.ram_size),
              "restored persistent map equals literal CPU and every RAM byte");
        /* The first new DATA access may refill through the literal callback,
         * so its exact raw-native counter is not prescribed here. */
        CHECK(s5l8900_static_a64_compact_tlb_read(&fast) >= reads_before,
              "host evidence survives snapshot restore");
        uint64_t hits = s5l8900_static_a64_compact_ram_map_fetch(&fast);
        CHECK(s5l8900_static_a64_set_compact_ram_map(&fast, false) &&
                  s5l8900_run(&fast, 510u, &fast_status) == 510u &&
                  s5l8900_static_a64_compact_ram_map_fetch(&fast) == hits,
              "same-machine OFF really stops new map hits");
        CHECK(s5l8900_static_a64_set_compact_raw(&fast, false) &&
                  !s5l8900_static_a64_set_compact_ram_map(&fast, true) &&
                  s5l8900_static_a64_set_compact_raw(&fast, true) &&
                  s5l8900_static_a64_set_compact_ram_map(&fast, true),
              "engine switching requires explicit map admission");
        arm_bus_t saved_bus = fast.bus;
        fast.bus.host_ram = NULL;
        CHECK(!s5l8900_static_a64_set_compact_ram_map(&fast, true),
              "revoked full-RAM grant fails closed");
        fast.bus = saved_bus;
        CHECK(s5l8900_run(&fast, 510u, &fast_status) == 510u &&
                  s5l8900_static_a64_compact_ram_map_fetch(&fast) == hits,
              "failed map admission cannot leave old experiment active");
    }
    s5l8900_free(&fast);
    s5l8900_free(&literal);
}
#endif

int main(void) {
    test_capability();
    test_exact_lookup();
    test_persistent_map();
#if defined(S5LBOX_STATIC_A64_ENGINE)
    test_native();
    test_native_persistent_map();
    test_native_refusals();
    test_native_map_revocation();
    test_native_memory_families();
    test_multiply_families();
    test_extra_transfer_forms();
    test_arith_extra_budget_loop();
    test_native_live_code();
    test_native_machine();
    test_native_map_machine();
#else
    (void)saved_ram; (void)expected_ram; (void)reference; (void)insn; (void)half;
#endif
    printf("RAM-window/TLB: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
