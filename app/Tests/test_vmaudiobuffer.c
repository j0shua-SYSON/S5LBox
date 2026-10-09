#include "VMAudioBuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static vm_audio_buffer_t pcm;
static void start(void) {
    CHECK(vm_audio_buffer_init(&pcm));
    vm_audio_buffer_active(&pcm, true);
    atomic_store(&pcm.host_running, true);
    vm_audio_render(&pcm, NULL, NULL, 0);
    float x[2] = {0, 0};
    vm_audio_guest_frame(&pcm, 0, 48000, x);
    vm_audio_render(&pcm, NULL, NULL, 1);
}
static void test_queue(void) {
    start();
    float x[2] = {0.25f, -0.5f}, l[64], r[64];
    for (unsigned wrap = 0; wrap < 1000; wrap++) {
        for (unsigned i = 0; i < 64; i++) vm_audio_guest_frame(&pcm, 0, 48000, x);
        vm_audio_render(&pcm, l, r, 64);
        for (unsigned i = 0; i < 64; i++) CHECK(l[i] == x[0] && r[i] == x[1]);
    }
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[0] == 0 && r[63] == 0);
    for (unsigned i = 0; i < VM_AUDIO_FRAMES + 10u; i++) vm_audio_guest_frame(&pcm, 0, 48000, x);
    CHECK(pcm.playback_dropped == 10);
    CHECK(atomic_load(&pcm.playback.write) - atomic_load(&pcm.playback.read) == VM_AUDIO_FRAMES);
    vm_audio_buffer_active(&pcm, false);
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[0] == 0 && r[63] == 0);
    vm_audio_buffer_active(&pcm, true);
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[0] == 0 && r[63] == 0); /* no old sound on resume */
    /* Index wrap is unsigned arithmetic, not a signed negative queue size. */
    atomic_store(&pcm.playback.read, UINT32_MAX - 4u);
    atomic_store(&pcm.playback.write, UINT32_MAX - 4u);
    for (unsigned i = 0; i < 64; i++) vm_audio_guest_frame(&pcm, 0, 48000, x);
    vm_audio_render(&pcm, l, r, 64);
    CHECK(l[63] == x[0] && r[63] == x[1]);
}
static void test_resample_and_privacy(void) {
    start();
    float x[2] = {0.25f, -0.5f};
    for (unsigned i = 0; i <= 441; i++) vm_audio_guest_frame(&pcm, 0, 44100, x);
    CHECK(atomic_load(&pcm.playback.write) - atomic_load(&pcm.playback.read) == 481);
    float l[481], r[481]; vm_audio_render(&pcm, l, r, 481);
    CHECK(l[480] == 0.25f && r[480] == -0.5f);
    float mic[128]; for (unsigned i = 0; i < 128; i++) mic[i] = 0.375f;
    vm_audio_microphone(&pcm, mic, NULL, 128, 48000);
    CHECK(atomic_load(&pcm.microphone.write) == 0); /* off by default */
    atomic_store(&pcm.microphone_enabled, true);
    vm_audio_microphone(&pcm, mic, NULL, 128, 48000);
    vm_audio_guest_frame(&pcm, 1, 16000, x);
    CHECK(x[0] == 0.375f && x[1] == 0.375f);
    for (unsigned i = 0; i < 30; i++) vm_audio_guest_frame(&pcm, 1, 16000, x);
    CHECK(x[0] == 0.375f && x[1] == 0.375f);
    atomic_store(&pcm.microphone_enabled, false);
    vm_audio_guest_frame(&pcm, 1, 16000, x);
    CHECK(x[0] == 0 && x[1] == 0);
    CHECK(atomic_load(&pcm.microphone.read) == atomic_load(&pcm.microphone.write));
    atomic_store(&pcm.host_running, false);
    vm_audio_guest_frame(&pcm, 1, 16000, x);
    CHECK(x[0] == 0 && x[1] == 0);
}
int main(void) {
    test_queue(); test_resample_and_privacy();
    printf("Audio handoff: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
