/* Checked physical-bus failures must stop before fabricated guest effects.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm.h"
#include "vfp.h"
#include <stdio.h>
#include <string.h>
#ifdef S5LBOX_STATIC_A64_ENGINE
#include "a64_static.h"
#endif

typedef struct {
    uint8_t ram[0x10000];
    uint32_t fail_address;
    unsigned fail_size, fail_nth, matches, accesses, after_failure;
    unsigned successful_byte_reads;
    uint32_t last_byte_address;
    bool fail_write, failed;
} fixture_t;
static unsigned passed, failed;
#define CHECK(c, msg) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: %s\n", __func__, __LINE__, msg); } } while (0)

static bool reject(fixture_t *f, uint32_t address, unsigned size, bool write) {
    f->accesses++;
    if (f->failed) { f->after_failure++; return true; }
    if ((uint64_t)address + size > sizeof f->ram ||
        (address == f->fail_address && size == f->fail_size && write == f->fail_write && ++f->matches == f->fail_nth)) {
        f->failed = true;
        return true;
    }
    return false;
}
#define ACCESSORS(bits) \
static uint##bits##_t read##bits(void *ctx, uint32_t address) { \
    fixture_t *f = ctx; uint##bits##_t value = 0u; \
    if (!reject(f, address, (bits)/8u, false)) { \
        memcpy(&value, f->ram + address, (bits)/8u); \
        if ((bits)==8u) { f->successful_byte_reads++; f->last_byte_address=address; } \
    } \
    return value; \
} \
static void write##bits(void *ctx, uint32_t address, uint##bits##_t value) { \
    fixture_t *f = ctx; \
    if (!reject(f, address, (bits)/8u, true)) memcpy(f->ram + address, &value, (bits)/8u); \
}
ACCESSORS(8)
ACCESSORS(16)
ACCESSORS(32)
static bool access_failed(void *ctx) { return ((fixture_t *)ctx)->failed; }
static uint8_t *host_ram(void *ctx, uint32_t address, uint32_t size) {
    fixture_t *f = ctx;
    if ((uint64_t)address + size > sizeof f->ram ||
        (f->fail_size && f->fail_address >= address && (uint64_t)f->fail_address < (uint64_t)address + size)) return NULL;
    return f->ram + address;
}
static void put16(fixture_t *f, uint32_t address, uint16_t value) { memcpy(f->ram + address, &value, 2u); }
static void put32(fixture_t *f, uint32_t address, uint32_t value) { memcpy(f->ram + address, &value, 4u); }
static uint32_t get32(const fixture_t *f, uint32_t address) { uint32_t value; memcpy(&value, f->ram + address, 4u); return value; }

static void setup(fixture_t *f, arm_bus_t *bus, arm_cpu_t *c, bool thumb, bool host) {
    memset(f, 0, sizeof *f);
    f->fail_address = UINT32_MAX; f->fail_nth = 1u;
    *bus = (arm_bus_t){.ctx=f,.read8=read8,.read16=read16,.read32=read32,
                      .write8=write8,.write16=write16,.write32=write32,.access_failed=access_failed};
    if (host) { bus->host_ram = host_ram; bus->host_ram_write = host_ram; }
    CHECK(arm_reset_profile(c, bus, ARM_ARCH_V7_CORTEX_A8), "reset");
    c->cpsr = ARM_MODE_SYS | ARM_CPSR_N | ARM_CPSR_C | (thumb ? ARM_CPSR_T : 0u);
    c->r[1] = 0x1000u; c->r[2] = 0x87654321u; c->r[3] = 0x12345678u;
    c->cp15.dfsr = 0x123u; c->cp15.ifsr = 0x456u;
    c->cp15.dfar = 0x789u; c->cp15.ifar = 0xabcu;
}
static void check_stop(fixture_t *f, arm_cpu_t *c, uint32_t pc, uint32_t flags) {
    CHECK(c->cycles == 0u, "failed host access counted as a retired instruction");
    CHECK(f->failed && f->after_failure == 0u, "callbacks continued after the first bus failure");
    CHECK(c->r[15] == pc && c->cpsr == flags && !c->abort_pending, "bus failure retired or entered a guest exception");
    CHECK(c->cp15.dfsr == 0x123u && c->cp15.ifsr == 0x456u &&
          c->cp15.dfar == 0x789u && c->cp15.ifar == 0xabcu, "host bus failure was published as a guest fault");
    unsigned accesses = f->accesses;
    uint64_t cycles = c->cycles;
    c->irq_line = true; /* A latched host failure must stop before IRQ entry. */
    CHECK(arm_step(c) == ARM_HALT && f->accesses == accesses && c->r[15] == pc && c->cycles == cycles,
          "latched failure did not stop a second step before fetch or IRQ");
    c->irq_line = false;
}

static void test_neon_memory_and_retry(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned host = 0; host < 2u; host++)
      for (unsigned load = 0; load < 2u; load++)
       for (unsigned size = 2u; size <= 3u; size++)
        for (unsigned stop = 0; stop < 8u; stop++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f, &bus, &c, thumb != 0u, host != 0u);
            if (thumb) c.cpsr |= 0x1800u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN; c.vfp_fpscr = 0x0bc00080u;
            uint32_t insn = (thumb ? 0xf941c20du : 0xf441c20du) | (load << 21) | (size << 6);
            if (thumb) { put16(&f, 0u, (uint16_t)(insn >> 16)); put16(&f, 2u, (uint16_t)insn); }
            else put32(&f, 0u, insn);
            for (unsigned word = 0; word < 8u; word++) put32(&f, 0x1000u + word * 4u, 0xabcdef00u + word);
            for (unsigned d = 0; d < 4u; d++) vfp_set_d(&c, 28u + d,
                (UINT64_C(0xdead0001) + 2u * d) << 32 | (UINT64_C(0xdead0000) + 2u * d));
            f.fail_address = 0x1000u + stop * 4u; f.fail_size = 4u; f.fail_write = !load;
            uint32_t flags = c.cpsr;
            CHECK(arm_step(&c) == ARM_HALT, "NEON failed data callback did not halt");
            check_stop(&f, &c, 0u, flags);
            CHECK(c.r[1] == 0x1000u && c.vfp_fpscr == 0x0bc00080u, "NEON failed writeback or FPSCR changed");
            for (unsigned word = 0; word < 8u; word++) {
                uint32_t value = (uint32_t)(vfp_get_d(&c, 28u + word / 2u) >> (32u * (word & 1u)));
                unsigned published = size == 2u ? stop : stop & ~1u;
                CHECK(value == (load && word < published ? 0xabcdef00u + word : 0xdead0000u + word),
                      "NEON load published a failed element or lost completed elements");
                CHECK(get32(&f, 0x1000u + word * 4u) == (!load && word < stop ? 0xdead0000u + word : 0xabcdef00u + word),
                      "NEON store lost completed words or published a failed write");
            }
            f.failed = false; f.fail_size = 0u;
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u && c.r[1] == 0x1020u &&
                  c.cpsr == (flags & ~0x0600fc00u), "NEON retry did not finish exactly once");
            for (unsigned word = 0; word < 8u; word++) {
                uint32_t value = load ? (uint32_t)(vfp_get_d(&c, 28u + word / 2u) >> (32u * (word & 1u))) :
                    get32(&f, 0x1000u + word * 4u);
                CHECK(value == (load ? 0xabcdef00u + word : 0xdead0000u + word), "NEON retry produced wrong data");
            }
        }
}

