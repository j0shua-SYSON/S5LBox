/* Portable lifecycle test for app/Sources/VMFirmwareHLE.c. */
#include "VMFirmwareHLE.h"
#include "ios3_hle.h"

#include <stdio.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition, message) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        printf("FAIL: %s\n", message); \
    } \
} while (0)

int main(void) {
    s5l8900_t first;
    s5l8900_t second;
    unsigned replacement_targets = 0u;

    printf("S5LBox app firmware HLE adapter tests\n");
    CHECK(!vm_firmware_hle_enable(NULL), "NULL machine was enabled");
    CHECK(s5l8900_init(&first, 0u, 1u << 20),
          "first machine initialization failed");
    CHECK(s5l8900_init(&second, 0u, 1u << 20),
          "second machine initialization failed");

    CHECK(vm_firmware_hle_enable(&first), "first adapter enable failed");
    CHECK(vm_firmware_hle_active(&first), "first adapter is not active");
    CHECK(!vm_firmware_hle_active(&second),
          "inactive second machine was reported active");
    for (unsigned i = 0u; i < ios3_hle_site_count(); i++) {
        const ios3_hle_site_t *site = ios3_hle_site_at(i);
        if (!site || site->mode != IOS3_HLE_REPLACE || !site->handler)
            continue;
        replacement_targets++;
        CHECK(s5l8900_pre_step_target(&first, site->va),
              "replacement site is not a machine boundary");
    }
    CHECK(replacement_targets > 0u,
          "HLE library exposed no replacement target");

    /* ios3_hle.c has one global site table. Enabling another machine moves the
     * global gate; the old machine's installed hook remains bounded but its
     * callback declines because it no longer owns that gate. */
    CHECK(vm_firmware_hle_enable(&second), "second adapter enable failed");
    CHECK(!vm_firmware_hle_active(&first) &&
              vm_firmware_hle_active(&second),
          "adapter ownership did not move to the second machine");
    vm_firmware_hle_release(&first);
    CHECK(vm_firmware_hle_active(&second),
          "releasing a stale owner disabled the live one");
    vm_firmware_hle_release(&second);
    CHECK(!vm_firmware_hle_active(&second),
          "live adapter release retained global ownership");

    s5l8900_free(&first);
    s5l8900_free(&second);
    printf("== app firmware HLE: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
