/* Fixed-size, lock-free PCM handoff. One producer and one consumer per queue.
 * No UIKit/Foundation, allocations, mutexes or device state in audio callbacks.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef VM_AUDIO_BUFFER_H
#define VM_AUDIO_BUFFER_H
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#define VM_AUDIO_RATE 48000u
#define VM_AUDIO_FRAMES 4096u
typedef struct {
    _Atomic uint32_t read, write;
    float samples[VM_AUDIO_FRAMES][2];
} vm_audio_queue_t;
typedef struct {
    float previous[2];
    uint32_t phase, rate;
    bool primed;
} vm_audio_resampler_t;
typedef struct {
    vm_audio_queue_t playback, microphone;
    _Atomic bool active, host_running, microphone_enabled;
    _Atomic uint32_t epoch;
    /* Each resampler belongs to just one thread. */
    vm_audio_resampler_t guest_output, host_input;
    uint32_t render_epoch, input_epoch, guest_epoch, capture_rate, capture_phase;
    float capture_a[2], capture_b[2];
    bool capture_primed;
    uint64_t rendered, playback_dropped, input_dropped; /* owner-thread counters */
} vm_audio_buffer_t;
/* Refuses a platform whose actual scalar atomics require locks. */
bool vm_audio_buffer_init(vm_audio_buffer_t *b);
/* Emulator thread only. */
void vm_audio_buffer_active(vm_audio_buffer_t *b, bool active);
void vm_audio_guest_frame(void *ctx, unsigned direction, uint32_t rate, float samples[2]);
/* Render thread and microphone tap, respectively. */
void vm_audio_render(vm_audio_buffer_t *b, float *left, float *right, unsigned frames);
void vm_audio_microphone(vm_audio_buffer_t *b, const float *left, const float *right,
                          unsigned frames, uint32_t rate);
#endif