static void test_neon_pairs_and_retry(void) {
    static const unsigned types[] = {8u,9u,3u};
    static const unsigned slots[3][8] = {{0u,2u,1u,3u},{0u,4u,1u,5u},{0u,4u,1u,5u,2u,6u,3u,7u}};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned host = 0; host < 2u; host++)
      for (unsigned load = 0; load < 2u; load++)
       for (unsigned kind = 0; kind < 3u; kind++)
        for (unsigned stop = 0; stop < (kind == 2u ? 8u : 4u); stop++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f, &bus, &c, thumb != 0u, host != 0u);
            if (thumb) c.cpsr |= 0x1800u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = ARM_FPEXC_EN; c.vfp_fpscr = 0x0bc00080u;
            uint32_t insn = (thumb ? 0xf941c08du : 0xf441c08du) | (load << 21) | (types[kind] << 8);
            if (thumb) { put16(&f, 0u, (uint16_t)(insn >> 16)); put16(&f, 2u, (uint16_t)insn); }
            else put32(&f, 0u, insn);
            uint64_t expected[32];
            for (unsigned d = 0; d < 32u; d++) {
                expected[d] = (UINT64_C(0xdead0001) + 2u * d) << 32 | (UINT64_C(0xdead0000) + 2u * d);
                vfp_set_d(&c, d, expected[d]);
            }
            unsigned words = kind == 2u ? 8u : 4u;
            for (unsigned word = 0; word < 8u; word++) put32(&f, 0x1000u + word * 4u, 0xabcdef00u + word);
            f.fail_address = 0x1000u + stop * 4u; f.fail_size = 4u; f.fail_write = !load;
            uint32_t flags = c.cpsr;
            CHECK(arm_step(&c) == ARM_HALT, "pair failed data callback did not halt");
            check_stop(&f, &c, 0u, flags);
            CHECK(c.r[1] == 0x1000u && c.vfp_fpscr == 0x0bc00080u && c.vfp_fpexc == ARM_FPEXC_EN,
                  "pair failed writeback or FP control changed");
            for (unsigned word = 0; word < 8u; word++) {
                unsigned slot = slots[kind][word];
                if (load && word < stop) {
                    unsigned d = 28u + slot / 2u, shift = 32u * (slot % 2u);
                    expected[d] = (expected[d] & ~(UINT64_C(0xffffffff) << shift)) | (UINT64_C(0xabcdef00) + word) << shift;
                }
                CHECK(get32(&f, 0x1000u + word * 4u) == (!load && word < stop ? 0xdead0038u + slot : 0xabcdef00u + word),
                      "pair store lost the interleaved prefix or wrote after the failed callback");
            }
            for (unsigned d = 0; d < 32u; d++) CHECK(vfp_get_d(&c, d) == expected[d], "pair partial load/gap preservation");
            f.failed = false; f.fail_size = 0u;
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u && c.r[1] == 0x1000u + words * 4u &&
                  c.cpsr == (flags & ~0x0600fc00u) && c.vfp_fpscr == 0x0bc00080u, "pair retry did not finish exactly once");
            for (unsigned word = 0; word < 8u; word++) {
                unsigned slot = slots[kind][word];
                if (load && word < words) {
                    unsigned d = 28u + slot / 2u, shift = 32u * (slot % 2u);
                    expected[d] = (expected[d] & ~(UINT64_C(0xffffffff) << shift)) | (UINT64_C(0xabcdef00) + word) << shift;
                }
                CHECK(get32(&f, 0x1000u + word * 4u) == (!load && word < words ? 0xdead0038u + slot : 0xabcdef00u + word),
                      "pair retry stored wrong words or overwrote the suffix");
            }
            for (unsigned d = 0; d < 32u; d++) CHECK(vfp_get_d(&c, d) == expected[d], "pair retry register order/preservation");
        }
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned host = 0; host < 2u; host++)
      for (unsigned load = 0; load < 2u; load++)
       for (unsigned kind = 0; kind < 3u; kind++)
        for (unsigned enabled = 0; enabled < 2u; enabled++)
         for (unsigned half = 0; half <= thumb; half++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f, &bus, &c, thumb != 0u, host != 0u);
            if (thumb) c.cpsr |= 0x1800u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u; c.vfp_fpscr = 0x0bc00080u;
            uint32_t insn = (thumb ? 0xf941c08du : 0xf441c08du) | (load << 21) | (types[kind] << 8);
            if (thumb) { put16(&f, 0u, (uint16_t)(insn >> 16)); put16(&f, 2u, (uint16_t)insn); }
            else put32(&f, 0u, insn);
            for (unsigned word = 0; word < 8u; word++) put32(&f, 0x1000u + word * 4u, 0xabcdef00u + word);
            uint32_t flags = c.cpsr;
            f.fail_address = half * 2u; f.fail_size = thumb ? 2u : 4u;
            CHECK(arm_step(&c) == ARM_HALT, "pair failed fetch did not halt before FP access checking");
            check_stop(&f, &c, 0u, flags);
            CHECK(c.r[1] == 0x1000u && c.vfp_fpscr == 0x0bc00080u && c.vfp_fpexc == (enabled ? ARM_FPEXC_EN : 0u),
                  "pair failed fetch changed base or FP control");
            for (unsigned d = 0; d < 32u; d++) CHECK(vfp_get_d(&c, d) == 0u, "pair load preceded complete fetch");
            for (unsigned word = 0; word < 8u; word++) CHECK(get32(&f, 0x1000u + word * 4u) == 0xabcdef00u + word,
                "pair store preceded complete fetch");
            f.failed = false; f.fail_size = 0u; c.vfp_fpexc = ARM_FPEXC_EN;
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
                  c.r[1] == 0x1000u + (kind == 2u ? 32u : 16u) && c.cpsr == (flags & ~0x0600fc00u), "pair fetch retry");
         }
}

static void test_vfp_binary_fetch_and_retry(void) {
    static const uint32_t insns[2][6] = {{0xee78fa28u,0xee78fa68u,0xee68fa28u,0xee68fa68u,0xeec8fa28u,0xeef1fac8u},
        {0xee70fba1u,0xee70fbe1u,0xee60fba1u,0xee60fbe1u,0xeec0fba1u,0xeef1fbe0u}};
    static const uint64_t result[2][6] = {{0x40a00000u,0xbf800000u,0x40c00000u,0xc0c00000u,0x3f2aaaaau,0x3fb504f3u},
        {UINT64_C(0x4014000000000000),UINT64_C(0xbff0000000000000),UINT64_C(0x4018000000000000),UINT64_C(0xc018000000000000),UINT64_C(0x3fe5555555555555),UINT64_C(0x3ff6a09e667f3bcc)}};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned dbl = 0; dbl < 2u; dbl++)
      for (unsigned op = 0; op < 6u; op++)
       for (unsigned host = 0; host < 2u; host++)
        for (unsigned second = 0; second < (thumb ? 2u : 1u); second++)
         for (unsigned enabled = 0; enabled < 2u; enabled++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f,&bus,&c,thumb != 0u,host != 0u);
            if (thumb) c.cpsr |= 0x1800u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u; c.vfp_fpscr = 0x4bc00080u;
            uint32_t insn = insns[dbl][op];
            if (thumb) { put16(&f,0u,(uint16_t)(insn>>16)); put16(&f,2u,(uint16_t)insn); }
            else put32(&f,0u,insn);
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c,d,UINT64_C(0xdead1234beef0000)+d);
            if (dbl) { vfp_set_d(&c,16u,UINT64_C(0x4000000000000000)); vfp_set_d(&c,17u,UINT64_C(0x4008000000000000)); }
            else { vfp_set_s(&c,16u,0x40000000u); vfp_set_s(&c,17u,0x40400000u); }
            uint64_t expected[32]; for (unsigned d = 0; d < 32u; d++) expected[d] = vfp_get_d(&c,d);
            uint32_t flags = c.cpsr;
            f.fail_address = second*2u; f.fail_size = thumb ? 2u : 4u;
            CHECK(arm_step(&c) == ARM_HALT,"VFP add executed an incomplete fetch");
            check_stop(&f,&c,0u,flags);
            bool match = true;
            for (unsigned d = 0; d < 32u; d++) match &= vfp_get_d(&c,d) == expected[d];
            CHECK(match && c.vfp_fpscr == 0x4bc00080u && c.vfp_fpexc == (enabled ? ARM_FPEXC_EN : 0u),
                  "VFP add availability/effects preceded full checked fetch");
            f.failed = false; f.fail_size = 0u; c.vfp_fpexc = ARM_FPEXC_EN;
            if (dbl) expected[31] = result[dbl][op];
            else expected[15] = (expected[15] & UINT64_C(0xffffffff)) | (result[dbl][op] << 32);
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
                  c.cpsr == (flags & ~0x0600fc00u) && c.vfp_fpscr == (0x4bc00080u | (op >= 4u ? ARM_FPSCR_IXC : 0u)),
                  "VFP binary checked fetch retry");
            match = true;
            for (unsigned d = 0; d < 32u; d++) match &= vfp_get_d(&c,d) == expected[d];
            CHECK(match,"VFP add retry register result/preservation");
         }
}

