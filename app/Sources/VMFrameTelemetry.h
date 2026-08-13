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

/* Why one vm_guest_display() lookup did or did not yield a publishable frame.
 * UNKNOWN is reserved for an uninstrumented/malformed caller; VALID is derived
 * from the actual pixel pointer, so diagnostics cannot declare a NULL scanout
 * healthy merely by passing the wrong enum value. */
typedef enum {
    VM_FRAME_SCANOUT_REASON_UNKNOWN = 0,
    VM_FRAME_SCANOUT_REASON_VALID,
    VM_FRAME_SCANOUT_REASON_NO_MACHINE,
    VM_FRAME_SCANOUT_REASON_NO_RAM,
    VM_FRAME_SCANOUT_REASON_STOPPED,
    VM_FRAME_SCANOUT_REASON_GLOBAL_DISABLED,
    VM_FRAME_SCANOUT_REASON_CLOCK_GATED,
    VM_FRAME_SCANOUT_REASON_NO_ACTIVE_WINDOW,
    VM_FRAME_SCANOUT_REASON_WINDOW_UNAVAILABLE,
    VM_FRAME_SCANOUT_REASON_UNSUPPORTED_FORMAT,
    VM_FRAME_SCANOUT_REASON_UNSUPPORTED_GEOMETRY,
    VM_FRAME_SCANOUT_REASON_STRIDE_TOO_SMALL,
    VM_FRAME_SCANOUT_REASON_FRAMEBUFFER_BELOW_RAM,
    VM_FRAME_SCANOUT_REASON_FRAMEBUFFER_OUTSIDE_RAM,
    VM_FRAME_SCANOUT_REASON_PUBLICATION_TOO_LARGE,
    VM_FRAME_SCANOUT_REASON_COUNT
} vm_frame_scanout_reason_t;

/* Raw CLCD state captured at the same lookup boundary as the reason. These are
 * observations only: enabling telemetry never changes guest or controller
 * state. active_window is UINT32_MAX when there is no active RGB window. */
typedef struct {
    vm_frame_scanout_reason_t reason;
    uint32_t scanning;
    uint32_t ctrl;
    uint32_t gate;
    uint32_t active_window;
    uint32_t framebuffer_phys;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
} vm_frame_scanout_observation_t;

/* Emulator-thread counters sampled only at the existing scanout lookup. This
 * keeps diagnostics out of the instruction hot path while still exposing
 * whether a signed invocation retired useful work or failed an entry gate. */
#define VM_COMPACT_PC_PROFILE_HOT_COUNT 8u
#define VM_MBX_3D_REJECTION_HISTORY 4u
#define VM_MBX_3D_REJECTION_RECORD_WORDS 44u
#define VM_MBX_3D_REJECTION_TA_WORDS 1024u
#define VM_MBX_3D_ACCEPT_HISTORY 16u
#define VM_MBX_3D_ACCEPT_RECORD_WORDS 8u
#define VM_MBX_3D_TARGET_LEDGER 8u

typedef struct {
    uint64_t sequence;
    uint64_t tiled_reason_hash;
    uint64_t status_reason_hash;
    uint64_t sprite_reason_hash;
    uint64_t solid_reason_hash;
    uint64_t ta_reason_hash;
    uint32_t ta_word_count;
    uint32_t ta_failure_word;
    uint32_t ta_window_start_word;
    uint32_t ta_window_valid_words;
    uint32_t ta_window_words[VM_MBX_3D_REJECTION_TA_WORDS];
    uint32_t region;
    uint32_t object;
    uint32_t target;
    uint32_t xclip;
    uint32_t yclip;
    uint32_t pixel_sample;
    uint32_t framebuffer_control;
    uint32_t framebuffer_stride;
    uint32_t list_valid_mask;
    uint32_t list_words[4];
    uint32_t record_base;
    uint32_t record_valid_words;
    uint32_t record_words[VM_MBX_3D_REJECTION_RECORD_WORDS];
} vm_mbx_3d_rejection_witness_t;

typedef struct {
    uint64_t sequence;
    uint64_t record_hash;
    uint32_t kind;
    uint32_t pixels;
    uint32_t region;
    uint32_t object;
    uint32_t target;
    uint32_t target_physical;
    uint32_t target_mapping_span;
    uint32_t xclip;
    uint32_t yclip;
    uint32_t pixel_sample;
    uint32_t framebuffer_control;
    uint32_t framebuffer_stride;
    uint32_t list_valid_mask;
    uint32_t list_words[4];
    uint32_t record_base;
    uint32_t record_valid_words;
    uint32_t record_words[VM_MBX_3D_ACCEPT_RECORD_WORDS];
} vm_mbx_3d_accept_witness_t;

typedef struct {
    uint64_t last_sequence;
    uint64_t completed;
    uint64_t pixels;
    uint32_t target;
    uint32_t target_physical;
    uint32_t target_mapping_span;
    uint32_t last_kind;
} vm_mbx_3d_target_ledger_t;

