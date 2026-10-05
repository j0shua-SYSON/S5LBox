/* Samsung DSIM clock programming, separate from the display/packet engine.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_DSIM_H
#define S5LBOX_S5L8920_DSIM_H
#include <stdbool.h>
#include <stdint.h>

#define S5L8920_DSIM_BASE UINT32_C(0x89000000)

typedef struct {
    uint32_t reference_hz;
    uint32_t idle_status, clkctrl, pllctrl, plltmr;
} s5l8920_dsim_clock_input_t;

typedef struct {
    s5l8920_dsim_clock_input_t initial;
    uint32_t clkctrl, pllctrl, plltmr, remaining;
    bool configured, stable, stable_event, timer_readable;
} s5l8920_dsim_clock_t;

/* Explicit powered, idle inputs, not inferred silicon reset defaults. The
 * reference clock must be present and remain unchanged. Initial PLL, byte,
 * lane, bypass and HS clocks must be off; idle_status may contain only the
 * supplied reset-release and stop-state bits (20,8,3..0). The component does
 * not establish any of these inputs from the target's boot state.
 * Configure a zero object once; identical repeats preserve running state.
 * Reset restores the supplied inputs and cancels pending completion/events. */
bool s5l8920_dsim_clock_configure(s5l8920_dsim_clock_t *d,
    const s5l8920_dsim_clock_input_t *input);
void s5l8920_dsim_clock_reset(s5l8920_dsim_clock_t *d);

/* Aligned word offsets: STATUS00, CLKCTRL08, PLLCTRL4c, PLLTMR50. CLKCTRL
 * accepts the escape prescaler and its enable only, while all output clocks
 * remain disabled. PLLCTRL accepts band/PMS with DpDnSwap=0; nonzero P and M
 * are required when enabled. Retuning/timer writes require PLL disabled.
 * Reads never advance time. After enable, PLLTMR readback remains unknown:
 * the available evidence does not distinguish a live counter from a reload
 * latch. Disabling alone does not restore its read observation; rewriting it
 * does. Refused accesses preserve state and output.
 *
 * Software/functional reset, lane control, PHY tuning, packet/FIFO ports,
 * interrupt registers and the rest of the aperture remain unsupported. This
 * component alone cannot provide the kernel's complete register snapshot. */
bool s5l8920_dsim_clock_read(const s5l8920_dsim_clock_t *d,
    uint32_t offset, uint32_t *value);
bool s5l8920_dsim_clock_write(s5l8920_dsim_clock_t *d,
    uint32_t offset, uint32_t value);

/* Supply actual elapsed DSIM system-clock cycles, independent of CPU or MMIO
 * read counts. The programmed PLLTMR interval gates STATUS31 and latches the
 * stable event for a future interrupt-register owner. Zero duration does not
 * advance; timer zero completes on the enable edge. No analog acquisition,
 * changing/absent reference clock, or board clock routing is inferred. */
bool s5l8920_dsim_clock_advance(s5l8920_dsim_clock_t *d, uint64_t cycles);

/* Exact rational bit-clock rate Fin*M/(P*2^S), only once stable. A separate
 * consumer must account for byte/escape dividers and gate enable. No rounded
 * frequency is used for timing. Failure preserves both outputs. */
bool s5l8920_dsim_clock_rate(const s5l8920_dsim_clock_t *d,
    uint64_t *numerator, uint32_t *denominator);

typedef enum {
    S5L8920_DSIM_STOP = 0,
    S5L8920_DSIM_ENTERING_ULPS,
    S5L8920_DSIM_ULPS,
    S5L8920_DSIM_EXITING_ULPS,
    S5L8920_DSIM_WAKEUP,
    S5L8920_DSIM_STOPPING
} s5l8920_dsim_lane_state_t;

typedef struct {
    s5l8920_dsim_clock_input_t clock;
    uint32_t reset_cycles; /* DSIM system clock, with stable internal PLL. */
    uint32_t stop_cycles, entry_cycles, exit_cycles, wakeup_cycles; /* PHY clock. */
} s5l8920_dsim_input_t;

typedef struct {
    s5l8920_dsim_lane_state_t state;
    uint32_t remaining;
} s5l8920_dsim_lane_t;

typedef struct {
    s5l8920_dsim_input_t initial;
    s5l8920_dsim_clock_t clock;
    uint32_t reg[19], known, events, reset_remaining;
    s5l8920_dsim_lane_t lane[3]; /* Clock, data0, data1. */
    bool configured, reset_pending, reset_released, events_known, fifos_empty;
} s5l8920_dsim_t;

/* Idle two-data-lane control model, independent of a board attachment. Inputs
 * must explicitly place clock/data0/data1 in stop state with reset released. All durations must
 * be nonzero; they are supplied timing parameters, not measured S5L8920
 * constants. Other initial configuration, FIFO and interrupt words are unknown.
 * Identical configuration preserves progress; conflicting input refuses.
 * Host reset restores these observations, not a software-reset register image. */
bool s5l8920_dsim_configure(s5l8920_dsim_t *d, const s5l8920_dsim_input_t *input);
void s5l8920_dsim_reset(s5l8920_dsim_t *d);

/* Word offsets only. Software reset1 is supported from stop state; functional
 * reset10000 preserves programmed configuration except escape requests. Both
 * clear FIFOs/events and release only after supplied system clocks with a
 * stable internal PLL. Software reset invalidates changed register values:
 * their silicon defaults are not guessed. Subsequent supported writes establish
 * readable latches. SWRST command readback remains unknown.
 *
 * Two data lanes support stop, ULPS entry, exit and wakeup. Gated lanes retain
 * their internal state; re-enabling resumes it. Duplicate requests preserve
 * progress; conflicting commands during transitions refuse. No HS clock,
 * external clock, active image, packet, BTA or remote-reset operation is
 * accepted. Empty FIFO status requires reset or all FIFO init inputs low.
 * PLL/reset interrupt causes latch and support W1C; masks/IRQ wiring remain
 * unavailable. Post-enable PLLTMR and unimplemented offsets still refuse.
 * Rejections preserve the entire component and read output. */
bool s5l8920_dsim_read(const s5l8920_dsim_t *d, uint32_t offset, uint32_t *value);
bool s5l8920_dsim_write(s5l8920_dsim_t *d, uint32_t offset, uint32_t value);

/* Independent clock-domain input. No CPU step or register read advances time.
 * System cycles drive PLL/reset. PHY cycles drive enabled lanes only while
 * internal PLL, byte, escape and the lane's escape clocks are enabled with a
 * nonzero prescaler. PHY clock routing/division and relative phase are the
 * caller's responsibility; no related-silicon divider is silently adopted. */
bool s5l8920_dsim_system_clock(s5l8920_dsim_t *d, uint64_t cycles);
bool s5l8920_dsim_phy_clock(s5l8920_dsim_t *d, uint64_t cycles);
#endif