static void test_vfp_precision_fetch_and_retry(void) {
    /* Mixed-format destinations: VCVT.F64.F32 d31,s16; VCVT.F32.F64 s31,d16. */
    static const uint32_t insns[] = {0xeef7fac8u,0xeef7fbe0u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned narrow = 0; narrow < 2u; narrow++)
      for (unsigned host = 0; host < 2u; host++)
       for (unsigned second = 0; second < (thumb ? 2u : 1u); second++)
        for (unsigned enabled = 0; enabled < 2u; enabled++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f,&bus,&c,thumb != 0u,host != 0u); if (thumb) c.cpsr |= 0x1800u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u; c.vfp_fpscr = 0x4bc00080u;
            uint32_t insn = insns[narrow];
            if (thumb) { put16(&f,0u,(uint16_t)(insn>>16)); put16(&f,2u,(uint16_t)insn); }
            else put32(&f,0u,insn);
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c,d,UINT64_C(0xdead1234beef0000)+d);
            vfp_set_s(&c,16u,0x7f800001u); vfp_set_d(&c,16u,UINT64_C(0x3ff0000030000000));
            uint64_t expected[32]; for (unsigned d = 0; d < 32u; d++) expected[d] = vfp_get_d(&c,d);
            uint32_t flags = c.cpsr;
            f.fail_address = second*2u; f.fail_size = thumb ? 2u : 4u;
            CHECK(arm_step(&c) == ARM_HALT,"precision executed an incomplete fetch");
            check_stop(&f,&c,0u,flags);
            bool match = true; for (unsigned d = 0; d < 32u; d++) match &= vfp_get_d(&c,d) == expected[d];
            CHECK(match && c.vfp_fpscr == 0x4bc00080u && c.vfp_fpexc == (enabled ? ARM_FPEXC_EN : 0u),
                  "precision availability/effects preceded full checked fetch");
            f.failed = false; f.fail_size = 0u; c.vfp_fpexc = ARM_FPEXC_EN;
            if (narrow) expected[15] = (expected[15] & UINT64_C(0xffffffff)) | UINT64_C(0x3f80000100000000);
            else expected[31] = UINT64_C(0x7ff8000000000000);
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
                  c.cpsr == (flags & ~0x0600fc00u) &&
                  c.vfp_fpscr == (0x4bc00080u | (narrow ? ARM_FPSCR_IXC : ARM_FPSCR_IOC)),
                  "precision checked fetch retry");
            match = true; for (unsigned d = 0; d < 32u; d++) match &= vfp_get_d(&c,d) == expected[d];
            CHECK(match,"precision retry register result/preservation");
        }
}

static void test_vfp_integer_fetch_and_retry(void) {
    /* kind order matches int-to-FP, VCVTR, VCVT; F32/F64 and unsigned/signed. */
    static const uint32_t insns[] = {0xeef8fa48u,0xeef8fb48u,0xeef8fac8u,0xeef8fbc8u,
        0xeefcfa48u,0xeefcfb60u,0xeefdfa48u,0xeefdfb60u,0xeefcfac8u,0xeefcfbe0u,0xeefdfac8u,0xeefdfbe0u};
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned kind = 0; kind < 12u; kind++)
      for (unsigned host = 0; host < 2u; host++)
       for (unsigned second = 0; second < (thumb ? 2u : 1u); second++)
        for (unsigned enabled = 0; enabled < 2u; enabled++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f,&bus,&c,thumb != 0u,host != 0u); if (thumb) c.cpsr |= 0x1800u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u; c.vfp_fpscr = 0x4bc00080u;
            uint32_t insn = insns[kind];
            if (thumb) { put16(&f,0u,(uint16_t)(insn>>16)); put16(&f,2u,(uint16_t)insn); }
            else put32(&f,0u,insn);
            for (unsigned d = 0; d < 32u; d++) vfp_set_d(&c,d,UINT64_C(0xdead1234beef0000)+d);
            vfp_set_s(&c,16u,kind<4u ? 0x01000003u : 0xbfc00000u); vfp_set_d(&c,16u,UINT64_C(0xbff8000000000000));
            uint64_t expected[32]; for (unsigned d = 0; d < 32u; d++) expected[d] = vfp_get_d(&c,d);
            uint32_t flags = c.cpsr;
            f.fail_address = second*2u; f.fail_size = thumb ? 2u : 4u;
            CHECK(arm_step(&c) == ARM_HALT,"integer conversion executed an incomplete fetch");
            check_stop(&f,&c,0u,flags);
            bool match = true; for (unsigned d = 0; d < 32u; d++) match &= vfp_get_d(&c,d) == expected[d];
            CHECK(match && c.vfp_fpscr == 0x4bc00080u && c.vfp_fpexc == (enabled ? ARM_FPEXC_EN : 0u),
                  "integer availability/effects preceded full checked fetch");
            f.failed = false; f.fail_size = 0u; c.vfp_fpexc = ARM_FPEXC_EN;
            uint64_t result = kind<4u ? (kind&1u ? UINT64_C(0x4170000030000000) : UINT64_C(0x4b800001)) :
                kind&2u ? UINT32_MAX : 0u;
            uint32_t exceptions = kind<4u ? (kind&1u ? 0u : ARM_FPSCR_IXC) : kind&2u ? ARM_FPSCR_IXC : ARM_FPSCR_IOC;
            if (kind<4u && (kind&1u)) expected[31] = result;
            else expected[15] = (expected[15] & UINT64_C(0xffffffff)) | (result<<32);
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
                  c.cpsr == (flags & ~0x0600fc00u) && c.vfp_fpscr == (0x4bc00080u | exceptions),
                  "integer checked fetch retry");
            match = true; for (unsigned d = 0; d < 32u; d++) match &= vfp_get_d(&c,d) == expected[d];
            CHECK(match,"integer retry register result/preservation");
        }
}

static void test_neon_by_scalar_fetch_and_retry(void) {
    const uint64_t a=UINT64_C(0x3fc000003fc00000), accumulator=UINT64_C(0x3f8000003f800000);
    static const uint64_t results[]={UINT64_C(0x4080000040800000),UINT64_C(0xc0000000c0000000),UINT64_C(0x4040000040400000)};
    for (unsigned thumb=0;thumb<2u;thumb++)
     for (unsigned kind=0;kind<3u;kind++)
      for (unsigned quad=0;quad<2u;quad++)
       for (unsigned index=0;index<2u;index++)
        for (unsigned host=0;host<2u;host++)
         for (unsigned enabled=0;enabled<2u;enabled++)
          for (unsigned half=0;half<=thumb;half++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f,&bus,&c,thumb!=0u,host!=0u); if (thumb) c.cpsr|=0x1800u;
            c.cp15.cpacr=0x00f00000u; c.vfp_fpexc=enabled ? ARM_FPEXC_EN : 0u; c.vfp_fpscr=0x0bc00080u;
            c.excl_valid=true; c.excl_addr=0x2468u; c.a8_excl_size=8u;
            uint32_t insn=(thumb ? 0xefe0e1cfu : 0xf2e0e1cfu)|(kind<<10)|(quad<<(thumb ? 28u : 24u))|(index<<5);
            if (thumb) { put16(&f,0u,(uint16_t)(insn>>16)); put16(&f,2u,(uint16_t)insn); } else put32(&f,0u,insn);
            uint64_t scalar=index ? UINT64_C(0x400000007f812345) : UINT64_C(0x7f81234540000000);
            vfp_set_d(&c,15u,scalar); vfp_set_d(&c,16u,a); vfp_set_d(&c,17u,a);
            vfp_set_d(&c,30u,accumulator); vfp_set_d(&c,31u,accumulator);
            uint32_t flags=c.cpsr;
            f.fail_address=half*2u; f.fail_size=thumb ? 2u : 4u;
            CHECK(arm_step(&c)==ARM_HALT,"scalar failed fetch did not halt"); check_stop(&f,&c,0u,flags);
            CHECK(vfp_get_d(&c,30u)==accumulator && vfp_get_d(&c,31u)==accumulator &&
                  c.vfp_fpscr==0x0bc00080u && c.vfp_fpexc==(enabled ? ARM_FPEXC_EN : 0u) &&
                  c.excl_valid && c.excl_addr==0x2468u && c.a8_excl_size==8u,"partial scalar fetch changed result/status/monitor");
            f.failed=false; f.fail_size=0u; c.vfp_fpexc=ARM_FPEXC_EN;
            CHECK(arm_step(&c)==ARM_OK && c.r[15]==4u && c.cycles==1u && c.cpsr==(flags&~0x0600fc00u) &&
                  c.vfp_fpscr==0x0bc00080u && c.excl_valid && c.a8_excl_size==8u &&
                  vfp_get_d(&c,30u)==results[kind] && vfp_get_d(&c,31u)==(quad ? results[kind] : accumulator),"scalar owner retry did not execute exactly once");
            CHECK(vfp_get_d(&c,15u)==scalar && vfp_get_d(&c,16u)==a && vfp_get_d(&c,17u)==a,"scalar retry changed a source lane");
          }
}

