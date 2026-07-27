/*
 * S5LBox — the drag gesture the harness schedules.
 *
 * ONE PROPERTY DOMINATES THIS FILE, and it is the one run90 paid a boot to
 * learn. A contact that is reported as landing twice is two fingers. Eight
 * --touch points were scheduled along the 3.1.3 lock screen's unlock slider to
 * fake a drag, and the funnel came out 16 / 17 / 9 / 2: 16 reports enqueued by
 * the kernel (probe 0xc043d6b8 saw exactly 16, none refused), 17 frames parsed
 * in userspace at _mt_HandleMultitouchFrame, 9 surviving to the MultitouchHID
 * plugin, and 2 reaching __UIApplicationHandleEvent -- the first finger-down
 * and the first finger-up. Nothing downstream was broken. UIKit was simply
 * never shown one contact moving, because MTZ2_PHASE_TOUCHING, which the
 * device model has accepted since it was written, was emitted by nothing in
 * this tree.
 *
 * So most of what follows is one question asked four ways: does the sequence
 * tools/mt_drag.c emits describe ONE finger? The phase order, the constant
 * identifier, the pressure that agrees with the phase, and the coordinates that
 * move monotonically from one endpoint to the other and land on both exactly
 * are the four.
 *
 * The expectations are written out here rather than derived from the
 * implementation: test_reference_contact() interpolates the same line
 * independently in double precision, so agreeing with mt_drag_contact() means
 * agreeing with a separate statement of the same intent and not with itself.
 * It has already earned that -- mt_drag_lerp() rounded the DELTA away from
 * zero in its first version, which put a drag and its mirror image a pixel
 * apart at every half-way point, and test_reverse_is_the_same_path_backwards()
 * now pins the rule that replaced it.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "mt_drag.h"
#include "soc.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { \
        g_fail++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* The gestures every sequence test runs over: forwards, backwards, both axes
 * at once, one axis only, the shortest legal step count, the longest, and the
 * degenerate stationary one. A property that holds for a rightward drag and
 * fails for a leftward one is exactly the kind of sign bug integer
 * interpolation invites. */
static const mt_drag_t CASES[] = {
    /* the unlock slider, left to right along a constant y */
    { 10u, 240u, 300u, 240u,  8u, 59328000ull },
    /* right to left: the same path with the sign of every delta flipped */
    { 300u, 240u, 10u, 240u,  8u, 59328000ull },
    /* both axes, and a delta that does not divide evenly by steps+1 */
    { 0u, 0u, 319u, 479u,   5u, 40000000ull },
    /* downward only */
    { 160u, 470u, 160u, 12u, 11u, 79104000ull },
    /* the shortest legal drag: exactly one intermediate report */
    { 1u, 2u, 3u, 4u,        1u, 13184000ull },
    /* the longest, and a span with an inexact division */
    { 5u, 400u, 250u, 33u,  64u, 1000000ull },
    /* endpoints coincident: a held contact, still a legal gesture */
    { 100u, 100u, 100u, 100u, 4u, 32960000ull },
};
#define NCASES (sizeof CASES / sizeof CASES[0])

static const char *case_name(unsigned i) {
    static char buf[64];
    snprintf(buf, sizeof buf, "case %u (%u,%u)->(%u,%u) steps %u", i,
             CASES[i].x0, CASES[i].y0, CASES[i].x1, CASES[i].y1,
             CASES[i].steps);
    return buf;
}

/* ------------------------------------------------------------------------- */

/*
 * THE SEQUENCE ITSELF: MakeTouch, then Touching once per intermediate point,
 * then BreakTouch. Written against the device's own phase constants rather
 * than against mt_drag.c's opinion of them, because the value that matters —
 * MTZ2_PHASE_TOUCHING, 4 — is the one that was never emitted.
 */
