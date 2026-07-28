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
/* A second one, for the two-directory layout: the imported artefacts stay in
 * DIR and one machine's work image lives here. */
static const char *WORKDIR = "vmfwboot-fixture-machine";

/* The one-directory layout the app had before machines had their own files.
 * Most of the cases below are about the four files and do not care which
 * layout they are in, so they use this. */
static vm_firmware_boot_paths_t SHARED;

static void write_at(const char *directory, const char *name, size_t bytes) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", directory, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  cannot create %s\n", path); return; }
    for (size_t i = 0; i < bytes; i++) fputc(0x5a, f);
    fclose(f);
}

static void remove_at(const char *directory, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", directory, name);
    remove(path);
}

static void write_file(const char *name, size_t bytes) {
    write_at(DIR, name, bytes);
}

static void remove_file(const char *name) { remove_at(DIR, name); }

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

    if (!make_dir(DIR) || !make_dir(WORKDIR)) {
        printf("SKIP: cannot create the fixture directory\n");
        return 0;
    }
    CHECK(vm_firmware_boot_paths_shared(&SHARED, DIR),
          "the one-directory layout was refused");

    remove_file(VM_FW_BOOT_KERNEL_FILE);
    remove_file(VM_FW_BOOT_DEVICETREE_FILE);
    remove_file(VM_FW_BOOT_ROOTFS_FILE);
    remove_file(VM_FW_BOOT_WORK_FILE);
    remove_at(WORKDIR, VM_FW_BOOT_WORK_FILE);

    /* Nothing imported at all. */
    vm_firmware_boot_probe(&SHARED, &state);
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
    vm_firmware_boot_probe(&SHARED, &state);
    CHECK(state.readiness == VM_FW_BOOT_INCOMPLETE,
          "two of three files reported readiness %d", (int)state.readiness);
    CHECK(mentions(state.detail, VM_FW_BOOT_ROOTFS_FILE) &&
          !mentions(state.detail, VM_FW_BOOT_KERNEL_FILE),
          "must name only the missing file, got \"%s\"", state.detail);

    /* All three imported, no work image: a distinct answer, because the fix
     * is a different action from importing. */
    write_file(VM_FW_BOOT_ROOTFS_FILE, 4096);
    vm_firmware_boot_probe(&SHARED, &state);
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
    vm_firmware_boot_probe(&SHARED, &state);
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
    vm_firmware_boot_probe(&SHARED, &state);
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

    {
        vm_firmware_boot_paths_t nowhere;
        CHECK(vm_firmware_boot_paths_shared(&nowhere, "no-such-directory-here"),
              "a plausible directory name was refused");
        vm_firmware_boot_probe(&nowhere, &state);
        CHECK(state.readiness == VM_FW_BOOT_INCOMPLETE,
              "a missing directory reported readiness %d", (int)state.readiness);

        /* An empty path is not a directory, and a path too long to hold is a
         * refusal rather than a truncated path pointing somewhere real. */
        CHECK(vm_firmware_boot_paths_shared(&nowhere, ""),
              "the empty directory name was refused by the builder");
        vm_firmware_boot_probe(&nowhere, &state);
        CHECK(state.readiness == VM_FW_BOOT_INCOMPLETE,
              "an empty directory reported readiness %d", (int)state.readiness);

        char huge[VM_FW_BOOT_PATH_CAPACITY + 8u];
        memset(huge, 'x', sizeof huge - 1u);
        huge[sizeof huge - 1u] = '\0';
        CHECK(!vm_firmware_boot_paths_shared(&nowhere, huge),
              "an over-long directory was accepted");
        CHECK(nowhere.firmware[0] == '\0' && nowhere.work[0] == '\0',
              "a refused path builder left something usable behind");
    }

    /*
     * THE TWO-DIRECTORY LAYOUT, which is what makes two machines two machines:
     * the imported artefacts are read from one place and the writable work
     * image from another. Everything above shares one directory, so this is
     * the case that proves the work image is looked for where the MACHINE is
     * and not where the import is.
     */
    {
        vm_firmware_boot_paths_t split;
        CHECK(vm_firmware_boot_paths_split(&split, DIR, WORKDIR),
              "the two-directory layout was refused");

        /* DIR still holds a work image from the case above; the machine's own
         * directory does not. The machine must report NEEDS_WORK_IMAGE rather
         * than quietly booting the shared one. */
        remove_at(WORKDIR, VM_FW_BOOT_WORK_FILE);
        remove_file(VM_FW_BOOT_KERNEL_FILE);
        write_file(VM_FW_BOOT_KERNEL_FILE, 64);
        vm_firmware_boot_probe(&split, &state);
        CHECK(state.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE,
              "a machine with no work image of its own reported %d",
              (int)state.readiness);
        CHECK(state.rootfs_present && !state.work_present,
              "the split layout lost the shared artefacts");

        /*
         * Give the machine one, and only it counts. 4777 rather than the 777
         * this used to be: a work image is a COPY of the root filesystem plus
         * growth, so one smaller than the 4096-octet rootfs fixture is now
         * refused as incomplete -- see the case below. Still a distinctive
         * number, so "the shared one was used" remains detectable.
         */
        write_at(WORKDIR, VM_FW_BOOT_WORK_FILE, 4777);
        vm_firmware_boot_probe(&split, &state);
        CHECK(state.readiness == VM_FW_BOOT_READY,
              "a machine with its own work image reported %d",
              (int)state.readiness);
        CHECK(state.work_size == 4777u,
              "the shared work image was used instead of the machine's: %llu",
              (unsigned long long)state.work_size);

        /*
         * AND A SHORT ONE IS NOT A WORK IMAGE. An interrupted or refused
         * provision leaves a truncated file, and treating "not empty" as
         * "prepared" boots a truncated HFS+ volume: it mounts, launchd cannot
         * create anything on it, and the boot stops immediately after
         * AppleMultitouchZ2SPI with a black screen and no error anywhere. That
         * is what a user hit, at 7.2 billion instructions of nothing.
         */
        remove_at(WORKDIR, VM_FW_BOOT_WORK_FILE);
        write_at(WORKDIR, VM_FW_BOOT_WORK_FILE, 4095);   /* one short */
        vm_firmware_boot_probe(&split, &state);
        CHECK(state.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE,
              "a work image one octet smaller than its source reported %d; a "
              "truncated volume must never be called ready",
              (int)state.readiness);
        CHECK(!state.work_present,
              "a short work image was counted as present");
        CHECK(strstr(state.detail, "incomplete") != NULL,
              "the reason does not say the image is incomplete, so the user "
              "cannot tell it apart from one never started: \"%s\"",
              state.detail);
        remove_at(WORKDIR, VM_FW_BOOT_WORK_FILE);
        write_at(WORKDIR, VM_FW_BOOT_WORK_FILE, 4777);
        remove_at(WORKDIR, VM_FW_BOOT_WORK_FILE);
        remove_file(VM_FW_BOOT_KERNEL_FILE);
        write_file(VM_FW_BOOT_KERNEL_FILE, 0);
    }

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
            bool ok = vm_firmware_boot_start(boot, &machine, &SHARED,
                                             NULL, 0u, &report);
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
        CHECK(!vm_firmware_boot_provision(NULL, NULL, 0u, detail, sizeof detail),
              "provisioning a NULL directory succeeded");
        CHECK(detail[0] != '\0', "provisioning must explain its refusal");
        /* An existing work image is never replaced, so a second boot cannot
         * silently discard the guest's writes from the first. */
        write_file(VM_FW_BOOT_WORK_FILE, 8192);
        CHECK(!vm_firmware_boot_provision(&SHARED, NULL, 0u,
                                          detail, sizeof detail),
              "provisioning overwrote an existing work image");
        printf("  (existing work image refused: %s)\n", detail);

        /* And with the destination clear, the fixture's rootfs.img -- 4 KB of
         * 0x5a, not an HFS volume -- is refused on its own merits. */
        remove_file(VM_FW_BOOT_WORK_FILE);
        CHECK(!vm_firmware_boot_provision(&SHARED, NULL, 0u,
                                          detail, sizeof detail),
              "provisioning accepted a rootfs.img that is not a volume");
        CHECK(detail[0] != '\0', "provisioning must explain its refusal");
        printf("  (junk rootfs refused: %s)\n", detail);

        /*
         * A machine whose own directory does not exist. The provisioner
         * creates no directories -- C has no portable mkdir -- so this has to
         * be a named refusal rather than a work image written somewhere else.
         */
        vm_firmware_boot_paths_t missing;
        CHECK(vm_firmware_boot_paths_split(&missing, DIR,
                                           "vmfwboot-no-such-machine"),
              "the split layout was refused");
        CHECK(!vm_firmware_boot_provision(&missing, NULL, 0u,
                                          detail, sizeof detail),
              "provisioning into a directory that does not exist succeeded");
        CHECK(detail[0] != '\0', "provisioning must explain its refusal");
    }

    /*
     * THE SWITCHES REACH THE REQUEST, and the report says which did not.
     *
     * This test cannot boot anything -- there is no kernel here -- so what it
     * pins is the contract the engine relies on: that a refusal before the
     * request is built leaves the option report empty rather than half filled
     * in, so nothing downstream can show a stale claim about the switches.
     */
    {
        vm_firmware_boot_t *boot = vm_firmware_boot_create();
        vm_firmware_boot_report_t report;
        s5l8900_t machine;
        if (boot) {
            CHECK(s5l8900_init(&machine, S5L_BRINGUP_PHYS_BASE,
                               S5L_BRINGUP_RAM_SIZE),
                  "s5l8900_init failed");
            bool values[VM_BOOT_OPTION_MAX];
            for (unsigned i = 0; i < vm_option_count(); i++)
                values[i] = !vm_option_at(i)->def;
            (void)vm_firmware_boot_start(boot, &machine, &SHARED, values,
                                         vm_option_count(), &report);
            CHECK(report.options.count == 0u,
                  "a refusal before the request was built still reported %u "
                  "option rows", report.options.count);
            s5l8900_free(&machine);
            vm_firmware_boot_destroy(&boot);
        }
    }

    remove_file(VM_FW_BOOT_KERNEL_FILE);
    remove_file(VM_FW_BOOT_DEVICETREE_FILE);
    remove_file(VM_FW_BOOT_ROOTFS_FILE);
    remove_file(VM_FW_BOOT_WORK_FILE);
    remove_at(WORKDIR, VM_FW_BOOT_WORK_FILE);

    printf("== vm firmware boot: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
