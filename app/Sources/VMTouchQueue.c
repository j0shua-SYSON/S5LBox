/*
 * S5LBox — VMTouchQueue. See the header for the rule this file implements.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMTouchQueue.h"

#include <string.h>

void vm_touch_queue_reset(vm_touch_queue_t *q) {
    if (!q) return;
    memset(q, 0, sizeof *q);
}

bool vm_touch_contact_from_ui(vm_touch_phase_t phase, int x, int y,
                              s5l_mt_contact_t *out) {
    if (!out) return false;

    uint8_t devPhase;
    uint8_t pressure = VM_TOUCH_PRESSURE;
    switch (phase) {
        case VM_TOUCH_BEGAN:     devPhase = MTZ2_PHASE_MAKE_TOUCH;  break;
        case VM_TOUCH_MOVED:     devPhase = MTZ2_PHASE_TOUCHING;    break;
        case VM_TOUCH_ENDED:
        case VM_TOUCH_CANCELLED: devPhase = MTZ2_PHASE_BREAK_TOUCH;
                                 pressure = 0u;
                                 break;
        default:
            return false;       /* not a phase this app produces */
    }

    /* vm_touch_map() should have clamped already, and this is the assertion
     * that it did. An out-of-range coordinate reaching the device would be
     * refused there anyway, but refusing it here says which layer was wrong. */
    if (x < 0 || y < 0 ||
        (unsigned)x >= S5L_MT_PANEL_W || (unsigned)y >= S5L_MT_PANEL_H)
        return false;

    memset(out, 0, sizeof *out);
    out->id       = VM_TOUCH_CONTACT_ID;
    out->phase    = devPhase;
    out->x        = (uint16_t)x;
    out->y        = (uint16_t)y;
    out->pressure = pressure;
    out->major    = VM_TOUCH_MAJOR;
    out->minor    = VM_TOUCH_MINOR;
    return true;
}

bool vm_touch_queue_push(vm_touch_queue_t *q, const s5l_mt_contact_t *c) {
    if (!q || !c) return false;

    if (q->count < VM_TOUCH_QUEUE_CAP) {
        q->slot[(q->head + q->count) % VM_TOUCH_QUEUE_CAP] = *c;
        q->count++;
        q->queued++;
        return true;
    }

    /* Full. A newer MOVED supersedes the MOVED at the back. */
    unsigned back = (q->head + q->count - 1u) % VM_TOUCH_QUEUE_CAP;
    if (c->phase == MTZ2_PHASE_TOUCHING &&
        q->slot[back].phase == MTZ2_PHASE_TOUCHING) {
        q->slot[back] = *c;
        q->coalesced++;
        return true;
    }

    /*
     * A lifecycle edge is worth more than an old position. Find the newest
     * queued MOVED, remove it, shift only the later reports toward the front,
     * and append the edge at the back. Chronological order is preserved: the
     * edge remains after every report that survives. The old implementation
     * dropped BREAK_TOUCH here even when the back was a MOVED, which is exactly
     * how a bounded drag queue becomes a finger held forever in the guest.
     */
    if (c->phase == MTZ2_PHASE_MAKE_TOUCH ||
        c->phase == MTZ2_PHASE_BREAK_TOUCH) {
        for (unsigned offset = q->count; offset > 0u; offset--) {
            unsigned logical = offset - 1u;
            unsigned at = (q->head + logical) % VM_TOUCH_QUEUE_CAP;
            if (q->slot[at].phase != MTZ2_PHASE_TOUCHING) continue;

            for (unsigned move = logical; move + 1u < q->count; move++) {
                unsigned dst = (q->head + move) % VM_TOUCH_QUEUE_CAP;
                unsigned src = (q->head + move + 1u) % VM_TOUCH_QUEUE_CAP;
                q->slot[dst] = q->slot[src];
            }
            q->count--;
            q->slot[(q->head + q->count) % VM_TOUCH_QUEUE_CAP] = *c;
            q->count++;
            q->queued++;
            q->coalesced++;
            return true;
        }
    }

    q->dropped++;
    return false;
}

bool vm_touch_queue_peek(const vm_touch_queue_t *q, s5l_mt_contact_t *out) {
    if (!q || !out || q->count == 0u) return false;
    *out = q->slot[q->head];
    return true;
}

void vm_touch_queue_pop(vm_touch_queue_t *q) {
    if (!q || q->count == 0u) return;
    q->head = (q->head + 1u) % VM_TOUCH_QUEUE_CAP;
    q->count--;
}
