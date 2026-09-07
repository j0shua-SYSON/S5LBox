/* Host-only write witnesses for reusable read-only execution. */
#ifndef S5LBOX_ARM_RAM_WATCH_H
#define S5LBOX_ARM_RAM_WATCH_H
#include "arm.h"

typedef struct arm_ram_watch arm_ram_watch_t;

/* The owner must revoke old full-window WRITE grants before installing this
 * watch. Thereafter direct-write grants must refuse watched pages, and every
 * other RAM publisher must call changed before writing. CPU/device/host work
 * is serialized. This is a consent contract, not OS memory protection.
 * No witness may outlive its owner or a RAM replacement. */
arm_ram_watch_t *arm_ram_watch_create(uint8_t *ram, uint32_t base, uint32_t size);
void arm_ram_watch_destroy(arm_ram_watch_t *watch);
void arm_ram_watch_reset(arm_ram_watch_t *watch);

/* A nonzero stamp identifies one physical 1 KiB page's unchanged bytes.
 * First capture revokes existing DWRITE aliases of that page. Stamps never
 * repeat in a watch's lifetime; exhaustion fails closed. The caller still
 * owns the current architectural READ mapping and ordinary RAM witness. */
uint64_t arm_ram_watch_capture(arm_ram_watch_t *watch, arm_cpu_t *cpu,
                               const uint8_t *page);
bool arm_ram_watch_unwatched(const arm_ram_watch_t *watch,
                             uint32_t pa, uint32_t length);
void arm_ram_watch_changed(arm_ram_watch_t *watch,
                           uint32_t pa, uint32_t length);
#endif
