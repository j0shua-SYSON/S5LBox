/*
 * S5LBox — the app's firmware boot path (app/Sources/VMFirmwareBoot.c).
 *
 * The property being defended is not "can it boot": that is what
 * core/tests/test_bringup.c measures, against the real kernel. It is that the
 * app can never tell a user it is running Apple's firmware when it is not.
 *
 * So the cases here are the ones where something is absent, empty, or wrong,
 * and each asserts two things: that the answer is a refusal, and that the
 * refusal names the file. A silent fallback and a wrong claim are the same bug
 * from the user's side, and this is the only place either can be caught before
 * a phone is involved.
 *
 * Needs no firmware, deliberately: every fixture here is a file this test
 * writes itself.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareBoot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks = 0;
static unsigned failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* The test's own scratch directory, created beside the test binary. */
static const char *DIR = "vmfwboot-fixture";

static void write_file(const char *name, size_t bytes) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", DIR, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  cannot create %s\n", path); return; }
    for (size_t i = 0; i < bytes; i++) fputc(0x5a, f);
    fclose(f);
}

static void remove_file(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", DIR, name);
    remove(path);
}

static bool make_dir(const char *path) {
#if defined(_WIN32)
    char command[512];
    snprintf(command, sizeof command, "cmd /c if not exist \"%s\" mkdir \"%s\"",
             path, path);
    return system(command) == 0;
#else
    char command[512];
    snprintf(command, sizeof command, "mkdir -p '%s'", path);
    return system(command) == 0;
#endif
}

static bool mentions(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

int main(void) {
    vm_firmware_boot_state_t state;

    printf("== vm firmware boot ==\n");

    if (!make_dir(DIR)) {
        printf("SKIP: cannot create the fixture directory\n");
        return 0;
    }
    remove_file(VM_FW_BOOT_KERNEL_FILE);
    remove_file(VM_FW_BOOT_DEVICETREE_FILE);
    remove_file(VM_FW_BOOT_ROOTFS_FILE);
    remove_file(VM_FW_BOOT_WORK_FILE);

    /* Nothing imported at all. */
    vm_firmware_boot_probe(DIR, &state);
    CHECK(state.readiness == VM_FW_BOOT_INCOMPLETE,
          "an empty directory reported readiness %d", (int)state.readiness);
    CHECK(mentions(state.detail, VM_FW_BOOT_KERNEL_FILE) &&
          mentions(state.detail, VM_FW_BOOT_DEVICETREE_FILE) &&
          mentions(state.detail, VM_FW_BOOT_ROOTFS_FILE),
          "an empty directory must name all three missing files, got \"%s\"",
          state.detail);

    /* Two of three: the message must name the one that is missing and not the
     * ones that are there. */
    write_file(VM_FW_BOOT_KERNEL_FILE, 64);
    write_file(VM_FW_BOOT_DEVICETREE_FILE, 64);
    vm_firmware_boot_probe(DIR, &state);
    CHECK(state.readiness == VM_FW_BOOT_INCOMPLETE,
          "two of three files reported readiness %d", (int)state.readiness);
    CHECK(mentions(state.detail, VM_FW_BOOT_ROOTFS_FILE) &&
          !mentions(state.detail, VM_FW_BOOT_KERNEL_FILE),
          "must name only the missing file, got \"%s\"", state.detail);

    /* All three imported, no work image: a distinct answer, because the fix
     * is a different action from importing. */
    write_file(VM_FW_BOOT_ROOTFS_FILE, 4096);
    vm_firmware_boot_probe(DIR, &state);
    CHECK(state.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE,
          "three imported artefacts reported readiness %d",
          (int)state.readiness);
    CHECK(state.detail[0] != '\0',
          "an unprepared root filesystem must say so");
    CHECK(state.kernel_present && state.devicetree_present &&
          state.rootfs_present && !state.work_present,
          "presence flags disagree with the files on disk");

    /* And with the work image, READY. */
    write_file(VM_FW_BOOT_WORK_FILE, 8192);
    vm_firmware_boot_probe(DIR, &state);
    CHECK(state.readiness == VM_FW_BOOT_READY,
          "four files reported readiness %d", (int)state.readiness);
    CHECK(state.detail[0] == '\0', "READY must carry no complaint, got \"%s\"",
          state.detail);
    CHECK(state.work_size == 8192u, "work image size %llu",
          (unsigned long long)state.work_size);

    /*
     * A ZERO-BYTE FILE IS NOT A FILE. The settings screen's existence check
     * calls one "present", and a failed or interrupted import leaves exactly
     * that. Booting it would fail deep inside the Mach-O parser with a message
     * about load commands rather than about an incomplete import.
     */
    remove_file(VM_FW_BOOT_KERNEL_FILE);
    write_file(VM_FW_BOOT_KERNEL_FILE, 0);
    vm_firmware_boot_probe(DIR, &state);
    CHECK(state.readiness == VM_FW_BOOT_INCOMPLETE,
          "a zero-byte kernel reported readiness %d", (int)state.readiness);
    CHECK(mentions(state.detail, VM_FW_BOOT_KERNEL_FILE),
          "a zero-byte kernel must be named as missing, got \"%s\"",
          state.detail);

    /* No directory at all. */
    vm_firmware_boot_probe(NULL, &state);
    CHECK(state.readiness == VM_FW_BOOT_INCOMPLETE,
          "a NULL directory reported readiness %d", (int)state.readiness);
    CHECK(state.detail[0] != '\0', "a NULL directory must say something");

    vm_firmware_boot_probe("no-such-directory-here", &state);
    CHECK(state.readiness == VM_FW_BOOT_INCOMPLETE,
          "a missing directory reported readiness %d", (int)state.readiness);

    /*
     * -start against an incomplete import must refuse, and must refuse with
     * the probe's own words rather than a generic failure. This is the string
     * the user sees.
     */
    {
        vm_firmware_boot_t *boot = vm_firmware_boot_create();
        vm_firmware_boot_report_t report;
        s5l8900_t machine;
        CHECK(boot != NULL, "could not create the boot context");
        if (boot) {
            CHECK(s5l8900_init(&machine, S5L_BRINGUP_PHYS_BASE, S5L_BRINGUP_RAM_SIZE),
                  "s5l8900_init failed");
            bool ok = vm_firmware_boot_start(boot, &machine, DIR, &report);
            CHECK(!ok, "an incomplete import was accepted as bootable");
            CHECK(!report.ok, "report.ok must follow the return value");
            CHECK(mentions(report.detail, VM_FW_BOOT_KERNEL_FILE),
                  "the refusal must name the missing file, got \"%s\"",
                  report.detail);
            CHECK(report.summary[0] != '\0',
                  "every report must carry a summary for the status bar");
            /* Nothing was started, so the CPU must not be pointing anywhere
             * that looks like a kernel entry. */
            CHECK(machine.cpu.r[15] == 0u || machine.cpu.r[15] < 0x08000000u,
                  "a refused bring-up left PC at 0x%08x", machine.cpu.r[15]);
            s5l8900_free(&machine);
            vm_firmware_boot_destroy(&boot);
            CHECK(boot == NULL, "destroy must clear the caller's pointer");
        }
    }

    /* Provisioning refuses a directory it cannot use, and says why. */
    {
        char detail[VM_FW_BOOT_DETAIL_CAPACITY];
        CHECK(!vm_firmware_boot_provision(NULL, detail, sizeof detail),
              "provisioning a NULL directory succeeded");
        CHECK(detail[0] != '\0', "provisioning must explain its refusal");
        /* An existing work image is never replaced, so a second boot cannot
         * silently discard the guest's writes from the first. */
        CHECK(!vm_firmware_boot_provision(DIR, detail, sizeof detail),
              "provisioning overwrote an existing work image");
        printf("  (existing work image refused: %s)\n", detail);

        /* And with the destination clear, the fixture's rootfs.img -- 4 KB of
         * 0x5a, not an HFS volume -- is refused on its own merits. */
        remove_file(VM_FW_BOOT_WORK_FILE);
        CHECK(!vm_firmware_boot_provision(DIR, detail, sizeof detail),
              "provisioning accepted a rootfs.img that is not a volume");
        CHECK(detail[0] != '\0', "provisioning must explain its refusal");
        printf("  (junk rootfs refused: %s)\n", detail);
    }

    remove_file(VM_FW_BOOT_KERNEL_FILE);
    remove_file(VM_FW_BOOT_DEVICETREE_FILE);
    remove_file(VM_FW_BOOT_ROOTFS_FILE);
    remove_file(VM_FW_BOOT_WORK_FILE);

    printf("== vm firmware boot: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
