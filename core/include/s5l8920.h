/* Partial N88/S5L8920 memory, interrupts, UART, timebase, GPIO and I2C.
 * No complete firmware boot.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_H
#define S5LBOX_S5L8920_H

#include "arm.h"
#include "pl192.h"
#include "s5l8920_uart.h"
#include "s5l8920_dart.h"
#include "s5l8920_spi.h"
#include "s5l8920_spi_flash.h"
#include "s5l8920_dsim.h"
#include "pinot_panel.h"
#include "s5l8920_swi.h"
#include "lis331dl.h"
#include "ak8973.h"
#include <stddef.h>

#define S5L8920_RAM_BASE UINT32_C(0x40000000)
#define S5L8920_RAM_SIZE UINT32_C(0x10000000)
/* Implemented extent of the explicitly selected low RAM boot window. */
#define S5L8920_RAM_BOOT_WINDOW S5L8920_RAM_SIZE
#define S5L8920_VIC_BASE UINT32_C(0xbf200000)
#define S5L8920_VIC_STRIDE UINT32_C(0x10000)
#define S5L8920_VIC_COUNT 3u
#define S5L8920_IRQ_COUNT (32u * S5L8920_VIC_COUNT)
#define S5L8920_DSIM_GATE 0x19u
#define S5L8920_SWI_GATE 0x0eu /* Actual provider mapping of logical DT gate0x35. */
#define S5L8920_PINOT_RESET_PIN 40u /* Original GPIO identifier0x500. */
#define S5L8920_UART0_BASE UINT32_C(0x82500000)
#define S5L8920_UART0_IRQ 24u
#define S5L8920_UART_COUNT 5u
#define S5L8920_UART_STRIDE UINT32_C(0x100000)
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
#define S5L8920_PMU_CONTROL_REGISTER 0x0du
#define S5L8920_PMU_ADC_CONTROL 0x30u
#define S5L8920_PMU_CONFIG_CONTROL 0x24u
#define S5L8920_PMU_EVENT_REGISTER 0x01u
#define S5L8920_PMU_IRQ_PIN 157u
#define S5L8920_PMU_BOOT_STATE_REGISTER 0x6fu
#define S5L8920_PMU_VOLTAGE_REGISTERS 6u
#define S5L8920_PMU_LDO_REGISTERS 14u
#define S5L8920_PMU_SAVED_REGISTER 0x60u
#define S5L8920_PMU_SAVED_COUNT 4u
#define S5L8920_CHIPID_BASE UINT32_C(0xbf500000)
#define S5L8920_CLOCK_GATE_BASE UINT32_C(0xbf100078)
#define S5L8920_CLOCK_GATE_COUNT 52u
#define S5L8920_CLOCK_SELECT_BASE UINT32_C(0xbf100010)
#define S5L8920_CLOCK_SELECT_COUNT 25u
#define S5L8920_PLL_BASE UINT32_C(0xbf100004)
#define S5L8920_PLL_COUNT 3u
#define S5L8920_POWERID UINT32_C(0xbf100158)
#define S5L8920_MIU_CONTROL S5L8920_PMGR_BASE
#define S5L8920_USB_PHY_BASE UINT32_C(0x86000000)
#define S5L8920_USB_BASE UINT32_C(0x86100000)
#define S5L8920_USB_CONTROL_COUNT 6u
#define S5L8920_AUDIO_NCO_BASE UINT32_C(0x84300014)
#define S5L8920_AUDIO_GATE 0x18u
#define S5L8920_DMC_BASE UINT32_C(0xbfc00000)
#define S5L8920_DMC_CONFIG_COUNT 36u

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
    bool inactive_valid, inactive_high;
} s5l8920_gpio_pin_t;

typedef struct {
    uint32_t remaining;
    bool programmed, enabled, expired, pending;
} s5l8920_deadline_t;

typedef struct {
    uint32_t initial, value;
    bool configured;
} s5l8920_clock_gate_t;

/* The 25 clock selectors accept aligned word programming before readback.
 * Ordinary configuration fields occupy bits0..11; index10 also uses16..23,
 * and index15 uses0..19. Other bits/access widths refuse atomically. The
 * firmware-written fields are retained as a logical configuration image,
 * not ready/busy/lock status. No reset values, output clocks, transient
 * update timing or PLL state are inferred. Reset invalidates programming. */
typedef struct {
    uint32_t value;
    bool programmed;
} s5l8920_clock_selector_t;

typedef struct {
    uint64_t settling_cycles, remaining;
    uint32_t initial, value, reference_hz;
    bool configured;
} s5l8920_pll_t;

typedef struct {
    uint32_t initial, value;
    bool configured;
} s5l8920_powerid_t;

