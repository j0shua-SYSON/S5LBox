/* Graceful shutdown through the supported guest's own power-off UI.
 * No disk writes, CPU halt, or invented PMU completion live here. */
#ifndef S5LBOX_VM_GUEST_SHUTDOWN_H
#define S5LBOX_VM_GUEST_SHUTDOWN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VM_SHUTDOWN_NONE, VM_SHUTDOWN_HOME_DOWN, VM_SHUTDOWN_HOME_UP,
    VM_SHUTDOWN_POWER_DOWN, VM_SHUTDOWN_POWER_UP,
    VM_SHUTDOWN_TOUCH_DOWN, VM_SHUTDOWN_TOUCH_MOVE, VM_SHUTDOWN_TOUCH_UP,
    VM_SHUTDOWN_COMPLETE, VM_SHUTDOWN_TIMEOUT
} vm_shutdown_action_t;

typedef struct {
    unsigned phase, drag_step;
    uint64_t began_ms, phase_ms, slider_ms;
} vm_guest_shutdown_t;

void vm_guest_shutdown_init(vm_guest_shutdown_t *state, uint64_t now_ms);
/* Call only while executing; shift deadlines by a foreground pause on resume. */
void vm_guest_shutdown_resume(vm_guest_shutdown_t *state, uint64_t paused_ms);
vm_shutdown_action_t vm_guest_shutdown_step(
    vm_guest_shutdown_t *state, uint64_t now_ms, bool input_idle,
    bool power_held, bool powered_off, bool power_slider);
int vm_guest_shutdown_touch_x(const vm_guest_shutdown_t *state);

/* Conservative shape check for the 320x480 iPhone OS 3 power slider AND
 * bottom Cancel button. A black screen, lock slider, or red icon is not proof.
 * Pixels are byte-ordered BGRA. No firmware artwork is bundled. */
bool vm_guest_shutdown_power_slider(const uint8_t *pixels, size_t size,
                                     unsigned width, unsigned height,
                                     unsigned stride);
#endif
