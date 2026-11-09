/* See arm_user.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm_user.h"
#include "arm.h"
#include <stdlib.h>
#include <string.h>

struct arm_user_cpu {
    arm_cpu_t cpu;
    arm_bus_t bus;
    const arm_user_memory_t *memory;
    bool memory_fault;
    int32_t stopped;
};

static bool accessible(arm_user_cpu_t *u, uint32_t address, size_t size) {
    return !u->memory_fault && u->memory && size <= sizeof(uint32_t) &&
           (uint64_t)address + size <= UINT64_C(0x100000000);
}
static uint32_t read_value(arm_user_cpu_t *u, uint32_t address, size_t size) {
    uint8_t bytes[4] = {0};
    if (!accessible(u, address, size) ||
        !u->memory->read(u->memory->context, address, bytes, size)) {
        u->memory_fault = true;
        u->cpu.abort_pending = true;
        u->cpu.abort_far = address;
        u->cpu.abort_fsr = 5;
        return 0;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= (uint32_t)bytes[i] << (8u * i);
    return value;
}
static void write_value(arm_user_cpu_t *u, uint32_t address, uint32_t value, size_t size) {
    uint8_t bytes[4];
    for (size_t i = 0; i < size; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
    if (!accessible(u, address, size) ||
        !u->memory->write(u->memory->context, address, bytes, size)) {
        u->memory_fault = true;
        u->cpu.abort_pending = true;
        u->cpu.abort_far = address;
        u->cpu.abort_fsr = 5;
    }
}
static uint8_t read8(void *u, uint32_t a) { return (uint8_t)read_value(u, a, 1); }
static uint16_t read16(void *u, uint32_t a) { return (uint16_t)read_value(u, a, 2); }
static uint32_t read32(void *u, uint32_t a) { return read_value(u, a, 4); }
static void write8(void *u, uint32_t a, uint8_t v) { write_value(u, a, v, 1); }
static void write16(void *u, uint32_t a, uint16_t v) { write_value(u, a, v, 2); }
static void write32(void *u, uint32_t a, uint32_t v) { write_value(u, a, v, 4); }

static uint8_t *map_read(void *context, uint32_t address, uint32_t length) {
    arm_user_cpu_t *u = context;
    if (!u->memory || !u->memory->map) return NULL;
    return u->memory->map(u->memory->context, address, length, false);
}
static uint8_t *map_write(void *context, uint32_t address, uint32_t length) {
    arm_user_cpu_t *u = context;
    if (!u->memory || !u->memory->map) return NULL;
    return u->memory->map(u->memory->context, address, length, true);
}

static uint32_t fetch_instruction(arm_user_cpu_t *u, uint32_t pc, unsigned size) {
    // Share the interpreter's checked fetch block, including the SVC probe.
    // Mapping refusal preserves the ordinary fault-reporting read path.
    if (u->memory->map && arm_fetch_cache_try_refill(&u->cpu, pc, false)) {
        const uint8_t *p = u->cpu.fetch_host + (pc & 0x3ffu);
        uint32_t value = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        if (size == 4) value |= ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        return value;
    }
    return read_value(u, pc, size);
}

arm_user_cpu_t *arm_user_create(void) {
    arm_user_cpu_t *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->bus.ctx = u;
    u->bus.read8 = read8; u->bus.read16 = read16; u->bus.read32 = read32;
    u->bus.write8 = write8; u->bus.write16 = write16; u->bus.write32 = write32;
    u->bus.host_ram = map_read; u->bus.host_ram_write = map_write;
    arm_reset(&u->cpu, &u->bus);
    u->cpu.cpsr = ARM_MODE_USR;
    u->cpu.cp15.sctlr = ARM_SCTLR_U;
    u->cpu.cp15.cpacr = 0xfu << ARM_CPACR_CP10_SHIFT;
    u->cpu.vfp_fpexc = ARM_FPEXC_EN;
    return u;
}
void arm_user_destroy(arm_user_cpu_t *u) { free(u); }
uint32_t *arm_user_regs(arm_user_cpu_t *u) { return u->cpu.r; }
uint32_t arm_user_cpsr(const arm_user_cpu_t *u) { return u->cpu.cpsr; }
void arm_user_set_cpsr(arm_user_cpu_t *u, uint32_t value) {
    /* Neither a caller nor a thread context can enable privileged execution. */
    const uint32_t mask = ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V |
                          ARM_CPSR_Q | ARM_CPSR_T | 0x000f0000u;
    u->cpu.cpsr = (value & mask) | ARM_MODE_USR;
}
void arm_user_get_context(const arm_user_cpu_t *u, arm_user_context_t *c) {
    memcpy(c->r, u->cpu.r, sizeof c->r);
    memcpy(c->vfp_s, u->cpu.vfp_s, sizeof c->vfp_s);
    c->cpsr = u->cpu.cpsr; c->fpscr = u->cpu.vfp_fpscr;
    c->tpidrurw = u->cpu.cp15.tpidrurw; c->tpidruro = u->cpu.cp15.tpidruro;
}
void arm_user_set_context(arm_user_cpu_t *u, const arm_user_context_t *c) {
    memcpy(u->cpu.r, c->r, sizeof c->r);
    memcpy(u->cpu.vfp_s, c->vfp_s, sizeof c->vfp_s);
    arm_user_set_cpsr(u, c->cpsr);
    u->cpu.vfp_fpscr = c->fpscr;
    u->cpu.cp15.tpidrurw = c->tpidrurw; u->cpu.cp15.tpidruro = c->tpidruro;
    /* The local exclusive monitor cannot survive a thread handoff. */
    u->cpu.excl_valid = false;
    u->cpu.abort_pending = false;
    u->memory_fault = false; u->stopped = 0;
}
void arm_user_swap_context(arm_user_cpu_t *u, arm_user_context_t *c) {
    arm_user_context_t previous;
    arm_user_get_context(u, &previous);
    arm_user_set_context(u, c);
    *c = previous;
}