typedef struct {
    uint32_t initial, value;
    bool configured;
} s5l8920_miu_t;

typedef struct {
    uint32_t initial, value;
    bool configured;
} s5l8920_usb_control_t;

typedef struct {
    uint64_t sequence;
    uint8_t control, result[2];
    bool programmed, result_valid;
} s5l8920_pmu_adc_t;

typedef struct {
    uint64_t sequence;
    uint8_t control; /* Channel bits0..3, start bit4, mode bit5, config bit7. */
} s5l8920_pmu_adc_request_t;

typedef struct {
    uint8_t control, selectors[3], selectors_programmed;
    bool control_programmed;
    uint8_t pins[8], pins_programmed;
} s5l8920_pmu_config_t;

typedef struct {
    uint32_t initial, pending, status, masks;
    uint8_t masks_programmed;
    bool configured, status_valid;
} s5l8920_pmu_events_t;

typedef struct {
    uint8_t initial[S5L8920_PMU_VOLTAGE_REGISTERS];
    uint8_t value[S5L8920_PMU_VOLTAGE_REGISTERS];
    uint8_t configured, programmed;
} s5l8920_pmu_voltage_t;

typedef struct {
    uint8_t initial[S5L8920_PMU_LDO_REGISTERS];
    uint8_t value[S5L8920_PMU_LDO_REGISTERS];
    uint16_t configured, programmed;
} s5l8920_pmu_ldo_t;

typedef struct {
    uint8_t initial[S5L8920_PMU_SAVED_COUNT], value[S5L8920_PMU_SAVED_COUNT];
    uint8_t configured, programmed;
} s5l8920_pmu_saved_t;

/* Observed NCO configuration only: the configured raw audio gate must be
 * fully open for access. Control writes 0/d00 and two full-width
 * coefficient words with readback after programming. Control/status reads
 * remain unavailable. No source frequency, output clock, readiness or DMA
 * behavior is inferred. Closing the gate preserves programming; functional
 * reset invalidates it and restores the supplied gate state. */
typedef struct {
    uint32_t control, coefficient[2];
    uint8_t programmed; /* Control bit0, coefficient A bit1, B bit2. */
} s5l8920_audio_nco_t;

typedef enum {
    S5L8920_DMC_CONFIG = 0,
    S5L8920_DMC_READY = 1,
    S5L8920_DMC_PAUSED = 2,
    S5L8920_DMC_LOW_POWER = 3
} s5l8920_dmc_state_t;

/* PL340 configuration phase for the observed 32-bit LPDDR/two-chip setup.
 * Indices0..17 cover offsets0x0c..0x50,18..33 cover QoS0..15,34..35 chips0..1.
 * No target tie-off values are supplied; words become readable after writes.
 * Reset enters Config and invalidates words. State-changing/direct commands,
 * status, PHY and live DRAM transactions are not yet implemented. */
typedef struct {
    uint32_t value[S5L8920_DMC_CONFIG_COUNT];
    uint64_t programmed;
    s5l8920_dmc_state_t state;
} s5l8920_dmc_t;

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
    bool pmu_rtc_configured;
    uint32_t pmu_rtc_counter, pmu_rtc_offset;
    uint32_t chipid_words[4];
    uint8_t chipid_configured;
    s5l8920_clock_gate_t clock_gate[S5L8920_CLOCK_GATE_COUNT];
    s5l8920_clock_selector_t clock_selector[S5L8920_CLOCK_SELECT_COUNT];
    s5l8920_pll_t pll[S5L8920_PLL_COUNT];
    s5l8920_powerid_t powerid;
    s5l8920_miu_t miu;
    s5l8920_usb_control_t usb_control[S5L8920_USB_CONTROL_COUNT];
    s5l8920_uart_t uart_extra[S5L8920_UART_COUNT-1u];
    uint32_t uart_divisor_initial[S5L8920_UART_COUNT];
    bool uart_divisor_configured[S5L8920_UART_COUNT];
    bool pmu_control_configured;
    uint8_t pmu_control_initial, pmu_control_value;
    s5l8920_pmu_adc_t pmu_adc;
    s5l8920_pmu_config_t pmu_config;
    s5l8920_pmu_events_t pmu_events;
    uint8_t pmu_boot_initial, pmu_boot_value;
    bool pmu_boot_configured, pmu_boot_programmed;
    s5l8920_pmu_voltage_t pmu_voltage;
    s5l8920_pmu_ldo_t pmu_ldo;
    s5l8920_pmu_saved_t pmu_saved;
    s5l8920_audio_nco_t audio_nco;
    s5l8920_dmc_t dmc;
    s5l8920_dart_t dart[S5L8920_DART_COUNT];
    s5l8920_spi_t spi[S5L8920_SPI_COUNT];
    sst25vf080b_t *spi_flash[S5L8920_SPI_COUNT];
    unsigned spi_flash_cs[S5L8920_SPI_COUNT], spi_flash_gate[S5L8920_SPI_COUNT];
    uint8_t spi_flash_bias[S5L8920_SPI_COUNT], spi_flash_bias_known[S5L8920_SPI_COUNT];
    s5l8920_dsim_t *dsim;
    pinot_panel_t *pinot;
    s5l8920_swi_t *swi;
} s5l8920_t;

