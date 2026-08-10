/*
 * S5LBox — VMButtonQueue. See the header for the rules this file implements.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMButtonQueue.h"

#include <string.h>

/*
 * THE TWO ORDERS, SIDE BY SIDE, ONCE.
 *
 * Indexed by the app's VMButton. The values are the core's S5L_BUTTON_*, which
 * is /device-tree/buttons' own order — and that order is not cosmetic: it is
 * the interrupt index AppleM68Buttons asks its provider for, so the core is not
 * free to reorder it to match the UI. The UI is not free to reorder either; the
 * control bar draws Home first because that is the button a person reaches for.
 *
 * So the two orders differ, permanently, and this table is the only place that
 * fact is allowed to live.
 */
static const uint8_t TO_GUEST[VM_BUTTON_COUNT] = {
    [VM_BUTTON_HOME]          = S5L_BUTTON_MENU,     /* the tree calls it menu */
    [VM_BUTTON_POWER]         = S5L_BUTTON_HOLD,     /* and Power hold        */
    [VM_BUTTON_VOLUME_UP]     = S5L_BUTTON_VOLUP,
    [VM_BUTTON_VOLUME_DOWN]   = S5L_BUTTON_VOLDOWN,
    [VM_BUTTON_RINGER_SILENT] = S5L_BUTTON_RINGERAB,
};

void vm_button_queue_reset(vm_button_queue_t *q) {
    if (!q) return;
    memset(q, 0, sizeof *q);
}

bool vm_button_to_guest(unsigned app_button, bool pressed,
                        unsigned *out_which, bool *out_pressed) {
    if (app_button >= VM_BUTTON_COUNT) return false;
    if (out_which) *out_which = TO_GUEST[app_button];
    if (out_pressed) {
        /*
         * Four of the five are the identity and the fifth is not, so all five
         * go through the same expression rather than four of them going through
         * nothing. The control bar's "Silent" key being DOWN must mean the
         * slider position the guest's own driver dispatches Phone Mute with
         * value 1 for, and core/include/soc.h is where that is decided.
         */
        if (app_button == VM_BUTTON_RINGER_SILENT)
            *out_pressed = pressed ? S5L_BUTTONS_RINGER_MUTED
                                   : !S5L_BUTTONS_RINGER_MUTED;
        else
            *out_pressed = pressed;
    }
    return true;
}

bool vm_button_queue_push(vm_button_queue_t *q, unsigned which, bool pressed) {
    if (!q) return false;
    if (which >= S5L_BUTTON_COUNT) return false;

    if (q->count >= VM_BUTTON_QUEUE_CAP) {
        /*
         * Full, and there is nothing to be done about it. A touch queue can
         * throw away a MOVED because a newer MOVED completely supersedes it; a
         * button has no equivalent, because a press and its release are two
         * edges and dropping either one leaves the guest holding a key nobody
         * is pressing. Counting it is the only honest option.
         */
        q->dropped++;
        return false;
    }

    q->slot[(q->head + q->count) % VM_BUTTON_QUEUE_CAP].which = (uint8_t)which;
    q->slot[(q->head + q->count) % VM_BUTTON_QUEUE_CAP].pressed = pressed;
    q->count++;
    q->queued++;
    return true;
}

bool vm_button_queue_peek(const vm_button_queue_t *q, vm_button_event_t *out) {
    if (!q || !out || q->count == 0u) return false;
    *out = q->slot[q->head];
    return true;
}

bool vm_button_power_release_ready(const vm_button_event_t *event,
                                   const vm_button_power_hold_t *hold,
                                   uint64_t now_ns,
                                   uint64_t now_cycles,
                                   bool display_running_now) {
    if (!event) return false;
    if (event->which != S5L_BUTTON_HOLD || event->pressed) return true;
    if (!hold || !hold->active) return true;

    bool host_ready = hold->delivered_ns == 0u || now_ns == 0u ||
                      now_ns < hold->delivered_ns ||
                      now_ns - hold->delivered_ns >=
                          VM_BUTTON_POWER_MIN_HOLD_NS;
    if (!host_ready) return false;

    /* An already-running display has no post-wake rebuild to wait through.
     * For a dark display, the off->on edge proves the guest consumed the press
     * and is a safer release boundary than elapsed retirements under load. */
    if (hold->display_running_at_press || display_running_now) return true;

    return now_cycles < hold->delivered_cycles ||
           now_cycles - hold->delivered_cycles >=
               VM_BUTTON_POWER_MIN_HOLD_CYCLES;
}

static bool uses_momentary_floor(unsigned which) {
    return which == S5L_BUTTON_MENU ||
           which == S5L_BUTTON_VOLUP ||
           which == S5L_BUTTON_VOLDOWN;
}

bool vm_button_momentary_release_ready(
    const vm_button_event_t *event,
    const vm_button_momentary_holds_t *holds,
    uint64_t now_ns) {
    if (!event) return false;
    if (event->pressed || !uses_momentary_floor(event->which)) return true;
    if (!holds || event->which >= S5L_BUTTON_COUNT ||
        !holds->active[event->which]) return true;

    uint64_t delivered_ns = holds->delivered_ns[event->which];
    if (delivered_ns == 0u || now_ns == 0u || now_ns < delivered_ns)
        return true;
    return now_ns - delivered_ns >= VM_BUTTON_MOMENTARY_MIN_HOLD_NS;
}

void vm_button_momentary_note_accepted(
    const vm_button_event_t *event,
    vm_button_momentary_holds_t *holds,
    uint64_t delivered_ns) {
    if (!event || !holds || event->which >= S5L_BUTTON_COUNT ||
        !uses_momentary_floor(event->which)) return;

    if (event->pressed) {
        holds->active[event->which] = true;
        holds->delivered_ns[event->which] = delivered_ns;
    } else {
        holds->active[event->which] = false;
        holds->delivered_ns[event->which] = 0u;
    }
}

void vm_button_queue_pop(vm_button_queue_t *q) {
    if (!q || q->count == 0u) return;
    q->head = (q->head + 1u) % VM_BUTTON_QUEUE_CAP;
    q->count--;
}
