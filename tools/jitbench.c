/*
 * S5LBox -- Apple-arm64 native-semantics ceiling benchmark.
 *
 * This is deliberately NOT a product-performance claim and not a JIT
 * dispatcher. It translates one small synthetic block once, then compares
 * repeated interpreter execution with repeated entry into that already-built
 * block. The useful question is narrow: can the repository's existing
 * register-pinned AArch64 semantics clear the interpreter by a large factor on
 * the Apple-Silicon runner at all?
 *
 * The answer is only a feasibility bound. There is no device tick, MMIO,
 * interrupt sampling, cache lookup, translation, chaining, framebuffer
 * publication, UIKit, or real iOS instruction mix here. In particular, a
 * positive result cannot be reported as phone FPS or as proof that a static
 * no-JIT interpreter will have the same speed. A negative result is useful:
 * this generated block specializes more than a static handler can, so a weak
 * result would make that larger implementation hard to justify.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "jit.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define RAM_SIZE       (1u << 20)
#define DATA_BASE      0x8000u
#define STACK_BASE     0x9000u
#define CODE_WORDS     4096u
#define DEFAULT_INSNS  20000000ull
#define DEFAULT_REPS   3u

typedef struct {
    const char *name;
    const void *program;
    unsigned insns;
    bool thumb;
} bench_case_t;

typedef struct {
    uint32_t r[16];
    uint32_t cpsr;
    uint64_t cycles;
    uint64_t ram_hash;
    arm_status_t status;
    int jit_exit;
} final_state_t;

static uint8_t g_ram[RAM_SIZE];

static uint32_t mem_r32(void *ctx, uint32_t addr) {
    uint32_t value;
    (void)ctx;
    memcpy(&value, &g_ram[addr & (RAM_SIZE - 1u)], sizeof value);
    return value;
}

static uint16_t mem_r16(void *ctx, uint32_t addr) {
    uint16_t value;
    (void)ctx;
    memcpy(&value, &g_ram[addr & (RAM_SIZE - 1u)], sizeof value);
    return value;
}

static uint8_t mem_r8(void *ctx, uint32_t addr) {
    (void)ctx;
    return g_ram[addr & (RAM_SIZE - 1u)];
}

static void mem_w32(void *ctx, uint32_t addr, uint32_t value) {
    (void)ctx;
    memcpy(&g_ram[addr & (RAM_SIZE - 1u)], &value, sizeof value);
}

static void mem_w16(void *ctx, uint32_t addr, uint16_t value) {
    (void)ctx;
    memcpy(&g_ram[addr & (RAM_SIZE - 1u)], &value, sizeof value);
}

static void mem_w8(void *ctx, uint32_t addr, uint8_t value) {
    (void)ctx;
    g_ram[addr & (RAM_SIZE - 1u)] = value;
}

static uint8_t *mem_host_ram(void *ctx, uint32_t addr, uint32_t len) {
    (void)ctx;
    if ((uint64_t)addr + len > RAM_SIZE) return NULL;
    return &g_ram[addr];
}

static const arm_bus_t g_bus = {
    .ctx = NULL,
    .read32 = mem_r32, .read16 = mem_r16, .read8 = mem_r8,
    .write32 = mem_w32, .write16 = mem_w16, .write8 = mem_w8,
    .host_ram = mem_host_ram,
};

/* Fifteen ordinary operations plus a branch back to address zero. The mixed
 * rows contain four memory operations out of sixteen instructions (25%),
 * close to the historical 22.6% guest share and enough to expose helper-call
 * cost, but still not presented as a replay of the measured guest mix. */
static const uint32_t A32_ALU[] = {
    0xe2800001u, 0xe2811003u, 0xe2422001u, 0xe0233000u,
    0xe0844001u, 0xe2455002u, 0xe1866002u, 0xe0200004u,
    0xe0811005u, 0xe2422003u, 0xe0833006u, 0xe2244007u,
    0xe2855001u, 0xe0466001u, 0xe0877002u, 0xeaffffefu,
};

