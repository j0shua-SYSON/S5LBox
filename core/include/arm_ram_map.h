/* Persistent, derived plain-RAM mappings for bounded native execution.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_ARM_RAM_MAP_H
#define S5LBOX_ARM_RAM_MAP_H

#include "arm.h"

#define ARM_RAM_MAP_ENTRIES 4096u

/* The low word separates the complete 1KiB VA and access kind. The high
 * word is the nonzero translation generation. Entries contain no code and
 * may never be serialized. User-only by design; kernel access cannot reuse
 * an unprivileged grant. A64 consumers audit this sixteen-byte layout. */
typedef struct {
    uint8_t *host;
    uint64_t key;
} arm_ram_map_entry_t;

typedef struct {
    arm_ram_map_entry_t entries[ARM_RAM_MAP_ENTRIES];
    arm_ram_window_t ram;
    const arm_cpu_t *owner;
    arm_cp15_t cp15;
    uint64_t flushes;
    uint32_t generation;
    bool bound;
} arm_ram_map_t;

/* Zero-initialize before first use. The owner MUST reset on machine reset,
 * snapshot restore or RAM replacement, even if the allocation address is
 * reused. Binding never requests a bus grant; it consumes an already captured
 * immutable RAM capability and rejects an inconsistent translation context.
 * A failed prepare unbinds the cache; no old entry can then be read. */
void arm_ram_map_reset(arm_ram_map_t *map);
bool arm_ram_map_prepare(arm_ram_map_t *map, const arm_ram_window_t *ram,
                         const arm_cpu_t *cpu);
bool arm_ram_map_current(const arm_ram_map_t *map, const arm_cpu_t *cpu);

/* Reference operations for signed-native execution. Publishing requires an
 * exact successful existing TLB entry plus the separate RAM/read/write grant;
 * it cannot walk, fault, touch a device, or retire an instruction. A mapping
 * remains authorized if another VA evicts that TLB slot, just like DREAD; it
 * dies on translation invalidation, not merely on software-cache eviction.
 * Lookup returns the block start, not the requested byte. Alignment/span
 * checks remain the individual instruction's responsibility. */
bool arm_ram_map_publish(arm_ram_map_t *map, const arm_cpu_t *cpu,
                         uint32_t va, arm_access_t access);
uint8_t *arm_ram_map_lookup(const arm_ram_map_t *map, const arm_cpu_t *cpu,
                            uint32_t va, arm_access_t access);

static inline unsigned arm_ram_map_slot(uint32_t va, arm_access_t access) {
    const uint32_t page = va >> 10;
    return ((page ^ (page >> 12)) + (uint32_t)access * 1024u) &
           (ARM_RAM_MAP_ENTRIES - 1u);
}
static inline uint64_t arm_ram_map_key(uint32_t va, arm_access_t access,
                                       uint32_t generation) {
    return ((uint64_t)generation << 32) |
           (va & ~UINT32_C(1023)) | ((uint32_t)access << 1);
}

#endif
