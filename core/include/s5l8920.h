/* Partial N88/S5L8920 memory, interrupt fabric, UART and timebase.
 * No complete firmware boot.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_H
#define S5LBOX_S5L8920_H

#include "arm.h"
#include "pl192.h"
#include "s5l8920_uart.h"
#include <stddef.h>

#define S5L8920_RAM_BASE UINT32_C(0x40000000)
#define S5L8920_RAM_SIZE UINT32_C(0x10000000)
/* Implemented extent of the explicitly selected low RAM boot window. */
#define S5L8920_RAM_BOOT_WINDOW S5L8920_RAM_SIZE
#define S5L8920_VIC_BASE UINT32_C(0xbf200000)
#define S5L8920_VIC_STRIDE UINT32_C(0x10000)
#define S5L8920_VIC_COUNT 3u
#define S5L8920_IRQ_COUNT (32u * S5L8920_VIC_COUNT)
#define S5L8920_UART0_BASE UINT32_C(0x82500000)
#define S5L8920_UART0_IRQ 24u
#define S5L8920_PMGR_BASE UINT32_C(0xbf100000)
#define S5L8920_TIMEBASE_LOW UINT32_C(0x200)
#define S5L8920_TIMEBASE_HIGH UINT32_C(0x204)
#define S5L8920_DEADLINE_COUNT UINT32_C(0x208)
#define S5L8920_DEADLINE_CONTROL UINT32_C(0x220)
#define S5L8920_DEADLINE_IRQ 6u

typedef struct {
    uint32_t remaining;
    bool programmed, enabled, expired, pending;
} s5l8920_deadline_t;

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
    s5l8920_uart_t uart0;
    uint64_t timebase_ticks;
    uint32_t input_levels[S5L8920_VIC_COUNT];
    s5l8920_bus_failure_t bus_failure;
    bool ram_boot_window;
    s5l8920_deadline_t deadline;
} s5l8920_t;

/* Requires a zero-initialized object, freed before reuse. Allocates the matching
 * iBoot RAM geometry and selects Cortex-A8. CPU identity/ECC configuration,
 * clocks, ROM, storage and other peripherals remain incomplete; UART traffic
 * advances only with explicit source-clock input from its caller. Reset PC zero
 * is not redirected into a fabricated boot image. No S5L8900 HLE is installed. */
bool s5l8920_init(s5l8920_t *m);
void s5l8920_free(s5l8920_t *m);

/* Reset CPU/controller/UART/timer state and clear diagnostics while preserving RAM
 * and externally driven interrupt levels. This is a functional reset, not a
 * model of power sequencing. The caller owns execution and device timing. */
bool s5l8920_reset(s5l8920_t *m);
bool s5l8920_set_irq(s5l8920_t *m, unsigned source, bool asserted);

/* Prepare the inherited RAM boot mapping selected by BF100000[1:0]=2 in
 * matching iBoot. Enabled low addresses cover the installed RAM and share
 * its storage at RAM_BASE. This models the RAM selection only, not the full
 * remap register, ROM/SRAM selections, or physical reset sequencing. The
 * implemented extent is bounded to RAM_SIZE; larger hardware decode ranges
 * are not established. A functional reset removes this explicit preparation.
 * A mapping change invalidates CPU translation/host-pointer caches and the
 * exclusive monitor, preserving registers, RAM and latched bus diagnostics.
 * Call between CPU steps; BF100000 MMIO remains unavailable. */
bool s5l8920_set_ram_boot_window(s5l8920_t *m, bool enabled);

/* Board-owned UART input advances refresh the real interrupt fabric before
 * returning. Use these instead of advancing the embedded component directly
 * when interrupt delivery matters. No source frequency, gating or conversion
 * from CPU instructions to UART cycles is invented here. Failed operations
 * preserve UART/output/interrupt state; external input24 is ORed with UART0.
 * Like set_irq, these host events may repair a latched CPU bus stop. */
bool s5l8920_uart0_clock(s5l8920_t *m, bool nclk, uint64_t ticks,
                        uint8_t *output, size_t capacity, size_t *count);
bool s5l8920_uart0_receive(s5l8920_t *m, uint8_t byte);

/* Supply timebase source ticks explicitly, modulo 2^64, and advance the enabled
 * deadline countdown. Reads and CPU steps do not advance time. A programmed
 * interval produces one latched source6 event after that many supplied ticks;
 * zero waits for the next supplied tick. This is logical deadline timing, not
 * measured bus/clock phase. Reprogramming replaces the interval without
 * acknowledging an existing event. Control bit0 runs/stops the countdown;
 * bit1 acknowledges the event. Other bits and control reads remain refused.
 * Unprogrammed or expired count reads are refused: physical reset count and
 * post-expiry underflow/reload/readback are not established. Acknowledgement
 * alone does not rearm an expired interval. External source6 is ORed with the
 * timer cause. Functional reset disables/unprograms the deadline and starts
 * the timebase at zero. No physical reset phase, source frequency or gating
 * is inferred. Like other host events this preserves a latched bus diagnostic. */
bool s5l8920_timebase_clock(s5l8920_t *m, uint64_t ticks);

/* Host preparation is bounded to RAM and never performs MMIO. Rejected loads
 * leave RAM and existing bus diagnostics untouched. No firmware is patched. */
bool s5l8920_load(s5l8920_t *m, uint32_t address, const void *data, size_t size);
void s5l8920_clear_bus_failure(s5l8920_t *m);

#endif
