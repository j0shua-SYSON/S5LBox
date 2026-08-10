/*
 * S5LBox — the buffer between a UIButton and the emulated board's switches.
 *
 * WHY THIS IS A SEPARATE FILE, and it is the same reason VMTouchQueue.h is. The
 * UI thread produces transitions when a finger lands on a control; the emulator
 * thread applies them between bounded chunks of guest execution. Those two rates
 * have nothing to do with each other, so something has to hold transitions in
 * between and decide what to do when it fills up.
 *
 * The rule, stated once:
 *
 *     A BUTTON TRANSITION MAY NEVER BE COALESCED AWAY.
 *
 * This is the one place the button queue is STRICTER than the touch queue, and
 * the difference is not stylistic. A drag's MOVED is completely superseded by a
 * later MOVED — the newer position is strictly better information. A button has
 * no such report: a press and the release that follows it are two edges, and
 * dropping either turns a tap into nothing or into a key the guest believes is
 * still held. There is therefore no coalescing case at all, only a bounded FIFO
 * that counts what it had to drop.
 *
 * The mapping is the other half of this file, and it is the part most likely to
 * be silently wrong: the app's VMButton enum is in the order the control bar
 * draws them (Home first, because that is what a person reaches for) and the
 * core's S5L_BUTTON_* is in the order /device-tree/buttons lists them (hold
 * first, because that is the order AppleM68Buttons asks its provider for
 * interrupt indices in). Neither order is negotiable and they are not the same
 * order, so the translation is a table, and app/Tests/test_vmbuttonqueue.c
 * checks every row of it against both enums on every host CI runner.
 *
 * NOTHING HERE TOUCHES THE MACHINE. This is a container and a lookup table.
 * Whether the emulated board accepts a transition it is handed is that model's
 * decision, made in core/src/soc/buttons.c, and it is entitled to say no.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMBUTTONQUEUE_H
#define S5LBOX_APP_VMBUTTONQUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "soc.h"          /* S5L_BUTTON_*, S5L_BUTTONS_RINGER_MUTED */

/*
 * The app's own button identifiers, mirroring VMEngine.h's VMButton so that
 * this file can be compiled and tested with no Objective-C at all.
 *
 * They must agree by VALUE with VMEngine.h's enum, and this header cannot
 * check that — VMEngine.h needs Foundation, which a host CI runner does not
 * have. So the check lives in VMEngine.m, where both are visible, as a
 * _Static_assert per row. That fires on the macOS build rather than the host
 * one, which is the right place: it is the only build where the two ever meet.
 */
#define VM_BUTTON_HOME          0u
#define VM_BUTTON_POWER         1u
#define VM_BUTTON_VOLUME_UP     2u
#define VM_BUTTON_VOLUME_DOWN   3u
#define VM_BUTTON_RINGER_SILENT 4u
#define VM_BUTTON_COUNT         5u

/*
 * How many transitions may wait. Ten is five complete press/release pairs — more
 * than a person can produce before the emulator thread's next chunk, and small
 * enough that a queue which is not draining is obvious rather than unbounded.
 */
#define VM_BUTTON_QUEUE_CAP 10u

/*
 * A real Sleep/Wake tap is not a zero-duration edge pair.  AppleM68Buttons
 * samples the pin from a 14 ms debounce callback, not from its interrupt, and
 * the callback may not execute before a UIKit tap has already queued release.
 *
 * The host floor preserves a physical-feeling pulse.  For a press that began
 * with the display dark, CLCD changing to running is stronger evidence than a
 * guessed instruction delay: the guest acted on that press, so release can no
 * longer erase it.  This distinction is load-bearing on a slow no-JIT idle
 * path.  Guest time there follows the active host clock while cpu.cycles still
 * counts retired instructions; waiting for eight million retirements took
 * several HOST seconds on a real device and turned a wake tap into "slide to
 * power off".
 *
 * Eight million retirements remains only a bounded fallback for a dark display
 * that exposes no CLCD edge.  It is above the 5,768,000-retirement debounce
 * floor used by deterministic runners with no active host clock.  A press that
 * began with CLCD already running needs only the host floor: that driver path
 * is already alive, and forcing the dark-display fallback under heavy UI load
 * would manufacture the same long-press bug.
 */
#define VM_BUTTON_POWER_MIN_HOLD_NS UINT64_C(500000000)
#define VM_BUTTON_POWER_MIN_HOLD_CYCLES UINT64_C(8000000)

/*
 * Home and the two volume keys use AppleM68Buttons' ordinary 14 ms debounce
 * path.  A UIKit tap can put both edges in the queue before the emulator has
 * delivered either one, so keep an accepted press electrically asserted long
 * enough for that callback to sample it.  Fifty milliseconds is still a short
 * physical tap, while leaving margin for a timer boundary and host scheduling.
 *
 * Power is deliberately not covered by this floor: its sleep/wake path has the
 * stronger display-edge and bounded-fallback policy above.  The ringer is a
 * two-position switch, not a momentary key.
 */