static void test_neon_macc_fetch_and_retry(void) {
    const uint64_t a = UINT64_C(0x3fc000003fc00000), b = UINT64_C(0x4000000040000000), accumulator = UINT64_C(0x3f8000003f800000);
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned subtract = 0; subtract < 2u; subtract++)
      for (unsigned quad = 0; quad < 2u; quad++)
       for (unsigned host = 0; host < 2u; host++)
        for (unsigned enabled = 0; enabled < 2u; enabled++)
         for (unsigned half = 0; half <= thumb; half++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f, &bus, &c, thumb != 0u, host != 0u); if (thumb) c.cpsr |= 0x1800u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u; c.vfp_fpscr = 0x0bc00080u;
            c.excl_valid = true; c.excl_addr = 0x2468u; c.a8_excl_size = 8u;
            uint32_t insn = (thumb ? 0xef40edb2u : 0xf240edb2u) | (subtract << 21) | (quad << 6);
            if (thumb) { put16(&f, 0u, (uint16_t)(insn >> 16)); put16(&f, 2u, (uint16_t)insn); } else put32(&f, 0u, insn);
            vfp_set_d(&c, 16u, a); vfp_set_d(&c, 17u, a); vfp_set_d(&c, 18u, b); vfp_set_d(&c, 19u, b);
            vfp_set_d(&c, 30u, accumulator); vfp_set_d(&c, 31u, accumulator);
            uint32_t flags = c.cpsr;
            f.fail_address = half * 2u; f.fail_size = thumb ? 2u : 4u;
            CHECK(arm_step(&c) == ARM_HALT, "macc failed fetch did not halt"); check_stop(&f, &c, 0u, flags);
            CHECK(vfp_get_d(&c, 30u) == accumulator && vfp_get_d(&c, 31u) == accumulator &&
                  c.vfp_fpscr == 0x0bc00080u && c.vfp_fpexc == (enabled ? ARM_FPEXC_EN : 0u) &&
                  c.excl_valid && c.excl_addr == 0x2468u && c.a8_excl_size == 8u, "failed macc fetch altered accumulator/status/monitor");
            f.failed = false; f.fail_size = 0u; c.vfp_fpexc = ARM_FPEXC_EN;
            uint64_t expected = subtract ? UINT64_C(0xc0000000c0000000) : UINT64_C(0x4080000040800000);
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u && c.cpsr == (flags & ~0x0600fc00u) &&
                  c.vfp_fpscr == 0x0bc00080u && c.excl_valid && c.a8_excl_size == 8u &&
                  vfp_get_d(&c, 30u) == expected && vfp_get_d(&c, 31u) == (quad ? expected : accumulator), "macc retry did not accumulate exactly once");
            CHECK(vfp_get_d(&c, 16u) == a && vfp_get_d(&c, 17u) == a && vfp_get_d(&c, 18u) == b && vfp_get_d(&c, 19u) == b,
                  "macc fetch/retry changed a source");
         }
}

static void test_neon_transpose_fetch_and_retry(void) {
    const uint64_t a = UINT64_C(0x0123456789abcdef), b = UINT64_C(0xffeeddccbbaa9988);
    static const uint64_t expected[3][2] = {
        {UINT64_C(0xee23cc67aaab88ef),UINT64_C(0xff01dd45bb8999cd)},
        {UINT64_C(0xddcc45679988cdef),UINT64_C(0xffee0123bbaa89ab)},
        {UINT64_C(0xbbaa998889abcdef),UINT64_C(0xffeeddcc01234567)}
    };
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned size = 0; size < 3u; size++)
      for (unsigned quad = 0; quad < 2u; quad++)
       for (unsigned host = 0; host < 2u; host++)
        for (unsigned enabled = 0; enabled < 2u; enabled++)
         for (unsigned half = 0; half <= thumb; half++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f, &bus, &c, thumb != 0u, host != 0u);
            if (thumb) c.cpsr |= 0x1800u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u; c.vfp_fpscr = 0x0bc00080u;
            c.excl_valid = true; c.excl_addr = 0x2468u; c.a8_excl_size = 8u;
            uint32_t insn = (thumb ? 0xfff2e0a0u : 0xf3f2e0a0u) | (size << 18) | (quad << 6);
            if (thumb) { put16(&f, 0u, (uint16_t)(insn >> 16)); put16(&f, 2u, (uint16_t)insn); }
            else put32(&f, 0u, insn);
            vfp_set_d(&c, 30u, a); vfp_set_d(&c, 31u, a); vfp_set_d(&c, 16u, b); vfp_set_d(&c, 17u, b);
            uint32_t flags = c.cpsr;
            f.fail_address = half * 2u; f.fail_size = thumb ? 2u : 4u;
            CHECK(arm_step(&c) == ARM_HALT, "transpose failed fetch did not halt");
            check_stop(&f, &c, 0u, flags);
            CHECK(vfp_get_d(&c, 30u) == a && vfp_get_d(&c, 31u) == a && vfp_get_d(&c, 16u) == b && vfp_get_d(&c, 17u) == b &&
                  c.vfp_fpscr == 0x0bc00080u && c.vfp_fpexc == (enabled ? ARM_FPEXC_EN : 0u) &&
                  c.excl_valid && c.excl_addr == 0x2468u && c.a8_excl_size == 8u, "failed transpose fetch altered operands/control/monitor");
            f.failed = false; f.fail_size = 0u; c.vfp_fpexc = ARM_FPEXC_EN;
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u && c.cpsr == (flags & ~0x0600fc00u) &&
                  c.vfp_fpscr == 0x0bc00080u && c.excl_valid && c.a8_excl_size == 8u, "transpose retry retirement/status");
            CHECK(vfp_get_d(&c, 30u) == expected[size][0] && vfp_get_d(&c, 16u) == expected[size][1] &&
                  vfp_get_d(&c, 31u) == (quad ? expected[size][0] : a) && vfp_get_d(&c, 17u) == (quad ? expected[size][1] : b),
                  "transpose retry result/operand order/upper halves");
         }
}

static void test_neon_sign_fetch_and_retry(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++)
     for (unsigned negate = 0; negate < 2u; negate++)
      for (unsigned quad = 0; quad < 2u; quad++)
       for (unsigned host = 0; host < 2u; host++)
        for (unsigned enabled = 0; enabled < 2u; enabled++)
         for (unsigned half = 0; half <= thumb; half++) {
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f, &bus, &c, thumb != 0u, host != 0u);
            if (thumb) c.cpsr |= 0x1800u;
            c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = enabled ? ARM_FPEXC_EN : 0u; c.vfp_fpscr = 0x0bc00080u;
            c.excl_valid = true; c.excl_addr = 0x2468u; c.a8_excl_size = 8u;
            uint32_t insn = (thumb ? 0xfff9e720u : 0xf3f9e720u) | (negate << 7) | (quad << 6);
            if (thumb) { put16(&f, 0u, (uint16_t)(insn >> 16)); put16(&f, 2u, (uint16_t)insn); }
            else put32(&f, 0u, insn);
            vfp_set_d(&c, 16u, UINT64_C(0xff81234580000001)); vfp_set_d(&c, 17u, UINT64_C(0x7fcabcde807fffff));
            vfp_set_d(&c, 30u, UINT64_C(0x0123456789abcdef)); vfp_set_d(&c, 31u, UINT64_C(0xfedcba9876543210));
            uint32_t flags = c.cpsr;
            f.fail_address = half * 2u; f.fail_size = thumb ? 2u : 4u;
            CHECK(arm_step(&c) == ARM_HALT, "NEON sign failed fetch did not halt");
            check_stop(&f, &c, 0u, flags);
            CHECK(vfp_get_d(&c, 30u) == UINT64_C(0x0123456789abcdef) && vfp_get_d(&c, 31u) == UINT64_C(0xfedcba9876543210) &&
                  c.vfp_fpscr == 0x0bc00080u && c.vfp_fpexc == (enabled ? ARM_FPEXC_EN : 0u) &&
                  c.excl_valid && c.excl_addr == 0x2468u && c.a8_excl_size == 8u, "failed sign fetch altered FP/monitor state");
            f.failed = false; f.fail_size = 0u; c.vfp_fpexc = ARM_FPEXC_EN;
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u && c.cpsr == (flags & ~0x0600fc00u) &&
                  c.vfp_fpscr == 0x0bc00080u && c.excl_valid && c.a8_excl_size == 8u, "NEON sign retry retirement/status");
            CHECK(vfp_get_d(&c, 30u) == UINT64_C(0x7f81234500000001) &&
                  vfp_get_d(&c, 31u) == (quad ? (negate ? UINT64_C(0xffcabcde007fffff) : UINT64_C(0x7fcabcde007fffff)) :
                    UINT64_C(0xfedcba9876543210)) &&
                  vfp_get_d(&c, 16u) == UINT64_C(0xff81234580000001) && vfp_get_d(&c, 17u) == UINT64_C(0x7fcabcde807fffff),
                  "NEON sign retry result or source preservation");
         }
}