/* Requires a zero-initialized object, freed before reuse. Allocates the matching
 * iBoot RAM geometry and selects Cortex-A8. CPU identity/ECC configuration,
 * clocks, ROM, storage and other peripherals remain incomplete; UART traffic
 * advances only with explicit source-clock input from its caller. Reset PC zero
 * is not redirected into a fabricated boot image. No S5L8900 HLE is installed. */
bool s5l8920_init(s5l8920_t *m);
void s5l8920_free(s5l8920_t *m);

/* Explicitly attach a caller-owned, configured DSIM to its physical aperture
 * and raw gate19. The gate must have an explicit initial observation. Keep the
 * peer alive and exclusively owned by this connection. Identical attachment
 * preserves state; replacement refuses. Board reset/free detach without
 * resetting or freeing the peer. No attachment is installed by default.
 * Only aligned words00..7c are routed; component refusals remain checked bus
 * failures. Closed gates refuse MMIO rather than supplying invented values.
 * IRQ, panel and clock-source/divider routing are not established here. */
bool s5l8920_dsim_attach(s5l8920_t *m, s5l8920_dsim_t *dsim);
/* Supply independent elapsed source cycles. Gate nibble0 pauses progress,
 * f permits it; unknown/intermediate gate state refuses without advancing.
 * Register reads and CPU steps never supply clocks automatically. Escape
 * cycles require the optional packet link and are independent of PHY cycles. */
bool s5l8920_dsim_board_system_clock(s5l8920_t *m, uint64_t cycles);
bool s5l8920_dsim_board_phy_clock(s5l8920_t *m, uint64_t cycles);
bool s5l8920_dsim_board_escape_clock(s5l8920_t *m, uint64_t cycles);

/* Optional caller-owned panel on the decoded DSIM link and GPIO40 reset.
 * Requires an attached packet controller and a known GPIO output level.
 * The caller supplies stable power and elapsed panel time independently;
 * this does not infer the regulator's electrical state. GPIO writes drive
 * reset immediately; changing the attached reset pin to an unknown/non-output
 * source refuses. Duplicate attachment preserves state, replacement refuses.
 * Board reset/free detach without changing either borrowed device.
 * Service uses the existing raw DSIM gate; closed pauses, intermediate refuses.
 * Neither a CPU step, read, service nor GPIO write advances panel time. */
bool s5l8920_pinot_attach(s5l8920_t *m, pinot_panel_t *panel);
bool s5l8920_pinot_service(s5l8920_t *m);

/* Optional borrowed SWI controller on raw physical gate0e. Attachment needs
 * explicit controller and gate configuration; duplicate preserves progress,
 * replacement refuses. Closed gate pauses clocks and refuses MMIO. Intermediate
 * gate states refuse. Reset/free detach without changing the borrowed peer.
 * Source cycles are explicit NCLK input; no CPU-step or clock-source inference. */
bool s5l8920_swi_attach(s5l8920_t *m, s5l8920_swi_t *swi);
bool s5l8920_swi_board_source_clock(s5l8920_t *m, uint64_t cycles);

/* Reset CPU/controller/UART/timer/I2C state and clear diagnostics, preserving RAM
 * and externally supplied interrupt levels/GPIO samples and configured PMU
 * clock/offset state and explicitly configured identification words. Gate controls
 * and POWERID return to their supplied initial words. Configured MIU control
 * restores its supplied initial word and supported mapping. USB controls restore
 * their supplied words. PLL controls return to
 * their supplied disabled words, cancelling settling. GPIO and clock-selector programming
 * and audio NCO/DMC programming are invalidated. DMC returns to Config.
 * DART restores supplied initial observations and invalidates segment tables.
 * This is a functional reset, not a
 * model of power sequencing. The caller owns execution and device timing. */
bool s5l8920_reset(s5l8920_t *m);
bool s5l8920_set_irq(s5l8920_t *m, unsigned source, bool asserted);