typedef struct {
    /* Current CPU state. These are witnesses, not monotonic counters, and are
     * therefore deliberately excluded from the consistency comparison. */
    uint32_t cpu_pc;
    uint32_t cpu_cpsr;
    uint32_t cpu_irq_line;
    uint32_t cpu_fiq_line;
    uint32_t wfi_host_pacing_enabled;
    uint32_t active_host_clock_enabled;
    uint32_t active_clock_input_guard_active;
    uint32_t active_clock_deadline_shield_active;
    uint64_t cpu_retired;
    uint64_t interpreter_tick_batches;
    uint64_t interpreter_tick_batched_retired;
    uint64_t static_native_retired;
    uint64_t compact_attempts;
    uint64_t compact_calls;
    uint64_t compact_native_retired;
    uint64_t compact_fallback_retired;
    uint64_t compact_privileged_attempts;
    uint64_t compact_privileged_calls;
    uint64_t compact_privileged_retired;
    uint64_t compact_window_crossings;
    uint64_t compact_window_reloads;
    uint64_t compact_window_fast_refills;
    uint64_t compact_window_stops;
    uint64_t compact_refused_guard;
    uint64_t compact_refused_privileged;
    uint64_t compact_refused_alignment;
    uint64_t compact_refused_fetch_witness;
    uint64_t compact_refused_runner;
    uint64_t compact_zero_retired;
    uint64_t fetch_refill_attempts;
    uint64_t fetch_refill_hits;
    uint64_t fetch_refill_skips;
    uint64_t known_negative_bypasses;
    uint64_t mbx_2d_candidates;
    uint64_t mbx_2d_completed;
    uint64_t mbx_2d_rejected;
    uint64_t mbx_2d_bytes;
    uint64_t mbx_2d_last_rejected_ring_offset;
    uint64_t mbx_2d_last_rejected_count;
    uint64_t mbx_2d_last_rejected_reason_hash;
    uint64_t mbx_3d_candidates;
    uint64_t mbx_3d_completed;
    uint64_t mbx_3d_rejected;
    uint64_t mbx_3d_pixels;
    vm_mbx_3d_rejection_witness_t mbx_3d_rejection_history[
        VM_MBX_3D_REJECTION_HISTORY];
    vm_mbx_3d_accept_witness_t mbx_3d_accept_history[
        VM_MBX_3D_ACCEPT_HISTORY];
    vm_mbx_3d_target_ledger_t mbx_3d_target_ledger[
        VM_MBX_3D_TARGET_LEDGER];
    uint64_t active_clock_updates;
    uint64_t active_clock_added_ticks;
    uint64_t active_clock_clamps;
    uint64_t active_clock_failures;
    uint64_t active_clock_input_guards;
    uint64_t active_clock_input_guard_quiesces;
    uint64_t active_clock_deadline_shields;
    uint64_t wfi_paced_waits;
    uint64_t wfi_paced_wait_ns;
    uint64_t wfi_paced_partial_advances;
    uint64_t wfi_paced_failures;
    uint64_t compact_privileged_window_refills;
    uint64_t compact_privileged_boundary_retired;
    uint64_t compact_window_cache_hits;
    uint64_t compact_pc_profile_polls;
    uint64_t compact_pc_profile_not_running;
    uint64_t compact_pc_profile_state_failures;
    uint64_t compact_pc_profile_target_races;
    uint64_t compact_pc_profile_samples;
    uint64_t compact_pc_profile_outside;
    uint64_t compact_pc_profile_entry;
    uint64_t compact_pc_profile_dp;
    uint64_t compact_pc_profile_memory;
    uint64_t compact_pc_profile_block_control;
    uint64_t compact_pc_profile_system;
    uint64_t compact_pc_profile_vfp;
    /* Compatibility aggregate for the old Thumb-to-retire address range.
     * The detailed counters below expose its A32-condition contamination. */
    uint64_t compact_pc_profile_thumb;
    uint64_t compact_pc_profile_thumb_decode;
    uint64_t compact_pc_profile_thumb_low_alu;
    uint64_t compact_pc_profile_thumb_alu_high;
    uint64_t compact_pc_profile_thumb_memory_form;
    uint64_t compact_pc_profile_thumb_misc;
    uint64_t compact_pc_profile_thumb_branch;
    uint64_t compact_pc_profile_thumb_memory_access;
    uint64_t compact_pc_profile_thumb_condition;
    uint64_t compact_pc_profile_a32_condition;
    uint64_t compact_pc_profile_retire;
    uint64_t compact_pc_profile_fallback;
    uint64_t compact_pc_profile_exit;
    uint64_t compact_pc_profile_reference_pc;
    uint64_t compact_pc_profile_outside_pc_captured;
    uint64_t compact_pc_profile_outside_pc_dropped;
    uint64_t compact_pc_profile_outside_hot_pc[
        VM_COMPACT_PC_PROFILE_HOT_COUNT];
    uint64_t compact_pc_profile_outside_hot_samples[
        VM_COMPACT_PC_PROFILE_HOT_COUNT];
} vm_execution_telemetry_observation_t;