static const uint32_t A32_MIXED[] = {
    0xe2800001u, 0xe5870000u, 0xe5971000u, 0xe0822001u,
    0xe2833001u, 0xe0244003u, 0xe2855001u, 0xe0466005u,
    0xe5874008u, 0xe5975008u, 0xe0866005u, 0xe0200006u,
    0xe2811001u, 0xe2422001u, 0xe0833002u, 0xeaffffefu,
};

static const uint16_t THUMB_ALU[] = {
    0x3001u, 0x3103u, 0x3a01u, 0x3305u,
    0x3c02u, 0x3507u, 0x3e03u, 0x3709u,
    0x3002u, 0x3901u, 0x3204u, 0x3b03u,
    0x3401u, 0x3d02u, 0x3605u, 0xe7efu,
};

static const uint16_t THUMB_MIXED[] = {
    0x3001u, 0x9001u, 0x9901u, 0x3203u,
    0x3301u, 0x4043u, 0x3401u, 0x3501u,
    0x9502u, 0x9e02u, 0x3601u, 0x3701u,
    0x3002u, 0x3901u, 0x3201u, 0xe7efu,
};

static const bench_case_t CASES[] = {
    { "a32-alu", A32_ALU, 16u, false },
    { "a32-mixed", A32_MIXED, 16u, false },
    { "thumb-alu", THUMB_ALU, 16u, true },
    { "thumb-mixed", THUMB_MIXED, 16u, true },
};

static double now_seconds(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) || frequency.QuadPart == 0)
        return -1.0;
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static uint64_t hash_ram(void) {
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t i;
    for (i = 0; i < sizeof g_ram; i++)
        hash = (hash ^ g_ram[i]) * UINT64_C(1099511628211);
    return hash;
}

static void seed_cpu(arm_cpu_t *cpu, const bench_case_t *bc) {
    unsigned i;
    memset(g_ram, 0, sizeof g_ram);
    if (bc->thumb) {
        const uint16_t *program = (const uint16_t *)bc->program;
        for (i = 0; i < bc->insns; i++) mem_w16(NULL, i * 2u, program[i]);
    } else {
        const uint32_t *program = (const uint32_t *)bc->program;
        for (i = 0; i < bc->insns; i++) mem_w32(NULL, i * 4u, program[i]);
    }

    arm_reset(cpu, &g_bus);
    cpu->cpsr = (cpu->cpsr & ~(ARM_CPSR_MODE_MASK | ARM_CPSR_T)) |
                ARM_MODE_SYS | (bc->thumb ? ARM_CPSR_T : 0u) | ARM_CPSR_C;
    for (i = 0; i < 13u; i++) cpu->r[i] = 0x10203040u + i * 0x01010101u;
    cpu->r[7] = DATA_BASE;
    cpu->r[13] = STACK_BASE;
    cpu->r[14] = 0xdead0000u;
    cpu->r[15] = 0u;
}

static void capture_state(final_state_t *out, const arm_cpu_t *cpu,
                          arm_status_t status, int jit_exit) {
    memcpy(out->r, cpu->r, sizeof out->r);
    out->cpsr = cpu->cpsr;
    out->cycles = cpu->cycles;
    out->ram_hash = hash_ram();
    out->status = status;
    out->jit_exit = jit_exit;
}

static bool states_equal(const final_state_t *a, const final_state_t *b) {
    return memcmp(a->r, b->r, sizeof a->r) == 0 &&
           a->cpsr == b->cpsr && a->cycles == b->cycles &&
           a->ram_hash == b->ram_hash &&
           a->status == ARM_OK && b->jit_exit == JIT_EXIT_NEXT;
}

