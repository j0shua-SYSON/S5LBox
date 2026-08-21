/* See VMFrameTelemetry.h for the measurement boundary and its limitations. */
#include "VMFrameTelemetry.h"

#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <stdatomic.h>
#endif

typedef struct {
    vm_frame_telemetry_snapshot_t public_state;
    bool scanout_signature_valid;
    bool layer_signature_valid;
    uint64_t scanout_signature;
    uint64_t layer_signature;
    bool execution_previous_valid;
    vm_execution_telemetry_observation_t execution_previous;
} vm_frame_telemetry_state_t;

static vm_frame_telemetry_state_t g_vm_frame_telemetry;

#if defined(_WIN32)
/* MSVC does not enable C11 atomics for the C mode used by this project.
 * Interlocked keeps the Windows test build portable without weakening the
 * stock-iOS path or requiring an experimental compiler switch. */
static volatile LONG g_vm_frame_telemetry_lock;
static volatile LONG g_vm_frame_telemetry_enabled;

static void vm_frame_telemetry_lock(void) {
    while (InterlockedCompareExchange(&g_vm_frame_telemetry_lock,
                                      1, 0) != 0) {
    }
}

static void vm_frame_telemetry_unlock(void) {
    (void)InterlockedExchange(&g_vm_frame_telemetry_lock, 0);
}

static void vm_frame_telemetry_set_enabled(bool enabled) {
    (void)InterlockedExchange(&g_vm_frame_telemetry_enabled,
                              enabled ? 1 : 0);
}

static bool vm_frame_telemetry_enabled(void) {
    return InterlockedCompareExchange(&g_vm_frame_telemetry_enabled,
                                      0, 0) != 0;
}
#else
static atomic_flag g_vm_frame_telemetry_lock = ATOMIC_FLAG_INIT;
static atomic_bool g_vm_frame_telemetry_enabled = ATOMIC_VAR_INIT(false);

static void vm_frame_telemetry_lock(void) {
    while (atomic_flag_test_and_set_explicit(
            &g_vm_frame_telemetry_lock, memory_order_acquire)) {
    }
}

static void vm_frame_telemetry_unlock(void) {
    atomic_flag_clear_explicit(&g_vm_frame_telemetry_lock,
                               memory_order_release);
}

static void vm_frame_telemetry_set_enabled(bool enabled) {
    atomic_store_explicit(&g_vm_frame_telemetry_enabled, enabled,
                          memory_order_release);
}

static bool vm_frame_telemetry_enabled(void) {
    return atomic_load_explicit(&g_vm_frame_telemetry_enabled,
                                memory_order_acquire);
}
#endif

/* This is intentionally the same bounded sampled signature used by VMEngine.
 * It touches 6,192 bytes of a 614,400-byte 320x480 scanout. A tiny update that
 * misses every sample can be missed; this diagnostic can undercount a change,
 * but it cannot invent one. */
static uint64_t vm_frame_telemetry_signature(const uint8_t *pixels,
                                             size_t bytes) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i + 4u <= bytes; i += 397u) {
        uint32_t word = (uint32_t)pixels[i]
            | ((uint32_t)pixels[i + 1u] << 8)
            | ((uint32_t)pixels[i + 2u] << 16)
            | ((uint32_t)pixels[i + 3u] << 24);
        hash = (hash ^ (uint64_t)word) * UINT64_C(1099511628211);
    }
    return hash;
}

static void vm_frame_telemetry_reset_locked(bool enabled) {
    uint64_t generation =
        g_vm_frame_telemetry.public_state.generation + UINT64_C(1);
    memset(&g_vm_frame_telemetry, 0, sizeof g_vm_frame_telemetry);
    g_vm_frame_telemetry.public_state.enabled = enabled;
    g_vm_frame_telemetry.public_state.generation = generation;
    vm_frame_telemetry_set_enabled(enabled);
}

void vm_frame_telemetry_reset(bool enabled) {
    vm_frame_telemetry_lock();
    vm_frame_telemetry_reset_locked(enabled);
    vm_frame_telemetry_unlock();
}

