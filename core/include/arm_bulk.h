/* Bounded native execution of witnessed A32 routines. */
#ifndef S5LBOX_ARM_BULK_H
#define S5LBOX_ARM_BULK_H

#include "arm.h"
#include <stddef.h>

typedef struct {
    /* The caller owns a live FETCH witness for this complete code span. */
    const uint8_t *code;
    uint32_t code_base;
    uint32_t code_bytes;
    /* Flat RAM is a harness-only alternative to existing DREAD witnesses. */
    const uint8_t *flat_ram;
    size_t flat_size;
    bool data_cache;
} arm_bulk_memory_t;

/* Returns the exact number of original instructions represented, bounded by
 * budget. The caller owns cycle/device accounting. Zero changes no CPU state,
 * cache counters or guest bytes. No page walk, MMIO or executable write occurs.
 * Matching is by complete routine/loop bytes, never by a name or assumed PC.
 * A long loop may return an exact prefix at its header for bounded resumption.
 * Ordinary execution enables this only through an explicit host-policy gate. */
unsigned arm_bulk_string_try(arm_cpu_t *cpu, const arm_bulk_memory_t *memory,
                             unsigned budget);

#endif
