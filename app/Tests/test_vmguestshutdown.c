#include "VMGuestShutdown.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static uint8_t pixels[320u * 480u * 4u];
static void rectangle(unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                      uint8_t b, uint8_t g, uint8_t r) {
    for (unsigned y = y0; y <= y1; y++)
        for (unsigned x = x0; x <= x1; x++) {
            uint8_t *p = pixels + (y * 320u + x) * 4u;
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255u;
        }
}
static bool slider(void) {
    return vm_guest_shutdown_power_slider(pixels, sizeof pixels, 320u, 480u,
                                          1280u);
}

int main(void) {
    CHECK(!slider());
    rectangle(28u, 48u, 84u, 84u, 30u, 35u, 180u);
    CHECK(!slider()); /* red icon alone */
    rectangle(35u, 420u, 285u, 448u, 190u, 190u, 190u);
    CHECK(slider());
    CHECK(!vm_guest_shutdown_power_slider(NULL, sizeof pixels, 320u, 480u, 1280u));
    CHECK(!vm_guest_shutdown_power_slider(pixels, 1024u, 320u, 480u, 1280u));
    CHECK(!vm_guest_shutdown_power_slider(pixels, sizeof pixels, 480u, 320u, 1920u));
    rectangle(105u, 48u, 280u, 48u, 30u, 35u, 180u);
    CHECK(!slider()); /* red navigation bar */
    rectangle(105u, 48u, 280u, 48u, 0u, 0u, 0u);
    rectangle(28u, 48u, 84u, 84u, 190u, 190u, 190u);
    CHECK(!slider()); /* neutral unlock slider */

    vm_guest_shutdown_t s;
    vm_guest_shutdown_init(&s, 100u);
    CHECK(vm_guest_shutdown_step(&s, 100u, false, false, false, false) == VM_SHUTDOWN_NONE);
    CHECK(vm_guest_shutdown_step(&s, 100u, true, false, false, false) == VM_SHUTDOWN_HOME_DOWN);
    CHECK(vm_guest_shutdown_step(&s, 349u, true, false, false, false) == VM_SHUTDOWN_NONE);
    CHECK(vm_guest_shutdown_step(&s, 350u, true, false, false, false) == VM_SHUTDOWN_HOME_UP);
    CHECK(vm_guest_shutdown_step(&s, 1550u, true, false, false, false) == VM_SHUTDOWN_POWER_DOWN);
    CHECK(vm_guest_shutdown_step(&s, 1551u, true, true, false, false) == VM_SHUTDOWN_NONE);
    CHECK(vm_guest_shutdown_step(&s, 4551u, true, true, false, false) == VM_SHUTDOWN_POWER_UP);
    CHECK(vm_guest_shutdown_step(&s, 4552u, true, false, false, true) == VM_SHUTDOWN_NONE);
    CHECK(vm_guest_shutdown_step(&s, 4752u, true, false, false, true) == VM_SHUTDOWN_NONE);
    CHECK(vm_guest_shutdown_step(&s, 4753u, true, false, false, true) == VM_SHUTDOWN_TOUCH_DOWN);
    CHECK(vm_guest_shutdown_touch_x(&s) == 55);
    uint64_t now = 4753u;
    for (unsigned i = 1u; i <= 20u; i++) {
        now += 80u;
        CHECK(vm_guest_shutdown_step(&s, now, false, false, false, false) == VM_SHUTDOWN_NONE);
        CHECK(vm_guest_shutdown_step(&s, now, true, false, false, false) == VM_SHUTDOWN_TOUCH_MOVE);
        CHECK(vm_guest_shutdown_touch_x(&s) == 55 + (int)i * 11);
    }
    CHECK(vm_guest_shutdown_step(&s, now + 100u, true, false, false, false) == VM_SHUTDOWN_TOUCH_UP);
    CHECK(vm_guest_shutdown_step(&s, now + 1000u, true, false, false, false) == VM_SHUTDOWN_NONE);
    CHECK(vm_guest_shutdown_step(&s, now + 1001u, false, false, true, false) == VM_SHUTDOWN_COMPLETE);
    CHECK(vm_guest_shutdown_step(&s, 180100u, true, false, false, false) == VM_SHUTDOWN_TIMEOUT);
    vm_guest_shutdown_resume(&s, 60000u);
    CHECK(vm_guest_shutdown_step(&s, 180100u, true, false, false, false) == VM_SHUTDOWN_NONE);
    CHECK(vm_guest_shutdown_step(&s, 240100u, true, false, false, false) == VM_SHUTDOWN_TIMEOUT);

    /* Never inject a slide just because enough time has passed. */
    vm_guest_shutdown_init(&s, 1u);
    for (now = 1u; now < 180001u; now += 100u) {
        vm_shutdown_action_t action = vm_guest_shutdown_step(
            &s, now, true, true, false, false);
        CHECK(action != VM_SHUTDOWN_TOUCH_DOWN && action != VM_SHUTDOWN_COMPLETE);
    }
    printf("guest shutdown: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