static void test_data_and_retry(void) {
    static const struct { uint32_t load, store; unsigned size; bool thumb, wide, writeback; } cases[] = {
        {0xe4912004u,0xe4812004u,4u,false,false,true},
        {0xe4d12001u,0xe4c12001u,1u,false,false,true},
        {0xe0d120b2u,0xe0c120b2u,2u,false,false,true},
        {0x680au,0x600au,4u,true,false,false},
        {0x780au,0x700au,1u,true,false,false},
        {0x880au,0x800au,2u,true,false,false},
        {0xf8512b04u,0xf8412b04u,4u,true,true,true},
        {0xf8112b01u,0xf8012b01u,1u,true,true,true},
        {0xf8312b02u,0xf8212b02u,2u,true,true,true}
    };
    for (unsigned n = 0; n < sizeof cases/sizeof cases[0]; n++)
     for (unsigned write = 0; write < 2u; write++)
      for (unsigned host = 0; host < 2u; host++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f, &bus, &c, cases[n].thumb, host != 0u);
        if (cases[n].thumb && host) c.cpsr |= 0x1800u; /* Last IT NE slot. */
        f.fail_address = 0x1000u; f.fail_size = cases[n].size; f.fail_write = write != 0u;
        uint32_t insn = write ? cases[n].store : cases[n].load, flags = c.cpsr;
        if (cases[n].wide) { put16(&f, 0u, (uint16_t)(insn >> 16)); put16(&f, 2u, (uint16_t)insn); }
        else if (cases[n].thumb) put16(&f, 0u, (uint16_t)insn);
        else put32(&f, 0u, insn);
        put32(&f, 0x1000u, 0x44332211u);
        CHECK(arm_step(&c) == ARM_HALT, "failed data callback did not halt");
        CHECK(c.r[1] == 0x1000u && c.r[2] == 0x87654321u && get32(&f,0x1000u) == 0x44332211u,
              "failed transfer committed data, destination or writeback");
        check_stop(&f, &c, 0u, flags);
        f.failed = false; f.fail_size = 0u;
        CHECK(arm_step(&c) == ARM_OK && c.r[15] == (cases[n].thumb && !cases[n].wide ? 2u : 4u), "cleared failure did not retry the same instruction");
        uint32_t mask = cases[n].size == 4u ? UINT32_MAX : (1u << (cases[n].size * 8u)) - 1u;
        CHECK(c.r[1] == 0x1000u + (cases[n].writeback ? cases[n].size : 0u) &&
              c.r[2] == (write ? 0x87654321u : 0x44332211u & mask) &&
              get32(&f,0x1000u) == (write ? (0x44332211u & ~mask) | (0x87654321u & mask) : 0x44332211u),
              "failed read or write was cached, or retry produced wrong data");
      }
}

static void test_table_branch_and_retry(void) {
    for (unsigned kind=0;kind<3u;kind++)
     for (unsigned host=0;host<2u;host++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,true,host!=0u);
        c.cpsr |= 0x1800u; /* Last IT NE slot. */
        c.r[0]=0u; c.r[1]=kind==2u ? 0xfffu : 0x1000u;
        put16(&f,0u,0xe8d1u); put16(&f,2u,kind ? 0xf010u : 0xf000u);
        uint32_t entry=kind==2u ? 0x1234u : 7u;
        put16(&f,c.r[1],(uint16_t)entry);
        f.fail_address=0x1000u; f.fail_size=kind==1u ? 2u : 1u;
        uint32_t flags=c.cpsr, before[16]; memcpy(before,c.r,sizeof before);
        if (kind==2u) {
            CHECK(arm_step(&c)==ARM_UNDEFINED && !f.failed && c.cpsr==flags &&
                  memcmp(c.r,before,sizeof before)==0 && !f.successful_byte_reads,
                  "MMU-off Strongly-ordered TBH issued a partial table read");
            continue;
        }
        CHECK(arm_step(&c)==ARM_HALT && memcmp(c.r,before,sizeof before)==0,"failed table read changed PC/registers");
        check_stop(&f,&c,0u,flags);
        f.failed=false; f.fail_size=0u;
        CHECK(arm_step(&c)==ARM_OK && c.r[15]==4u+2u*entry && c.cycles==1u &&
              c.cpsr==(flags & ~0x0600fc00u),"table branch retry failed or advanced IT before successful read");
     }
}

static void test_fetch_and_latched_cache(void) {
    for (unsigned kind = 0; kind < 3u; kind++)
     for (unsigned second = 0; second < (kind == 2u ? 2u : 1u); second++)
      for (unsigned host = 0; host < 2u; host++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f, &bus, &c, kind != 0u, host != 0u);
        if (kind == 0u) put32(&f,0u,0xe3a02001u);
        else if (kind == 1u) put16(&f,0u,0x2201u);
        else { put16(&f,0u,0xf240u); put16(&f,2u,0x0201u); }
        f.fail_address = second * 2u; f.fail_size = kind ? 2u : 4u;
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT && c.r[2] == 0x87654321u, "failed instruction fetch executed returned zero bytes");
        check_stop(&f,&c,0u,flags);
        f.failed = false; f.fail_size = 0u;
        CHECK(arm_step(&c) == ARM_OK && c.r[2] == 1u, "instruction fetch failure did not retry");
      }
    fixture_t f; arm_bus_t bus; arm_cpu_t c;
    setup(&f,&bus,&c,false,true);
    put32(&f,0u,0xe1a00000u); put32(&f,4u,0xe3a02001u);
    CHECK(arm_step(&c) == ARM_OK && c.fetch_host != NULL, "warm fetch cache");
    f.failed = true;
    unsigned accesses = f.accesses;
    CHECK(arm_step(&c) == ARM_HALT && c.r[15] == 4u && c.r[2] == 0x87654321u && f.accesses == accesses,
          "populated fetch cache bypassed a latched failure");
}

static void map_pages(fixture_t *f, arm_cpu_t *c) {
    c->cp15.sctlr = ARM_SCTLR_M | ARM_SCTLR_XP; c->cp15.ttbr0 = 0x4000u; c->cp15.dacr = 1u;
    put32(f,0x4000u,0x6001u); put32(f,0x6000u,0x803eu);
    put32(f,0x6004u,0xa03eu); put32(f,0x6008u,0xc03eu);
}
static void test_unaligned_table_branch_and_retry(void) {
    for (unsigned host=0;host<2u;host++)
     for (unsigned phase=0;phase<3u;phase++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,true,host!=0u); map_pages(&f,&c);
        c.cpsr|=0x1800u; c.r[0]=0u; c.r[1]=0xfffu;
        put16(&f,0x8000u,0xe8d1u); put16(&f,0x8002u,0xf010u);
        f.ram[0x8fffu]=0x23u; f.ram[0xa000u]=0x81u;
        f.fail_address=phase==0u ? 0x8fffu : phase==1u ? 0xa000u : 0x6004u;
        f.fail_size=phase==2u ? 4u : 1u;
        uint32_t flags=c.cpsr, before[16]; memcpy(before,c.r,sizeof before);
        CHECK(arm_step(&c)==ARM_HALT && memcmp(before,c.r,sizeof before)==0,
              "unaligned table failure committed registers/PC");
        CHECK(f.successful_byte_reads==(phase ? 1u : 0u) && (!phase || f.last_byte_address==0x8fffu),
              "unaligned table failure lost or extended the completed byte prefix");
        check_stop(&f,&c,0u,flags);
        f.failed=false; f.fail_size=0u;
        CHECK(arm_step(&c)==ARM_OK && c.r[15]==0x1024au && c.cycles==1u &&
              c.cpsr==(flags & ~0x0600fc00u) && f.successful_byte_reads==(phase ? 3u : 2u) &&
              f.last_byte_address==0xa000u,"unaligned table retry reused failed data or skipped a byte");
     }
}
static void test_walk_failures(void) {
    for (unsigned data = 0; data < 2u; data++)
     for (unsigned level = 0; level < 2u; level++)
      for (unsigned host = 0; host < 2u; host++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,false,host != 0u); map_pages(&f,&c);
        put32(&f,0x8000u,0xe5912000u); put32(&f,0xa000u,0x44332211u);
        f.fail_address = level ? (data ? 0x6004u : 0x6000u) : 0x4000u;
        f.fail_size = 4u; f.fail_nth = data && !level ? 2u : 1u;
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT && c.r[2] == 0x87654321u, "failed table walk became a fabricated translation abort");
        check_stop(&f,&c,0u,flags);
        f.failed = false; f.fail_size = 0u;
        CHECK(arm_step(&c) == ARM_OK && c.r[2] == 0x44332211u && c.r[15] == 4u, "host table-walk failure was cached as a guest fault");
      }
    fixture_t f; arm_bus_t bus; arm_cpu_t c;
    setup(&f,&bus,&c,false,false); map_pages(&f,&c);
    f.fail_address = 0x6004u; f.fail_size = 4u;
    uint32_t pa = 0xdeadbeefu;
    CHECK(arm_mmu_translate(&c,0x1000u,ARM_ACCESS_READ,true,&pa) == ARM_MMU_BUS_FAILURE && pa == 0xdeadbeefu,
          "direct MMU caller lost host failure distinction or output preservation");
    f.failed = false; f.fail_size = 0u;
    CHECK(arm_mmu_translate(&c,0x1000u,ARM_ACCESS_READ,true,&pa) == 0u && pa == 0xa000u, "MMU cached a failed physical read");
}

static void test_small_register_load_and_retry(void) {
    const uint16_t op[]={0xf810u,0xf830u,0xf910u,0xf930u};
    for (unsigned kind=0;kind<4u;kind++)
     for (unsigned host=0;host<2u;host++)
      for (unsigned phase=0;phase<((kind&1u) ? 3u : 1u);phase++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,true,host!=0u); map_pages(&f,&c);
        c.cpsr|=0x1800u; c.r[0]=0u; c.r[1]=phase ? 0xfffu : 0x1000u;
        c.excl_valid=true; c.excl_addr=0x2340u;
        put16(&f,0x8000u,(uint16_t)(op[kind]|1u)); put16(&f,0x8002u,0x2000u);
        uint32_t pa=phase ? 0x8fffu : 0xa000u;
        f.ram[pa]=0x83u; f.ram[phase ? 0xa000u : pa+1u]=0x81u;
        f.fail_address=phase==2u ? 0xa000u : pa;
        f.fail_size=phase || !(kind&1u) ? 1u : 2u;
        uint32_t flags=c.cpsr, before[16]; memcpy(before,c.r,sizeof before);
        CHECK(arm_step(&c)==ARM_HALT && memcmp(before,c.r,sizeof before)==0 && c.excl_valid && c.excl_addr==0x2340u,
              "failed small register load changed registers or exclusive state");
        CHECK(f.successful_byte_reads==(phase==2u ? 1u : 0u),"failed small register load lost its completed byte prefix");
        check_stop(&f,&c,0u,flags);
        f.failed=false; f.fail_size=0u; f.ram[pa]=0x84u;
        uint32_t expected=(kind&1u) ? 0x8184u : 0x84u;
        if (kind&2u) expected|=(kind&1u) ? 0xffff0000u : 0xffffff00u;
        CHECK(arm_step(&c)==ARM_OK && c.r[15]==4u && c.r[2]==expected && c.cycles==1u &&
              c.cpsr==(flags & ~0x0600fc00u) && c.excl_valid,
              "small register retry lost extension, used stale partial data or advanced IT early");
      }
}

static void test_unprivileged_transfer_and_retry(void) {
    static const struct { uint16_t first; unsigned size; bool load, sign; } cases[]={
        {0xf810u,1u,true,false},{0xf830u,2u,true,false},{0xf910u,1u,true,true},{0xf930u,2u,true,true},
        {0xf850u,4u,true,false},{0xf800u,1u,false,false},{0xf820u,2u,false,false},{0xf840u,4u,false,false}
    };
    for (unsigned kind=0;kind<8u;kind++)
     for (unsigned host=0;host<2u;host++)
      for (unsigned phase=0;phase<(cases[kind].size>1u ? cases[kind].size+2u : 1u);phase++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,true,host!=0u); map_pages(&f,&c);
        unsigned size=cases[kind].size, prefix=phase ? phase>size ? size-1u : phase-1u : 0u;
        bool load=cases[kind].load, walk=phase==size+1u;
        c.cpsr|=0x1800u; c.r[1]=phase ? 0x2000u-(size-1u) : 0x1000u;
        c.excl_valid=true; c.excl_addr=0x2340u;
        put16(&f,0x8000u,(uint16_t)(cases[kind].first|1u)); put16(&f,0x8002u,0x2e00u);
        uint32_t physical[4];
        for (unsigned i=0;i<size;i++) {
            physical[i]=phase ? i==size-1u ? 0xc000u : 0xb000u-(size-1u)+i : 0xa000u+i;
            f.ram[physical[i]]=(uint8_t)(0x80u+i);
        }
        f.fail_address=walk ? 0x6008u : physical[prefix];
        f.fail_size=walk ? 4u : phase ? 1u : size; f.fail_write=!load && !walk;
        uint32_t flags=c.cpsr, before[16]; memcpy(before,c.r,sizeof before);
        CHECK(arm_step(&c)==ARM_HALT && memcmp(before,c.r,sizeof before)==0 && c.excl_valid && c.excl_addr==0x2340u,
              "failed unprivileged transfer changed registers or exclusive state");
        CHECK(f.successful_byte_reads==(load ? prefix : 0u),"failed unprivileged load lost or extended its completed prefix");
        for (unsigned i=0;i<size;i++) CHECK(f.ram[physical[i]]==(!load && i<prefix ? (uint8_t)(0x87654321u>>(i*8u)) : 0x80u+i),
              "failed unprivileged store lost or extended its completed prefix");
        check_stop(&f,&c,0u,flags);
        f.failed=false; f.fail_size=0u;
        if (load) f.ram[physical[0]]=0x84u; else c.r[2]=0x44332215u;
        uint32_t mask=size==4u ? UINT32_MAX : (1u<<(size*8u))-1u, expected=0x83828184u&mask;
        if (cases[kind].sign) expected|=~mask;
        CHECK(arm_step(&c)==ARM_OK && c.r[15]==4u && c.r[1]==before[1] &&
              c.r[2]==(load ? expected : 0x44332215u) && c.cycles==1u && c.cpsr==(flags & ~0x0600fc00u) && c.excl_valid,
              "unprivileged retry used stale partial data or changed base/IT before success");
        if (!load) for (unsigned i=0;i<size;i++) CHECK(f.ram[physical[i]]==(uint8_t)(0x44332215u>>(i*8u)),
              "unprivileged retry did not rewrite its completed prefix and finish the store");
      }
}

static void test_partial_transfers_and_vfp(void) {
    for (unsigned write = 0; write < 2u; write++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,false,false);
        put32(&f,0u,write ? 0xe8a1000cu : 0xe8b1000cu);
        put32(&f,0x1000u,0x11111111u); put32(&f,0x1004u,0x22222222u);
        f.fail_address = 0x1004u; f.fail_size = 4u; f.fail_write = write != 0u;
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT && c.r[1] == 0x1000u && c.r[3] == 0x12345678u &&
              c.r[2] == 0x87654321u && /* LDM stages the entire register list. */
              get32(&f,0x1000u) == (write ? 0x87654321u : 0x11111111u) && get32(&f,0x1004u) == 0x22222222u,
              "multiple transfer lost completed prefix or committed failed destination/writeback");
        check_stop(&f,&c,0u,flags);

        setup(&f,&bus,&c,false,false); map_pages(&f,&c);
        c.r[1] = 0x1fffu;
        put32(&f,0x8000u,write ? 0xe4812004u : 0xe4912004u);
        f.ram[0xafffu] = 0x11u; put32(&f,0xc000u,0xeeeeeeeeu);
        f.fail_address = 0xc000u; f.fail_size = 1u; f.fail_write = write != 0u;
        flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT && c.r[1] == 0x1fffu && c.r[2] == 0x87654321u &&
              f.ram[0xafffu] == (write ? 0x21u : 0x11u) && get32(&f,0xc000u) == 0xeeeeeeeeu,
              "cross-page bus failure read beyond the failed byte or committed a partial load");
        check_stop(&f,&c,0u,flags);

        setup(&f,&bus,&c,false,false);
        c.cp15.cpacr = 0x00f00000u; c.vfp_fpexc = 0x40000000u; c.vfp_s[2] = 0x76543210u;
        put32(&f,0u,write ? 0xed811a00u : 0xed911a00u); put32(&f,0x1000u,0x44332211u);
        f.fail_address = 0x1000u; f.fail_size = 4u; f.fail_write = write != 0u;
        flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT && c.vfp_s[2] == 0x76543210u && get32(&f,0x1000u) == 0x44332211u,
              "VFP transfer used failed bus data or committed a failed store");
        check_stop(&f,&c,0u,flags);
    }
}

static arm_svc_result_t failed_svc(void *ctx, arm_cpu_t *c, uint32_t pc, uint32_t encoding) {
    (void)pc; (void)encoding;
    c->r[2] = read32(ctx, 0x1000u);
    c->r[15] = 0x200u; c->cpsr = ARM_MODE_USR; c->cycles += 20u;
    return ARM_SVC_HANDLED; /* A host failure overrides even an erroneous success. */
}
static bool failed_wait(void *ctx) { (void)read32(ctx, 0x1000u); return true; }

