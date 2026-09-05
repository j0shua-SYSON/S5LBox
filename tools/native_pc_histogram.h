/* Bounded streaming host-PC diagnostics; never guest execution state.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_NATIVE_PC_HISTOGRAM_H
#define S5LBOX_NATIVE_PC_HISTOGRAM_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NATIVE_PC_HISTOGRAM_CAPACITY 4096u
#define NATIVE_PC_HISTOGRAM_MASK (NATIVE_PC_HISTOGRAM_CAPACITY - 1u)
#define NATIVE_PC_HISTOGRAM_REGION_BYTES 256u

typedef struct {
    uintptr_t key;
    uintptr_t pc;
    uint64_t samples;
} native_pc_histogram_bucket_t;

typedef struct {
    native_pc_histogram_bucket_t bucket[NATIVE_PC_HISTOGRAM_CAPACITY];
    uint64_t captured;
    uint64_t dropped;
} native_pc_histogram_t;

_Static_assert((NATIVE_PC_HISTOGRAM_CAPACITY & NATIVE_PC_HISTOGRAM_MASK) == 0,
               "host PC table capacity must be a power of two");

static inline void native_pc_histogram_reset(native_pc_histogram_t *h) {
    memset(h, 0, sizeof *h);
}

/* Call under the diagnostic owner's lock, not from a guest hot path. Unlike
 * a first-N raw-sample buffer, this keeps counting repeated regions throughout
 * the complete run. A full DISTINCT-region table still accepts known regions;
 * unknown ones are explicitly dropped, never silently attributed elsewhere.
 * Counts are exact for accepted observations until uint64 saturation. */
static inline bool native_pc_histogram_note(native_pc_histogram_t *h,
                                            uintptr_t pc) {
    uintptr_t key = pc & ~((uintptr_t)NATIVE_PC_HISTOGRAM_REGION_BYTES - 1u);
    uint64_t mixed = (uint64_t)key >> 8u;
    mixed ^= mixed >> 33u;
    mixed *= UINT64_C(0xff51afd7ed558ccd);
    mixed ^= mixed >> 33u;
    unsigned index = (unsigned)mixed & NATIVE_PC_HISTOGRAM_MASK;
    for (unsigned probe = 0; probe < NATIVE_PC_HISTOGRAM_CAPACITY; ++probe) {
        native_pc_histogram_bucket_t *b = &h->bucket[index];
        if (b->samples == 0u || b->key == key) {
            if (b->samples == 0u) {
                b->key = key;
                b->pc = pc;
            }
            if (b->samples != UINT64_MAX) b->samples++;
            if (h->captured != UINT64_MAX) h->captured++;
            return true;
        }
        index = (index + 1u) & NATIVE_PC_HISTOGRAM_MASK;
    }
    if (h->dropped != UINT64_MAX) h->dropped++;
    return false;
}
#endif