/* Prepare the inherited RAM boot mapping selected by BF100000[1:0]=2 in
 * matching iBoot. Enabled low addresses cover the installed RAM and share
 * its storage at RAM_BASE. This models the RAM selection only, without
 * ROM/SRAM backing or physical reset sequencing. The
 * implemented extent is bounded to RAM_SIZE; larger hardware decode ranges
 * are not established. Without MIU configuration, a functional reset removes
 * this preparation. Configured MIU control owns the selection: a conflicting
 * request here refuses without changing state.
 * A mapping change invalidates CPU translation/host-pointer caches and the
 * exclusive monitor, preserving registers, RAM and latched bus diagnostics.
 * Call between CPU steps; this API does not configure BF100000 readback. */
bool s5l8920_set_ram_boot_window(s5l8920_t *m, bool enabled);

/* Explicit SPI word-link inputs use the component contract and refresh VIC
 * sources29-bank, ORed with external inputs. Failures preserve output/device/
 * interrupt state. Successful events preserve latched bus diagnostics and CPU
 * registers, while IRQ/FIQ reflect resulting causes. No clock-source conversion,
 * peripheral pin routing, NOR reply or reset threshold is inferred here.
 * Direct serial input refuses on a port with an attached flash peer. */
bool s5l8920_spi_bank_configure_link(s5l8920_t *m, unsigned bank, unsigned tx_low, unsigned rx_high);
bool s5l8920_spi_bank_serial_clock(s5l8920_t *m, unsigned bank, uint64_t cycles,
    const uint32_t *received, size_t received_count, uint32_t *transmitted,
    size_t capacity, size_t *count);
bool s5l8920_spi_bank_delay_clock(s5l8920_t *m, unsigned bank, uint64_t cycles);
/* Explicit wiring: caller owns initialized flash/image/pins and elapsed time.
 * GPIO must be a known plain output, gate independently configured, and SPI
 * control not yet programmed. Accepted GPIO writes drive CE; other pad modes
 * and midword CS edges refuse. Gate nibble0 pauses source input, f permits it.
 * Wiring/bias are explicit, not inferred from logical device-tree gate numbers.
 * Identical repeats preserve state; chip/CS sharing or replacement refuses.
 * Functional reset/free detach wiring without changing the borrowed chip.
 * No automatic clocks, image, power sequence or controller reset defaults. */
bool s5l8920_spi_attach_flash(s5l8920_t *m, unsigned bank, sst25vf080b_t *flash,
    unsigned cs_pin, unsigned gate, unsigned tx_low, unsigned rx_high,
    uint8_t bias_value, uint8_t bias_known);
/* Raw PCLK/NCLK source cycles to the bound chip, refreshing real IRQ/FIQ.
 * Word-delay PCLK and flash elapsed time are separately supplied in order.
 * CPU registers and latched bus diagnostics are preserved. */
bool s5l8920_spi_bank_flash_clock(s5l8920_t *m, unsigned bank, bool nclk,
    uint64_t cycles, size_t *count);

/* Board-owned UART input advances refresh the real interrupt fabric before
 * returning. Use these instead of advancing the embedded component directly
 * when interrupt delivery matters. No source frequency, gating or conversion
 * from CPU instructions to UART cycles is invented here. Failed operations
 * preserve UART/output/interrupt state; external input24 is ORed with UART0.
 * Like set_irq, these host events may repair a latched CPU bus stop. */
bool s5l8920_uart0_clock(s5l8920_t *m, bool nclk, uint64_t ticks,
                        uint8_t *output, size_t capacity, size_t *count);
bool s5l8920_uart0_receive(s5l8920_t *m, uint8_t byte);

/* Five independent banks at UART0_BASE + bank*UART_STRIDE, IRQ24-bank.
 * UART0 has no modem flow-control pins; its traffic does not require UMCON
 * programming. Other ports require that word and explicit CTS for automatic
 * flow control. The uart0 wrappers above retain their bank0 behavior.
 * Clock inputs remain external: no clock-gate index or frequency is inferred.
 * CTS and receive-timeout inputs have the component contracts above; reset
 * clears observations/traffic, restores only explicit initial divisors, and
 * free/init invalidates initial inputs. CPU registers and diagnostics remain
 * unchanged by successful events; IRQ/FIQ reflect resulting device causes. */
bool s5l8920_uart_bank_clock(s5l8920_t *m, unsigned bank, bool nclk, uint64_t ticks,
                            uint8_t *output, size_t capacity, size_t *count);
bool s5l8920_uart_bank_receive(s5l8920_t *m, unsigned bank, uint8_t byte);
bool s5l8920_uart_bank_cts(s5l8920_t *m, unsigned bank, bool asserted);
bool s5l8920_uart_bank_receive_timeout(s5l8920_t *m, unsigned bank);
/* Supply an initial divisor before its first guest write. Known divider and
 * sample fields only (rates8..16); no reset value is inferred. Repeating the
 * same input preserves guest programming; changing it refuses. Call between
 * CPU steps. Other UART configuration/traffic and latched failures persist. */
bool s5l8920_uart_divisor_configure(s5l8920_t *m, unsigned bank, uint32_t initial);

/* Supply a digital sample for an ordinary GPIO pin (port * 8 + bit). No pin
 * configuration is inferred. Each sample invalidates any inactive readback
 * observation for that pin. Samples persist across functional
 * reset and preserve latched bus diagnostics. Call between CPU steps.
 * Word MMIO supports explicitly programmed, interrupt-masked input/output
 * controls: 0x210/0x212, data bit0, pull selection 0/0x80/0x100. Input
 * reads require a supplied sample; output reads return the programmed level.
 * Pulls and drive field0xc00 are retained without analog/floating-pin behavior.
 * Peripheral selectors0x20/0x40/0x60 preserve masked input/output/off modes
 * and either input-enable setting. Enabled reads use explicit pad samples,
 * even when the preserved low mode is output; no peripheral signal routing
 * or software-latch-to-pad connection is inferred. Disabled peripheral reads
 * remain refused. Interrupt-off mode0xe requires mask0x10. With no peripheral
 * selector, disabled-input off-mode reads accept the separate observation
 * below. Unknown combinations and register bits remain refused.
 * Pins0..223 also support input IRQ modes 0x204 high, 0x206 low, 0x208 rising,
 * 0x20a falling and 0x20c either edge; bit0x10 masks delivery to source94.
 * Seven pending words at0x800..0x818 are W1C. Masked events latch; initial
 * samples cannot create edges. Active levels relatch on acknowledgement.
 * Configuration changes create no edges; pending survives until W1C/reset.
 * Functional reset clears pending. External source94 is ORed with GPIO.
 * This is logical sampling/latching, not measured phase or debounce timing. */
bool s5l8920_gpio_input(s5l8920_t *m, unsigned pin, bool high);

/* Supply bit0 readback for a programmed interrupt-off GPIO pin whose input
 * enable and peripheral selector are clear. The firmware reads its control
 * word before enabling input, but
 * the inactive data bit is not inferred from live samples or guest writes.
 * Aligned word reads retain the programmed control fields and this supplied
 * bit. This is an explicit logical observation, not a physical sampler/latch
 * transfer model. Repeated reads retain it; every accepted configuration
 * write, gpio_input event and functional reset invalidates it. Free clears it.
 * Missing observations refuse. Calls for unprogrammed/active pins refuse
 * without mutation. Valid calls replace only this observation, preserving
 * CPU, interrupt state and latched diagnostics. Call between CPU steps. */
bool s5l8920_gpio_inactive_readback(s5l8920_t *m, unsigned pin, bool high);

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

/* Service one matching request using a caller-owned, explicitly initialized
 * LIS331DL. No attachment, scheduling, device clock or default sensor is added.
 * Unknown device operations remain pending with device/controller unchanged.
 * Successful transfers use normal FIFO/completion/IRQ semantics. The external
 * device survives controller reset/free and must outlive this call only. */
bool s5l8920_lis331dl_service(s5l8920_t *m, unsigned bus, uint64_t sequence,
                            lis331dl_t *device);

/* Same explicit transaction boundary for a caller-owned AK8973. Unknown
 * EEPROM bytes or unsupported operations leave both participants unchanged.
 * No attachment, device clock, reset pin or sensor interrupt is inferred. */
bool s5l8920_ak8973_service(s5l8920_t *m, unsigned bus, uint64_t sequence,
                           ak8973_t *device);

/* Bounded D1755 clock endpoint at I2C0 address0x74. Explicit configuration
 * supplies a raw32-bit counter and offset; it is refused while bus0 has an
 * active request, pending status or unread RX. No revision, battery, power,
 * alarm or other PMU register is inferred. Exact4-byte little-endian reads
 * at0x4c/0x64 and writes at0x64 are supported. Other requests remain pending.
 * Service completes only the specified active sequence, sampling the counter
 * at that host call. Advance adds caller-supplied raw units modulo2^32;
 * CPU execution, I2C reads and board timebase ticks do not advance this clock.
 * No physical rate or epoch is assumed. Calls run between CPU steps, preserve
 * bus diagnostics and reject invalid inputs without mutation. Functional
 * reset retains this explicitly configured domain; free/init invalidates it.
 * This is a logical reset policy, not measured backup-power behavior. */
bool s5l8920_pmu_rtc_configure(s5l8920_t *m, uint32_t counter, uint32_t offset);
bool s5l8920_pmu_rtc_advance(s5l8920_t *m, uint32_t units);
bool s5l8920_pmu_rtc_service(s5l8920_t *m, uint64_t sequence);

