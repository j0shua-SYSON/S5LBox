#include "arm_ram_map.h"
#include <string.h>

static bool controls_match(const arm_cp15_t *a, const arm_cp15_t *b) {
    return a->sctlr == b->sctlr && a->ttbr0 == b->ttbr0 &&
        a->ttbr1 == b->ttbr1 && a->ttbcr == b->ttbcr &&
        a->dacr == b->dacr && a->context_id == b->context_id;
}

static bool admissible(const arm_cpu_t *cpu) {
    return cpu && cpu->tlb_gen &&
        (cpu->cp15.sctlr & ARM_SCTLR_M) &&
        (cpu->cpsr & (ARM_CPSR_MODE_MASK | ARM_CPSR_E)) == ARM_MODE_USR &&
        !cpu->abort_pending &&
        !(cpu->irq_line && !(cpu->cpsr & ARM_CPSR_I)) &&
        !(cpu->fiq_line && !(cpu->cpsr & ARM_CPSR_F)) &&
        cpu->tlb_stamp.sctlr == cpu->cp15.sctlr &&
        cpu->tlb_stamp.ttbr0 == cpu->cp15.ttbr0 &&
        cpu->tlb_stamp.ttbr1 == cpu->cp15.ttbr1 &&
        cpu->tlb_stamp.ttbcr == cpu->cp15.ttbcr &&
        cpu->tlb_stamp.dacr == cpu->cp15.dacr &&
        cpu->tlb_stamp.context_id == cpu->cp15.context_id;
}

void arm_ram_map_reset(arm_ram_map_t *map) {
    if (map) memset(map, 0, sizeof *map);
}

bool arm_ram_map_current(const arm_ram_map_t *map, const arm_cpu_t *cpu) {
    return map && map->bound && map->owner == cpu && admissible(cpu) &&
        map->generation == cpu->tlb_gen && map->flushes == cpu->tlb_flushes &&
        controls_match(&map->cp15, &cpu->cp15) &&
        arm_ram_window_current(&map->ram, cpu);
}

bool arm_ram_map_prepare(arm_ram_map_t *map, const arm_ram_window_t *ram,
                         const arm_cpu_t *cpu) {
    if (!map) return false;
    if (!admissible(cpu) || !arm_ram_window_current(ram, cpu)) {
        map->bound = false;
        return false;
    }
    /* Generations avoid a table clear at ordinary flushes. A reset/wrap or
     * out-of-band context/lifetime change must not resurrect a prior key.
     * The 64-bit flush count detects a full 32-bit generation lap, including
     * equal generation numbers with otherwise identical controls. */
    bool same_owner = map->bound && map->owner == cpu &&
        arm_ram_window_current(&map->ram, cpu) &&
        map->ram.read_host == ram->read_host &&
        map->ram.write_host == ram->write_host &&
        map->ram.base == ram->base && map->ram.bytes == ram->bytes;
    uint64_t flush_delta = cpu->tlb_flushes - map->flushes;
    uint32_t generation_delta = cpu->tlb_gen - map->generation;
    if (!same_owner || cpu->tlb_flushes < map->flushes ||
        cpu->tlb_gen < map->generation ||
        flush_delta != generation_delta ||
        (cpu->tlb_gen == map->generation &&
         !controls_match(&map->cp15, &cpu->cp15)))
        memset(map->entries, 0, sizeof map->entries);
    map->ram = *ram;
    map->owner = cpu;
    map->cp15 = cpu->cp15;
    map->flushes = cpu->tlb_flushes;
    map->generation = cpu->tlb_gen;
    map->bound = true;
    return true;
}

bool arm_ram_map_publish(arm_ram_map_t *map, const arm_cpu_t *cpu,
                         uint32_t va, arm_access_t access) {
    if ((unsigned)access > ARM_ACCESS_FETCH ||
        !arm_ram_map_current(map, cpu)) return false;
    uint8_t *host = arm_ram_window_tlb_lookup(&map->ram, cpu, va, access, false);
    if (!host) return false;
    arm_ram_map_entry_t *entry = &map->entries[arm_ram_map_slot(va, access)];
    entry->host = host;
    entry->key = arm_ram_map_key(va, access, map->generation);
    return true;
}

uint8_t *arm_ram_map_lookup(const arm_ram_map_t *map, const arm_cpu_t *cpu,
                            uint32_t va, arm_access_t access) {
    if ((unsigned)access > ARM_ACCESS_FETCH ||
        !arm_ram_map_current(map, cpu)) return NULL;
    const arm_ram_map_entry_t *entry =
        &map->entries[arm_ram_map_slot(va, access)];
    return entry->key == arm_ram_map_key(va, access, map->generation)
        ? entry->host : NULL;
}