int32_t arm_user_run(arm_user_cpu_t *u, const arm_user_memory_t *memory,
                     uint64_t *budget, uint32_t *svc) {
    if (svc) *svc = 0;
    if (!u || !memory || !memory->read || !memory->write) return ARM_USER_MEMORY_FAULT;
    if (u->stopped) return u->stopped;
    uint64_t left = budget ? *budget : 1;
    // Guest VM mappings can change while host framework calls run between
    // interpreter batches. Never reuse a pointer from a previous borrow.
    arm_mmu_tlb_flush(&u->cpu);
    u->memory = memory;
    int32_t result = ARM_USER_BUDGET;
    while (left) {
        uint32_t pc = u->cpu.r[15];
        uint32_t cpsr = u->cpu.cpsr;
        bool thumb = (cpsr & ARM_CPSR_T) != 0;
        unsigned size = thumb ? 2u : 4u;
        if (pc & (size - 1u)) {
            result = ARM_USER_MEMORY_FAULT;
            break;
        }
        uint32_t instruction = fetch_instruction(u, pc, size);
        if (u->memory_fault) { result = ARM_USER_MEMORY_FAULT; break; }
        bool conditional = thumb || ((instruction >> 28) != 15u &&
                                      arm_cond_passed(&u->cpu, instruction >> 28));
        bool is_svc = thumb ? (instruction & 0xff00u) == 0xdf00u
                            : (instruction & 0x0f000000u) == 0x0f000000u;
        bool is_break = thumb ? (instruction & 0xff00u) == 0xbe00u
                              : (instruction & 0x0ff000f0u) == 0x01200070u;
        if (conditional && is_svc) {
            if (svc) *svc = instruction & (thumb ? 0xffu : 0xffffffu);
            u->cpu.r[15] += size;
            u->cpu.cycles++; left--;
            result = ARM_USER_SVC;
            break;
        }
        if (conditional && is_break) { result = ARM_USER_BREAKPOINT; break; }
        arm_status_t status = arm_step(&u->cpu);
        if (u->memory_fault || (u->cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT)
            result = ARM_USER_MEMORY_FAULT;
        else if (status != ARM_OK || (u->cpu.cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR)
            result = ARM_USER_UNDEFINED;
        if (result != ARM_USER_BUDGET) {
            /* No guest exception handler exists. Preserve the fault address
             * rather than allowing an exception vector to become app code. */
            u->cpu.r[15] = pc;
            arm_user_set_cpsr(u, cpsr);
            break;
        }
        left--;
    }
    arm_mmu_tlb_flush(&u->cpu);
    u->memory = NULL;
    if (budget) *budget = left;
    if (result >= ARM_USER_MEMORY_FAULT) u->stopped = result;
    return result;
}
