/*
 * S5LBox - opt-in frame-pipeline telemetry for physical-device diagnosis.
 *
 * There are deliberately two boundaries:
 *
 *   scanout   vm_guest_display() has validated the guest-owned pixels that
 *             VMEngine is about to copy into its publication buffer;
 *   layer     VMFramebufferView has built an immutable CGImage and assigned
 *             it to CALayer.contents on the main thread.
 *
 * A layer submission is NOT proof that Core Animation displayed the image.
 * Target-device compositor telemetry is still required for that last step.
 * Keeping the names literal prevents an instrumentation result from quietly
 * turning into a visible-FPS claim.
 *
 * The counters are disabled for ordinary app launches. Device automation
 * enables and resets them before it opens a machine. All state is protected
 * internally because scanout and layer submissions come from different host
 * threads.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VM_FRAME_TELEMETRY_H
#define S5LBOX_APP_VM_FRAME_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    uint64_t generation;

    uint64_t scanout_attempts;
    uint64_t scanout_valid;
    uint64_t scanout_changes;
    uint64_t scanout_first_host_ns;
    uint64_t scanout_last_host_ns;

    bool scanout_guest_clock_captured;
    bool scanout_guest_clock_consistent;
    uint64_t scanout_first_timer_ticks;
    uint64_t scanout_last_timer_ticks;
    uint64_t scanout_first_clcd_frames;
    uint64_t scanout_last_clcd_frames;
    uint32_t scanout_timebase_hz;
    uint32_t scanout_cpu_hz;

    uint64_t layer_attempts;
    uint64_t layer_accepted;
    uint64_t layer_rejected;
    uint64_t layer_changes;
    uint64_t layer_first_host_ns;
    uint64_t layer_last_host_ns;
    uint64_t layer_total_work_ns;
    uint64_t layer_max_work_ns;
} vm_frame_telemetry_snapshot_t;

/* Reset every counter and select whether subsequent calls do any work. */
void vm_frame_telemetry_reset(bool enabled);

/* Fast gate for call sites that want to avoid even reading a timer. */
bool vm_frame_telemetry_is_enabled(void);

/* Monotonic where the host exposes CLOCK_MONOTONIC; zero means unavailable. */
uint64_t vm_frame_telemetry_now_ns(void);

/* One VMEngine scanout lookup. pixels==NULL records an unavailable attempt. */
void vm_frame_telemetry_note_scanout(
    const void *pixels, size_t bytes,
    uint64_t timer_ticks, uint64_t clcd_frames,
    uint32_t timebase_hz, uint32_t cpu_hz);

/* One main-thread image build. accepted means layer.contents was assigned. */
void vm_frame_telemetry_note_layer_submission(
    const void *pixels, size_t bytes, bool accepted, uint64_t work_ns);

/* A coherent copy; out may be read without retaining any internal storage. */
void vm_frame_telemetry_snapshot(vm_frame_telemetry_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif
