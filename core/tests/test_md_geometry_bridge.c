/* 64-bit md0 geometry boundary tests. Copyright (c) 2026 j0shua-SYSON. */
#include "md_geometry_bridge.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x)                                                                       \
    do {                                                                               \
        ++checks;                                                                      \
        if (!(x)) {                                                                    \
            ++failures;                                                                \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                \
        }                                                                              \
    } while (0)

static uint8_t ram[65536];
static arm_cpu_t cpu;
static md_bridge_t strategy;
static md_geometry_bridge_t geometry;
static vm_block_t block;
static uint64_t io_offset;
static unsigned io_calls;
static uint8_t stored[4096];
static uint32_t rd32(void *ctx, uint32_t p) {
    (void)ctx;
    if (p > sizeof ram - 4)
        return 0;
    return (uint32_t)ram[p] | (uint32_t)ram[p + 1] << 8 | (uint32_t)ram[p + 2] << 16 |
           (uint32_t)ram[p + 3] << 24;
}
static uint16_t rd16(void *ctx, uint32_t p) { return (uint16_t)rd32(ctx, p); }
static void wr32(uint32_t p, uint32_t x) {
    for (unsigned i = 0; i < 4; ++i)
        ram[p + i] = (uint8_t)(x >> (i * 8));
}
static vm_block_io_status_t read_at(void *ctx, uint64_t off, void *dst, size_t n,
                                    size_t *actual) {
    (void)ctx;
    io_offset = off;
    ++io_calls;
    memcpy(dst, stored, n);
    *actual = n;
    return VM_BLOCK_IO_OK;
}
static vm_block_io_status_t write_at(void *ctx, uint64_t off, const void *src, size_t n,
                                     size_t *actual) {
    (void)ctx;
    io_offset = off;
    ++io_calls;
    memcpy(stored, src, n);
    *actual = n;
    return VM_BLOCK_IO_OK;
}
static arm_bus_t bus = {.read32 = rd32, .read16 = rd16};
static void setup(uint64_t size) {
    memset(ram, 0, sizeof ram);
    memset(&cpu, 0, sizeof cpu);
    cpu.bus = &bus;
    cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_T;
    block = (vm_block_t){.size = size, .read_at = read_at, .write_at = write_at};
    md_bridge_config_t c = {.read_site = {0x100, 0xdfe1},
                            .write_site = {0x110, 0xdfe2},
                            .ram = ram,
                            .ram_size = sizeof ram,
                            .token_base = UINT64_C(0xe0000000),
                            .media_size = size,
                            .block = &block};
    md_bridge_init(&strategy, &c);
    geometry = (md_geometry_bridge_t){.strategy = &strategy,
                                      .sites = {.register_pc = 0x200,
                                                .bounds_pc = 0x210,
                                                .count64_pc = 0x220,
                                                .count32_pc = 0x230,
                                                .device_va = 0x1000,
                                                .map_pc = 0x300,
                                                .trim_pc = 0x310,
                                                .eof_pc = 0x320,
                                                .invalid_pc = 0x330,
                                                .count64_done_pc = 0x340,
                                                .count32_done_pc = 0x350}};
    wr32(0x1000, 0xe0000);
    wr32(0x1008, (uint32_t)(size >> 12));
    wr32(0x100c, 5);
    wr32(0x1010, 512);
    cpu.r[13] = 0x2000;
    cpu.r[8] = 0x3000;
    io_calls = 0;
    CHECK(md_geometry_bridge_valid(&geometry));
}
static arm_svc_result_t svc(uint32_t pc, uint32_t op) {
    return md_geometry_bridge_svc(&geometry, &cpu, pc, op);
}
static void bounds(uint64_t offset, uint32_t count, uint32_t target) {
    cpu.r[5] = geometry.sites.device_va;
    wr32(cpu.r[13] + 0x10, (uint32_t)offset);
    wr32(cpu.r[13] + 0x14, (uint32_t)(offset >> 32));
    wr32(cpu.r[8] + 0x30, count);
    CHECK(svc(0x210, 0xdfe6) == ARM_SVC_REDIRECTED);
    CHECK(cpu.r[15] == target);
    CHECK(io_calls == 0);
}
static void test_sizes(void) {
    const uint64_t sizes[] = {UINT64_C(0x80000000), UINT64_C(0x100000000),
                              UINT64_C(0x200000000)};
    const uint32_t sectors[] = {512, 1024, 4096, 65536, 513, UINT32_MAX};
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t size = sizes[i];
        setup(size);
        cpu.r[0] = 1;
        cpu.r[1] = 0xe0000;
        cpu.r[2] = 0x80000000;
        cpu.r[3] = 1;
        CHECK(svc(0x200, 0xdfe5) == ARM_SVC_HANDLED);
        CHECK(cpu.r[2] == size / 4096);
        for (unsigned j = 0; j < 6; ++j)
            for (unsigned old = 0; old < 2; ++old) {
                wr32(0x1010, sectors[j]);
                cpu.r[0] = (uint32_t)(size >> 12);
                cpu.r[1] = sectors[j];
                cpu.r[5] = 0;
                cpu.r[6] = 0;
                CHECK(svc(old ? 0x230 : 0x220, old ? 0xdfe8 : 0xdfe7) ==
                      ARM_SVC_REDIRECTED);
                CHECK(cpu.r[0] == (size + sectors[j] - 1) / sectors[j]);
                CHECK(cpu.r[15] == (old ? 0x350u : 0x340u));
            }
        wr32(0x1010, 512);
        bounds(0, 4096, 0x300);
        bounds(size - 4096, 4096, 0x300);
        bounds(size, 4096, 0x320);
        bounds(size + 512, 4096, 0x330);
        bounds(UINT64_MAX - 511, 512, 0x330); /* negative native block number */
        bounds(size - 512, 4096, 0x310);
        CHECK((uint32_t)(cpu.r[1] - cpu.r[4]) == 512);
        bounds(size - 4096, UINT32_MAX, 0x310);
        CHECK((uint32_t)(cpu.r[1] - cpu.r[4]) == 4096);
        if (size > UINT32_MAX) {
            bounds(UINT64_C(0xfffff000), 4096, 0x300);
            bounds(UINT64_C(0x100000000), 4096,
                   size == UINT64_C(0x100000000) ? 0x320 : 0x300);
        }
    }
}
static void unchanged_failure(uint32_t pc, uint32_t op, arm_svc_result_t result) {
    arm_cpu_t before = cpu;
    uint8_t previous[sizeof ram];
    memcpy(previous, ram, sizeof ram);
    CHECK(svc(pc, op) == result);
    CHECK(memcmp(&before, &cpu, offsetof(arm_cpu_t, tlb)) == 0);
    CHECK(memcmp(previous, ram, sizeof ram) == 0);
    CHECK(io_calls == 0);
}
static void test_rejections(void) {
    setup(MD_GEOMETRY_MAX_SIZE);
    cpu.r[5] = 0x1000;
    unchanged_failure(0x211, 0xdfe6, ARM_SVC_UNHANDLED);
    unchanged_failure(0x210, 0xdfe7, ARM_SVC_UNHANDLED);
    cpu.cpsr = ARM_MODE_USR | ARM_CPSR_T;
    unchanged_failure(0x210, 0xdfe6, ARM_SVC_UNHANDLED);
    cpu.cpsr = ARM_MODE_SVC;
    unchanged_failure(0x210, 0xdfe6, ARM_SVC_UNHANDLED);
    cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_T;
    cpu.r[5] += 36;
    unchanged_failure(0x210, 0xdfe6, ARM_SVC_ERROR);
    cpu.r[5] -= 36;
    const uint32_t fields[] = {0, 4, 8, 12, 16};
    for (unsigned i = 0; i < 5; ++i) {
        uint32_t p = 0x1000 + fields[i], old = rd32(NULL, p);
        wr32(p, 0);
        /* Base high is already zero; a nonzero high word must be refused. */
        if (i == 1)
            wr32(p, 1);
        unchanged_failure(0x210, 0xdfe6, ARM_SVC_ERROR);
        wr32(p, old);
    }
    cpu.r[13] = 0xfffffffc;
    unchanged_failure(0x210, 0xdfe6, ARM_SVC_ERROR);
    cpu.r[13] = 0x2001;
    unchanged_failure(0x210, 0xdfe6, ARM_SVC_ERROR);
    cpu.r[13] = 0x2000;
    cpu.r[8] = 0xfffffff0;
    unchanged_failure(0x210, 0xdfe6, ARM_SVC_ERROR);
    cpu.r[8] = 0x3000;
    cpu.cp15.sctlr = ARM_SCTLR_M;
    cpu.cp15.ttbr0 = 0x4000;
    cpu.cp15.dacr = 1; /* unmapped device page */
    unchanged_failure(0x210, 0xdfe6, ARM_SVC_ERROR);
    cpu.cp15.sctlr = 0;
    cpu.r[0] = 1;
    cpu.r[1] = 0xe0000;
    cpu.r[2] = 0;
    cpu.r[3] = 1;
    unchanged_failure(0x200, 0xdfe5, ARM_SVC_ERROR);
    cpu.r[0] = 0;
    cpu.r[1] = 512;
    cpu.r[5] = 0;
    unchanged_failure(0x220, 0xdfe7, ARM_SVC_ERROR);
    geometry.sites.trim_pc = geometry.sites.eof_pc;
    CHECK(!md_geometry_bridge_valid(&geometry));
    unchanged_failure(0x210, 0xdfe6, ARM_SVC_ERROR);
}
static void test_interpreter_and_io(void) {
    setup(MD_GEOMETRY_MAX_SIZE);
    /* Exercise real SVC retirement, not just calls into the helper. */
    wr32(0x210, 0x46c0dfe6);
    cpu.r[15] = 0x210;
    cpu.r[5] = 0x1000;
    wr32(0x2010, 0x1000);
    wr32(0x2014, 1);
    wr32(0x3030, 4096);
    arm_bus_set_privileged_svc_handler(&bus, md_geometry_bridge_svc, &geometry);
    CHECK(arm_step(&cpu) == ARM_OK);
    CHECK(cpu.r[15] == 0x300);
    /* The exact replacement ldr r3,[sp,#0x14] preserves the high offset. */
    wr32(0x400, 0x46c09b05);
    cpu.r[15] = 0x400;
    cpu.r[3] = 0;
    CHECK(arm_step(&cpu) == ARM_OK);
    CHECK(cpu.r[3] == 1);
    /* The existing copy bridge must pass offsets above 4 GiB and near 8 GiB
     * to the host without truncation. Small backing buffer, no huge allocation. */
    const uint64_t offsets[] = {UINT64_C(0xfffff000), UINT64_C(0x100000000),
                                UINT64_C(0x100001000), MD_GEOMETRY_MAX_SIZE - 4096};
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t token = strategy.config.token_base + offsets[i];
        memset(ram + 0x4000, (int)(i + 21), 4096);
        wr32(cpu.r[13], 4096);
        cpu.r[0] = 0x4000;
        cpu.r[1] = 0;
        cpu.r[2] = (uint32_t)token;
        cpu.r[3] = (uint32_t)(token >> 32);
        CHECK(md_bridge_handle_svc(&strategy, &cpu, 0x110, 0xdfe2) == ARM_SVC_HANDLED);
        CHECK(io_offset == offsets[i]);
        CHECK(stored[4095] == i + 21);
        memset(ram + 0x4000, 0, 4096);
        cpu.r[0] = (uint32_t)token;
        cpu.r[1] = (uint32_t)(token >> 32);
        cpu.r[2] = 0x4000;
        cpu.r[3] = 0;
        CHECK(md_bridge_handle_svc(&strategy, &cpu, 0x100, 0xdfe1) == ARM_SVC_HANDLED);
        CHECK(io_offset == offsets[i]);
        CHECK(ram[0x4fff] == i + 21);
    }
    arm_bus_set_privileged_svc_handler(&bus, NULL, NULL);
}
int main(void) {
    test_sizes();
    test_rejections();
    test_interpreter_and_io();
    printf("md geometry: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
