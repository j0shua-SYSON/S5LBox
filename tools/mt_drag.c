/*
 * S5LBox -- the drag gesture the harness schedules. See mt_drag.h for what a
 * drag is and for run90, the measurement that says why --touch could not be
 * one.
 *
 * Nothing here touches a machine, a device or a clock. That is deliberate: it
 * is the half of a drag that can be wrong silently, so it is the half that is
 * unit-tested.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "mt_drag.h"

#include <string.h>

/*
 * One coordinate of report `k` of `den` intervals: a + (b-a)*k/den, rounded to
 * nearest with halves UP.
 *
 * Done in integers rather than floating point because the properties that have
 * to hold are exactness at the endpoints and monotonicity in between, and both
 * are trivially true of integer arithmetic and merely very probably true of a
 * double. `pos` is the exact position scaled by `den`; it cannot be negative,
 * because the position lies between two unsigned panel coordinates, so the
 * doubled form below is a plain round-half-up with no sign case.
 *
 * HALVES UP RATHER THAN AWAY FROM THE START POINT, and it is not arbitrary: it
 * is what makes a drag and its reverse visit the same pixels. Rounding the
 * DELTA away from zero -- the obvious first version, and what this function did
 * until the tests caught it -- rounds away from wherever the gesture began, so
 * (160,470)->(160,12) put report 3 at y=355 while (160,12)->(160,470) put the
 * mirroring report at y=356. Two runs meant to differ only in direction would
 * have differed by a pixel at every half-way point, which is a difference
 * nobody would think to look for and everybody would eventually have to
 * explain. test_reverse_is_the_same_path_backwards() pins it.
 *
 * At k == den the exact position is b, so the bias cannot carry it past b: the
 * last report is (x1,y1) exactly, never one pixel short. A drag that stops a
 * pixel before the slider's end is a drag that does not unlock the phone and
 * does not say why.
 */
static uint16_t mt_drag_lerp(uint16_t a, uint16_t b, unsigned k, unsigned den) {
    int64_t pos = (int64_t)a * (int64_t)den +
                  ((int64_t)b - (int64_t)a) * (int64_t)k;
    return (uint16_t)((2 * pos + (int64_t)den) / (2 * (int64_t)den));
}

bool mt_drag_valid(const mt_drag_t *d) {
    if (!d) return false;
    if (d->steps < MT_DRAG_STEPS_MIN || d->steps > MT_DRAG_STEPS_MAX)
        return false;
    /* Both endpoints on the panel. s5l_mtz2_set_contacts() refuses an
     * off-panel coordinate on every instruction it is offered, so catching it
     * here is what keeps that refusal from being mistaken for a busy device. */
    if (d->x0 >= S5L_MT_PANEL_W || d->x1 >= S5L_MT_PANEL_W) return false;
    if (d->y0 >= S5L_MT_PANEL_H || d->y1 >= S5L_MT_PANEL_H) return false;
    /* Every gap must be at least one instruction: two reports due at the same
     * instruction cannot both be accepted -- the device holds exactly one --
     * so a span that rounds a gap to zero is a gesture that can never
     * complete, and it must fail on the command line rather than after an
     * hour of retries. */
    if (d->span < (uint64_t)d->steps + 1u) return false;
    return true;
}

unsigned mt_drag_reports(const mt_drag_t *d) {
    if (!mt_drag_valid(d)) return 0u;
    return d->steps + 2u;      /* landing + intermediates + lift */
}

uint64_t mt_drag_gap(const mt_drag_t *d) {
    if (!mt_drag_valid(d)) return 0u;
    return d->span / ((uint64_t)d->steps + 1u);
}

uint64_t mt_drag_span_default(unsigned steps) {
    return ((uint64_t)steps + 1u) * MT_DRAG_FRAME_INSTRS;
}

bool mt_drag_stationary(const mt_drag_t *d) {
    return d && d->x0 == d->x1 && d->y0 == d->y1;
}

bool mt_drag_contact(const mt_drag_t *d, unsigned k, s5l_mt_contact_t *out) {
    unsigned reports = mt_drag_reports(d);
    unsigned den;
    if (!out || !reports || k >= reports) return false;

    /* `steps + 1` intervals span the path, so report k sits at k/(steps+1) and
     * the last report -- index steps+1 -- lands exactly on the far end. */
    den = d->steps + 1u;

    memset(out, 0, sizeof *out);
    out->id = MT_DRAG_CONTACT_ID;
    out->x  = mt_drag_lerp(d->x0, d->x1, k, den);
    out->y  = mt_drag_lerp(d->y0, d->y1, k, den);
    /*
     * The sequence, and the only reason this file exists. The first report
     * says the finger LANDED, every middle one says it is STILL DOWN AND
     * MOVING, and the last says it LIFTED. run90 shows what the middle value
     * buys: without it the guest sees a fresh finger-down per point, and
     * __UIApplicationHandleEvent saw 2 events out of 17 frames.
     */
    if (k == 0u)                 out->phase = MTZ2_PHASE_MAKE_TOUCH;
    else if (k == reports - 1u)  out->phase = MTZ2_PHASE_BREAK_TOUCH;
    else                         out->phase = MTZ2_PHASE_TOUCHING;
    /*
     * Pressure agrees with the phase instead of contradicting it: the parsed Z
     * amplitude is what the HID plugin tests for "is this finger down", so the
     * lift -- and only the lift -- carries zero. The ellipse is unchanged on
     * the breaking frame, because a real part reports the last geometry it
     * measured.
     */
    out->pressure = out->phase == MTZ2_PHASE_BREAK_TOUCH ? 0u : MT_DRAG_PRESSURE;
    out->major    = MT_DRAG_MAJOR;
    out->minor    = MT_DRAG_MINOR;
    return true;
}

const char *mt_drag_phase_name(uint8_t phase) {
    switch (phase) {
    case MTZ2_PHASE_NOT_TRACKING: return "NotTracking";
    case MTZ2_PHASE_START:        return "Start";
    case MTZ2_PHASE_HOVER:        return "Hover";
    case MTZ2_PHASE_MAKE_TOUCH:   return "MakeTouch";
    case MTZ2_PHASE_TOUCHING:     return "Touching";
    case MTZ2_PHASE_BREAK_TOUCH:  return "BreakTouch";
    case MTZ2_PHASE_LINGER:       return "Linger";
    case MTZ2_PHASE_OUT_OF_RANGE: return "OutOfRange";
    default:                      return "?";
    }
}
