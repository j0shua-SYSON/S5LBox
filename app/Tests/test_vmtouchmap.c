/*
 * Host-side tests for the touch coordinate mapping used by the iOS app.
 *
 * The app is built by a macOS CI runner and run on a phone, so nothing about
 * its Objective-C can be exercised on the dev box. The arithmetic that decides
 * WHICH GUEST PIXEL a finger landed on is the part where a mistake is both
 * easy and silent -- an off-by-one at the edge, a truncation instead of a
 * floor, a letterbox measured on the wrong axis -- and it is pure C, so it is
 * tested here, on every host, in milliseconds.
 *
 * The scale factors below are deliberately not round numbers: 375/320 is what
 * an iPhone 6/7/8-width view actually produces, and it is exactly the case a
 * test written with a 2x scale would never catch.
 */
#include "VMTouchMap.h"

#include <math.h>
#include <stdio.h>

static unsigned tests;
static unsigned failed;

#define CHECK(expr, ...) do {                                                \
    tests++;                                                                 \
    if (!(expr)) {                                                           \
        failed++;                                                            \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                 \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
    }                                                                        \
} while (0)

/* One place to say "this point must land exactly here", so a failure prints
 * the coordinate it got rather than just the line number. */
static void expect_inside(double vw, double vh, unsigned gw, unsigned gh,
                          double px, double py, int want_x, int want_y,
                          const char *what) {
    vm_touch_point_t got = vm_touch_map(vw, vh, gw, gh, px, py);
    CHECK(got.inside && got.x == want_x && got.y == want_y,
          "%s: (%.4f,%.4f) in %.1fx%.1f -> {%d,%d,%s}, wanted {%d,%d,inside}",
          what, px, py, vw, vh, got.x, got.y,
          got.inside ? "inside" : "outside", want_x, want_y);
}

static void expect_outside(double vw, double vh, unsigned gw, unsigned gh,
                           double px, double py, const char *what) {
    vm_touch_point_t got = vm_touch_map(vw, vh, gw, gh, px, py);
    CHECK(!got.inside && got.x == 0 && got.y == 0,
          "%s: (%.4f,%.4f) in %.1fx%.1f -> {%d,%d,%s}, wanted outside+zeroed",
          what, px, py, vw, vh, got.x, got.y,
          got.inside ? "inside" : "outside");
}

/* The view is exactly the guest's aspect: no bars, scale 1. */
static void test_exact_fit_corners(void) {
    expect_inside(320, 480, 320, 480, 0.0, 0.0, 0, 0, "top-left corner");
    expect_inside(320, 480, 320, 480, 0.5, 0.5, 0, 0, "inside first pixel");
    expect_inside(320, 480, 320, 480, 319.5, 479.5, 319, 479, "last pixel");
    expect_inside(320, 480, 320, 480, 319.999, 479.999, 319, 479,
                  "just inside the far edge");

    /* Half-open: the far edge itself belongs to the pixel that does not exist. */
    expect_outside(320, 480, 320, 480, 320.0, 240.0, "exact right edge");
    expect_outside(320, 480, 320, 480, 160.0, 480.0, "exact bottom edge");
    expect_outside(320, 480, 320, 480, -0.001, 0.0, "one hair left of origin");
    expect_outside(320, 480, 320, 480, 0.0, -0.001, "one hair above origin");
}

/* A view wider than 320:480 puts the bars on the left and right. */
static void test_horizontal_letterbox(void) {
    /* 800x480: the height runs out first, scale 1.0, 240 points of bar a side. */
    expect_outside(800, 480, 320, 480, 0.0, 240.0, "far left bar");
    expect_outside(800, 480, 320, 480, 239.999, 240.0, "right edge of left bar");
    expect_inside(800, 480, 320, 480, 240.0, 0.0, 0, 0, "panel top-left");
    expect_inside(800, 480, 320, 480, 559.999, 479.999, 319, 479,
                  "panel bottom-right");
    expect_outside(800, 480, 320, 480, 560.0, 240.0, "left edge of right bar");
    expect_outside(800, 480, 320, 480, 799.0, 240.0, "far right bar");
    expect_inside(800, 480, 320, 480, 400.0, 240.0, 160, 240, "panel centre");
}

/* A view taller than 320:480 puts the bars above and below. */
static void test_vertical_letterbox(void) {
    /* 320x960: the width runs out first, scale 1.0, 240 points of bar top and
     * bottom. This is the axis a mapping written for one orientation forgets. */
    expect_outside(320, 960, 320, 480, 160.0, 0.0, "top bar");
    expect_outside(320, 960, 320, 480, 160.0, 239.999, "bottom of top bar");
    expect_inside(320, 960, 320, 480, 0.0, 240.0, 0, 0, "panel top-left");
    expect_inside(320, 960, 320, 480, 319.999, 719.999, 319, 479,
                  "panel bottom-right");
    expect_outside(320, 960, 320, 480, 160.0, 720.0, "top of bottom bar");
    expect_outside(320, 960, 320, 480, 160.0, 959.0, "bottom bar");
}

/*
 * The case a 2x test never reaches: a 375-point-wide view showing a 320-wide
 * guest is a scale of 1.171875, and every boundary below lands on a fraction.
 * 320 * 1.171875 == 375 exactly and 480 * 1.171875 == 562.5, so the vertical
 * bars are 52.25 points each.
 */
static void test_fractional_scale(void) {
    const double vw = 375.0, vh = 667.0;
    const double scale = 375.0 / 320.0;          /* 1.171875 */
    const double bar = (vh - 480.0 * scale) * 0.5;   /* 52.25 */

    CHECK(fabs(scale - 1.171875) < 1e-12, "scale drifted: %.17g", scale);
    CHECK(fabs(bar - 52.25) < 1e-12, "bar drifted: %.17g", bar);

    expect_outside(vw, vh, 320, 480, 10.0, bar - 0.001, "just above the panel");
    expect_inside(vw, vh, 320, 480, 0.0, bar, 0, 0, "panel top-left");
    expect_inside(vw, vh, 320, 480, 374.999, bar + 562.499, 319, 479,
                  "panel bottom-right");
    expect_outside(vw, vh, 320, 480, 187.0, bar + 562.5, "exact bottom edge");
    expect_outside(vw, vh, 320, 480, 375.0, bar + 10.0, "exact right edge");

    /* Halfway across and halfway down, to the pixel. */
    expect_inside(vw, vh, 320, 480, 160.0 * scale, bar + 240.0 * scale,
                  160, 240, "panel centre");

    /* Every pixel boundary in the first and last few columns, so a rounding
     * error of one ULP in the divide would show up as an off-by-one. */
    for (int column = 0; column < 320; column++) {
        double left = column * scale;
        expect_inside(vw, vh, 320, 480, left + scale * 0.5, bar + 1.0,
                      column, 0, "column centre");
    }
}

/* The guest can also be bigger than the view -- an iPhone SE in landscape, or
 * the app in a small window -- and minification must map just as exactly. */
static void test_minification(void) {
    expect_inside(160, 240, 320, 480, 0.0, 0.0, 0, 0, "half scale top-left");
    expect_inside(160, 240, 320, 480, 159.999, 239.999, 319, 479,
                  "half scale bottom-right");
    expect_inside(160, 240, 320, 480, 80.0, 120.0, 160, 240, "half scale centre");
    expect_outside(160, 240, 320, 480, 160.0, 120.0, "half scale right edge");

    /* Absurdly small is still legal. 5/320 is 1/64, so the scale is a power of
     * two and the expected answer is exact rather than a rounding argument. */
    vm_touch_point_t got = vm_touch_map(5.0, 7.5, 320, 480, 2.5, 3.75);
    CHECK(got.inside && got.x == 160 && got.y == 240,
          "five-point view -> {%d,%d,%s}", got.x, got.y,
          got.inside ? "inside" : "outside");
}

/* Nothing may divide by zero, index with a negative, or trust a NaN. */
static void test_degenerate_inputs(void) {
    expect_outside(0.0, 480.0, 320, 480, 0.0, 0.0, "zero-width view");
    expect_outside(320.0, 0.0, 320, 480, 0.0, 0.0, "zero-height view");
    expect_outside(-320.0, 480.0, 320, 480, 0.0, 0.0, "negative view width");
    expect_outside(320.0, -480.0, 320, 480, 0.0, 0.0, "negative view height");
    expect_outside(320.0, 480.0, 0u, 480u, 10.0, 10.0, "zero-width guest");
    expect_outside(320.0, 480.0, 320u, 0u, 10.0, 10.0, "zero-height guest");
    expect_outside(320.0, 480.0, 0x80000000u, 480u, 10.0, 10.0,
                   "guest wider than INT_MAX");
    expect_outside(320.0, 480.0, 320u, 0x80000000u, 10.0, 10.0,
                   "guest taller than INT_MAX");

    const double nan_value = NAN;
    const double inf_value = INFINITY;
    expect_outside(320.0, 480.0, 320, 480, nan_value, 10.0, "NaN x");
    expect_outside(320.0, 480.0, 320, 480, 10.0, nan_value, "NaN y");
    expect_outside(nan_value, 480.0, 320, 480, 10.0, 10.0, "NaN view width");
    expect_outside(320.0, nan_value, 320, 480, 10.0, 10.0, "NaN view height");
    expect_outside(inf_value, 480.0, 320, 480, 10.0, 10.0, "infinite view width");
    expect_outside(320.0, 480.0, 320, 480, inf_value, 10.0, "infinite x");
    expect_outside(320.0, 480.0, 320, 480, -inf_value, 10.0, "negative infinite x");
}