static void test_the_phase_sequence_is_one_finger(void) {
    for (unsigned i = 0; i < NCASES; i++) {
        unsigned n = mt_drag_reports(&CASES[i]);
        unsigned touching = 0;
        CHECK(n == CASES[i].steps + 2u,
              "%s reports %u, expected steps+2 = %u — a drag is the landing, "
              "every intermediate point, and the lift",
              case_name(i), n, CASES[i].steps + 2u);
        for (unsigned k = 0; k < n; k++) {
            s5l_mt_contact_t c;
            uint8_t want = k == 0u        ? MTZ2_PHASE_MAKE_TOUCH
                         : k == n - 1u    ? MTZ2_PHASE_BREAK_TOUCH
                                          : MTZ2_PHASE_TOUCHING;
            CHECK(mt_drag_contact(&CASES[i], k, &c),
                  "%s refused to describe report %u of %u",
                  case_name(i), k, n);
            CHECK(c.phase == want,
                  "%s report %u is phase %u (%s), expected %u (%s)",
                  case_name(i), k, c.phase, mt_drag_phase_name(c.phase),
                  want, mt_drag_phase_name(want));
            if (c.phase == MTZ2_PHASE_TOUCHING) touching++;
        }
        CHECK(touching == CASES[i].steps,
              "%s emitted %u Touching reports, expected %u — run90 is what a "
              "gesture with none of them looks like from UIKit",
              case_name(i), touching, CASES[i].steps);
    }
}

/*
 * THE IDENTIFIER, which is the single fact that separates a drag from run90's
 * eight taps. It must be constant across the whole gesture AND inside the
 * 1..MTZ2_CONTACT_MAX range _mt_getPathLifeCycle accepts — zero in particular
 * is not a contact, it is what an unset slot reads as.
 */
static void test_the_contact_identifier_never_changes(void) {
    for (unsigned i = 0; i < NCASES; i++) {
        unsigned n = mt_drag_reports(&CASES[i]);
        uint8_t first = 0;
        for (unsigned k = 0; k < n; k++) {
            s5l_mt_contact_t c;
            if (!mt_drag_contact(&CASES[i], k, &c)) continue;
            if (k == 0u) first = c.id;
            CHECK(c.id == first,
                  "%s report %u carries id %u after id %u — a contact that "
                  "changes identity is a new finger",
                  case_name(i), k, c.id, first);
        }
        CHECK(first >= 1u && first <= MTZ2_CONTACT_MAX,
              "%s uses path identifier %u, outside the 1..%u the driver's "
              "_mt_getPathLifeCycle accepts",
              case_name(i), first, MTZ2_CONTACT_MAX);
    }
}

/*
 * PRESSURE AGREES WITH THE PHASE INSTEAD OF CONTRADICTING IT. The parsed Z
 * amplitude is what the HID plugin tests for "is this finger down", so exactly
 * the last report — the BreakTouch — may carry zero, and every earlier one
 * must not.
 */
static void test_pressure_says_the_same_thing_as_the_phase(void) {
    for (unsigned i = 0; i < NCASES; i++) {
        unsigned n = mt_drag_reports(&CASES[i]);
        for (unsigned k = 0; k < n; k++) {
            s5l_mt_contact_t c;
            if (!mt_drag_contact(&CASES[i], k, &c)) continue;
            if (k == n - 1u)
                CHECK(c.pressure == 0u,
                      "%s lifts with pressure %u; a BreakTouch that still "
                      "reports amplitude contradicts its own phase byte",
                      case_name(i), c.pressure);
            else
                CHECK(c.pressure > 0u,
                      "%s report %u (%s) carries pressure 0 — the HID plugin "
                      "reads that as a finger that is not down",
                      case_name(i), k, mt_drag_phase_name(c.phase));
        }
    }
}

/*
 * THE PATH. Both endpoints exactly, monotone in between, and never off the
 * panel. Stopping one pixel short of the slider's end is a drag that does not
 * unlock the phone and does not say why, so the endpoints are checked for
 * equality and not for proximity.
 */
