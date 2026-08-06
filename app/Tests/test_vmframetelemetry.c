/* Host checks for the opt-in cross-thread frame-pipeline counter. */
#include "VMFrameTelemetry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned tests;
static unsigned failed;

#define CHECK(expr, ...) do {                                                \
    tests++;                                                                 \
    if (!(expr)) {                                                           \
        failed++;                                                            \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
    }                                                                        \
} while (0)

static void test_disabled_is_inert(void) {
    uint8_t pixels[800];
    memset(pixels, 0x5a, sizeof pixels);
    vm_frame_telemetry_reset(false);
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 100u, 5u, 6000000u, 412000000u);
    vm_frame_telemetry_note_layer_submission(
        pixels, sizeof pixels, true, 500u);

    vm_frame_telemetry_snapshot_t state;
    memset(&state, 0xa5, sizeof state);
    vm_frame_telemetry_snapshot(&state);
    CHECK(!state.enabled, "disabled state reported enabled");
    CHECK(state.scanout_attempts == 0u && state.layer_attempts == 0u,
          "disabled instrumentation changed counters");
}

static void test_boundaries_and_sampled_changes(void) {
    uint8_t pixels[800];
    for (size_t i = 0; i < sizeof pixels; i++)
        pixels[i] = (uint8_t)(i * 37u + 11u);

    vm_frame_telemetry_reset(true);
    CHECK(vm_frame_telemetry_is_enabled(), "reset did not enable telemetry");

    vm_frame_telemetry_note_scanout(
        NULL, 0, 100u, 5u, 6000000u, 412000000u);
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 200u, 6u, 6000000u, 412000000u);
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 300u, 7u, 6000000u, 412000000u);
    pixels[5] ^= 1u; /* deliberately outside the 397-byte sample */
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 400u, 8u, 6000000u, 412000000u);
    pixels[397] ^= 1u;
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 500u, 9u, 6000000u, 412000000u);

    vm_frame_telemetry_note_layer_submission(
        NULL, 0, false, 100u);
    vm_frame_telemetry_note_layer_submission(
        pixels, sizeof pixels, true, 200u);
    vm_frame_telemetry_note_layer_submission(
        pixels, sizeof pixels, true, 300u);
    pixels[397] ^= 2u;
    vm_frame_telemetry_note_layer_submission(
        pixels, sizeof pixels, true, 400u);

    vm_frame_telemetry_snapshot_t state;
    memset(&state, 0, sizeof state);
    vm_frame_telemetry_snapshot(&state);
    CHECK(state.enabled, "enabled state reported disabled");
    CHECK(state.scanout_attempts == 5u && state.scanout_valid == 4u,
          "scanout counts=%llu/%llu",
          (unsigned long long)state.scanout_attempts,
          (unsigned long long)state.scanout_valid);
    CHECK(state.scanout_changes == 2u,
          "sampled scanout changes=%llu, expected 2",
          (unsigned long long)state.scanout_changes);
    CHECK(state.scanout_guest_clock_captured &&
          state.scanout_guest_clock_consistent,
          "guest clock was not captured consistently");
    CHECK(state.scanout_first_timer_ticks == 100u &&
          state.scanout_last_timer_ticks == 500u &&
          state.scanout_first_clcd_frames == 5u &&
          state.scanout_last_clcd_frames == 9u,
          "guest clock endpoints are wrong");
    CHECK(state.scanout_timebase_hz == 6000000u &&
          state.scanout_cpu_hz == 412000000u,
          "guest clock rates are wrong");

    CHECK(state.layer_attempts == 4u && state.layer_accepted == 3u &&
          state.layer_rejected == 1u,
          "layer counts=%llu/%llu/%llu",
          (unsigned long long)state.layer_attempts,
          (unsigned long long)state.layer_accepted,
          (unsigned long long)state.layer_rejected);
    CHECK(state.layer_changes == 2u,
          "sampled layer changes=%llu, expected 2",
          (unsigned long long)state.layer_changes);
    CHECK(state.layer_total_work_ns == 1000u &&
          state.layer_max_work_ns == 400u,
          "layer work totals=%llu/%llu",
          (unsigned long long)state.layer_total_work_ns,
          (unsigned long long)state.layer_max_work_ns);

    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 10u, 1u, 3000000u, 412000000u);
    vm_frame_telemetry_snapshot(&state);
    CHECK(!state.scanout_guest_clock_consistent,
          "clock-rate/counter regression was accepted as consistent");
}

int main(void) {
    test_disabled_is_inert();
    test_boundaries_and_sampled_changes();
    printf("vm frame telemetry: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}
