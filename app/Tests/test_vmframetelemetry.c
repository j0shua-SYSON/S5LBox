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

static vm_frame_scanout_observation_t observation(
        vm_frame_scanout_reason_t reason) {
    vm_frame_scanout_observation_t value;
    memset(&value, 0, sizeof value);
    value.reason = reason;
    value.active_window = UINT32_MAX;
    return value;
}

static vm_execution_telemetry_observation_t execution_observation(
        uint64_t base) {
    vm_execution_telemetry_observation_t value;
    memset(&value, 0, sizeof value);
    value.cpu_retired = base + 1u;
    value.interpreter_tick_batches = base + 2u;
    value.interpreter_tick_batched_retired = base + 3u;
    value.static_native_retired = base + 4u;
    value.compact_attempts = base + 5u;
    value.compact_calls = base + 6u;
    value.compact_native_retired = base + 7u;
    value.compact_fallback_retired = base + 8u;
    value.compact_privileged_attempts = base + 9u;
    value.compact_privileged_calls = base + 10u;
    value.compact_privileged_retired = base + 11u;
    value.compact_window_crossings = base + 12u;
    value.compact_window_reloads = base + 13u;
    value.compact_window_fast_refills = base + 14u;
    value.compact_window_stops = base + 15u;
    value.compact_refused_guard = base + 16u;
    value.compact_refused_privileged = base + 17u;
    value.compact_refused_alignment = base + 18u;
    value.compact_refused_fetch_witness = base + 19u;
    value.compact_refused_runner = base + 20u;
    value.compact_zero_retired = base + 21u;
    value.fetch_refill_attempts = base + 22u;
    value.fetch_refill_hits = base + 23u;
    value.fetch_refill_skips = base + 24u;
    value.known_negative_bypasses = base + 25u;
    value.mbx_2d_candidates = base + 26u;
    value.mbx_2d_completed = base + 27u;
    value.mbx_2d_rejected = base + 28u;
    value.mbx_2d_bytes = base + 29u;
    value.mbx_3d_candidates = base + 30u;
    value.mbx_3d_completed = base + 31u;
    value.mbx_3d_rejected = base + 32u;
    value.mbx_3d_pixels = base + 33u;
    value.active_clock_updates = base + 34u;
    value.active_clock_added_ticks = base + 35u;
    value.active_clock_clamps = base + 36u;
    value.active_clock_failures = base + 37u;
    value.compact_privileged_window_refills = base + 38u;
    value.compact_privileged_boundary_retired = base + 39u;
    value.compact_window_cache_hits = base + 40u;
    value.compact_pc_profile_samples = base + 41u;
    value.compact_pc_profile_outside = base + 42u;
    value.compact_pc_profile_entry = base + 43u;
    value.compact_pc_profile_dp = base + 44u;
    value.compact_pc_profile_memory = base + 45u;
    value.compact_pc_profile_block_control = base + 46u;
    value.compact_pc_profile_system = base + 47u;
    value.compact_pc_profile_vfp = base + 48u;
    value.compact_pc_profile_thumb = base + 49u;
    value.compact_pc_profile_retire = base + 50u;
    value.compact_pc_profile_fallback = base + 51u;
    value.compact_pc_profile_exit = base + 52u;
    return value;
}

static void test_disabled_is_inert(void) {
    uint8_t pixels[800];
    memset(pixels, 0x5a, sizeof pixels);
    vm_frame_scanout_observation_t valid =
        observation(VM_FRAME_SCANOUT_REASON_VALID);
    vm_frame_telemetry_reset(false);
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 100u, 5u, 6000000u, 412000000u, &valid);
    vm_frame_telemetry_note_layer_submission(
        pixels, sizeof pixels, true, 500u);
    vm_execution_telemetry_observation_t execution =
        execution_observation(100u);
    vm_frame_telemetry_note_execution(&execution);

    vm_frame_telemetry_snapshot_t state;
    memset(&state, 0xa5, sizeof state);
    vm_frame_telemetry_snapshot(&state);
    CHECK(!state.enabled, "disabled state reported enabled");
    CHECK(state.scanout_attempts == 0u && state.layer_attempts == 0u,
          "disabled instrumentation changed counters");
    CHECK(!state.execution_captured && state.execution_observations == 0u,
          "disabled instrumentation captured execution counters");
}