/*
 * A sweep, because the cases above are the ones I thought of. Walk a grid over
 * a letterboxed view and assert the invariant that actually matters: an
 * "inside" verdict is never a coordinate the guest cannot index.
 */
static void test_sweep_never_escapes_the_panel(void) {
    const double vw = 414.0, vh = 736.0;      /* iPhone 8 Plus points */
    const unsigned gw = 320u, gh = 480u;
    unsigned inside_count = 0;
    int escaped = 0;

    for (int i = 0; i <= 400; i++) {
        for (int j = 0; j <= 400; j++) {
            double px = -20.0 + (vw + 40.0) * (double)i / 400.0;
            double py = -20.0 + (vh + 40.0) * (double)j / 400.0;
            vm_touch_point_t got = vm_touch_map(vw, vh, gw, gh, px, py);
            if (!got.inside) {
                if (got.x != 0 || got.y != 0) escaped = 1;
                continue;
            }
            inside_count++;
            if (got.x < 0 || got.y < 0 ||
                got.x >= (int)gw || got.y >= (int)gh) escaped = 1;
        }
    }

    CHECK(!escaped, "a swept point escaped the guest's coordinate range");
    CHECK(inside_count > 100000u,
          "sweep only found %u points on the panel; the fit looks wrong",
          inside_count);
}

/* ------------------------------------------------------------------ tracker */

static vm_touch_point_t at(int x, int y) {
    vm_touch_point_t p;
    p.x = x; p.y = y; p.inside = true;
    return p;
}

static vm_touch_point_t offscreen(void) {
    vm_touch_point_t p;
    p.x = 0; p.y = 0; p.inside = false;
    return p;
}

/* Feed one event and assert both the verdict and, when reported, where. */
static void expect_report(vm_touch_tracker_t *t, vm_touch_phase_t phase,
                          vm_touch_point_t mapped, int want_x, int want_y,
                          const char *what) {
    int x = -1, y = -1;
    bool reported = vm_touch_track(t, phase, mapped, &x, &y);
    CHECK(reported && x == want_x && y == want_y,
          "%s: reported=%d at (%d,%d), wanted (%d,%d)",
          what, (int)reported, x, y, want_x, want_y);
}

static void expect_silence(vm_touch_tracker_t *t, vm_touch_phase_t phase,
                           vm_touch_point_t mapped, const char *what) {
    int x = -1, y = -1;
    CHECK(!vm_touch_track(t, phase, mapped, &x, &y),
          "%s: was reported at (%d,%d) when it should have been dropped",
          what, x, y);
}

static void test_tracker_ordinary_gesture(void) {
    vm_touch_tracker_t t;
    vm_touch_tracker_reset(&t);

    expect_report(&t, VM_TOUCH_BEGAN, at(10, 20), 10, 20, "began on the panel");
    expect_report(&t, VM_TOUCH_MOVED, at(11, 25), 11, 25, "moved on the panel");
    expect_report(&t, VM_TOUCH_ENDED, at(12, 30), 12, 30, "ended on the panel");

    // And the gesture is over: a stray move afterwards is not a gesture.
    expect_silence(&t, VM_TOUCH_MOVED, at(13, 31), "moved after the end");
    expect_silence(&t, VM_TOUCH_ENDED, at(13, 31), "ended twice");
}

static void test_tracker_requires_a_begin(void) {
    vm_touch_tracker_t t;
    vm_touch_tracker_reset(&t);

    expect_silence(&t, VM_TOUCH_MOVED, at(5, 5), "moved with no begin");
    expect_silence(&t, VM_TOUCH_ENDED, at(5, 5), "ended with no begin");
    expect_silence(&t, VM_TOUCH_CANCELLED, at(5, 5), "cancelled with no begin");
}