static void test_exception_and_host_hook_paths(void) {
    for (unsigned thumb = 0; thumb < 2u; thumb++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,thumb != 0u,false);
        if (thumb) put16(&f,0u,0xdf00u); else put32(&f,0u,0xef000000u);
        bus.privileged_svc_handler = failed_svc; bus.privileged_svc_ctx = &f;
        f.fail_address = 0x1000u; f.fail_size = 4u;
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT && c.r[2] == 0x87654321u && c.cycles == 0u,
              "SVC callback committed failed host I/O or retired");
        check_stop(&f,&c,0u,flags);
        setup(&f,&bus,&c,thumb != 0u,false);
        if (thumb) put16(&f,0u,0xbf30u); else put32(&f,0u,0xe320f003u);
        bus.wait_for_interrupt = failed_wait;
        f.fail_address = 0x1000u; f.fail_size = 4u; flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT, "WFI retired after a failed platform wait");
        check_stop(&f,&c,0u,flags);
    }
    for (unsigned write = 0; write < 2u; write++)
     for (unsigned second = 0; second < 2u; second++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,false,false);
        c.cpsr = ARM_MODE_SVC | ARM_CPSR_N | ARM_CPSR_C;
        c.r[13] = 0x1000u; c.r[14] = 0x22222222u; c.spsr[ARM_BANK_SVC] = ARM_MODE_SYS;
        /* SRSIA sp!,#SVC / RFEIA r1! exercise the direct exception-return paths. */
        put32(&f,0u,write ? 0xf8ed0513u : 0xf8b10a00u);
        put32(&f,0x1000u,0x200u); put32(&f,0x1004u,ARM_MODE_SYS);
        f.fail_address = 0x1000u + 4u * second; f.fail_size = 4u; f.fail_write = write != 0u;
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT && c.r[1] == 0x1000u && c.r[13] == 0x1000u &&
              get32(&f,0x1000u) == (write && second ? 0x22222222u : 0x200u) &&
              get32(&f,0x1004u) == ARM_MODE_SYS, "SRS/RFE committed failed data or writeback");
        check_stop(&f,&c,0u,flags);
    }
}

static void test_thumb_byte_reverse_fetch_retry(void) {
    static const unsigned ops[]={0x80u,0x90u,0xb0u};
    static const uint32_t results[]={0x80563412u,0x34128056u,0xffff8056u};
    for (unsigned kind=0;kind<3u;kind++)
     for (unsigned host=0;host<2u;host++)
      for (unsigned half=0;half<2u;half++)
       for (unsigned skip=0;skip<2u;skip++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,true,host!=0u);
        c.cpsr|=skip ? 0x0c00u : 0x1c00u;
        c.r[15]=0x3feu; c.r[2]=0x12345680u; c.r[8]=0xdeadbeefu;
        c.excl_valid=true; c.excl_addr=0x1000u; c.a8_excl_size=8u;
        put16(&f,0x3feu,0xfa92u); put16(&f,0x400u,(uint16_t)(0xf802u|ops[kind]));
        uint32_t flags=c.cpsr, before[16]; memcpy(before,c.r,sizeof before);
        f.fail_address=0x3feu+half*2u; f.fail_size=2u;
        CHECK(arm_step(&c)==ARM_HALT,"wide reversal did not halt on failed fetch");
        check_stop(&f,&c,0x3feu,flags);
        CHECK(!memcmp(before,c.r,sizeof before) && c.excl_valid && c.excl_addr==0x1000u && c.a8_excl_size==8u,
              "wide reversal published effects from a partial fetch");
        f.failed=false; f.fail_size=0u;
        CHECK(arm_step(&c)==ARM_OK && c.r[15]==0x402u && c.cycles==1u && c.r[2]==0x12345680u &&
              c.r[8]==(skip ? 0xdeadbeefu : results[kind]) &&
              c.cpsr==((flags&~0x0600fc00u)|0x1800u) && c.excl_valid &&
              c.excl_addr==0x1000u && c.a8_excl_size==8u,"wide reversal owner retry did not retire/advance IT exactly once");
       }
}

static void test_second_halfword_walk_failure(void) {
    for (unsigned level = 0; level < 2u; level++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,true,true); map_pages(&f,&c);
        c.r[15] = 0xffeu;
        put16(&f,0x8ffeu,0xf240u); put16(&f,0xa000u,0x0201u);
        f.fail_address = level ? 0x6004u : 0x4000u; f.fail_size = 4u; f.fail_nth = level ? 1u : 2u;
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT && c.r[2] == 0x87654321u,
              "second-halfword walk failure decoded a partial Thumb instruction");
        check_stop(&f,&c,0xffeu,flags);
        f.failed = false; f.fail_size = 0u;
        CHECK(arm_step(&c) == ARM_OK && c.r[2] == 1u && c.r[15] == 0x1002u,
              "second-halfword walk failure was cached or changed framing");
    }
}

static void test_exclusive_store_retry(void) {
    static const struct { uint32_t insn; unsigned size; } cases[] = {
        {0xe1813f92u,4u}, {0xe1c13f92u,1u}, {0xe1e13f92u,2u}, {0xe1a14f92u,4u}
    }; /* STREX/STREXB/STREXH r3,r2,[r1]; STREXD r4,r2,r3,[r1]. */
    for (unsigned n = 0; n < sizeof cases/sizeof cases[0]; n++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        setup(&f,&bus,&c,false,false);
        put32(&f,0u,cases[n].insn); put32(&f,0x1000u,0x44332211u);
        c.cp15.sctlr |= ARM_SCTLR_M; c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
        put32(&f,0x4000u,0xc0eu); /* explicit Normal identity mapping */
        c.excl_valid = true; c.excl_addr = 0x1000u; c.r[4] = 0xabcdef01u;
        c.a8_excl_size = (uint8_t)(n == 3u ? 8u : cases[n].size);
        f.fail_address = 0x1000u; f.fail_size = cases[n].size; f.fail_write = true;
        uint32_t flags = c.cpsr;
        CHECK(arm_step(&c) == ARM_HALT && c.excl_valid && c.excl_addr == 0x1000u &&
              c.r[3] == 0x12345678u && c.r[4] == 0xabcdef01u && get32(&f,0x1000u) == 0x44332211u,
              "failed exclusive store consumed its monitor or committed a result");
        check_stop(&f,&c,0u,flags);
        f.failed = false; f.fail_size = 0u;
        CHECK(arm_step(&c) == ARM_OK && !c.excl_valid && c.r[n == 3u ? 4u : 3u] == 0u,
              "retry of a failed exclusive store reported spurious monitor failure");
    }
}

static void test_a8_exclusive_access_retry(void) {
    static const struct { uint32_t arm_load, arm_store, thumb_load, thumb_store; unsigned size; } cases[] = {
        {0xe1d12f9fu,0xe1c14f92u,0xe8d12f4fu,0xe8c12f44u,1u},
        {0xe1f12f9fu,0xe1e14f92u,0xe8d12f5fu,0xe8c12f54u,2u},
        {0xe1912f9fu,0xe1814f92u,0xe8512f00u,0xe8412400u,4u},
        {0xe1b12f9fu,0xe1a14f92u,0xe8d1237fu,0xe8c12374u,8u}
    };
    for (unsigned k = 0; k < 4u; k++)
     for (unsigned thumb = 0; thumb < 2u; thumb++)
      for (unsigned host = 0; host < 2u; host++)
       for (unsigned load = 0; load < 2u; load++)
        for (unsigned stop = 0; stop < 6u; stop++) {
            if ((stop == 1u && cases[k].size != 8u) || (stop == 5u && !thumb)) continue;
            fixture_t f; arm_bus_t bus; arm_cpu_t c;
            setup(&f,&bus,&c,thumb != 0u,host != 0u);
            if (thumb) c.cpsr |= 0x1800u; /* Last IT NE slot. */
            c.cp15.sctlr |= ARM_SCTLR_M; c.cp15.ttbr0 = 0x4000u; c.cp15.dacr = 1u;
            put32(&f,0x4000u,0xc0eu); put32(&f,0x4004u,0x6001u); put32(&f,0x6004u,0x103fu);
            c.r[1] = 0x101000u; c.r[4] = 0xabcdef01u;
            c.excl_valid = true; c.excl_addr = load ? 0x88u : 0x1000u;
            c.a8_excl_size = (uint8_t)(load ? 1u : cases[k].size);
            uint32_t old_tag = c.excl_addr; uint8_t old_size = c.a8_excl_size;
            put32(&f,0x1000u,0x44332211u); put32(&f,0x1004u,0xfedcba98u);
            uint32_t insn = thumb ? (load ? cases[k].thumb_load : cases[k].thumb_store) :
                                   (load ? cases[k].arm_load : cases[k].arm_store);
            if (thumb) { put16(&f,0u,(uint16_t)(insn >> 16)); put16(&f,2u,(uint16_t)insn); }
            else put32(&f,0u,insn);
            f.fail_address = stop < 2u ? 0x1000u + 4u * stop : stop == 2u ? 0x4004u :
                stop == 3u ? 0x6004u : stop == 4u ? 0u : 2u;
            f.fail_size = stop >= 4u ? (thumb ? 2u : 4u) : stop >= 2u || cases[k].size == 8u ? 4u : cases[k].size;
            f.fail_write = stop < 2u && !load;
            uint32_t flags = c.cpsr;
            CHECK(arm_step(&c) == ARM_HALT, "exclusive checked failure did not stop");
            check_stop(&f,&c,0u,flags);
            CHECK(c.r[1] == 0x101000u && c.r[2] == 0x87654321u && c.r[3] == 0x12345678u &&
                  c.r[4] == 0xabcdef01u && c.excl_valid && c.excl_addr == old_tag && c.a8_excl_size == old_size,
                  "exclusive failed fetch/walk/data published a partial register or monitor");
            CHECK(get32(&f,0x1000u) == (!load && stop == 1u ? 0x87654321u : 0x44332211u) &&
                  get32(&f,0x1004u) == 0xfedcba98u, "exclusive host stop lost completed prefix or wrote failed word");
            f.failed = false; f.fail_size = 0u;
            CHECK(arm_step(&c) == ARM_OK && c.r[15] == 4u && c.cycles == 1u &&
                  c.cpsr == (flags & ~0x0600fc00u), "exclusive retry did not retire/advance IT exactly once");
            uint32_t mask = cases[k].size >= 4u ? UINT32_MAX : (1u << (8u * cases[k].size)) - 1u;
            CHECK(c.r[2] == (load ? 0x44332211u & mask : 0x87654321u) &&
                  c.r[3] == (load && cases[k].size == 8u ? 0xfedcba98u : 0x12345678u) &&
                  c.r[4] == (load ? 0xabcdef01u : 0u) && c.excl_valid == (load != 0u) &&
                  (!load || (c.excl_addr == 0x1000u && c.a8_excl_size == cases[k].size)),
                  "exclusive retry data/status/physical tag");
            CHECK(get32(&f,0x1000u) == (load ? 0x44332211u : (0x44332211u & ~mask) | (0x87654321u & mask)) &&
                  get32(&f,0x1004u) == (!load && cases[k].size == 8u ? 0x12345678u : 0xfedcba98u),
                  "exclusive retry wrote wrong bytes");
        }
}

