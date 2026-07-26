/*
 * iOS3-VM — turning a finger on the host's screen into a guest coordinate.
 * See VMTouchMap.h for why this is C and not Objective-C.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMTouchMap.h"

#include <limits.h>
#include <math.h>

vm_touch_point_t vm_touch_map(double view_w, double view_h,
                              unsigned guest_w, unsigned guest_h,
                              double point_x, double point_y) {
    vm_touch_point_t out;
    out.x = 0;
    out.y = 0;
    out.inside = false;

    /* Reject before dividing. isfinite() first, then the sign tests: a NaN
     * fails every comparison, so `> 0.0` would let it through as "not less
     * than or equal" only if the test were written the other way round. */
    if (!isfinite(view_w) || !isfinite(view_h)) return out;
    if (!isfinite(point_x) || !isfinite(point_y)) return out;
    if (view_w <= 0.0 || view_h <= 0.0) return out;
    if (guest_w == 0u || guest_h == 0u) return out;
    if (guest_w > (unsigned)INT_MAX || guest_h > (unsigned)INT_MAX) return out;

    const double gw = (double)guest_w;
    const double gh = (double)guest_h;

    /* The aspect-preserving fit: whichever axis runs out first sets the scale,
     * and the slack on the other axis becomes two equal bars. */
    double scale = view_w / gw;
    const double vertical = view_h / gh;
    if (vertical < scale) scale = vertical;
    if (!(scale > 0.0)) return out;

    const double origin_x = (view_w - gw * scale) * 0.5;
    const double origin_y = (view_h - gh * scale) * 0.5;

    /* floor(), not a cast: a cast truncates towards zero, which would fold the
     * whole first column of the left-hand bar onto guest pixel 0. */
    const double x = floor((point_x - origin_x) / scale);
    const double y = floor((point_y - origin_y) / scale);

    if (x < 0.0 || y < 0.0 || x >= gw || y >= gh) return out;

    out.x = (int)x;
    out.y = (int)y;
    out.inside = true;
    return out;
}

void vm_touch_tracker_reset(vm_touch_tracker_t *tracker) {
    if (!tracker) return;
    tracker->active = false;
    tracker->x = 0;
    tracker->y = 0;
}

bool vm_touch_track(vm_touch_tracker_t *tracker, vm_touch_phase_t phase,
                    vm_touch_point_t mapped, int *out_x, int *out_y) {
    if (!tracker) return false;

    if (phase == VM_TOUCH_BEGAN) {
        /* Unconditional, not just on the inside branch. A begin in the
         * letterbox has to CLEAR any gesture the tracker still believes in,
         * or a lost end — a modal presented mid-drag, the view pulled out of
         * the hierarchy — would let this untouchable gesture be reported at
         * the previous one's coordinates. */
        tracker->active = mapped.inside;
        if (!mapped.inside) return false;
        tracker->x = mapped.x;
        tracker->y = mapped.y;
    } else {
        if (!tracker->active) return false;

        if (mapped.inside) {
            tracker->x = mapped.x;
            tracker->y = mapped.y;
        } else if (phase == VM_TOUCH_MOVED) {
            // Dragged into the letterbox. Say nothing, but stay live: the end
            // of this gesture still has to be reported.
            return false;
        }

        if (phase == VM_TOUCH_ENDED || phase == VM_TOUCH_CANCELLED)
            tracker->active = false;
    }

    if (out_x) *out_x = tracker->x;
    if (out_y) *out_y = tracker->y;
    return true;
}
