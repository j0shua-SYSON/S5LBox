/* Exact TLB-to-RAM continuation: portable proof and real AArch64 execution.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm.h"
#include <stdio.h>
#include <string.h>
#if defined(S5LBOX_STATIC_A64_ENGINE)
#include "a64_static.h"
#endif

#define BASE UINT32_C(0x10000000)
#define VA UINT32_C(0x70000000)
#define SIZE 65536u
#define CODE (VA + 0x8000u)
#define NEXT (VA + 0x8400u)
#define DATA (VA + 0xc000u)
static uint8_t ram[SIZE], saved_ram[SIZE];
static arm_cpu_t cpu, before, reference;
static arm_bus_t bus;
static unsigned checks, failures, reads, writes, grants;
static bool refuse_range;
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
} fallback_t;
static a64_compact_raw_fallback_result_t fallback(
        void *opaque, a64_compact_raw_code_window_t *next,
        const a64_compact_raw_data_miss_t *miss) {
    fallback_t *f = opaque;
    f->calls++;
    (void)miss;
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
            reads = writes = 0;
            CHECK(a64_compact_raw_run_code_window_resident_options(&cpu,
                      ram + 0x8000u, CODE, 1024u, budget, fallback, &f,
                      &options, NULL, NULL, &stats, &total, &native, &slow),
                  "native execution");
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
    for (unsigned mutate = 1; mutate <= 22u; mutate++) {
        for (unsigned retire = 0; retire < 2u; retire++) {
            setup();
            insn(CODE, 0xe1a0000fu); /* MOV r0, PC: exact interpreter fallback */
            insn(CODE + 4u, 0xe3a00007u);
            arm_ram_window_t w;
            CHECK(arm_ram_window_capture(&w, &cpu, BASE, SIZE), "capture");
            a64_compact_raw_options_t options = { .ram_window = &w };
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
    puts("NATIVE-TLB-REFILL real execution and mutation-boundary checks complete");
}

static void test_native_refusals(void) {
    if (!a64_static_host_available()) return;
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
            a64_compact_raw_options_t options = { .ram_window = &w };
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

static void test_native_memory_families(void) {
    if (!a64_static_host_available()) return;
    static const struct { uint32_t opcode; bool thumb, write; } cases[] = {
        {0xe5910000u, false, false}, {0xe5d10000u, false, false},
        {0xe1d100b0u, false, false}, {0xe1d100d0u, false, false},
        {0xe1d100f0u, false, false}, {0xe5810000u, false, true},
        {0xe5c10000u, false, true}, {0xe1c100b0u, false, true},
        {0xe8910015u, false, false}, {0xe8810015u, false, true},
        {0xe4910004u, false, false}, {0xe4810004u, false, true},
        {0xe4b10004u, false, false}, {0xe4a10004u, false, true},
        {0xed910a00u, false, false}, {0xed810a00u, false, true},
        {0xed910b00u, false, false}, {0xed810b00u, false, true},
        {0x6808u, true, false}, {0x7808u, true, false},
        {0x8808u, true, false}, {0x6008u, true, true},
        {0x7008u, true, true}, {0x8008u, true, true},
        {0xc915u, true, false}, {0xc115u, true, true},
        {0x5688u, true, false}, {0x5e88u, true, false},
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
#endif

int main(void) {
    test_capability();
    test_exact_lookup();
#if defined(S5LBOX_STATIC_A64_ENGINE)
    test_native();
    test_native_refusals();
    test_native_memory_families();
#else
    (void)saved_ram; (void)reference; (void)insn; (void)half;
#endif
    printf("RAM-window/TLB: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
