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
    value.cpu_pc = UINT32_C(0xc0000000) + (uint32_t)base;
    value.cpu_cpsr = UINT32_C(0x60000013);
    value.cpu_irq_line = (uint32_t)(base & 1u);
    value.cpu_fiq_line = (uint32_t)((base >> 1u) & 1u);
    value.wfi_host_pacing_enabled = 1u;
    value.active_host_clock_enabled = 1u;
    value.active_clock_input_guard_active = 1u;
    value.active_clock_deadline_shield_active = 1u;
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
    value.mbx_2d_degraded = base + 118u;
    value.mbx_2d_bytes = base + 29u;
    value.mbx_2d_last_rejected_ring_offset = base + 76u;
    value.mbx_2d_last_rejected_count = base + 77u;
    value.mbx_2d_last_rejected_reason_hash = base + 78u;
    value.mbx_3d_candidates = base + 30u;
    value.mbx_3d_completed = base + 31u;
    value.mbx_3d_rejected = base + 32u;
    value.mbx_3d_degraded = base + 119u;
    value.mbx_3d_pixels = base + 33u;
    value.mbx_3d_rejection_history[0].sequence = base + 79u;
    value.mbx_3d_rejection_history[0].sprite_reason_hash = base + 80u;
    value.mbx_3d_rejection_history[0].ta_reason_hash = base + 101u;
    value.mbx_3d_rejection_history[0].ta_word_count =
        (uint32_t)(base + 102u);
    value.mbx_3d_rejection_history[0].ta_failure_word =
        (uint32_t)(base + 103u);
    value.mbx_3d_rejection_history[0].ta_window_start_word =
        (uint32_t)(base + 104u);
    value.mbx_3d_rejection_history[0].ta_window_valid_words =
        VM_MBX_3D_REJECTION_TA_WORDS;
    value.mbx_3d_rejection_history[0].ta_window_words[63] =
        (uint32_t)(base + 105u);
    value.mbx_3d_rejection_history[0].record_valid_words =
        VM_MBX_3D_REJECTION_RECORD_WORDS;
    value.mbx_3d_rejection_history[0].record_words[43] = base + 81u;
    value.mbx_3d_accept_history[15].sequence = base + 86u;
    value.mbx_3d_accept_history[15].record_hash = base + 87u;
    value.mbx_3d_accept_history[15].kind = (uint32_t)(base + 88u);
    value.mbx_3d_accept_history[15].pixels = (uint32_t)(base + 89u);
    value.mbx_3d_accept_history[15].record_words[7] = (uint32_t)(base + 90u);
    value.mbx_3d_accept_history[15].target_physical =
        (uint32_t)(base + 91u);
    value.mbx_3d_accept_history[15].target_mapping_span =
        (uint32_t)(base + 92u);
    value.mbx_3d_target_ledger[7].last_sequence = base + 93u;
    value.mbx_3d_target_ledger[7].completed = base + 94u;
    value.mbx_3d_target_ledger[7].pixels = base + 95u;
    value.mbx_3d_target_ledger[7].target = (uint32_t)(base + 96u);
    value.mbx_3d_target_ledger[7].target_physical =
        (uint32_t)(base + 97u);
    value.mbx_3d_target_ledger[7].target_mapping_span =
        (uint32_t)(base + 98u);
    value.mbx_3d_target_ledger[7].last_kind = (uint32_t)(base + 99u);
    value.active_clock_updates = base + 34u;
    value.active_clock_added_ticks = base + 35u;
    value.active_clock_clamps = base + 36u;
    value.active_clock_failures = base + 37u;
    value.active_clock_input_guards = base + 100u;
    value.active_clock_input_guard_quiesces = base + 101u;
    value.active_clock_deadline_shields = base + 102u;
    value.wfi_paced_waits = base + 82u;
    value.wfi_paced_wait_ns = base + 83u;
    value.wfi_paced_partial_advances = base + 84u;
    value.wfi_paced_failures = base + 85u;
    value.power_trace_sequence = base + 106u;
    value.power_trace_ticks_left = base + 107u;
    {
        vm_power_trace_entry_t *entry = &value.power_trace[
            (value.power_trace_sequence - 1u) % VM_POWER_TRACE_HISTORY];
        entry->sequence = value.power_trace_sequence;
        entry->cpu_cycles = base + 108u;
        entry->cpu_pc = UINT32_C(0xc0061eb0) + (uint32_t)base;
        entry->changes = (uint16_t)(base + 109u);
        entry->event = VM_POWER_TRACE_EVENT_WAKE_RESET;
        entry->buttons_pressed = (uint8_t)(base + 110u);
        entry->pmu_shutdown = (uint8_t)(base + 111u);
        entry->pmu_int2 = (uint8_t)(base + 112u);
        entry->pmu_int2_mask = (uint8_t)(base + 113u);
        entry->power_gpio = (uint8_t)(base + 114u);
        entry->pmu_gpio = (uint8_t)(base + 115u);
        entry->clcd = (uint8_t)(base + 116u);
        entry->cpu_lines = (uint8_t)(base + 117u);
    }
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
    value.compact_pc_profile_reference_pc = UINT64_C(0x100000000) + base;
    value.compact_pc_profile_outside_pc_captured = base + 53u;
    value.compact_pc_profile_outside_pc_dropped = base + 54u;
    value.compact_pc_profile_polls = base + 63u;
    value.compact_pc_profile_not_running = base + 64u;
    value.compact_pc_profile_state_failures = base + 65u;
    value.compact_pc_profile_target_races = base + 66u;
    value.compact_pc_profile_thumb_decode = base + 67u;
    value.compact_pc_profile_thumb_low_alu = base + 68u;
    value.compact_pc_profile_thumb_alu_high = base + 69u;
    value.compact_pc_profile_thumb_memory_form = base + 70u;
    value.compact_pc_profile_thumb_misc = base + 71u;
    value.compact_pc_profile_thumb_branch = base + 72u;
    value.compact_pc_profile_thumb_memory_access = base + 73u;
    value.compact_pc_profile_thumb_condition = base + 74u;
    value.compact_pc_profile_a32_condition = base + 75u;
    for (unsigned i = 0u; i < VM_COMPACT_PC_PROFILE_HOT_COUNT; i++) {
        value.compact_pc_profile_outside_hot_pc[i] =
            UINT64_C(0x200000000) + base + (uint64_t)i * UINT64_C(0x100);
        value.compact_pc_profile_outside_hot_samples[i] = base + 55u + i;
    }
    return value;
}