static void test_the_path_hits_both_ends_and_never_turns_back(void) {
    for (unsigned i = 0; i < NCASES; i++) {
        const mt_drag_t *d = &CASES[i];
        unsigned n = mt_drag_reports(d);
        s5l_mt_contact_t prev;
        memset(&prev, 0, sizeof prev);

        for (unsigned k = 0; k < n; k++) {
            s5l_mt_contact_t c;
            if (!mt_drag_contact(d, k, &c)) continue;

            if (k == 0u)
                CHECK(c.x == d->x0 && c.y == d->y0,
                      "%s lands at (%u,%u), not at its start point",
                      case_name(i), c.x, c.y);
            if (k == n - 1u)
                CHECK(c.x == d->x1 && c.y == d->y1,
                      "%s lifts at (%u,%u), not at its end point",
                      case_name(i), c.x, c.y);

            CHECK(c.x < S5L_MT_PANEL_W && c.y < S5L_MT_PANEL_H,
                  "%s report %u is at (%u,%u), off a %ux%u panel — "
                  "s5l_mtz2_set_contacts() would refuse it forever",
                  case_name(i), k, c.x, c.y,
                  S5L_MT_PANEL_W, S5L_MT_PANEL_H);

            if (k) {
                bool ok_x = d->x1 >= d->x0 ? c.x >= prev.x : c.x <= prev.x;
                bool ok_y = d->y1 >= d->y0 ? c.y >= prev.y : c.y <= prev.y;
                CHECK(ok_x, "%s x went %u -> %u between reports %u and %u, "
                            "against the direction of travel",
                      case_name(i), prev.x, c.x, k - 1u, k);
                CHECK(ok_y, "%s y went %u -> %u between reports %u and %u, "
                            "against the direction of travel",
                      case_name(i), prev.y, c.y, k - 1u, k);
            }
            prev = c;
        }
    }
}

/*
 * The whole path against an INDEPENDENT statement of it: plain
 * double-precision interpolation, rounded to nearest with halves up. An
 * implementation checked against itself checks nothing, and this is the file's
 * only defence against mt_drag_lerp() being confidently and consistently off
 * by one — which it was, until this test said so.
 */
static void test_reference_contact(void) {
    for (unsigned i = 0; i < NCASES; i++) {
        const mt_drag_t *d = &CASES[i];
        unsigned n = mt_drag_reports(d);
        double den = (double)(d->steps + 1u);
        for (unsigned k = 0; k < n; k++) {
            s5l_mt_contact_t c;
            double t  = (double)k / den;
            double rx = (double)d->x0 + ((double)d->x1 - (double)d->x0) * t;
            double ry = (double)d->y0 + ((double)d->y1 - (double)d->y0) * t;
            uint16_t wx, wy;
            /* Both endpoints are unsigned panel coordinates, so no point on
             * the segment between them can be negative and round-half-up is
             * the whole rule. */
            CHECK(rx >= 0.0 && ry >= 0.0,
                  "%s report %u interpolates to a negative coordinate "
                  "(%.2f,%.2f)", case_name(i), k, rx, ry);
            wx = (uint16_t)floor(rx + 0.5);
            wy = (uint16_t)floor(ry + 0.5);
            if (!mt_drag_contact(d, k, &c)) continue;
            CHECK(c.x == wx && c.y == wy,
                  "%s report %u is at (%u,%u); interpolating the same line "
                  "independently gives (%u,%u)",
                  case_name(i), k, c.x, c.y, wx, wy);
        }
    }
}

/*
 * A drag and its reverse visit the same pixels in the opposite order.
 *
 * This is the property that decides how the halves round, and rounding the
 * DELTA away from zero — the obvious first implementation — breaks it: with
 * that rule (160,470)->(160,12) put report 3 at y=355 and the mirror-image
 * gesture put its report at y=356. Two runs meant to differ only in direction
 * would have differed by a pixel at every half-way point, silently. Rounding
 * the POSITION half up makes both agree, because both are computing the same
 * position on the same segment.
 */
static void test_reverse_is_the_same_path_backwards(void) {
    for (unsigned i = 0; i < NCASES; i++) {
        mt_drag_t back = CASES[i];
        unsigned n = mt_drag_reports(&CASES[i]);
        back.x0 = CASES[i].x1; back.y0 = CASES[i].y1;
        back.x1 = CASES[i].x0; back.y1 = CASES[i].y0;
        CHECK(mt_drag_reports(&back) == n,
              "%s reversed has %u reports, not %u",
              case_name(i), mt_drag_reports(&back), n);
        for (unsigned k = 0; k < n; k++) {
            s5l_mt_contact_t f, r;
            if (!mt_drag_contact(&CASES[i], k, &f)) continue;
            if (!mt_drag_contact(&back, n - 1u - k, &r)) continue;
            CHECK(f.x == r.x && f.y == r.y,
                  "%s report %u is at (%u,%u) but the reverse drag's "
                  "report %u is at (%u,%u) — the same segment, rounded two "
                  "different ways",
                  case_name(i), k, f.x, f.y, n - 1u - k, r.x, r.y);
        }
    }
}

