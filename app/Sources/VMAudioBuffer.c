#include "VMAudioBuffer.h"
#include <string.h>
#include <math.h>

/* Feature macros need not promise universal lock freedom (MSVC). Check the
 * actual scalar objects at initialization; never discover this in render. */

static bool put(vm_audio_queue_t *q, const float x[2]) {
    uint32_t w = atomic_load_explicit(&q->write, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&q->read, memory_order_acquire);
    if (w - r >= VM_AUDIO_FRAMES) return false;
    q->samples[w % VM_AUDIO_FRAMES][0] = isfinite(x[0]) ? x[0] : 0;
    q->samples[w % VM_AUDIO_FRAMES][1] = isfinite(x[1]) ? x[1] : 0;
    atomic_store_explicit(&q->write, w + 1u, memory_order_release);
    return true;
}
static bool get(vm_audio_queue_t *q, float x[2]) {
    uint32_t r = atomic_load_explicit(&q->read, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&q->write, memory_order_acquire);
    if (w == r) { x[0] = x[1] = 0; return false; }
    memcpy(x, q->samples[r % VM_AUDIO_FRAMES], sizeof(float) * 2);
    atomic_store_explicit(&q->read, r + 1u, memory_order_release);
    return true;
}
/* Only the consumer may discard frames. Never reset the producer's index. */
static void discard(vm_audio_queue_t *q) {
    atomic_store_explicit(&q->read,
        atomic_load_explicit(&q->write, memory_order_acquire), memory_order_release);
}
static void resample(vm_audio_resampler_t *s, vm_audio_queue_t *q,
                     uint32_t rate, const float x[2], uint64_t *drops) {
    if (rate < 8000u || rate > 192000u) return;
    if (rate != s->rate || !s->primed) {
        s->rate = rate; s->phase = rate; s->primed = true;
        memcpy(s->previous, x, sizeof s->previous);
        if (!put(q, x)) (*drops)++;
        return;
    }
    while (s->phase <= VM_AUDIO_RATE) {
        float t = (float)s->phase / VM_AUDIO_RATE;
        float y[2];
        for (unsigned ch = 0; ch < 2; ch++)
            y[ch] = s->previous[ch] + (x[ch] - s->previous[ch]) * t;
        if (!put(q, y)) (*drops)++;
        s->phase += rate;
    }
    s->phase -= VM_AUDIO_RATE;
    memcpy(s->previous, x, sizeof s->previous);
}
bool vm_audio_buffer_init(vm_audio_buffer_t *b) {
    if (!b) return false;
    memset(b, 0, sizeof *b);
    atomic_init(&b->playback.read, 0); atomic_init(&b->playback.write, 0);
    atomic_init(&b->microphone.read, 0); atomic_init(&b->microphone.write, 0);
    atomic_init(&b->active, false); atomic_init(&b->microphone_enabled, false);
    atomic_init(&b->host_running, false);
    atomic_init(&b->epoch, 0);
    return atomic_is_lock_free(&b->playback.read) &&
           atomic_is_lock_free(&b->playback.write) &&
           atomic_is_lock_free(&b->microphone.read) &&
           atomic_is_lock_free(&b->microphone.write) &&
           atomic_is_lock_free(&b->active) && atomic_is_lock_free(&b->host_running) &&
           atomic_is_lock_free(&b->microphone_enabled) && atomic_is_lock_free(&b->epoch);
}
void vm_audio_buffer_active(vm_audio_buffer_t *b, bool active) {
    if (!b) return;
    if (atomic_load_explicit(&b->active, memory_order_relaxed) == active) return;
    memset(&b->guest_output, 0, sizeof b->guest_output);
    b->capture_primed = false; b->capture_phase = 0;
    discard(&b->microphone);
    atomic_fetch_add_explicit(&b->epoch, 1, memory_order_release);
    atomic_store_explicit(&b->active, active, memory_order_release);
}
void vm_audio_guest_frame(void *ctx, unsigned direction, uint32_t rate, float x[2]) {
    vm_audio_buffer_t *b = ctx;
    if (!b) return;
    uint32_t epoch = atomic_load_explicit(&b->epoch, memory_order_acquire);
    if (epoch != b->guest_epoch) {
        memset(&b->guest_output, 0, sizeof b->guest_output);
        discard(&b->microphone); b->capture_primed = false; b->guest_epoch = epoch;
    }
    if (!atomic_load_explicit(&b->active, memory_order_acquire) ||
        !atomic_load_explicit(&b->host_running, memory_order_acquire)) {
        if (direction) x[0] = x[1] = 0;
        return;
    }
    if (!direction) {
        resample(&b->guest_output, &b->playback, rate, x, &b->playback_dropped);
        return;
    }
    if (!atomic_load_explicit(&b->microphone_enabled, memory_order_acquire) ||
        rate < 8000u || rate > 96000u) {
        discard(&b->microphone); b->capture_primed = false;
        x[0] = x[1] = 0; return;
    }
    if (rate != b->capture_rate) {
        b->capture_rate = rate; b->capture_primed = false;
    }
    if (!b->capture_primed) {
        bool a = get(&b->microphone, b->capture_a);
        bool c = get(&b->microphone, b->capture_b);
        b->capture_primed = a && c; b->capture_phase = 0;
        if (!b->capture_primed) { x[0] = x[1] = 0; return; }
    }
    float t = (float)b->capture_phase / rate;
    for (unsigned ch = 0; ch < 2; ch++)
        x[ch] = b->capture_a[ch] + (b->capture_b[ch] - b->capture_a[ch]) * t;
    b->capture_phase += VM_AUDIO_RATE;
    while (b->capture_phase >= rate) {
        b->capture_phase -= rate;
        memcpy(b->capture_a, b->capture_b, sizeof b->capture_a);
        if (!get(&b->microphone, b->capture_b)) b->capture_primed = false;
    }
}
void vm_audio_render(vm_audio_buffer_t *b, float *left, float *right, unsigned n) {
    uint32_t epoch = atomic_load_explicit(&b->epoch, memory_order_acquire);
    if (epoch != b->render_epoch) {
        discard(&b->playback); b->render_epoch = epoch;
    }
    bool active = atomic_load_explicit(&b->active, memory_order_acquire) &&
                  atomic_load_explicit(&b->host_running, memory_order_acquire);
    for (unsigned i = 0; i < n; i++) {
        float x[2] = {0, 0};
        if (active) (void)get(&b->playback, x);
        for (unsigned ch = 0; ch < 2; ch++) {
            if (x[ch] < -1) x[ch] = -1;
            if (x[ch] > 1) x[ch] = 1;
        }
        if (left) left[i] = x[0];
        if (right) right[i] = x[1];
    }
    if (!active) discard(&b->playback);
    b->rendered += n;
}
void vm_audio_microphone(vm_audio_buffer_t *b, const float *left, const float *right,
                          unsigned n, uint32_t rate) {
    uint32_t epoch = atomic_load_explicit(&b->epoch, memory_order_acquire);
    if (epoch != b->input_epoch) {
        memset(&b->host_input, 0, sizeof b->host_input); b->input_epoch = epoch;
    }
    if (!left || !atomic_load_explicit(&b->active, memory_order_acquire) ||
        !atomic_load_explicit(&b->microphone_enabled, memory_order_acquire)) return;
    for (unsigned i = 0; i < n; i++) {
        float x[2] = {left[i], right ? right[i] : left[i]};
        resample(&b->host_input, &b->microphone, rate, x, &b->input_dropped);
    }
}
