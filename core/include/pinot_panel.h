/* Pinot-compatible panel control through a decoded DSI command link.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_PINOT_PANEL_H
#define S5LBOX_PINOT_PANEL_H
#include "s5l8920_dsim.h"

#define PINOT_PANEL_ID_BYTES 15u
typedef struct {
    uint8_t identity[PINOT_PANEL_ID_BYTES], channel;
    uint64_t reset_low_ns, reset_release_ns, sleep_out_ns, sleep_in_ns, reply_ns;
} pinot_panel_input_t;
typedef enum {
    PINOT_PANEL_ASLEEP = 0, PINOT_PANEL_WAKING,
    PINOT_PANEL_AWAKE, PINOT_PANEL_SLEEPING
} pinot_panel_sleep_t;
typedef struct {
    pinot_panel_input_t initial;
    uint64_t reset_low_elapsed, recovery_remaining, sleep_remaining, reply_remaining;
    pinot_panel_sleep_t sleep;
    bool configured, powered, reset_high, reset_valid, display_on, reply_pending;
} pinot_panel_t;

/* Explicit variant/calibration bytes and nonzero functional timing inputs.
 * No physical ID, factory calibration, rail or timing is inferred. Configure
 * a zero object once; identical repeats retain state, conflicting input refuses.
 * Initial power is off and reset asserted. Host reset restores those explicit
 * conditions, retaining configuration. Caller supplies stable rail changes.
 * A powered reset-low interval must meet reset_low_ns before release; a short
 * pulse leaves the device unavailable until a valid pulse. Repeating a level
 * does not restart time. Power loss/asserted reset cancels replies/display. */
bool pinot_panel_configure(pinot_panel_t *p, const pinot_panel_input_t *input);
void pinot_panel_reset(pinot_panel_t *p);
bool pinot_panel_power(pinot_panel_t *p, bool on);
bool pinot_panel_reset_pin(pinot_panel_t *p, bool high);
bool pinot_panel_advance(pinot_panel_t *p, uint64_t nanoseconds);

/* A decoded short packet at its actual transmission boundary, not a register
 * write. Unpowered/reset/recovering devices and other channels consume no
 * command and send no response, while the physical transmitter may still drain.
 * Supported commands: DCS NOP, software reset, sleep in/out, display off/on;
 * generic one-parameter B1 read schedules the explicit15-byte identity reply.
 * Unknown commands and unsupported sequencing refuse atomically. Sleep entry
 * requires display off; display on requires completed sleep exit. Duplicate
 * sleep transitions do not extend their deadlines. No pixels, TE, backlight,
 * gamma programming or wire encoding/integrity checking is provided here. */
bool pinot_panel_command(pinot_panel_t *p, uint32_t header);

/* Service the controller's decoded packet boundary without advancing clocks.
 * Uses the actual TX queue and automatic BTA; panel response and direction
 * change occur only for a received B1 request while available. Reset/power loss
 * or a timed-out controller transaction cannot leak a reply into a later read.
 * FIFO backpressure retains the reply for retry. Unsupported command refusal
 * preserves both objects. Header upper byte0 is this decoded-link contract,
 * not a physical ECC observation; no wire CRC is placed in the RX FIFO.
 * The panel and controller must be distinct, exclusive caller-owned objects. */
bool pinot_panel_service(pinot_panel_t *p, s5l8920_dsim_t *d);
#endif