/* Explicit D1755 control byte at I2C0/0x74 register0x0d. Matching firmware
 * writes/reads this byte and changes bit4 while preserving the other bits.
 * Only that change is supported; power-transition and unknown-bit changes
 * remain pending, without ACK. No physical power effect/readiness is inferred.
 * Configure once with an observed initial byte before active/pending/unread
 * I2C0 traffic. Identical repeated configuration is an idempotent no-op that
 * preserves guest programming. Exact one-byte requests require explicit service
 * with their current sequence; all other requests are untouched. Functional
 * SoC reset preserves this external PMU domain, like the RTC, and cancels I2C
 * traffic. This is a logical policy, not measured backup-power behavior.
 * Free/init invalidates it. Calls occur between CPU steps and preserve CPU
 * registers and latched diagnostics; service may change the I2C IRQ level. */
bool s5l8920_pmu_control_configure(s5l8920_t *m, uint8_t initial);
bool s5l8920_pmu_control_service(s5l8920_t *m, uint64_t sequence);

/* D1755 ADC endpoint on I2C0/74: exact byte control30 read/write and two-byte
 * result31/32 read. Guest programming establishes control; bit6 is unsupported.
 * A start write creates a conversion token; another start while busy refuses.
 * A write without start cancels it. Every accepted control write invalidates
 * the old result. Reads never advance a conversion. Only explicit completion
 * with the current token supplies both raw bytes and clears busy. Consumers
 * decode (low & 3) | (high << 2); upper low-byte bits are supplied, not invented.
 * No analog values, calibration or timing are inferred. With the optional
 * event domain configured, explicit completion latches register02 bit5;
 * programmed masks and GPIO/VIC configuration determine interrupt delivery.
 * Functional SoC reset retains the external PMU state/token, cancelling only
 * controller transactions. Free/init invalidates it. Tokens never wrap.
 * Call between CPU steps. Failures preserve state and request output. Valid
 * events preserve CPU registers/diagnostics; I2C and PMU events can change
 * interrupt levels. */
bool s5l8920_pmu_adc_service(s5l8920_t *m, uint64_t sequence);
bool s5l8920_pmu_adc_request(const s5l8920_t *m, s5l8920_pmu_adc_request_t *request);
bool s5l8920_pmu_adc_complete(s5l8920_t *m, uint64_t sequence, uint8_t low, uint8_t high);

/* Matching D1755 configuration at I2C0/74. Register24 supports only the
 * complete 0x2a configuration verified by the original bootloader. Other
 * values, including the kernel's power-related bit7 update, remain refused.
 * Registers59..5b hold eight packed three-bit fields: exact byte accesses or
 * a three-byte transfer beginning at59. Every read requires prior programming
 * of all requested bytes. Registers50..57 are independent pin configuration
 * bytes, supporting exact byte writes and retained reads after programming.
 * No initial values are inferred. These are logical
 * configuration images, without electrical routing or power effects.
 * Service only the matching active I2C transaction, preserving CPU registers
 * and diagnostics while refreshing the controller IRQ. Unknown requests leave
 * all state unchanged. Functional SoC reset retains external PMU programming;
 * free/init invalidates it. Call between CPU steps. */
bool s5l8920_pmu_config_service(s5l8920_t *m, uint64_t sequence);

/* D1755 logical events1..4, independent live status5..8 and masks9..0c.
 * Configure supplies the complete initial event image once; identical repeats
 * do not reload consumed events. Raise ORs explicit causes into that image.
 * Status replaces the independent supplied status image; reads retain it.
 * Supported event reads are byte02 or four bytes at01. Successful transfers
 * consume only the returned event bytes. This read-clear model is inferred
 * from the matching level-interrupt handler's read/no-write-ACK contract and
 * saved wake-event merge, not physical read-phase/timing measurements.
 * Status uses exact four-byte reads at05. Masks support single bytes9..0c or
 * four bytes at09; reads require prior programming of all requested bytes.
 * Mask bits suppress delivery without discarding events. Once events and all
 * mask bytes are known, unmasked causes drive GPIO157 low, otherwise high.
 * GPIO pending/acknowledgement and VIC routing retain their normal behavior.
 * Explicit ADC completion raises register02 bit5 when events are configured.
 * No initial status/masks, spontaneous events or ADC completion are inferred.
 * Unsupported/stale requests refuse without mutation. Functional SoC reset
 * retains the external PMU domain; free/init clears it. Call between CPU steps. */
bool s5l8920_pmu_events_configure(s5l8920_t *m, uint32_t initial);
bool s5l8920_pmu_events_raise(s5l8920_t *m, uint32_t causes);
bool s5l8920_pmu_status_input(s5l8920_t *m, uint32_t status);
bool s5l8920_pmu_events_service(s5l8920_t *m, uint64_t sequence);