static void test_boundaries_and_sampled_changes(void) {
    uint8_t pixels[800];
    for (size_t i = 0; i < sizeof pixels; i++)
        pixels[i] = (uint8_t)(i * 37u + 11u);

    vm_frame_telemetry_reset(true);
    CHECK(vm_frame_telemetry_is_enabled(), "reset did not enable telemetry");
    vm_execution_telemetry_observation_t execution_first =
        execution_observation(1000u);
    vm_frame_telemetry_note_execution(&execution_first);

    vm_frame_scanout_observation_t stopped =
        observation(VM_FRAME_SCANOUT_REASON_STOPPED);
    stopped.ctrl = 0x41u;
    stopped.gate = 0x441u;
    stopped.active_window = 0u;
    vm_frame_scanout_observation_t valid =
        observation(VM_FRAME_SCANOUT_REASON_VALID);
    valid.scanning = 1u;
    valid.ctrl = 0x41u;
    valid.gate = 0x441u;
    valid.active_window = 0u;
    valid.framebuffer_phys = 0x0ff6a000u;
    valid.width = 320u;
    valid.height = 480u;
    valid.stride = 1280u;
    valid.format = 6u;

    vm_frame_telemetry_note_scanout(
        NULL, 0, 100u, 5u, 6000000u, 412000000u, &stopped);
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 200u, 6u, 6000000u, 412000000u, &valid);
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 300u, 7u, 6000000u, 412000000u, &valid);
    pixels[5] ^= 1u; /* deliberately outside the 397-byte sample */
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 400u, 8u, 6000000u, 412000000u, &valid);
    pixels[397] ^= 1u;
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 500u, 9u, 6000000u, 412000000u, &valid);

    vm_frame_telemetry_note_layer_submission(
        NULL, 0, false, 100u);
    vm_frame_telemetry_note_layer_submission(
        pixels, sizeof pixels, true, 200u);
    vm_frame_telemetry_note_layer_submission(
        pixels, sizeof pixels, true, 300u);
    pixels[397] ^= 2u;
    vm_frame_telemetry_note_layer_submission(
        pixels, sizeof pixels, true, 400u);
    vm_execution_telemetry_observation_t execution_last =
        execution_observation(2000u);
    vm_frame_telemetry_note_execution(&execution_last);

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
    CHECK(state.scanout_reason_counts[VM_FRAME_SCANOUT_REASON_STOPPED] == 1u &&
          state.scanout_reason_counts[VM_FRAME_SCANOUT_REASON_VALID] == 4u,
          "scanout reason counts stopped=%llu valid=%llu",
          (unsigned long long)state.scanout_reason_counts[
              VM_FRAME_SCANOUT_REASON_STOPPED],
          (unsigned long long)state.scanout_reason_counts[
              VM_FRAME_SCANOUT_REASON_VALID]);
    CHECK(state.scanout_last.reason == VM_FRAME_SCANOUT_REASON_VALID &&
          state.scanout_last_reason_streak == 4u,
          "last scanout reason/streak=%s/%llu",
          vm_frame_scanout_reason_name(state.scanout_last.reason),
          (unsigned long long)state.scanout_last_reason_streak);
    CHECK(state.scanout_last.framebuffer_phys == 0x0ff6a000u &&
          state.scanout_last.width == 320u &&
          state.scanout_last.height == 480u &&
          state.scanout_last.stride == 1280u,
          "last raw scanout metadata was not preserved");
    CHECK(state.scanout_last_valid_timer_ticks == 500u &&
          state.scanout_last_valid_clcd_frames == 9u &&
          state.scanout_last_valid_host_ns != 0u,
          "last-valid boundary was not captured");
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
    CHECK(state.execution_captured && state.execution_consistent &&
          state.execution_observations == 2u,
          "execution endpoint state captured/consistent/count=%u/%u/%llu",
          state.execution_captured ? 1u : 0u,
          state.execution_consistent ? 1u : 0u,
          (unsigned long long)state.execution_observations);
    CHECK(state.execution_first.cpu_retired == 1001u &&
           state.execution_last.cpu_retired == 2001u &&
           state.execution_first.compact_refused_privileged == 1017u &&
           state.execution_last.compact_window_fast_refills == 2014u &&
           state.execution_last.fetch_refill_skips == 2024u &&
           state.execution_last.known_negative_bypasses == 2025u &&
           state.execution_last.mbx_2d_bytes == 2029u &&
           state.execution_last.mbx_3d_pixels == 2033u &&
           state.execution_last.active_clock_updates == 2034u &&
           state.execution_last.active_clock_added_ticks == 2035u &&
           state.execution_last.active_clock_clamps == 2036u &&
           state.execution_last.active_clock_failures == 2037u &&
           state.execution_last.compact_privileged_window_refills == 2038u &&
           state.execution_last.compact_privileged_boundary_retired == 2039u &&
           state.execution_last.compact_window_cache_hits == 2040u &&
           state.execution_last.compact_pc_profile_samples == 2041u &&
           state.execution_last.compact_pc_profile_outside == 2042u &&
           state.execution_last.compact_pc_profile_entry == 2043u &&
           state.execution_last.compact_pc_profile_dp == 2044u &&
           state.execution_last.compact_pc_profile_memory == 2045u &&
           state.execution_last.compact_pc_profile_block_control == 2046u &&
           state.execution_last.compact_pc_profile_system == 2047u &&
           state.execution_last.compact_pc_profile_vfp == 2048u &&
           state.execution_last.compact_pc_profile_thumb == 2049u &&
           state.execution_last.compact_pc_profile_retire == 2050u &&
           state.execution_last.compact_pc_profile_fallback == 2051u &&
           state.execution_last.compact_pc_profile_exit == 2052u,
          "execution counter endpoints are wrong");

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
        pixels, sizeof pixels, 10u, 1u, 3000000u, 412000000u, &valid);
    vm_frame_telemetry_snapshot(&state);
    CHECK(!state.scanout_guest_clock_consistent,
          "clock-rate/counter regression was accepted as consistent");
    vm_execution_telemetry_observation_t execution_regressed =
        execution_observation(1500u);
    vm_frame_telemetry_note_execution(&execution_regressed);
    vm_frame_telemetry_snapshot(&state);
    CHECK(!state.execution_consistent &&
          state.execution_observations == 3u,
          "execution counter regression was accepted as consistent");
    CHECK(strcmp(vm_frame_scanout_reason_name(
                     VM_FRAME_SCANOUT_REASON_FRAMEBUFFER_OUTSIDE_RAM),
                 "framebuffer_outside_ram") == 0 &&
          strcmp(vm_frame_scanout_reason_name(
                     (vm_frame_scanout_reason_t)UINT32_MAX),
                 "unknown") == 0,
          "scanout reason names are not stable");
}