static void test_tracker_ignores_the_letterbox(void) {
    vm_touch_tracker_t t;
    vm_touch_tracker_reset(&t);

    expect_silence(&t, VM_TOUCH_BEGAN, offscreen(), "began in the letterbox");
    // Nothing was started, so nothing that follows it is a gesture either.
    expect_silence(&t, VM_TOUCH_MOVED, at(9, 9), "moved after a letterbox begin");
    expect_silence(&t, VM_TOUCH_ENDED, at(9, 9), "ended after a letterbox begin");
}

/*
 * The defect this state machine was extracted to make testable: if an end is
 * ever missed, a NEW gesture beginning in the letterbox must not inherit the
 * old one's coordinates. VMFramebufferView.h promises a letterbox touch is not
 * reported at all, and that promise has to survive a lost end.
 */
static void test_tracker_letterbox_begin_clears_a_stuck_gesture(void) {
    vm_touch_tracker_t t;
    vm_touch_tracker_reset(&t);

    expect_report(&t, VM_TOUCH_BEGAN, at(100, 200), 100, 200, "first gesture");
    // ...and its end never arrives: a modal appears, the view is removed.

    expect_silence(&t, VM_TOUCH_BEGAN, offscreen(), "second began in the bar");
    CHECK(!t.active, "a letterbox begin left the tracker live");
    expect_silence(&t, VM_TOUCH_MOVED, offscreen(),
                   "second gesture moved while untracked");
    expect_silence(&t, VM_TOUCH_ENDED, offscreen(),
                   "second gesture ended at the first one's coordinates");
}

static void test_tracker_drag_off_the_panel(void) {
    vm_touch_tracker_t t;
    vm_touch_tracker_reset(&t);

    expect_report(&t, VM_TOUCH_BEGAN, at(300, 400), 300, 400, "began");
    expect_report(&t, VM_TOUCH_MOVED, at(319, 470), 319, 470, "moved to the edge");
    expect_silence(&t, VM_TOUCH_MOVED, offscreen(), "dragged into the bar");
    CHECK(t.active, "dragging off the panel ended the gesture early");

    // The end must still arrive, at the last coordinate that was on the panel.
    expect_report(&t, VM_TOUCH_ENDED, offscreen(), 319, 470,
                  "ended outside the panel");
    CHECK(!t.active, "the end left the tracker live");
}

static void test_tracker_cancel_and_restart(void) {
    vm_touch_tracker_t t;
    vm_touch_tracker_reset(&t);

    expect_report(&t, VM_TOUCH_BEGAN, at(1, 2), 1, 2, "first began");
    expect_report(&t, VM_TOUCH_CANCELLED, at(3, 4), 3, 4, "first cancelled");
    expect_silence(&t, VM_TOUCH_MOVED, at(5, 6), "moved after the cancel");

    // A second gesture is completely independent of the first.
    expect_report(&t, VM_TOUCH_BEGAN, at(7, 8), 7, 8, "second began");
    expect_report(&t, VM_TOUCH_ENDED, at(7, 8), 7, 8, "second ended");
}

static void test_tracker_null_and_optional_outputs(void) {
    CHECK(!vm_touch_track(NULL, VM_TOUCH_BEGAN, at(1, 1), NULL, NULL),
          "a null tracker reported a touch");
    vm_touch_tracker_reset(NULL);   /* must not crash */

    vm_touch_tracker_t t;
    vm_touch_tracker_reset(&t);
    CHECK(!t.active && t.x == 0 && t.y == 0, "reset left state behind");
    CHECK(vm_touch_track(&t, VM_TOUCH_BEGAN, at(2, 3), NULL, NULL),
          "a begin with no output pointers was dropped");
    CHECK(t.x == 2 && t.y == 3, "the tracker did not record the begin");
}

int main(void) {
    test_exact_fit_corners();
    test_horizontal_letterbox();
    test_vertical_letterbox();
    test_fractional_scale();
    test_minification();
    test_degenerate_inputs();
    test_sweep_never_escapes_the_panel();

    test_tracker_ordinary_gesture();
    test_tracker_requires_a_begin();
    test_tracker_ignores_the_letterbox();
    test_tracker_letterbox_begin_clears_a_stuck_gesture();
    test_tracker_drag_off_the_panel();
    test_tracker_cancel_and_restart();
    test_tracker_null_and_optional_outputs();

    printf("vmtouchmap: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}
