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

void vm_frame_telemetry_reset(bool enabled) {
    vm_frame_telemetry_lock();
    uint64_t generation =
        g_vm_frame_telemetry.public_state.generation + UINT64_C(1);
    memset(&g_vm_frame_telemetry, 0, sizeof g_vm_frame_telemetry);
    g_vm_frame_telemetry.public_state.enabled = enabled;
    g_vm_frame_telemetry.public_state.generation = generation;
    vm_frame_telemetry_set_enabled(enabled);
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
    state->scanout_attempts++;
    if (state->scanout_attempts == UINT64_C(1))
        state->scanout_first_host_ns = host_ns;
    state->scanout_last_host_ns = host_ns;
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
            state->scanout_changes++;
        }
    }
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
    state->layer_attempts++;
    if (state->layer_attempts == UINT64_C(1))
        state->layer_first_host_ns = host_ns;
    state->layer_last_host_ns = host_ns;
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
