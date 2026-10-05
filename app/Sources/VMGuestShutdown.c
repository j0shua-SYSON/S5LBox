#include "VMGuestShutdown.h"
#include <string.h>

enum { WAKE, HOME_HELD, WAKE_WAIT, POWER_START, POWER_HELD,
       SLIDER_WAIT, DRAG_BEGIN, DRAGGING, DRAG_RELEASE, POWER_OFF_WAIT };

void vm_guest_shutdown_init(vm_guest_shutdown_t *s, uint64_t now) {
    memset(s, 0, sizeof *s);
    s->began_ms = s->phase_ms = now;
}

void vm_guest_shutdown_resume(vm_guest_shutdown_t *s, uint64_t pause) {
    s->began_ms += pause;
    s->phase_ms += pause;
    if (s->slider_ms) s->slider_ms += pause;
}

vm_shutdown_action_t vm_guest_shutdown_step(
    vm_guest_shutdown_t *s, uint64_t now, bool idle, bool held,
    bool off, bool slider) {
    if (off) return VM_SHUTDOWN_COMPLETE; /* PMU witness, never pixel darkness */
    if (now < s->began_ms || now - s->began_ms >= 180000u)
        return VM_SHUTDOWN_TIMEOUT;
    if (!idle) return VM_SHUTDOWN_NONE;
    switch (s->phase) {
    case WAKE:
        s->phase = HOME_HELD;
        s->phase_ms = now;
        return VM_SHUTDOWN_HOME_DOWN;
    case HOME_HELD:
        if (now - s->phase_ms < 250u) break;
        s->phase = WAKE_WAIT;
        s->phase_ms = now;
        return VM_SHUTDOWN_HOME_UP;
    case WAKE_WAIT:
        if (now - s->phase_ms < 1200u) break;
        s->phase = POWER_START;
        return VM_SHUTDOWN_POWER_DOWN;
    case POWER_START:
        if (!held) break;
        s->phase = POWER_HELD;
        s->phase_ms = now;
        break;
    case POWER_HELD:
        if (!slider && now - s->phase_ms < 3000u) break;
        s->phase = SLIDER_WAIT;
        s->phase_ms = now;
        s->slider_ms = 0u;
        return VM_SHUTDOWN_POWER_UP;
    case SLIDER_WAIT:
        if (held) break;
        if (slider) {
            if (!s->slider_ms) s->slider_ms = now;
            if (now - s->slider_ms >= 200u) s->phase = DRAG_BEGIN;
        } else {
            s->slider_ms = 0u;
            if (now - s->phase_ms >= 8000u) s->phase = WAKE;
        }
        break;
    case DRAG_BEGIN:
        if (!slider) { s->phase = SLIDER_WAIT; s->slider_ms = 0u; break; }
        s->drag_step = 0u;
        s->phase = DRAGGING;
        s->phase_ms = now;
        return VM_SHUTDOWN_TOUCH_DOWN;
    case DRAGGING:
        if (now - s->phase_ms < 80u) break;
        s->phase_ms = now;
        if (++s->drag_step == 20u) s->phase = DRAG_RELEASE;
        return VM_SHUTDOWN_TOUCH_MOVE;
    case DRAG_RELEASE:
        if (now - s->phase_ms < 100u) break;
        s->phase = POWER_OFF_WAIT;
        return VM_SHUTDOWN_TOUCH_UP;
    case POWER_OFF_WAIT:
        break;
    }
    return VM_SHUTDOWN_NONE;
}

int vm_guest_shutdown_touch_x(const vm_guest_shutdown_t *s) {
    return 55 + (int)s->drag_step * 11;
}

static bool red(const uint8_t *p) {
    return p[2] > 110u && p[2] > (unsigned)p[1] + 45u &&
           p[2] > (unsigned)p[0] + 45u;
}
static bool gray(const uint8_t *p) {
    unsigned low = p[0], high = p[0];
    for (unsigned i = 1u; i < 3u; i++) {
        if (p[i] < low) low = p[i];
        if (p[i] > high) high = p[i];
    }
    return low > 100u && high - low < 25u;
}

bool vm_guest_shutdown_power_slider(const uint8_t *p, size_t size,
                                     unsigned w, unsigned h, unsigned stride) {
    if (!p || w != 320u || h != 480u || stride < 1280u ||
        (size_t)stride > size / h) return false;
    unsigned red_count = 0u, red_samples = 0u;
    /* Red thumb, excluding the white arrow through its middle. */
    for (unsigned y = 48u; y <= 84u; y += 6u)
        for (unsigned x = 28u; x <= 84u; x += 7u) {
            red_samples++;
            if (red(p + (size_t)y * stride + x * 4u)) red_count++;
        }
    if (red_count * 100u < red_samples * 55u) return false;
    /* Cancel is a wide neutral button at the bottom, not the unlock thumb. */
    for (unsigned x = 35u; x <= 285u; x += 25u) {
        if (!gray(p + (size_t)420u * stride + x * 4u) ||
            !gray(p + (size_t)448u * stride + x * 4u)) return false;
    }
    /* The track beside the red thumb is dark; a red navigation bar is not it. */
    for (unsigned x = 105u; x <= 280u; x += 25u) {
        const uint8_t *q = p + (size_t)48u * stride + x * 4u;
        if (q[0] > 95u || q[1] > 95u || q[2] > 95u) return false;
    }
    return true;
}