static bool run_interpreter(const bench_case_t *bc, uint64_t total,
                            final_state_t *out, double *seconds) {
    arm_cpu_t cpu;
    arm_status_t status = ARM_OK;
    uint64_t i;
    double start, end;

    seed_cpu(&cpu, bc);
    start = now_seconds();
    for (i = 0; i < total; i++) {
        status = arm_step(&cpu);
        if (status != ARM_OK) break;
    }
    end = now_seconds();
    capture_state(out, &cpu, status, -1);
    *seconds = end - start;
    return i == total && status == ARM_OK && *seconds > 0.0;
}

static bool run_native(const bench_case_t *bc, const jit_buf_t *arena,
                       const jit_block_t *block, uint64_t blocks,
                       final_state_t *out, double *seconds) {
    arm_cpu_t cpu;
    uint64_t i;
    int exit_reason = JIT_EXIT_INTERPRET;
    double start, end;

    seed_cpu(&cpu, bc);
    start = now_seconds();
    for (i = 0; i < blocks; i++) {
        exit_reason = jit_enter(arena, block, &cpu);
        if (exit_reason != JIT_EXIT_NEXT) break;
    }
    end = now_seconds();
    capture_state(out, &cpu, ARM_OK, exit_reason);
    *seconds = end - start;
    return i == blocks && exit_reason == JIT_EXIT_NEXT && *seconds > 0.0;
}

static int cmp_double(const void *lhs, const void *rhs) {
    double a = *(const double *)lhs, b = *(const double *)rhs;
    return (a > b) - (a < b);
}

static bool bench_one(const bench_case_t *bc, uint64_t requested,
                      unsigned reps) {
    jit_buf_t arena;
    jit_block_t block;
    arm_cpu_t translate_cpu;
    uint32_t *code;
    double *interp_rates = NULL, *native_rates = NULL;
    uint64_t blocks = (requested + bc->insns - 1u) / bc->insns;
    uint64_t total = blocks * bc->insns;
    bool ok = false;
    unsigned rep;

    memset(&arena, 0, sizeof arena);
    memset(&block, 0, sizeof block);
    if (!jit_buf_alloc(&arena, 1u << 20)) {
        fprintf(stderr, "jitbench: executable arena unavailable for %s\n", bc->name);
        return false;
    }
    code = jit_buf_take(&arena, CODE_WORDS);
    seed_cpu(&translate_cpu, bc);
    if (!code || !jit_buf_begin_write(&arena) ||
        !jit_translate(&translate_cpu, 0u, code, CODE_WORDS, &block) ||
        !jit_buf_end_write(&arena) || !jit_block_commit(&arena, &block)) {
        fprintf(stderr, "jitbench: could not translate/commit %s\n", bc->name);
        goto done;
    }
    if (block.insn_count != bc->insns || block.native_count != bc->insns ||
        block.end_reason != JIT_END_BRANCH) {
        fprintf(stderr,
                "jitbench: %s covered %u/%u instructions (native %u, end %d)\n",
                bc->name, block.insn_count, bc->insns, block.native_count,
                (int)block.end_reason);
        goto done;
    }

    interp_rates = (double *)calloc(reps, sizeof *interp_rates);
    native_rates = (double *)calloc(reps, sizeof *native_rates);
    if (!interp_rates || !native_rates) {
        fprintf(stderr, "jitbench: out of memory\n");
        goto done;
    }

    for (rep = 0; rep < reps; rep++) {
        final_state_t interp, native;
        double interp_s = 0.0, native_s = 0.0;
        bool ran;

        /* Reverse alternate repetitions to reduce monotonic frequency/thermal
         * drift without pretending a shared CI host is a stable lab. */
        if ((rep & 1u) == 0u) {
            ran = run_interpreter(bc, total, &interp, &interp_s) &&
                  run_native(bc, &arena, &block, blocks, &native, &native_s);
        } else {
            ran = run_native(bc, &arena, &block, blocks, &native, &native_s) &&
                  run_interpreter(bc, total, &interp, &interp_s);
        }
        if (!ran || !states_equal(&interp, &native)) {
            fprintf(stderr,
                    "jitbench: %s repetition %u failed state/exit equality\n",
                    bc->name, rep + 1u);
            goto done;
        }
        interp_rates[rep] = (double)total / interp_s / 1.0e6;
        native_rates[rep] = (double)total / native_s / 1.0e6;
        printf("NATIVE-CEILING-SAMPLE case=%s rep=%u order=%s "
               "interpreter=%.3f native-block=%.3f Minsn/s\n",
               bc->name, rep + 1u, (rep & 1u) ? "native-first" : "interp-first",
               interp_rates[rep], native_rates[rep]);
    }

    qsort(interp_rates, reps, sizeof *interp_rates, cmp_double);
    qsort(native_rates, reps, sizeof *native_rates, cmp_double);
    printf("NATIVE-CEILING case=%s guest-insns=%" PRIu64
           " block-insns=%u reps=%u interpreter-median=%.3f "
           "native-block-median=%.3f speedup=%.3fx\n",
           bc->name, total, bc->insns, reps,
           interp_rates[reps / 2u], native_rates[reps / 2u],
           native_rates[reps / 2u] / interp_rates[reps / 2u]);
    ok = true;

done:
    free(interp_rates);
    free(native_rates);
    if (!jit_buf_free(&arena)) {
        fprintf(stderr, "jitbench: could not release executable arena\n");
        ok = false;
    }
    return ok;
}

