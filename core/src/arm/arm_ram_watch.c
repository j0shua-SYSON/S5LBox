/* See arm_ram_watch.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm_ram_watch.h"
#include <stdlib.h>
#include <string.h>

struct arm_ram_watch {
    uint8_t *ram;
    uint32_t base, size, pages;
    uint64_t sequence;
    uint64_t stamp[];
};

arm_ram_watch_t *arm_ram_watch_create(uint8_t *ram, uint32_t base, uint32_t size) {
    if (!ram || !size || (base & 1023u) || (size & 1023u) ||
        (uint64_t)base + size > UINT64_C(0x100000000)) return NULL;
    uint32_t pages = size / 1024u;
    arm_ram_watch_t *watch = calloc(1u, sizeof *watch +
                                      (size_t)pages * sizeof(uint64_t));
    if (!watch) return NULL;
    watch->ram = ram; watch->base = base; watch->size = size; watch->pages = pages;
    return watch;
}

void arm_ram_watch_destroy(arm_ram_watch_t *watch) { free(watch); }

void arm_ram_watch_reset(arm_ram_watch_t *watch) {
    if (watch) memset(watch->stamp, 0, (size_t)watch->pages * sizeof(uint64_t));
}

static uint64_t next_stamp(arm_ram_watch_t *watch) {
    if (watch->sequence == UINT64_MAX) return 0u;
    return ++watch->sequence;
}

uint64_t arm_ram_watch_capture(arm_ram_watch_t *watch, arm_cpu_t *cpu,
                               const uint8_t *page) {
    if (!watch || !cpu || !page || watch->sequence == UINT64_MAX) return 0u;
    uintptr_t address = (uintptr_t)page, first = (uintptr_t)watch->ram;
    if (address < first || address - first >= watch->size ||
        ((address - first) & 1023u)) return 0u;
    uint32_t index = (uint32_t)((address - first) / 1024u);
    if (!watch->stamp[index]) {
        watch->stamp[index] = next_stamp(watch);
        /* Aliases are keyed by VA, so revoke by the proved physical pointer,
         * not by the particular VA through which the read was captured. */
        for (size_t i = 0u; i < sizeof cpu->dwrite / sizeof cpu->dwrite[0]; i++)
            if (cpu->dwrite[i].host == page)
                memset(&cpu->dwrite[i], 0, sizeof cpu->dwrite[i]);
    }
    return watch->stamp[index];
}

bool arm_ram_watch_unwatched(const arm_ram_watch_t *watch,
                             uint32_t pa, uint32_t length) {
    if (!watch) return true;
    if (!length || pa < watch->base ||
        (uint64_t)pa - watch->base + length > watch->size) return false;
    uint32_t first = (pa - watch->base) / 1024u;
    uint32_t last = (uint32_t)(((uint64_t)pa - watch->base + length - 1u) / 1024u);
    for (uint32_t i = first; i <= last; i++)
        if (watch->stamp[i]) return false;
    return true;
}

void arm_ram_watch_changed(arm_ram_watch_t *watch,
                           uint32_t pa, uint32_t length) {
    if (!watch || !length) return;
    uint64_t first = pa, last = (uint64_t)pa + length;
    if (first < watch->base) first = watch->base;
    uint64_t end = (uint64_t)watch->base + watch->size;
    if (last > end) last = end;
    if (last <= first) return;
    uint32_t a = (uint32_t)((first - watch->base) / 1024u);
    uint32_t b = (uint32_t)((last - 1u - watch->base) / 1024u);
    for (uint32_t i = a; i <= b; i++)
        if (watch->stamp[i]) {
            uint64_t stamp = next_stamp(watch);
            /* Zero means unobserved and would re-grant stores. Keep exhausted
             * pages watched, while capture refuses all further stamps. */
            watch->stamp[i] = stamp ? stamp : UINT64_MAX;
        }
}