static void test_native_cache_refill_refused(void) {
    fixture_t f; arm_bus_t bus; arm_cpu_t c;
    setup(&f,&bus,&c,false,true); map_pages(&f,&c);
    put32(&f,0x8000u,0xe5912000u);
    CHECK(arm_step(&c) == ARM_OK && c.fetch_host != NULL, "warm checked-bus caches");
    uint64_t hits = c.tlb_hits, misses = c.tlb_misses;
    const uint8_t *fetch = c.fetch_host;
    CHECK(!arm_fetch_cache_try_refill(&c,0u,true) &&
          !arm_data_cache_try_refill(&c,0x1000u,ARM_ACCESS_READ,true) &&
          !arm_data_cache_try_refill(&c,0x1000u,ARM_ACCESS_WRITE,true),
          "native cache refill accepted a checked bus before failure");
    CHECK(c.fetch_host == fetch && c.tlb_hits == hits && c.tlb_misses == misses,
          "refused native refill changed cache state or counters");
}

static void test_counter_wraparound(void) {
    fixture_t f; arm_bus_t bus; arm_cpu_t c;
    setup(&f,&bus,&c,false,false);
    put32(&f,0u,0xe5912000u);
    c.cycles = UINT64_MAX;
    f.fail_address = 0x1000u; f.fail_size = 4u;
    CHECK(arm_step(&c) == ARM_HALT && c.cycles == UINT64_MAX && c.r[15] == 0u,
          "failed access did not reverse a wrapping retirement increment");
    f.failed = false; f.fail_size = 0u;
    CHECK(arm_step(&c) == ARM_OK && c.cycles == 0u && c.r[15] == 4u,
          "successful retry did not retire exactly once across wraparound");
}

#ifdef S5LBOX_STATIC_A64_ENGINE
static void test_signed_runner_entry_guards(void) {
    static const arm_arch_t profiles[] = {
        ARM_ARCH_V6_ARM1176, ARM_ARCH_V6_ARM1176,
        ARM_ARCH_V7_CORTEX_A8, ARM_ARCH_V7_SWIFT, (arm_arch_t)99
    };
    if (!a64_static_host_available())
        printf("SKIP native signed execution: no AArch64 handlers on this host\n");
    for (unsigned p = 0; p < sizeof profiles/sizeof profiles[0]; p++)
     for (unsigned runner = 0; runner < 12u; runner++) {
        fixture_t f; arm_bus_t bus; arm_cpu_t c;
        a64_static_block_t block = {0};
        a64_static_graph_node_t nodes[A64_STATIC_GRAPH_SLOTS] = {{0}};
        setup(&f,&bus,&c,false,true);
        c.arch = profiles[p];
        if (p != 1u) bus.access_failed = NULL;
        c.r[2] = 0u;
        c.fetch_host = f.ram; c.fetch_gen = c.tlb_gen; c.fetch_priv = true;
        put32(&f,0u,0xe2822001u); put32(&f,4u,0xeafffffdu);
        CHECK(a64_static_decode_memory_hits_bytes_at(f.ram,2u,false,0u,&block), "decode signed control block");
        unsigned completed = 0u, blocks = 0u, native = 0u, fallback = 0u;
        uint64_t hits = 0u;
        bool result = false;
        switch (runner) {
        case 0: result = a64_static_run(&c,&block,1u,f.ram,sizeof f.ram); break;
        case 1: result = a64_static_run_read_hits(&c,&block,f.ram,sizeof f.ram,&completed); break;
        case 2: result = a64_static_run_memory_hits(&c,&block,f.ram,sizeof f.ram,&completed); break;
        case 3: result = a64_static_run_read_hits_decoded(&c,&block,f.ram,sizeof f.ram,&completed); break;
        case 4: result = a64_static_run_memory_hits_decoded(&c,&block,f.ram,sizeof f.ram,true,&completed); break;
        case 5: result = a64_static_run_read_hits_chain(&c,&block,f.ram,sizeof f.ram,2u,NULL,NULL,&completed,&blocks); break;
        case 6: result = a64_static_run_memory_hits_chain(&c,&block,f.ram,sizeof f.ram,2u,NULL,NULL,true,&completed,&blocks); break;
        case 7: result = a64_static_run_read_hits_graph(&c,&block,f.ram,sizeof f.ram,2u,nodes,&completed,&blocks); break;
        case 8: result = a64_static_run_memory_hits_graph(&c,&block,f.ram,sizeof f.ram,2u,nodes,true,&completed,&blocks); break;
        case 9: result = a64_compact_raw_run(&c,f.ram,0u,8u,2u,f.ram,sizeof f.ram,&completed); break;
        case 10: result = a64_compact_raw_run_code_window(&c,f.ram,0u,8u,2u,&completed); break;
        case 11: result = a64_compact_raw_run_code_window_resident_cached(&c,f.ram,0u,8u,2u,NULL,NULL,true,&hits,&completed,&native,&fallback); break;
        }
        bool expected = p == 0u && a64_static_host_available();
        CHECK(result == expected, "signed runner profile/bus decision");
        CHECK(c.r[2] == (expected ? 1u : 0u) && c.r[15] == 0u && c.cycles == (expected ? 2u : 0u),
              "signed runner did not preserve refused state or execute the valid control");
        CHECK(f.accesses == 0u, "signed runner guard entered a bus callback");
    }
}
#endif

int main(void) {
    test_vfp_binary_fetch_and_retry();
    test_vfp_precision_fetch_and_retry();
    test_vfp_integer_fetch_and_retry();
    test_neon_by_scalar_fetch_and_retry();
    test_thumb_byte_reverse_fetch_retry();
    test_neon_macc_fetch_and_retry();
    test_neon_transpose_fetch_and_retry();
    test_neon_pairs_and_retry();
    test_neon_sign_fetch_and_retry();
    test_neon_memory_and_retry();
    test_data_and_retry();
    test_table_branch_and_retry();
    test_unaligned_table_branch_and_retry();
    test_fetch_and_latched_cache();
    test_walk_failures();
    test_small_register_load_and_retry();
    test_unprivileged_transfer_and_retry();
    test_partial_transfers_and_vfp();
    test_exception_and_host_hook_paths();
    test_second_halfword_walk_failure();
    test_exclusive_store_retry();
    test_a8_exclusive_access_retry();
    test_native_cache_refill_refused();
    test_counter_wraparound();
#ifdef S5LBOX_STATIC_A64_ENGINE
    test_signed_runner_entry_guards();
#endif
    printf("%u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