/*
 * The pacing. `span` is divided into steps+1 gaps, and the default is one gap
 * per 60 Hz scan of the modelled part: MTZ2_FRAME_PERIOD_MS of guest time at
 * one retired instruction per CPU tick. Recomputed here from the device's own
 * constants rather than read back from the header's macro.
 */
static void test_the_default_pacing_is_the_devices_own_scan_rate(void) {
    uint64_t frame = (uint64_t)S5L8900_CPU_HZ * MTZ2_FRAME_PERIOD_MS / 1000u;
    CHECK(frame == 6592000ull,
          "one %u ms scan at %u Hz is %llu instructions, not the 6,592,000 "
          "this option's default is documented as",
          MTZ2_FRAME_PERIOD_MS, S5L8900_CPU_HZ, (unsigned long long)frame);
    CHECK(MT_DRAG_FRAME_INSTRS == frame,
          "MT_DRAG_FRAME_INSTRS is %llu, the device's scan period is %llu",
          (unsigned long long)MT_DRAG_FRAME_INSTRS,
          (unsigned long long)frame);
    for (unsigned s = MT_DRAG_STEPS_MIN; s <= MT_DRAG_STEPS_MAX; s++) {
        mt_drag_t d = { 0u, 0u, 100u, 100u, s, mt_drag_span_default(s) };
        CHECK(mt_drag_span_default(s) == ((uint64_t)s + 1u) * frame,
              "the default span for %u steps is %llu, expected one scan per "
              "gap", s, (unsigned long long)mt_drag_span_default(s));
        CHECK(mt_drag_gap(&d) == frame,
              "%u steps at the default span pace one report every %llu "
              "instructions, not one per scan frame",
              s, (unsigned long long)mt_drag_gap(&d));
    }
    /* An inexact division truncates, and truncation must never reach zero for
     * a gesture the validator accepted: two reports due on the same
     * instruction can never both be accepted, because the device holds one. */
    for (unsigned s = MT_DRAG_STEPS_MIN; s <= MT_DRAG_STEPS_MAX; s++) {
        mt_drag_t d = { 0u, 0u, 1u, 1u, s, (uint64_t)s + 1u };
        CHECK(mt_drag_valid(&d) && mt_drag_gap(&d) >= 1u,
              "the shortest legal span for %u steps paces reports %llu "
              "instructions apart", s, (unsigned long long)mt_drag_gap(&d));
    }
}

/*
 * FAIL CLOSED. Every rejection here is one the command-line parser also makes,
 * and each exists so a malformed gesture is refused before a boot rather than
 * by the device on every instruction for an hour — which reads exactly like
 * "the guest never drained a report".
 */
static void test_an_impossible_gesture_is_refused(void) {
    const struct { mt_drag_t d; const char *what; } BAD[] = {
        { { 0u, 0u, 10u, 10u, 0u, 10000000ull },
          "zero intermediate reports (that is a tap, not a drag)" },
        { { 0u, 0u, 10u, 10u, MT_DRAG_STEPS_MAX + 1u, 10000000ull },
          "more steps than the ceiling" },
        { { S5L_MT_PANEL_W, 0u, 10u, 10u, 4u, 10000000ull },
          "a start x off the panel" },
        { { 0u, S5L_MT_PANEL_H, 10u, 10u, 4u, 10000000ull },
          "a start y off the panel" },
        { { 0u, 0u, S5L_MT_PANEL_W, 10u, 4u, 10000000ull },
          "an end x off the panel" },
        { { 0u, 0u, 10u, S5L_MT_PANEL_H, 4u, 10000000ull },
          "an end y off the panel" },
        { { 0u, 0u, 10u, 10u, 4u, 0ull },  "a zero span" },
        { { 0u, 0u, 10u, 10u, 4u, 4ull },
          "a span one instruction short of pacing its own reports" },
    };
    for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        s5l_mt_contact_t c;
        memset(&c, 0xa5, sizeof c);
        CHECK(!mt_drag_valid(&BAD[i].d), "%s was accepted", BAD[i].what);
        CHECK(mt_drag_reports(&BAD[i].d) == 0u,
              "%s has a report count", BAD[i].what);
        CHECK(mt_drag_gap(&BAD[i].d) == 0u, "%s has a pacing", BAD[i].what);
        CHECK(!mt_drag_contact(&BAD[i].d, 0u, &c),
              "%s produced a contact", BAD[i].what);
        CHECK(c.id == 0xa5u,
              "%s was refused but the caller's contact was written anyway",
              BAD[i].what);
    }

    /* Out-of-range indices on a VALID gesture, and null arguments. */
    {
        mt_drag_t d = { 10u, 10u, 20u, 20u, 3u, 26368000ull };
        unsigned n = mt_drag_reports(&d);
        s5l_mt_contact_t c;
        CHECK(n == 5u, "a 3-step drag is %u reports, expected 5", n);
        CHECK(!mt_drag_contact(&d, n, &c),
              "report %u of a %u-report gesture was described", n, n);
        CHECK(!mt_drag_contact(&d, n + 1000u, &c),
              "a wildly out-of-range report index was described");
        CHECK(!mt_drag_contact(&d, 0u, NULL),
              "a null output pointer was accepted");
        CHECK(!mt_drag_contact(NULL, 0u, &c), "a null gesture was described");
        CHECK(!mt_drag_valid(NULL), "a null gesture is valid");
        CHECK(mt_drag_reports(NULL) == 0u, "a null gesture has reports");
        CHECK(mt_drag_gap(NULL) == 0u, "a null gesture has a pacing");
        CHECK(!mt_drag_stationary(NULL), "a null gesture is stationary");
    }
}