typedef struct {
    bool enabled;
    uint64_t generation;

    uint64_t scanout_attempts;
    uint64_t scanout_valid;
    uint64_t scanout_changes;
    uint64_t scanout_first_host_ns;
    uint64_t scanout_last_host_ns;
    uint64_t scanout_max_attempt_gap_ns;
    uint64_t scanout_attempt_gaps_over_100ms;
    uint64_t scanout_attempt_gaps_over_500ms;
    uint64_t scanout_last_valid_host_ns;
    uint64_t scanout_last_change_host_ns;
    uint64_t scanout_max_change_gap_ns;
    uint64_t scanout_last_valid_timer_ticks;
    uint64_t scanout_last_valid_clcd_frames;
    uint64_t scanout_reason_counts[VM_FRAME_SCANOUT_REASON_COUNT];
    uint64_t scanout_last_reason_streak;
    vm_frame_scanout_observation_t scanout_last;

    bool scanout_guest_clock_captured;
    bool scanout_guest_clock_consistent;
    uint64_t scanout_first_timer_ticks;
    uint64_t scanout_last_timer_ticks;
    uint64_t scanout_first_clcd_frames;
    uint64_t scanout_last_clcd_frames;
    uint32_t scanout_timebase_hz;
    uint32_t scanout_cpu_hz;

    bool execution_captured;
    bool execution_consistent;
    uint64_t execution_observations;
    vm_execution_telemetry_observation_t execution_first;
    vm_execution_telemetry_observation_t execution_last;

    /* Work retired across the single worst emulator-thread scanout gap. This
     * is the causal witness for a visible stall: renderer work here implicates
     * synchronous MBX; no renderer work but a normal instruction slice points
     * back at guest execution instead. */
    bool scanout_max_gap_execution_captured;
    uint64_t scanout_max_gap_cpu_retired;
    uint64_t scanout_max_gap_mbx_2d_candidates;
    uint64_t scanout_max_gap_mbx_2d_completed;
    uint64_t scanout_max_gap_mbx_2d_rejected;
    uint64_t scanout_max_gap_mbx_2d_bytes;
    uint64_t scanout_max_gap_mbx_3d_candidates;
    uint64_t scanout_max_gap_mbx_3d_completed;
    uint64_t scanout_max_gap_mbx_3d_rejected;
    uint64_t scanout_max_gap_mbx_3d_pixels;

    uint64_t layer_attempts;
    uint64_t layer_accepted;
    uint64_t layer_rejected;
    uint64_t layer_changes;
    uint64_t layer_first_host_ns;
    uint64_t layer_last_host_ns;
    uint64_t layer_max_attempt_gap_ns;
    uint64_t layer_attempt_gaps_over_100ms;
    uint64_t layer_attempt_gaps_over_500ms;
    uint64_t layer_last_change_host_ns;
    uint64_t layer_max_change_gap_ns;
    uint64_t layer_total_work_ns;
    uint64_t layer_max_work_ns;
} vm_frame_telemetry_snapshot_t;

/* Reset every counter and select whether subsequent calls do any work. */
void vm_frame_telemetry_reset(bool enabled);

/* Start a new machine generation without changing whether telemetry is
 * enabled. A restarted machine's counters begin at zero, so carrying the old
 * endpoints across this boundary would manufacture regressions and stale gap
 * witnesses. Call only after a new machine has completed bring-up. */
void vm_frame_telemetry_begin_machine(void);

/* Fast gate for call sites that want to avoid even reading a timer. */
bool vm_frame_telemetry_is_enabled(void);

/* Monotonic where the host exposes CLOCK_MONOTONIC; zero means unavailable. */
uint64_t vm_frame_telemetry_now_ns(void);

/* One VMEngine scanout lookup. pixels==NULL records the supplied failure
 * reason; a non-NULL pixel buffer is always recorded as VALID. */
void vm_frame_telemetry_note_scanout(
    const void *pixels, size_t bytes,
    uint64_t timer_ticks, uint64_t clcd_frames,
    uint32_t timebase_hz, uint32_t cpu_hz,
    const vm_frame_scanout_observation_t *observation);

/* One coherent emulator-thread execution sample. Counter regressions are
 * retained as an explicit inconsistency instead of producing wrapped deltas. */
void vm_frame_telemetry_note_execution(
    const vm_execution_telemetry_observation_t *observation);

/* Stable machine-readable spelling for accessibility/syslog diagnostics. */
const char *vm_frame_scanout_reason_name(vm_frame_scanout_reason_t reason);

/* One main-thread image build. accepted means layer.contents was assigned. */
void vm_frame_telemetry_note_layer_submission(
    const void *pixels, size_t bytes, bool accepted, uint64_t work_ns);

/* A coherent copy; out may be read without retaining any internal storage. */
void vm_frame_telemetry_snapshot(vm_frame_telemetry_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif
