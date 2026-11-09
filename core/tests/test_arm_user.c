/* Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm_user.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
static uint8_t ram[16384];
static unsigned reads, writes, maps;
static bool writable = true;
static bool read_memory(void *ctx, uint32_t a, void *out, size_t n) {
    reads++;
    if (a < 4096 || (uint64_t)a + n > sizeof ram) return false;
    memcpy(out, (ctx ? (uint8_t *)ctx : ram) + a, n); return true;
}
static bool write_memory(void *ctx, uint32_t a, const void *in, size_t n) {
    writes++;
    if (!writable || a < 4096 || (uint64_t)a + n > sizeof ram) return false;
    memcpy((ctx ? (uint8_t *)ctx : ram) + a, in, n); return true;
}
static uint8_t *map_memory(void *ctx, uint32_t a, size_t n, bool write) {
    maps++;
    if ((write && !writable) || a < 4096 || (uint64_t)a + n > sizeof ram) return NULL;
    return (ctx ? (uint8_t *)ctx : ram) + a;
}
static arm_user_memory_t memory = {NULL, read_memory, write_memory, NULL};
static void word(unsigned at, uint32_t value) { memcpy(ram + at, &value, 4); }
static void half(unsigned at, uint16_t value) { memcpy(ram + at, &value, 2); }
static void reset(arm_user_cpu_t *cpu, bool thumb) {
    arm_user_context_t c = {0};
    memset(ram, 0, sizeof ram);
    c.r[15] = 4096; c.r[13] = sizeof ram - 16;
    c.cpsr = thumb ? 0x30 : 0x10;
    arm_user_set_context(cpu, &c);
}
static void integer_and_svc(arm_user_cpu_t *cpu) {
    reset(cpu, false);
    word(4096, 0xe3a00007); /* mov r0, #7 */
    word(4100, 0xe2801005); /* add r1, r0, #5 */
    word(4104, 0xef123456); /* svc #0x123456 */
    uint64_t ticks = 9; uint32_t svc = 0;
    CHECK(arm_user_run(cpu, &memory, &ticks, &svc) == ARM_USER_SVC);
    CHECK(svc == 0x123456 && ticks == 6);
    CHECK(arm_user_regs(cpu)[0] == 7 && arm_user_regs(cpu)[1] == 12);
    CHECK(arm_user_regs(cpu)[15] == 4108 && arm_user_cpsr(cpu) == 0x10);
    word(4108, 0xeafffffe); /* b . */
    ticks = 13;
    CHECK(arm_user_run(cpu, &memory, &ticks, &svc) == ARM_USER_BUDGET);
    CHECK(ticks == 0 && arm_user_regs(cpu)[15] == 4108);
    word(4108, 0xe3a0002a); /* self-modified instruction, no JIT cache */
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_BUDGET);
    CHECK(arm_user_regs(cpu)[0] == 42 && arm_user_regs(cpu)[15] == 4112);
}
static void thumb_and_conditions(arm_user_cpu_t *cpu) {
    reset(cpu, true);
    half(4096, 0x2003); half(4098, 0x3004); half(4100, 0xdfab);
    uint64_t ticks = 3; uint32_t svc;
    CHECK(arm_user_run(cpu, &memory, &ticks, &svc) == ARM_USER_SVC);
    CHECK(arm_user_regs(cpu)[0] == 7 && svc == 0xab && ticks == 0);
    CHECK(arm_user_regs(cpu)[15] == 4102 && (arm_user_cpsr(cpu) & 0x3f) == 0x30);
    reset(cpu, false);
    word(4096, 0x0f000055); /* svceq, skipped with Z clear */
    word(4100, 0xef000066);
    ticks = 2;
    CHECK(arm_user_run(cpu, &memory, &ticks, &svc) == ARM_USER_SVC);
    CHECK(svc == 0x66 && ticks == 0);
    reset(cpu, false);
    word(4096, 0xe12fff10); /* bx r0 */
    half(4100, 0xdf13); arm_user_regs(cpu)[0] = 4101;
    ticks = 2;
    CHECK(arm_user_run(cpu, &memory, &ticks, &svc) == ARM_USER_SVC && svc == 0x13);
}
static void contexts_and_vfp(arm_user_cpu_t *cpu) {
    reset(cpu, false);
    word(4096, 0xee000a10); /* vmov s0, r0 */
    word(4100, 0xee100a10); /* vmov r0, s0 */
    arm_user_regs(cpu)[0] = 0x3fc00000;
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_BUDGET);
    arm_user_context_t a, b = {0};
    arm_user_get_context(cpu, &a);
    CHECK(a.vfp_s[0] == 0x3fc00000);
    b.r[15] = 8192; b.r[0] = 27; b.vfp_s[0] = 99;
    b.cpsr = 0xffffffff; b.fpscr = 0x12340000; b.tpidrurw = 77; b.tpidruro = 88;
    arm_user_swap_context(cpu, &b);
    CHECK(b.r[15] == 4100 && b.vfp_s[0] == 0x3fc00000);
    CHECK((arm_user_cpsr(cpu) & 0x1f) == 0x10);
    arm_user_context_t c; arm_user_get_context(cpu, &c);
    CHECK(c.fpscr == 0x12340000 && c.tpidrurw == 77 && c.tpidruro == 88);
    arm_user_swap_context(cpu, &b);
    arm_user_regs(cpu)[0] = 0;
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_BUDGET);
    CHECK(arm_user_regs(cpu)[0] == 0x3fc00000);
}
static void faults_and_memory(arm_user_cpu_t *cpu) {
    reset(cpu, false);
    word(4096, 0xe5810000); word(4100, 0xe5912000); /* str / ldr */
    arm_user_regs(cpu)[0] = 0x12345678; arm_user_regs(cpu)[1] = 8193;
    uint64_t ticks = 2;
    CHECK(arm_user_run(cpu, &memory, &ticks, NULL) == ARM_USER_BUDGET);
    CHECK(arm_user_regs(cpu)[2] == 0x12345678 && ram[8193] == 0x78);
    reset(cpu, false);
    word(4096, 0xe5910000); arm_user_regs(cpu)[1] = 4;
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_MEMORY_FAULT);
    CHECK(arm_user_regs(cpu)[15] == 4096 && (arm_user_cpsr(cpu) & 0x1f) == 0x10);
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_MEMORY_FAULT);
    reset(cpu, false); arm_user_regs(cpu)[15] = 0xfffffffcu;
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_MEMORY_FAULT);
    reset(cpu, false); arm_user_regs(cpu)[15] = 4097;
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_MEMORY_FAULT);
    reset(cpu, false); word(4096, 0xe1200070); /* bkpt */
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_BREAKPOINT);
    CHECK(arm_user_regs(cpu)[15] == 4096);
    reset(cpu, true); half(4096, 0xbe00);
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_BREAKPOINT);
    reset(cpu, false); word(4096, 0xee010f10); /* privileged SCTLR write */
    CHECK(arm_user_run(cpu, &memory, NULL, NULL) == ARM_USER_UNDEFINED);
    CHECK(arm_user_regs(cpu)[15] == 4096);
    reset(cpu, false); arm_user_regs(cpu)[15] = 0; ticks = 0;
    CHECK(arm_user_run(cpu, &memory, &ticks, NULL) == ARM_USER_BUDGET);
}
static void mapped_memory(arm_user_cpu_t *cpu) {
    reset(cpu, false);
    word(4096, 0xe5810000); /* str r0, [r1] */
    word(4100, 0xe5912000); /* ldr r2, [r1] */
    word(4104, 0xeafffffc); /* b 4096 */
    arm_user_regs(cpu)[0] = 42; arm_user_regs(cpu)[1] = 8192;
    reads = writes = maps = 0;
    uint64_t ticks = 3000;
    CHECK(arm_user_run(cpu, &memory, &ticks, NULL) == ARM_USER_BUDGET);
    CHECK(arm_user_regs(cpu)[2] == 42 && ticks == 0);
    CHECK(reads < 8 && writes < 8 && maps < 8);

    // Same guest addresses, different owner after a run boundary.
    uint8_t replacement[sizeof ram]; memcpy(replacement, ram, sizeof ram);
    arm_user_memory_t replaced = {replacement, read_memory, write_memory, map_memory};
    arm_user_regs(cpu)[0] = 99; ticks = 3;
    CHECK(arm_user_run(cpu, &replaced, &ticks, NULL) == ARM_USER_BUDGET);
    CHECK(arm_user_regs(cpu)[2] == 99 && ram[8192] == 42 && replacement[8192] == 99);
    // A formerly writable mapping can become read-only between calls.
    writable = false; ticks = 3;
    CHECK(arm_user_run(cpu, &replaced, &ticks, NULL) == ARM_USER_MEMORY_FAULT);
    CHECK(arm_user_regs(cpu)[15] == 4096 && replacement[8192] == 99);
    writable = true;

    // Code stores in the same batch must be visible to the cached fetch path.
    reset(cpu, false);
    word(4096, 0xe5810000); word(4100, 0xe3a02000); word(4104, 0xef000023);
    arm_user_regs(cpu)[0] = 0xe3a0202a; arm_user_regs(cpu)[1] = 4100;
    ticks = 3; uint32_t svc;
    CHECK(arm_user_run(cpu, &memory, &ticks, &svc) == ARM_USER_SVC);
    CHECK(arm_user_regs(cpu)[2] == 42 && svc == 35);
}
int main(void) {
    arm_user_cpu_t *cpu = arm_user_create();
    if (!cpu) return 1;
    integer_and_svc(cpu); thumb_and_conditions(cpu);
    contexts_and_vfp(cpu); faults_and_memory(cpu);
    memory.map = map_memory;
    integer_and_svc(cpu); thumb_and_conditions(cpu);
    contexts_and_vfp(cpu); faults_and_memory(cpu);
    mapped_memory(cpu);
    arm_user_destroy(cpu);
    printf("ARM user interpreter: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