static bool parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;
    if (!s || !*s) return false;
    value = strtoull(s, &end, 10);
    if (!end || *end || value == 0u) return false;
    *out = (uint64_t)value;
    return true;
}

static bool parse_unsigned(const char *s, unsigned *out) {
    uint64_t value;
    if (!parse_u64(s, &value) || value > 99u) return false;
    *out = (unsigned)value;
    return true;
}

static bool validate_case_translation(const bench_case_t *bc) {
    arm_cpu_t cpu;
    jit_block_t block;
    uint32_t code[CODE_WORDS];

    seed_cpu(&cpu, bc);
    memset(&block, 0, sizeof block);
    if (!jit_translate(&cpu, 0u, code, CODE_WORDS, &block) ||
        block.insn_count != bc->insns || block.native_count != bc->insns ||
        block.end_reason != JIT_END_BRANCH) {
        fprintf(stderr,
                "jitbench: structural translation failed for %s (%u/%u, "
                "native %u, end %d)\n",
                bc->name, block.insn_count, bc->insns, block.native_count,
                (int)block.end_reason);
        return false;
    }
    printf("NATIVE-CEILING-STRUCTURAL case=%s block-insns=%u native=%u\n",
           bc->name, block.insn_count, block.native_count);
    return true;
}

int main(int argc, char **argv) {
    uint64_t insns = DEFAULT_INSNS;
    unsigned reps = DEFAULT_REPS;
    unsigned i;

    for (i = 1u; i < (unsigned)argc; i++) {
        if (strcmp(argv[i], "--insns") == 0 && i + 1u < (unsigned)argc) {
            if (!parse_u64(argv[++i], &insns)) {
                fprintf(stderr, "jitbench: invalid --insns value\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--reps") == 0 && i + 1u < (unsigned)argc) {
            if (!parse_unsigned(argv[++i], &reps)) {
                fprintf(stderr, "jitbench: invalid --reps value\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: %s [--insns N] [--reps N]\n", argv[0]);
            return 2;
        }
    }

    printf("Apple-arm64 native-semantics ceiling benchmark\n");
    printf("NOT PHONE FPS: synthetic blocks; no tick/MMIO/interrupt/cache/"
           "framebuffer/UI path.\n");
    for (i = 0u; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (!validate_case_translation(&CASES[i])) return 1;
    }
    if (!jit_host_can_execute()) {
        printf("SKIP: not an arm64 execution host.\n");
        return 0;
    }

    for (i = 0u; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (!bench_one(&CASES[i], insns, reps)) return 1;
    }
    return 0;
}