/*
 * A gesture whose endpoints coincide is legal — it is a held contact with the
 * stationary updates --touch cannot make — and must be LABELLED, because a
 * "drag" that never moves is exactly the thing this whole option exists to
 * stop happening silently.
 */
static void test_a_stationary_drag_is_legal_and_says_so(void) {
    mt_drag_t still  = { 100u, 100u, 100u, 100u, 4u, 32960000ull };
    mt_drag_t moving = { 100u, 100u, 101u, 100u, 4u, 32960000ull };
    unsigned n = mt_drag_reports(&still);
    CHECK(mt_drag_valid(&still), "a held contact was refused");
    CHECK(mt_drag_stationary(&still), "a held contact is not labelled");
    CHECK(!mt_drag_stationary(&moving),
          "a one-pixel drag is labelled as stationary");
    for (unsigned k = 0; k < n; k++) {
        s5l_mt_contact_t c;
        if (!mt_drag_contact(&still, k, &c)) continue;
        CHECK(c.x == 100u && c.y == 100u,
              "a held contact moved to (%u,%u) at report %u", c.x, c.y, k);
    }
}

/* ------------------------------------------------------------------------- */

/* Bring a device up the way the guest does, condensed from the sequence
 * test_mtz2.c replays in full: the dummy 16-byte HBPP transfer while the reset
 * line is asserted, then the real one with it released. Anything less leaves
 * hbpp_answered clear and s5l_mtz2_set_contacts() refuses everything. */
