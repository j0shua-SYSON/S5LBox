/* Partial N88/S5L8920 memory, interrupts, UART, timebase, GPIO and I2C.
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
#define S5L8920_NVRAM_PROXY_SIZE 8192u
#define S5L8920_GPIO_BASE UINT32_C(0x83000000)
#define S5L8920_GPIO_PIN_COUNT 368u
#define S5L8920_GPIO_IRQ_GROUPS 7u
#define S5L8920_GPIO_IRQ_PINS (32u * S5L8920_GPIO_IRQ_GROUPS)
#define S5L8920_GPIO_IRQ_STATUS UINT32_C(0x800)
#define S5L8920_GPIO_IRQ 94u
#define S5L8920_I2C_BASE UINT32_C(0x83200000)
#define S5L8920_I2C_STRIDE UINT32_C(0x100000)
#define S5L8920_I2C_COUNT 3u
#define S5L8920_I2C_CAPACITY 128u
#define S5L8920_I2C0_IRQ 19u /* Consecutive banks use sources 19, 18, 17. */

typedef struct {
    uint64_t sequence;
    uint8_t control, address, subaddress, length, programmed, status;
    uint8_t tx[S5L8920_I2C_CAPACITY], rx[S5L8920_I2C_CAPACITY];
    unsigned tx_count, rx_count, rx_cursor;
    bool active, write;
} s5l8920_i2c_t;

typedef struct {
    uint64_t sequence;
    uint8_t address, subaddress, length;
    bool write;
    uint8_t data[S5L8920_I2C_CAPACITY];
} s5l8920_i2c_request_t;

typedef struct {
    uint16_t control;
    bool programmed, input_valid, input_high;
} s5l8920_gpio_pin_t;

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
    s5l8920_gpio_pin_t gpio[S5L8920_GPIO_PIN_COUNT];
    uint32_t gpio_pending[S5L8920_GPIO_IRQ_GROUPS];
    bool gpio_irq;
    s5l8920_i2c_t i2c[S5L8920_I2C_COUNT];
} s5l8920_t;

/* Requires a zero-initialized object, freed before reuse. Allocates the matching
 * iBoot RAM geometry and selects Cortex-A8. CPU identity/ECC configuration,
 * clocks, ROM, storage and other peripherals remain incomplete; UART traffic
 * advances only with explicit source-clock input from its caller. Reset PC zero
 * is not redirected into a fabricated boot image. No S5L8900 HLE is installed. */
bool s5l8920_init(s5l8920_t *m);
void s5l8920_free(s5l8920_t *m);

/* Reset CPU/controller/UART/timer/I2C state and clear diagnostics, preserving RAM
 * and externally supplied interrupt levels/GPIO samples. GPIO programming is
 * invalidated. This is a functional reset, not a
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

/* Supply a digital sample for an ordinary GPIO pin (port * 8 + bit). No pin
 * configuration is inferred. Samples persist across functional
 * reset and preserve latched bus diagnostics. Call between CPU steps.
 * Word MMIO supports explicitly programmed, interrupt-masked input/output
 * controls: 0x210/0x212, data bit0, pull selection 0/0x80/0x100. Input
 * reads require a supplied sample; output reads return the programmed level.
 * Pulls and drive field0xc00 are retained without analog/floating-pin behavior.
 * Peripheral selectors0x20/0x40/0x60 are stored only with masked input mode;
 * reads use explicit samples, without simulating peripheral signal routing.
 * Interrupt-off mode0xe requires mask0x10 and no peripheral selector. Input
 * enable0x200 is optional for its writes; disabled-input reads remain refused
 * even with a sample. Unknown combinations and register bits remain refused.
 * Pins0..223 also support input IRQ modes 0x204 high, 0x206 low, 0x208 rising,
 * 0x20a falling and 0x20c either edge; bit0x10 masks delivery to source94.
 * Seven pending words at0x800..0x818 are W1C. Masked events latch; initial
 * samples cannot create edges. Active levels relatch on acknowledgement.
 * Configuration changes create no edges; pending survives until W1C/reset.
 * Functional reset clears pending. External source94 is ORed with GPIO.
 * This is logical sampling/latching, not measured phase or debounce timing. */
bool s5l8920_gpio_input(s5l8920_t *m, unsigned pin, bool high);

/* Bounded I2C host endpoint interface, called between CPU steps. Commands
 * expose a stable request; no slave ACK, status or RX data is synthesized.
 * Success reads require exactly length supplied bytes; writes and failures
 * require NULL/zero data. Only a matching active sequence can complete.
 * Functional reset discards requests while retaining the sequence counter,
 * so a delayed event cannot complete a later request. Invalid calls preserve
 * state and output; valid host events preserve a latched bus diagnostic.
 *
 * Byte and aligned word MMIO: target0, control8 (observed0/0x30/0xf0), W1C
 * status0xc, subaddress0x10, explicitly zero auxiliary0x14, length0x18,
 * byte FIFO0x20, start0x24 (4 read/5 write). Reads support status/FIFO only.
 * Transfers require all fields programmed, at most128 bytes, complete TX
 * data, no unread RX or pending status, and nonzero control. Staging/control
 * writes while active are refused. Status0x10 completes,0x20 reports error;
 * W1C mask0x37 clears modeled causes without discarding RX. Nonzero control
 * permits delivery, zero suppresses it, ORed with external sources19/18/17.
 * This is explicit logical scheduling, not physical clocks, arbitration,
 * measured power-on state or emulation of an attached PMU/sensor. */
bool s5l8920_i2c_request(const s5l8920_t *m, unsigned bus, s5l8920_i2c_request_t *request);
bool s5l8920_i2c_complete(s5l8920_t *m, unsigned bus, uint64_t sequence,
                         bool success, const uint8_t *data, size_t size);

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

/* Build an explicitly selected empty-variable N88 NVRAM handoff image using
 * the matching iBoot-1537.9.55 layout: generation 1, empty common partition,
 * header checksums and body Adler-32. Size must be S5L8920_NVRAM_PROXY_SIZE;
 * NULL or wrong sizes fail without writing. Unaligned buffers are supported.
 * No allocation, board mutation, device-tree installation or default selection.
 * This constructs fresh emulated configuration, not NOR hardware, persistence,
 * physical provisioning or a complete bootloader result. */
bool s5l8920_build_empty_nvram_proxy(void *data, size_t size);

#endif
