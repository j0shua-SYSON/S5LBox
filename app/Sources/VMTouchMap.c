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