#define VM_BUTTON_MOMENTARY_MIN_HOLD_NS UINT64_C(50000000)

typedef struct {
    uint8_t  which;     /* an S5L_BUTTON_*, already translated  */
    bool     pressed;   /* what the guest should end up reporting */
} vm_button_event_t;

/* The two clock anchors captured when the board, not UIKit, accepts Power. */
typedef struct {
    bool     active;
    bool     display_running_at_press;
    uint64_t delivered_ns;
    uint64_t delivered_cycles;
} vm_button_power_hold_t;

/* Per-core-button anchors for Home and the two volume keys. */
typedef struct {
    bool     active[S5L_BUTTON_COUNT];
    uint64_t delivered_ns[S5L_BUTTON_COUNT];
} vm_button_momentary_holds_t;

typedef struct {
    vm_button_event_t slot[VM_BUTTON_QUEUE_CAP];
    unsigned          head;    /* index of the oldest transition */
    unsigned          count;
    /* Bounded accounting, so a caller that ignores every return value still
     * leaves evidence. `dropped` is the one that means input was lost. */
    uint64_t          queued, dropped;
} vm_button_queue_t;

/* Empty the queue and zero the counters. Safe on a zeroed struct. */
void vm_button_queue_reset(vm_button_queue_t *q);

/*
 * Cancel transitions not yet accepted by the board, preserving lifetime
 * accounting. Returns how many were removed. Lifecycle cancellation is not a
 * capacity drop, so this does not increment `dropped`.
 */
unsigned vm_button_queue_cancel_pending(vm_button_queue_t *q);

/*
 * Translate one app-side button into the core's identifier, and the UI's
 * "pressed" into the state the GUEST will report.
 *
 * Returns false — and writes nothing — for a button this app does not have.
 *
 * The `pressed` translation is the identity for four of the five and is stated
 * anyway, because the fifth is not obvious: the ringer is a slider, and the
 * control bar's "Silent" being down must mean the position the guest's own
 * driver dispatches HID Phone Mute with value 1 for. core/include/soc.h defines
 * that as S5L_BUTTONS_RINGER_MUTED, and going through it here is what stops
 * this file from having its own opinion about which way round the slider is.
 */
bool vm_button_to_guest(unsigned app_button, bool pressed,
                        unsigned *out_which, bool *out_pressed);

/*
 * Add a transition. Returns whether it is now in the queue.
 *
 * False means it was dropped and `dropped` was incremented: the queue was full.
 * There is deliberately no coalescing escape hatch — see the rule at the top.
 *
 * A transition identical to the one already at the back IS still queued. It
 * costs one slot, and the board answers a redundant set cheaply and without
 * counting it as an edge, whereas deciding here that it is redundant would mean
 * this file tracking a state it cannot see the guest's half of.
 */
bool vm_button_queue_push(vm_button_queue_t *q, unsigned which, bool pressed);

/* Copy the oldest transition out without removing it. False if empty. */
bool vm_button_queue_peek(const vm_button_queue_t *q, vm_button_event_t *out);

/*
 * Whether the event at the front of the queue may be delivered now.  Every
 * event except a Power release is immediately ready.  An inactive or NULL hold
 * means no accepted press is known and fails open rather than wedging Power.
 * The host floor always applies when its clock is usable.  After that, a press
 * which began on a running display is ready; a dark-display press is ready
 * when CLCD has started or when the retired-instruction fallback expires.
 * Missing/backwards clocks fail open only for their own half so a discontinuity
 * cannot hold the key forever.
 */
bool vm_button_power_release_ready(const vm_button_event_t *event,
                                   const vm_button_power_hold_t *hold,
                                   uint64_t now_ns,
                                   uint64_t now_cycles,
                                   bool display_running_now);

/*
 * Apply the ordinary Home/volume debounce floor.  Presses, Power, ringer and
 * an unpaired release are immediately ready.  A missing or backwards host
 * clock fails open rather than leaving a physical key stuck forever.
 */
bool vm_button_momentary_release_ready(
    const vm_button_event_t *event,
    const vm_button_momentary_holds_t *holds,
    uint64_t now_ns);

/* Update those anchors only after the emulated board accepts the transition. */
void vm_button_momentary_note_accepted(
    const vm_button_event_t *event,
    vm_button_momentary_holds_t *holds,
    uint64_t delivered_ns);

/* Remove the oldest transition. Harmless on an empty queue. */
void vm_button_queue_pop(vm_button_queue_t *q);

#endif /* S5LBOX_APP_VMBUTTONQUEUE_H */