static void test_machine_generation_boundary(void) {
    vm_frame_telemetry_reset(true);
    vm_execution_telemetry_observation_t old = execution_observation(5000u);
    vm_frame_telemetry_note_execution(&old);

    vm_frame_telemetry_snapshot_t before;
    vm_frame_telemetry_snapshot(&before);
    vm_frame_telemetry_begin_machine();

    vm_frame_telemetry_snapshot_t after;
    vm_frame_telemetry_snapshot(&after);
    CHECK(after.enabled && vm_frame_telemetry_is_enabled(),
          "a machine boundary disabled enabled telemetry");
    CHECK(after.generation == before.generation + 1u,
          "machine boundary generation=%llu after %llu",
          (unsigned long long)after.generation,
          (unsigned long long)before.generation);
    CHECK(!after.execution_captured && after.execution_observations == 0u &&
              after.scanout_attempts == 0u && after.layer_attempts == 0u,
          "machine boundary retained stale telemetry");

    vm_execution_telemetry_observation_t fresh = execution_observation(100u);
    vm_frame_telemetry_note_execution(&fresh);
    vm_frame_telemetry_snapshot(&after);
    CHECK(after.execution_captured && after.execution_consistent &&
              after.execution_observations == 1u &&
              after.execution_first.cpu_retired == 101u,
          "fresh lower machine counters were treated as a regression");

    vm_frame_telemetry_reset(false);
    vm_frame_telemetry_snapshot(&before);
    vm_frame_telemetry_begin_machine();
    vm_frame_telemetry_snapshot(&after);
    CHECK(!after.enabled && !vm_frame_telemetry_is_enabled() &&
              after.generation == before.generation + 1u,
          "disabled telemetry did not survive a machine boundary");
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
    execution_last.cpu_pc = 0x100u;
    execution_last.cpu_cpsr = 0x13u;
    execution_last.cpu_irq_line = 0u;
    execution_last.cpu_fiq_line = 1u;
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
           state.execution_last.cpu_pc == 0x100u &&
           state.execution_last.cpu_cpsr == 0x13u &&
           state.execution_last.cpu_irq_line == 0u &&
           state.execution_last.cpu_fiq_line == 1u &&
           state.execution_last.wfi_host_pacing_enabled == 1u &&
            state.execution_last.active_host_clock_enabled == 1u &&
            state.execution_last.active_clock_input_guard_active == 1u &&
            state.execution_last.active_clock_deadline_shield_active == 1u &&
           state.execution_first.compact_refused_privileged == 1017u &&
           state.execution_last.compact_window_fast_refills == 2014u &&
           state.execution_last.fetch_refill_skips == 2024u &&
           state.execution_last.known_negative_bypasses == 2025u &&
           state.execution_last.mbx_2d_degraded == 2118u &&
           state.execution_last.mbx_2d_bytes == 2029u &&
           state.execution_last.mbx_2d_last_rejected_ring_offset == 2076u &&
           state.execution_last.mbx_2d_last_rejected_count == 2077u &&
           state.execution_last.mbx_2d_last_rejected_reason_hash == 2078u &&
           state.execution_last.mbx_3d_pixels == 2033u &&
           state.execution_last.mbx_3d_degraded == 2119u &&
           state.execution_last.mbx_3d_rejection_history[0].sequence ==
               2079u &&
           state.execution_last.mbx_3d_rejection_history[0].
               sprite_reason_hash == 2080u &&
           state.execution_last.mbx_3d_rejection_history[0].
               ta_reason_hash == 2101u &&
           state.execution_last.mbx_3d_rejection_history[0].
               ta_word_count == 2102u &&
           state.execution_last.mbx_3d_rejection_history[0].
               ta_failure_word == 2103u &&
           state.execution_last.mbx_3d_rejection_history[0].
               ta_window_start_word == 2104u &&
           state.execution_last.mbx_3d_rejection_history[0].
               ta_window_valid_words == VM_MBX_3D_REJECTION_TA_WORDS &&
           state.execution_last.mbx_3d_rejection_history[0].
               ta_window_words[63] == 2105u &&
           state.execution_last.mbx_3d_rejection_history[0].
               record_valid_words == VM_MBX_3D_REJECTION_RECORD_WORDS &&
           state.execution_last.mbx_3d_rejection_history[0].
               record_words[43] == 2081u &&
           state.execution_last.mbx_3d_accept_history[15].sequence == 2086u &&
           state.execution_last.mbx_3d_accept_history[15].record_hash ==
               2087u &&
           state.execution_last.mbx_3d_accept_history[15].kind == 2088u &&
           state.execution_last.mbx_3d_accept_history[15].pixels == 2089u &&
           state.execution_last.mbx_3d_accept_history[15].record_words[7] ==
               2090u &&
           state.execution_last.mbx_3d_accept_history[15].target_physical ==
               2091u &&
           state.execution_last.mbx_3d_accept_history[15].
               target_mapping_span == 2092u &&
           state.execution_last.mbx_3d_target_ledger[7].last_sequence ==
               2093u &&
           state.execution_last.mbx_3d_target_ledger[7].completed == 2094u &&
           state.execution_last.mbx_3d_target_ledger[7].pixels == 2095u &&
           state.execution_last.mbx_3d_target_ledger[7].target == 2096u &&
           state.execution_last.mbx_3d_target_ledger[7].target_physical ==
               2097u &&
           state.execution_last.mbx_3d_target_ledger[7].
               target_mapping_span == 2098u &&
           state.execution_last.mbx_3d_target_ledger[7].last_kind == 2099u &&
           state.execution_last.active_clock_updates == 2034u &&
           state.execution_last.active_clock_added_ticks == 2035u &&
            state.execution_last.active_clock_clamps == 2036u &&
            state.execution_last.active_clock_failures == 2037u &&
            state.execution_last.active_clock_input_guards == 2100u &&
            state.execution_last.active_clock_input_guard_quiesces == 2101u &&
            state.execution_last.active_clock_deadline_shields == 2102u &&
           state.execution_last.wfi_paced_waits == 2082u &&
           state.execution_last.wfi_paced_wait_ns == 2083u &&
           state.execution_last.wfi_paced_partial_advances == 2084u &&
           state.execution_last.wfi_paced_failures == 2085u &&
           state.execution_last.power_trace_sequence == 2106u &&
           state.execution_last.power_trace_ticks_left == 2107u &&
           state.execution_last.power_trace[
               (2106u - 1u) % VM_POWER_TRACE_HISTORY].sequence == 2106u &&
           state.execution_last.power_trace[
               (2106u - 1u) % VM_POWER_TRACE_HISTORY].cpu_cycles == 2108u &&
           state.execution_last.power_trace[
               (2106u - 1u) % VM_POWER_TRACE_HISTORY].cpu_pc ==
               UINT32_C(0xc0061eb0) + 2000u &&
           state.execution_last.power_trace[
               (2106u - 1u) % VM_POWER_TRACE_HISTORY].event ==
               VM_POWER_TRACE_EVENT_WAKE_RESET &&
           state.execution_last.power_trace[
               (2106u - 1u) % VM_POWER_TRACE_HISTORY].cpu_lines ==
               (uint8_t)(2000u + 117u) &&
           state.execution_last.compact_privileged_window_refills == 2038u &&
           state.execution_last.compact_privileged_boundary_retired == 2039u &&
           state.execution_last.compact_window_cache_hits == 2040u &&
           state.execution_last.compact_pc_profile_polls == 2063u &&
           state.execution_last.compact_pc_profile_not_running == 2064u &&
           state.execution_last.compact_pc_profile_state_failures == 2065u &&
           state.execution_last.compact_pc_profile_target_races == 2066u &&
           state.execution_last.compact_pc_profile_samples == 2041u &&
           state.execution_last.compact_pc_profile_outside == 2042u &&
           state.execution_last.compact_pc_profile_entry == 2043u &&
           state.execution_last.compact_pc_profile_dp == 2044u &&
           state.execution_last.compact_pc_profile_memory == 2045u &&
           state.execution_last.compact_pc_profile_block_control == 2046u &&
           state.execution_last.compact_pc_profile_system == 2047u &&
           state.execution_last.compact_pc_profile_vfp == 2048u &&
           state.execution_last.compact_pc_profile_thumb == 2049u &&
           state.execution_last.compact_pc_profile_thumb_decode == 2067u &&
           state.execution_last.compact_pc_profile_thumb_low_alu == 2068u &&
           state.execution_last.compact_pc_profile_thumb_alu_high == 2069u &&
           state.execution_last.compact_pc_profile_thumb_memory_form == 2070u &&
           state.execution_last.compact_pc_profile_thumb_misc == 2071u &&
           state.execution_last.compact_pc_profile_thumb_branch == 2072u &&
           state.execution_last.compact_pc_profile_thumb_memory_access ==
               2073u &&
           state.execution_last.compact_pc_profile_thumb_condition == 2074u &&
           state.execution_last.compact_pc_profile_a32_condition == 2075u &&
           state.execution_last.compact_pc_profile_retire == 2050u &&
           state.execution_last.compact_pc_profile_fallback == 2051u &&
           state.execution_last.compact_pc_profile_exit == 2052u &&
           state.execution_last.compact_pc_profile_reference_pc ==
               UINT64_C(0x100000000) + 2000u &&
           state.execution_last.compact_pc_profile_outside_pc_captured ==
               2053u &&
           state.execution_last.compact_pc_profile_outside_pc_dropped ==
               2054u &&
           state.execution_last.compact_pc_profile_outside_hot_pc[0] ==
               UINT64_C(0x200000000) + 2000u &&
           state.execution_last.compact_pc_profile_outside_hot_samples[0] ==
               2055u &&
           state.execution_last.compact_pc_profile_outside_hot_pc[7] ==
               UINT64_C(0x200000000) + 2000u + UINT64_C(0x700) &&
           state.execution_last.compact_pc_profile_outside_hot_samples[7] ==
               2062u,
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
              state.scanout_max_gap_mbx_2d_degraded == 100u &&
              state.scanout_max_gap_mbx_2d_bytes == 100u &&
              state.scanout_max_gap_mbx_3d_candidates == 100u &&
              state.scanout_max_gap_mbx_3d_completed == 100u &&
              state.scanout_max_gap_mbx_3d_rejected == 100u &&
              state.scanout_max_gap_mbx_3d_degraded == 100u &&
              state.scanout_max_gap_mbx_3d_pixels == 100u,
              "worst scanout gap did not retain its exact execution/MBX delta");
    }
}

int main(void) {
    test_disabled_is_inert();
    test_machine_generation_boundary();
    test_boundaries_and_sampled_changes();
    test_worst_scanout_gap_keeps_its_work_witness();
    printf("vm frame telemetry: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}
