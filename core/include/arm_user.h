/* ARMv6 user-mode interpreter adapter. No executable memory or guest kernel.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_ARM_USER_H
#define S5LBOX_ARM_USER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct arm_user_cpu arm_user_cpu_t;
typedef struct {
    void *context;
    /* Return false on inaccessible memory; never unwind across this boundary.
     * A failed write must not modify memory. Addresses cannot wrap at 4 GiB.
     * Calls are synchronous and only valid for the duration of run(). */
    bool (*read)(void *, uint32_t, void *, size_t);
    bool (*write)(void *, uint32_t, const void *, size_t);
    /* Optional plain-RAM mapping. NULL refuses the range and keeps callbacks.
     * A non-NULL pointer covers the ENTIRE range, aliases read/write memory,
     * and remains valid until run() returns, including across other callbacks.
     * writable=true explicitly permits bypassing write callbacks. No mapping
     * may have MMIO, protection, tracing or other required per-access effects.
     * The adapter invalidates all cached pointers at every run boundary. */
    uint8_t *(*map)(void *, uint32_t, size_t, bool writable);
} arm_user_memory_t;

typedef struct {
    uint32_t r[16];
    uint32_t vfp_s[32];
    uint32_t cpsr, fpscr;
    uint32_t tpidrurw, tpidruro;
} arm_user_context_t;

enum {
    ARM_USER_BUDGET = 0,
    ARM_USER_SVC = 1,
    ARM_USER_MEMORY_FAULT = 2,
    ARM_USER_UNDEFINED = 3,
    ARM_USER_BREAKPOINT = 4
};

arm_user_cpu_t *arm_user_create(void);
void arm_user_destroy(arm_user_cpu_t *);
/* Pointers remain valid until destroy. Access only while run() is not active. */
uint32_t *arm_user_regs(arm_user_cpu_t *);
uint32_t arm_user_cpsr(const arm_user_cpu_t *);
void arm_user_set_cpsr(arm_user_cpu_t *, uint32_t);
void arm_user_get_context(const arm_user_cpu_t *, arm_user_context_t *);
void arm_user_set_context(arm_user_cpu_t *, const arm_user_context_t *);
void arm_user_swap_context(arm_user_cpu_t *, arm_user_context_t *);

/* NULL budget steps once; otherwise decrements an instruction budget.
 * SVC is retired, with PC pointing after it. Faults retain the offending PC
 * and latch until set_context(); partial multi-store effects are not undone.
 * A single CPU may only be called from one host thread at a time. */
int32_t arm_user_run(arm_user_cpu_t *, const arm_user_memory_t *,
                     uint64_t *budget, uint32_t *svc);
#endif
