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

typedef struct {
    uint8_t  which;     /* an S5L_BUTTON_*, already translated  */
    bool     pressed;   /* what the guest should end up reporting */
} vm_button_event_t;

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

/* Remove the oldest transition. Harmless on an empty queue. */
void vm_button_queue_pop(vm_button_queue_t *q);

#endif /* S5LBOX_APP_VMBUTTONQUEUE_H */
