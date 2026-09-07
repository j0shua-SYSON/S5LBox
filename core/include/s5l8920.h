/* Partial N88/S5L8920 memory and interrupt fabric. No complete firmware boot.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_H
#define S5LBOX_S5L8920_H

#include "arm.h"
#include "pl192.h"
#include <stddef.h>

#define S5L8920_RAM_BASE UINT32_C(0x40000000)
#define S5L8920_RAM_SIZE UINT32_C(0x10000000)
#define S5L8920_VIC_BASE UINT32_C(0xbf200000)
#define S5L8920_VIC_STRIDE UINT32_C(0x10000)
#define S5L8920_VIC_COUNT 3u
#define S5L8920_IRQ_COUNT (32u * S5L8920_VIC_COUNT)

typedef enum {
    S5L8920_BUS_OK = 0,
    S5L8920_BUS_UNMAPPED,
    S5L8920_BUS_ACCESS_UNIMPLEMENTED,
    S5L8920_BUS_REGISTER_REFUSED
} s5l8920_bus_reason_t;

typedef struct {
    s5l8920_bus_reason_t reason;
    uint32_t address, pc, value;
    unsigned size;
    bool write;
} s5l8920_bus_failure_t;

typedef struct {
    arm_cpu_t cpu;
    arm_bus_t bus;
    uint8_t *ram;
    pl192_t vic[S5L8920_VIC_COUNT];
    uint32_t input_levels[S5L8920_VIC_COUNT];
    s5l8920_bus_failure_t bus_failure;
} s5l8920_t;

/* Requires a zero-initialized object, freed before reuse. Allocates the matching
 * iBoot RAM geometry and selects Cortex-A8. CPU identity/ECC configuration,
 * clocks, ROM, storage and other peripherals remain incomplete; reset PC zero
 * is not redirected into a fabricated boot image. No S5L8900 HLE is installed. */
bool s5l8920_init(s5l8920_t *m);
void s5l8920_free(s5l8920_t *m);

/* Reset CPU/controller state and clear host diagnostics while preserving RAM
 * and externally driven interrupt levels. This is a functional reset, not a
 * model of power sequencing. The caller owns execution and device timing. */
bool s5l8920_reset(s5l8920_t *m);
bool s5l8920_set_irq(s5l8920_t *m, unsigned source, bool asserted);

/* Host preparation is bounded to RAM and never performs MMIO. Rejected loads
 * leave RAM and existing bus diagnostics untouched. No firmware is patched. */
bool s5l8920_load(s5l8920_t *m, uint32_t address, const void *data, size_t size);
void s5l8920_clear_bus_failure(s5l8920_t *m);

#endif