static void test_worst_scanout_gap_keeps_its_work_witness(void) {
    uint8_t pixels[800];
    memset(pixels, 0x6b, sizeof pixels);
    vm_frame_scanout_observation_t valid =
        observation(VM_FRAME_SCANOUT_REASON_VALID);

    vm_frame_telemetry_reset(true);
    vm_execution_telemetry_observation_t first =
        execution_observation(1000u);
    vm_frame_telemetry_note_execution(&first);
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 100u, 1u, 6000000u, 412000000u, &valid);

    vm_frame_telemetry_snapshot_t state;
    vm_frame_telemetry_snapshot(&state);
    uint64_t anchor = state.scanout_last_host_ns;
    uint64_t now = anchor;
    for (unsigned i = 0; i < 1000000u && now <= anchor; i++)
        now = vm_frame_telemetry_now_ns();

    vm_execution_telemetry_observation_t second =
        execution_observation(1100u);
    vm_frame_telemetry_note_execution(&second);
    vm_frame_telemetry_note_scanout(
        pixels, sizeof pixels, 200u, 2u, 6000000u, 412000000u, &valid);
    vm_frame_telemetry_snapshot(&state);

    if (anchor != 0u && now > anchor) {
        CHECK(state.scanout_max_attempt_gap_ns > 0u,
              "advanced host clock produced a zero scanout gap");
        CHECK(state.scanout_max_gap_execution_captured &&
              state.scanout_max_gap_cpu_retired == 100u &&
              state.scanout_max_gap_mbx_2d_candidates == 100u &&
              state.scanout_max_gap_mbx_2d_completed == 100u &&
              state.scanout_max_gap_mbx_2d_rejected == 100u &&
              state.scanout_max_gap_mbx_2d_bytes == 100u &&
              state.scanout_max_gap_mbx_3d_candidates == 100u &&
              state.scanout_max_gap_mbx_3d_completed == 100u &&
              state.scanout_max_gap_mbx_3d_rejected == 100u &&
              state.scanout_max_gap_mbx_3d_pixels == 100u,
              "worst scanout gap did not retain its exact execution/MBX delta");
    }
}

int main(void) {
    test_disabled_is_inert();
    test_boundaries_and_sampled_changes();
    test_worst_scanout_gap_keeps_its_work_witness();
    printf("vm frame telemetry: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}