void vm_frame_telemetry_begin_machine(void) {
    vm_frame_telemetry_lock();
    bool enabled = g_vm_frame_telemetry.public_state.enabled;
    vm_frame_telemetry_reset_locked(enabled);
    vm_frame_telemetry_unlock();
}

bool vm_frame_telemetry_is_enabled(void) {
    return vm_frame_telemetry_enabled();
}

uint64_t vm_frame_telemetry_now_ns(void) {
    struct timespec now;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
#elif defined(TIME_UTC)
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0;
#else
    return 0;
#endif
    if (now.tv_sec < 0 || now.tv_nsec < 0) return 0;
    uint64_t seconds = (uint64_t)now.tv_sec;
    if (seconds > UINT64_MAX / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

const char *vm_frame_scanout_reason_name(vm_frame_scanout_reason_t reason) {
    switch (reason) {
    case VM_FRAME_SCANOUT_REASON_VALID:                   return "valid";
    case VM_FRAME_SCANOUT_REASON_NO_MACHINE:              return "no_machine";
    case VM_FRAME_SCANOUT_REASON_NO_RAM:                  return "no_ram";
    case VM_FRAME_SCANOUT_REASON_STOPPED:                 return "stopped";
    case VM_FRAME_SCANOUT_REASON_GLOBAL_DISABLED:         return "global_disabled";
    case VM_FRAME_SCANOUT_REASON_CLOCK_GATED:             return "clock_gated";
    case VM_FRAME_SCANOUT_REASON_NO_ACTIVE_WINDOW:        return "no_active_window";
    case VM_FRAME_SCANOUT_REASON_WINDOW_UNAVAILABLE:      return "window_unavailable";
    case VM_FRAME_SCANOUT_REASON_UNSUPPORTED_FORMAT:      return "unsupported_format";
    case VM_FRAME_SCANOUT_REASON_UNSUPPORTED_GEOMETRY:    return "unsupported_geometry";
    case VM_FRAME_SCANOUT_REASON_STRIDE_TOO_SMALL:        return "stride_too_small";
    case VM_FRAME_SCANOUT_REASON_FRAMEBUFFER_BELOW_RAM:   return "framebuffer_below_ram";
    case VM_FRAME_SCANOUT_REASON_FRAMEBUFFER_OUTSIDE_RAM: return "framebuffer_outside_ram";
    case VM_FRAME_SCANOUT_REASON_PUBLICATION_TOO_LARGE:   return "publication_too_large";
    case VM_FRAME_SCANOUT_REASON_UNKNOWN:
    case VM_FRAME_SCANOUT_REASON_COUNT:
    default:                                               return "unknown";
    }
}

static bool vm_execution_telemetry_not_before(
        const vm_execution_telemetry_observation_t *now,
        const vm_execution_telemetry_observation_t *before) {
#define VM_EXEC_NOT_BEFORE(field_) (now->field_ >= before->field_)
    return VM_EXEC_NOT_BEFORE(cpu_retired) &&
           VM_EXEC_NOT_BEFORE(interpreter_tick_batches) &&
           VM_EXEC_NOT_BEFORE(interpreter_tick_batched_retired) &&
           VM_EXEC_NOT_BEFORE(static_native_retired) &&
           VM_EXEC_NOT_BEFORE(compact_attempts) &&
           VM_EXEC_NOT_BEFORE(compact_calls) &&
           VM_EXEC_NOT_BEFORE(compact_native_retired) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_retired) &&
           VM_EXEC_NOT_BEFORE(compact_privileged_attempts) &&
           VM_EXEC_NOT_BEFORE(compact_privileged_calls) &&
           VM_EXEC_NOT_BEFORE(compact_privileged_retired) &&
           VM_EXEC_NOT_BEFORE(compact_window_crossings) &&
           VM_EXEC_NOT_BEFORE(compact_window_reloads) &&
           VM_EXEC_NOT_BEFORE(compact_window_fast_refills) &&
           VM_EXEC_NOT_BEFORE(compact_data_refill_attempts) &&
           VM_EXEC_NOT_BEFORE(compact_data_fast_refills) &&
           VM_EXEC_NOT_BEFORE(compact_window_stops) &&
           VM_EXEC_NOT_BEFORE(compact_refused_guard) &&
           VM_EXEC_NOT_BEFORE(compact_refused_privileged) &&
           VM_EXEC_NOT_BEFORE(compact_refused_alignment) &&
           VM_EXEC_NOT_BEFORE(compact_refused_fetch_witness) &&
           VM_EXEC_NOT_BEFORE(compact_refused_runner) &&
           VM_EXEC_NOT_BEFORE(compact_zero_retired) &&
           VM_EXEC_NOT_BEFORE(fetch_refill_attempts) &&
           VM_EXEC_NOT_BEFORE(fetch_refill_hits) &&
           VM_EXEC_NOT_BEFORE(fetch_refill_skips) &&
           VM_EXEC_NOT_BEFORE(known_negative_bypasses) &&
           VM_EXEC_NOT_BEFORE(mbx_2d_candidates) &&
           VM_EXEC_NOT_BEFORE(mbx_2d_completed) &&
           VM_EXEC_NOT_BEFORE(mbx_2d_rejected) &&
           VM_EXEC_NOT_BEFORE(mbx_2d_degraded) &&
           VM_EXEC_NOT_BEFORE(mbx_2d_bytes) &&
           VM_EXEC_NOT_BEFORE(mbx_3d_candidates) &&
           VM_EXEC_NOT_BEFORE(mbx_3d_completed) &&
           VM_EXEC_NOT_BEFORE(mbx_3d_rejected) &&
           VM_EXEC_NOT_BEFORE(mbx_3d_degraded) &&
           VM_EXEC_NOT_BEFORE(mbx_3d_pixels) &&
           VM_EXEC_NOT_BEFORE(active_clock_updates) &&
           VM_EXEC_NOT_BEFORE(active_clock_added_ticks) &&
           VM_EXEC_NOT_BEFORE(active_clock_clamps) &&
           VM_EXEC_NOT_BEFORE(active_clock_failures) &&
           VM_EXEC_NOT_BEFORE(active_clock_input_guards) &&
           VM_EXEC_NOT_BEFORE(active_clock_input_guard_quiesces) &&
           VM_EXEC_NOT_BEFORE(active_clock_deadline_shields) &&
           VM_EXEC_NOT_BEFORE(mtz2_frames_queued) &&
           VM_EXEC_NOT_BEFORE(mtz2_frames_read) &&
           VM_EXEC_NOT_BEFORE(mtz2_length_reads) &&
           VM_EXEC_NOT_BEFORE(mtz2_data_reads) &&
           VM_EXEC_NOT_BEFORE(mtz2_injects_refused) &&
           VM_EXEC_NOT_BEFORE(wfi_paced_waits) &&
           VM_EXEC_NOT_BEFORE(wfi_paced_wait_ns) &&
           VM_EXEC_NOT_BEFORE(wfi_paced_partial_advances) &&
           VM_EXEC_NOT_BEFORE(wfi_paced_failures) &&
           VM_EXEC_NOT_BEFORE(power_trace_sequence) &&
           VM_EXEC_NOT_BEFORE(compact_privileged_window_refills) &&
           VM_EXEC_NOT_BEFORE(compact_privileged_boundary_retired) &&
           VM_EXEC_NOT_BEFORE(compact_window_cache_hits) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_polls) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_not_running) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_state_failures) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_target_races) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_samples) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_outside) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_entry) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_dp) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_memory) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_block_control) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_system) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_vfp) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_thumb) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_thumb_decode) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_thumb_low_alu) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_thumb_alu_high) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_thumb_memory_form) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_thumb_misc) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_thumb_branch) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_thumb_memory_access) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_thumb_condition) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_a32_condition) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_retire) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_fallback) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_exit) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_outside_pc_captured) &&
           VM_EXEC_NOT_BEFORE(compact_pc_profile_outside_pc_dropped) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_events) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_witness_misses) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_dread_hits) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_dread_misses) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_dwrite_hits) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_dwrite_misses) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_dread_events) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_dwrite_events) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_mixed_data_events) &&
           VM_EXEC_NOT_BEFORE(compact_fallback_profile_no_data_events) &&
           VM_EXEC_NOT_BEFORE(
               compact_fallback_profile_admitted_data_miss_events) &&
           VM_EXEC_NOT_BEFORE(
               compact_fallback_profile_admitted_data_tlb_hits) &&
           VM_EXEC_NOT_BEFORE(
               compact_fallback_profile_admitted_data_tlb_misses) &&
           VM_EXEC_NOT_BEFORE(
               compact_fallback_profile_admitted_data_tlb_hit_only_events) &&
           VM_EXEC_NOT_BEFORE(
               compact_fallback_profile_admitted_data_tlb_miss_only_events) &&
           VM_EXEC_NOT_BEFORE(
               compact_fallback_profile_admitted_data_tlb_mixed_events) &&
           VM_EXEC_NOT_BEFORE(
               compact_fallback_profile_admitted_data_no_tlb_events);
#undef VM_EXEC_NOT_BEFORE
}

void vm_frame_telemetry_note_execution(
        const vm_execution_telemetry_observation_t *observation) {
    if (!observation || !vm_frame_telemetry_is_enabled()) return;

    vm_frame_telemetry_lock();
    if (!g_vm_frame_telemetry.public_state.enabled) {
        vm_frame_telemetry_unlock();
        return;
    }

    vm_frame_telemetry_snapshot_t *state =
        &g_vm_frame_telemetry.public_state;
    if (!state->execution_captured) {
        state->execution_captured = true;
        state->execution_consistent = true;
        state->execution_first = *observation;
        g_vm_frame_telemetry.execution_previous_valid = false;
    } else {
        g_vm_frame_telemetry.execution_previous = state->execution_last;
        g_vm_frame_telemetry.execution_previous_valid = true;
        if (!vm_execution_telemetry_not_before(
                observation, &state->execution_last))
            state->execution_consistent = false;
    }
    state->execution_last = *observation;
    state->execution_observations++;
    vm_frame_telemetry_unlock();
}

void vm_frame_telemetry_note_scanout(
        const void *pixels, size_t bytes,
        uint64_t timer_ticks, uint64_t clcd_frames,
        uint32_t timebase_hz, uint32_t cpu_hz,
        const vm_frame_scanout_observation_t *observation) {
    if (!vm_frame_telemetry_is_enabled()) return;

    const bool valid = pixels != NULL && bytes >= 4u;
    vm_frame_scanout_observation_t observed;
    memset(&observed, 0, sizeof observed);
    observed.active_window = UINT32_MAX;
    if (observation) observed = *observation;
    if (valid) {
        observed.reason = VM_FRAME_SCANOUT_REASON_VALID;
    } else if ((uint32_t)observed.reason >=
                   (uint32_t)VM_FRAME_SCANOUT_REASON_COUNT ||
               observed.reason == VM_FRAME_SCANOUT_REASON_VALID) {
        observed.reason = VM_FRAME_SCANOUT_REASON_UNKNOWN;
    }
    const uint64_t signature = valid
        ? vm_frame_telemetry_signature((const uint8_t *)pixels, bytes) : 0;
    const uint64_t host_ns = vm_frame_telemetry_now_ns();

    vm_frame_telemetry_lock();
    if (!g_vm_frame_telemetry.public_state.enabled) {
        vm_frame_telemetry_unlock();
        return;
    }

    vm_frame_telemetry_snapshot_t *state =
        &g_vm_frame_telemetry.public_state;
    uint64_t attempt_gap = 0u;
    if (state->scanout_last_host_ns != 0u && host_ns != 0u &&
        host_ns >= state->scanout_last_host_ns)
        attempt_gap = host_ns - state->scanout_last_host_ns;
    state->scanout_attempts++;
    if (state->scanout_attempts == UINT64_C(1))
        state->scanout_first_host_ns = host_ns;
    state->scanout_last_host_ns = host_ns;
    if (attempt_gap > UINT64_C(100000000))
        state->scanout_attempt_gaps_over_100ms++;
    if (attempt_gap > UINT64_C(500000000))
        state->scanout_attempt_gaps_over_500ms++;
    if (attempt_gap > state->scanout_max_attempt_gap_ns) {
        state->scanout_max_attempt_gap_ns = attempt_gap;
        state->scanout_max_gap_execution_captured = false;
        if (g_vm_frame_telemetry.execution_previous_valid &&
            vm_execution_telemetry_not_before(
                &state->execution_last,
                &g_vm_frame_telemetry.execution_previous)) {
#define VM_STALL_DELTA(field_) \
    (state->execution_last.field_ - \
     g_vm_frame_telemetry.execution_previous.field_)
            state->scanout_max_gap_execution_captured = true;
            state->scanout_max_gap_cpu_retired =
                VM_STALL_DELTA(cpu_retired);
            state->scanout_max_gap_mbx_2d_candidates =
                VM_STALL_DELTA(mbx_2d_candidates);
            state->scanout_max_gap_mbx_2d_completed =
                VM_STALL_DELTA(mbx_2d_completed);
            state->scanout_max_gap_mbx_2d_rejected =
                VM_STALL_DELTA(mbx_2d_rejected);
            state->scanout_max_gap_mbx_2d_degraded =
                VM_STALL_DELTA(mbx_2d_degraded);
            state->scanout_max_gap_mbx_2d_bytes =
                VM_STALL_DELTA(mbx_2d_bytes);
            state->scanout_max_gap_mbx_3d_candidates =
                VM_STALL_DELTA(mbx_3d_candidates);
            state->scanout_max_gap_mbx_3d_completed =
                VM_STALL_DELTA(mbx_3d_completed);
            state->scanout_max_gap_mbx_3d_rejected =
                VM_STALL_DELTA(mbx_3d_rejected);
            state->scanout_max_gap_mbx_3d_degraded =
                VM_STALL_DELTA(mbx_3d_degraded);
            state->scanout_max_gap_mbx_3d_pixels =
                VM_STALL_DELTA(mbx_3d_pixels);
#undef VM_STALL_DELTA
        }
    }
    if (state->scanout_attempts > UINT64_C(1) &&
        state->scanout_last.reason == observed.reason) {
        state->scanout_last_reason_streak++;
    } else {
        state->scanout_last_reason_streak = UINT64_C(1);
    }
    state->scanout_last = observed;
    state->scanout_reason_counts[observed.reason]++;

    if (timebase_hz != 0u && cpu_hz != 0u) {
        if (!state->scanout_guest_clock_captured) {
            state->scanout_guest_clock_captured = true;
            state->scanout_guest_clock_consistent = true;
            state->scanout_first_timer_ticks = timer_ticks;
            state->scanout_first_clcd_frames = clcd_frames;
            state->scanout_timebase_hz = timebase_hz;
            state->scanout_cpu_hz = cpu_hz;
        } else if (state->scanout_timebase_hz != timebase_hz ||
                   state->scanout_cpu_hz != cpu_hz ||
                   timer_ticks < state->scanout_last_timer_ticks ||
                   clcd_frames < state->scanout_last_clcd_frames) {
            state->scanout_guest_clock_consistent = false;
        }
        state->scanout_last_timer_ticks = timer_ticks;
        state->scanout_last_clcd_frames = clcd_frames;
    }

    if (valid) {
        state->scanout_valid++;
        state->scanout_last_valid_host_ns = host_ns;
        state->scanout_last_valid_timer_ticks = timer_ticks;
        state->scanout_last_valid_clcd_frames = clcd_frames;
        if (!g_vm_frame_telemetry.scanout_signature_valid ||
            signature != g_vm_frame_telemetry.scanout_signature) {
            g_vm_frame_telemetry.scanout_signature_valid = true;
            g_vm_frame_telemetry.scanout_signature = signature;
            if (state->scanout_last_change_host_ns != 0u && host_ns != 0u &&
                host_ns >= state->scanout_last_change_host_ns) {
                uint64_t gap = host_ns - state->scanout_last_change_host_ns;
                if (gap > state->scanout_max_change_gap_ns)
                    state->scanout_max_change_gap_ns = gap;
            }
            state->scanout_last_change_host_ns = host_ns;
            state->scanout_changes++;
        }
    }
    /* One execution pair belongs to one scanout interval. Do not let a caller
     * that omits the next execution sample attach stale work to a later gap. */
    g_vm_frame_telemetry.execution_previous_valid = false;
    vm_frame_telemetry_unlock();
}

void vm_frame_telemetry_note_layer_submission(
        const void *pixels, size_t bytes, bool accepted, uint64_t work_ns) {
    if (!vm_frame_telemetry_is_enabled()) return;

    const bool signature_available = accepted && pixels != NULL && bytes >= 4u;
    const uint64_t signature = signature_available
        ? vm_frame_telemetry_signature((const uint8_t *)pixels, bytes) : 0;
    const uint64_t host_ns = vm_frame_telemetry_now_ns();

    vm_frame_telemetry_lock();
    if (!g_vm_frame_telemetry.public_state.enabled) {
        vm_frame_telemetry_unlock();
        return;
    }

    vm_frame_telemetry_snapshot_t *state =
        &g_vm_frame_telemetry.public_state;
    uint64_t attempt_gap = 0u;
    if (state->layer_last_host_ns != 0u && host_ns != 0u &&
        host_ns >= state->layer_last_host_ns)
        attempt_gap = host_ns - state->layer_last_host_ns;
    state->layer_attempts++;
    if (state->layer_attempts == UINT64_C(1))
        state->layer_first_host_ns = host_ns;
    state->layer_last_host_ns = host_ns;
    if (attempt_gap > state->layer_max_attempt_gap_ns)
        state->layer_max_attempt_gap_ns = attempt_gap;
    if (attempt_gap > UINT64_C(100000000))
        state->layer_attempt_gaps_over_100ms++;
    if (attempt_gap > UINT64_C(500000000))
        state->layer_attempt_gaps_over_500ms++;
    if (UINT64_MAX - state->layer_total_work_ns < work_ns)
        state->layer_total_work_ns = UINT64_MAX;
    else
        state->layer_total_work_ns += work_ns;
    if (work_ns > state->layer_max_work_ns)
        state->layer_max_work_ns = work_ns;

    if (accepted) {
        state->layer_accepted++;
        if (signature_available &&
            (!g_vm_frame_telemetry.layer_signature_valid ||
             signature != g_vm_frame_telemetry.layer_signature)) {
            g_vm_frame_telemetry.layer_signature_valid = true;
            g_vm_frame_telemetry.layer_signature = signature;
            if (state->layer_last_change_host_ns != 0u && host_ns != 0u &&
                host_ns >= state->layer_last_change_host_ns) {
                uint64_t gap = host_ns - state->layer_last_change_host_ns;
                if (gap > state->layer_max_change_gap_ns)
                    state->layer_max_change_gap_ns = gap;
            }
            state->layer_last_change_host_ns = host_ns;
            state->layer_changes++;
        }
    } else {
        state->layer_rejected++;
    }
    vm_frame_telemetry_unlock();
}

void vm_frame_telemetry_snapshot(vm_frame_telemetry_snapshot_t *out) {
    if (!out) return;
    vm_frame_telemetry_lock();
    *out = g_vm_frame_telemetry.public_state;
    vm_frame_telemetry_unlock();
}