/* D1755 register6f software boot-state byte, used by matching LLB/kernel.
 * Exact byte reads require an explicit initial value or a successful guest
 * byte write. Configure is immutable/idempotent, never reloading writes; it
 * refuses first configuration after guest programming. Writes replace all
 * eight bits and reads retain them. No erased/power-on value, autonomous
 * flag changes, neighboring registers or power transitions are inferred.
 * Functional SoC reset retains this external PMU byte; free/init clears it.
 * Unsupported/stale requests preserve state. Call between CPU steps. */
bool s5l8920_pmu_boot_state_configure(s5l8920_t *m, uint8_t initial);
bool s5l8920_pmu_boot_state_service(s5l8920_t *m, uint64_t sequence);

/* D1755 saved software state60..63 at I2C0/74. Matching firmware stores boot
 * stages, reasons, counters and flags here. Exact byte writes replace state;
 * reads retain it and require explicit initial state or a completed write.
 * Configure is immutable/idempotent and never reloads guest programming;
 * first configuration after an unconfigured guest write refuses. Register6f
 * stays in the independent boot-state endpoint above. No RTC64..67, other GP
 * bytes, defaults, automatic counters, watchdog or reset effects are inferred.
 * Unknown/stale requests preserve all state. SoC reset retains these external
 * bytes; free/init clears them. Call between CPU steps; CPU registers and bus
 * diagnostics are preserved while I2C completion refreshes its interrupt. */
bool s5l8920_pmu_saved_configure(s5l8920_t *m, unsigned reg, uint8_t initial);
bool s5l8920_pmu_saved_service(s5l8920_t *m, uint64_t sequence);

/* D1755 voltage configuration bytes14,23,2c..2f at I2C0/74. These addresses
 * remain independent: matching kernel initialization and setters use different
 * preset offsets. No alias or preset numbering is inferred. Configure supplies
 * one immutable initial byte; identical repeats never reload guest writes.
 * First configuration after programming refuses. Reads require known state.
 * Voltage bytes14/2c..2f update low5 while preserving known upper bits. Full
 * writes with zero upper bits are also supported at14/2f, as used by LLB.
 * Register23 requires initial state and supports retaining/setting bits6/7;
 * clearing them or changing other bits remains unimplemented. Only exact byte
 * transactions with the current sequence complete. Unknown requests preserve
 * all state. These are retained settings, not analog voltage, settling, ready
 * status or power transitions. Functional SoC reset retains the external PMU
 * domain; free/init invalidates it. Call between CPU steps; CPU registers and
 * bus diagnostics are preserved while the controller IRQ is refreshed. */
bool s5l8920_pmu_voltage_configure(s5l8920_t *m, unsigned reg, uint8_t initial);
bool s5l8920_pmu_voltage_service(s5l8920_t *m, uint64_t sequence);

/* D1755 LDO settings17..21, selector22 and enable fields10/11 at I2C0/74.
 * Configure supplies immutable initial bytes; identical repeats do not reload
 * guest writes, and first configuration after programming refuses. Exact byte
 * reads require known state. Writes preserve bits outside the original kernel
 * table's fields. The matching LLB's complete configuration bytes at17..21
 * and10/11 may also establish/replace state; no complete write is inferred
 * for22. Raw field encodings are retained without clamping or translating
 * them into physical voltages. Unsupported fields/shapes and stale requests
 * remain pending without mutation. SoC reset retains these external settings;
 * free/init invalidates them. No defaults, settling, readiness or power effects
 * are inferred. Call between CPU steps; CPU registers and diagnostics are
 * preserved while the I2C completion refreshes the controller IRQ. */
bool s5l8920_pmu_ldo_configure(s5l8920_t *m, unsigned reg, uint8_t initial);
bool s5l8920_pmu_ldo_service(s5l8920_t *m, uint64_t sequence);

/* Supply one immutable identification word at offset0/4/8/12. Only aligned
 * word reads in this 16-byte span are modeled; each requires its own explicit
 * value. No fuse defaults, unique identifier, revision or reserved-bit values
 * are inferred, and no CPU identity or device-tree property is changed.
 * Reapplying an identical value succeeds; replacing it fails. Guest writes,
 * other widths and unconfigured reads remain checked failures. Call between
 * CPU steps; configuration preserves CPU state and latched bus diagnostics.
 * Functional reset retains these inputs; free/init invalidates them. This
 * models fixed emulated inputs, not fuse programming or physical reset state. */
bool s5l8920_chipid_configure(s5l8920_t *m, unsigned offset, uint32_t value);