static void bring_up(s5l_mtz2_t *dev, s5l_spi_slave_t *s) {
    uint8_t tx[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(dev);
    s5l_mtz2_bind(dev, s);
    memset(tx, 0, sizeof tx);
    tx[0] = 0x1au; tx[1] = 0xa1u;
    for (unsigned i = 2; i < MTZ2_FRAME_LEN; i += 2u) {
        tx[i] = 0x18u; tx[i + 1u] = 0xe1u;
    }
    for (unsigned i = 0; i < MTZ2_FRAME_LEN; i++)
        (void)s->transfer(s->ctx, tx[i]);        /* the dummy, inside reset  */
    s5l_mtz2_reset_pin(dev, true);
    for (unsigned i = 0; i < MTZ2_FRAME_LEN; i++)
        (void)s->transfer(s->ctx, tx[i]);        /* the probe, out of reset  */
}

/*
 * EVERY REPORT IS ONE THE DEVICE WILL TAKE. The planner can be internally
 * consistent and still describe a contact s5l_mtz2_set_contacts() rejects — an
 * identifier outside 1..5, a phase above OUT_OF_RANGE, a coordinate off the
 * panel — and that failure would look identical to a busy device from inside
 * the run. Encoding every report proves the whole gesture is injectable.
 */
static void test_the_device_encodes_every_report(void) {
    s5l_mtz2_t dev;
    s5l_mtz2_reset(&dev);
    for (unsigned i = 0; i < NCASES; i++) {
        unsigned n = mt_drag_reports(&CASES[i]);
        for (unsigned k = 0; k < n; k++) {
            uint8_t out[MTZ2_PAYLOAD_LIMIT];
            s5l_mt_contact_t c;
            unsigned len;
            if (!mt_drag_contact(&CASES[i], k, &c)) continue;
            len = s5l_mtz2_encode(&dev, &c, 1u, 1u, 16u, out);
            CHECK(len == MTZ2_FRAME_HEADER + MTZ2_CONTACT_STRIDE,
                  "%s report %u encoded to %u bytes, expected %u — the device "
                  "rejected the contact this gesture describes",
                  case_name(i), k, len,
                  MTZ2_FRAME_HEADER + MTZ2_CONTACT_STRIDE);
            if (!len) continue;
            /* The bytes the guest actually reads back, at the offsets
             * mtz2.c documents: r[0] path id, r[1] stage, r[20..21] Z total. */
            CHECK(out[MTZ2_FRAME_HEADER + 0u] == c.id,
                  "%s report %u encodes path id %u, not %u",
                  case_name(i), k, out[MTZ2_FRAME_HEADER], c.id);
            CHECK(out[MTZ2_FRAME_HEADER + 1u] == c.phase,
                  "%s report %u encodes stage %u (%s), not %u (%s)",
                  case_name(i), k, out[MTZ2_FRAME_HEADER + 1u],
                  mt_drag_phase_name(out[MTZ2_FRAME_HEADER + 1u]),
                  c.phase, mt_drag_phase_name(c.phase));
            CHECK((out[MTZ2_FRAME_HEADER + 20u] |
                   (out[MTZ2_FRAME_HEADER + 21u] << 8)) == c.pressure,
                  "%s report %u encodes Z total %u, not %u", case_name(i), k,
                  out[MTZ2_FRAME_HEADER + 20u] |
                      (out[MTZ2_FRAME_HEADER + 21u] << 8), c.pressure);
        }
    }
}

/*
 * WHY THE HARNESS RETRIES RATHER THAN PACES BLINDLY, demonstrated rather than
 * asserted in a comment: a brought-up device accepts one report and then
 * REFUSES the next while the first is unread. That is the reason
 * touch_drag_step() never attempts report k+1 before report k was accepted —
 * a planner that just emitted on a timer would silently drop the middle of a
 * gesture and leave a lift the guest cannot explain.
 */
static void test_the_device_holds_one_report_at_a_time(void) {
    mt_drag_t d = { 10u, 240u, 300u, 240u, 8u, 59328000ull };
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    s5l_mt_contact_t c0, c1;
    bring_up(&dev, &s);
    CHECK(mt_drag_contact(&d, 0u, &c0) && mt_drag_contact(&d, 1u, &c1),
          "the first two reports of a valid drag could not be built");
    CHECK(s5l_mtz2_set_contacts(&dev, &c0, 1u),
          "a brought-up device refused the landing (refused=%llu)",
          (unsigned long long)dev.injects_refused);
    CHECK(c0.phase == MTZ2_PHASE_MAKE_TOUCH && c1.phase == MTZ2_PHASE_TOUCHING,
          "the first two reports are %s then %s",
          mt_drag_phase_name(c0.phase), mt_drag_phase_name(c1.phase));
    CHECK(!s5l_mtz2_set_contacts(&dev, &c1, 1u),
          "the device took a second report while the first was unread — the "
          "finger-down half of every gesture would be silently discarded");
    CHECK(dev.injects_refused == 1u,
          "the refusal was not counted (injects_refused=%llu)",
          (unsigned long long)dev.injects_refused);
    CHECK(dev.frames_queued == 1u,
          "%llu frames are queued after one accepted report",
          (unsigned long long)dev.frames_queued);
}

int main(void) {
    printf("S5LBox multi-touch drag gesture tests\n");
    test_the_phase_sequence_is_one_finger();
    test_the_contact_identifier_never_changes();
    test_pressure_says_the_same_thing_as_the_phase();
    test_the_path_hits_both_ends_and_never_turns_back();
    test_reference_contact();
    test_reverse_is_the_same_path_backwards();
    test_the_default_pacing_is_the_devices_own_scan_rate();
    test_an_impossible_gesture_is_refused();
    test_a_stationary_drag_is_legal_and_says_so();
    test_the_device_encodes_every_report();
    test_the_device_holds_one_report_at_a_time();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