/* Supply the initial raw word for one of 52 clock gates. Each gate requires
 * independent configuration before reads or writes. Identical reapplication
 * preserves guest programming; replacing the supplied initial word fails.
 * Aligned guest word writes may select low nibble 0/f only, preserving all
 * upper bits. Other widths, mixed modes, reset-bit changes and unconfigured
 * accesses refuse. Functional reset restores these inputs; free/init clears
 * them. Call between CPU steps; CPU and latched diagnostics are preserved.
 * This models bounded programming state, not clock signal generation, source
 * readiness, connected-device reset effects or physical power-on values. */
bool s5l8920_clock_gate_configure(s5l8920_t *m, unsigned gate, uint32_t initial);

/* Three independently configured PLL words. Supply a disabled initial word,
 * reference frequency (zero means absent), and nonzero settling interval in
 * reference cycles. These are explicit model inputs, not measured reset values
 * or acquisition timing. Supported programming is bit30, multiplier8..15,
 * divisor20..25, shift1..3 and enable0. Enabled programming requires bit30 and
 * nonzero multiplier/divisor. Bit17 is read-only status; writes cannot set it.
 * Unknown bits, widths, alignment and unconfigured accesses refuse atomically.
 * Every accepted enabled write restarts settling; disabling cancels it.
 * Only supplied reference cycles with a nonzero reference rate can complete
 * settling. Reads, CPU steps and timebase ticks do not advance this state.
 * Identical configuration preserves guest state; conflicting input refuses.
 * Functional reset restores disabled inputs; free/init invalidates them.
 * Host calls preserve CPU/latched diagnostics and run between CPU steps.
 *
 * Rate query returns numerator/denominator Hz after settling, 0/1 if disabled,
 * and refuses absent/pending/unconfigured output without changing outputs.
 * This is a logical timing/rational-rate model, not analog lock, VCO waveforms,
 * jitter or physical timing. Outputs do not drive other board clocks yet. */
bool s5l8920_pll_configure(s5l8920_t *m, unsigned pll, uint32_t initial,
                          uint32_t reference_hz, uint64_t settling_cycles);
bool s5l8920_pll_reference_clock(s5l8920_t *m, unsigned pll, uint64_t cycles);
bool s5l8920_pll_rate(const s5l8920_t *m, unsigned pll,
                     uint64_t *numerator, uint32_t *denominator);

/* Supply the initial POWERID word explicitly. Aligned word accesses retain
 * the upper three software cache bytes and flags0/1; guest changes to bits2..7
 * refuse atomically, preserving these supplied unknown fields. Other widths,
 * misalignment and unconfigured accesses refuse. No identity, GPIO samples,
 * entropy, power behavior or field relationships are generated. Identical
 * configuration preserves guest writes; conflicting initial input refuses.
 * Functional reset restores the supplied initial word, not measured warm or
 * power reset behavior. Free/init invalidates it. Call between CPU steps;
 * CPU state and latched diagnostics are preserved. */
bool s5l8920_powerid_configure(s5l8920_t *m, uint32_t initial);

/* Supply the MIU control word explicitly; no hardware reset word is assumed.
 * Aligned word writes may select field1 or field2 in bits0..1, preserving all
 * supplied upper bits. Field2 aliases installed RAM at physical zero; other
 * selections remove that RAM window. Their boot sources remain unimplemented,
 * so low accesses refuse instead of using invented ROM/SRAM contents. Unknown
 * upper-bit changes, other written modes, widths and unconfigured accesses
 * refuse atomically. No readiness, power sequencing or transition delay is
 * synthesized. Identical configuration preserves guest writes; a different
 * initial word refuses. Functional reset restores the supplied word and its
 * supported mapping. Free/init invalidates it. Call between CPU steps; mapping
 * changes invalidate translation/host caches and the exclusive monitor while
 * preserving registers, RAM and latched diagnostics. */
bool s5l8920_miu_configure(s5l8920_t *m, uint32_t initial);

/* Supply one aligned USB clock/PHY control word at its physical address.
 * Supported words: controller+E00 clock gates (bits0..1), PHY+0 power (0..4),
 * PHY+4 reference-clock selection (0..1), PHY+8 reset request (0), PHY+1C
 * configuration (1..2), and PHY+44 tuning. Writes preserve all unknown bits;
 * tuning changes accept only the matching E3F field pattern. Identical writes
 * preserve state. Unconfigured words, other widths/registers and unsupported
 * bit changes refuse. Configuration is idempotent only for the same initial
 * word; functional reset restores supplied words and free/init invalidates them.
 * These are logical control requests, not analog power/reset completion or
 * readiness. No cable, lock, FIFO, endpoint, transfer, interrupt or clock
 * progress is synthesized; those controller accesses remain refused in every
 * power/clock state. Call between CPU steps; CPU/IRQ and latched diagnostics
 * are preserved. */
bool s5l8920_usb_control_configure(s5l8920_t *m, uint32_t address, uint32_t initial);

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
