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
#include "md_snapshot.h"

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

/*
 * The real kernelcache and device tree, which cannot be committed. Absent is
 * not a failure -- CI without firmware still runs everything else -- but it is
 * announced, because a silent skip is how a seam test stops covering the seam.
 */
static uint8_t *slurp_firmware(const char *name, size_t *len_out) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", S5LBOX_FIRMWARE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    rewind(f);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b); fclose(f); return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return b;
}

/*
 * THE SEAM: the app's own settings, through the app's own mapping, into a real
 * bring-up -- and then look at the machine that came out.
 *
 * THIS IS THE TEST THAT WAS MISSING, and its absence is why a hang shipped
 * while forty-nine tests stayed green. Both halves were covered and the join
 * was not: test_vmbootoptions checks the mapping table but does not link the
 * emulator core at all, and everything that boots a real guest builds its own
 * request rather than the app's. Every one of those tests made true statements
 * about a machine that hung on a phone.
 *
 * So this asserts the one thing none of them could: that setting a switch in
 * the app changes the DEVICE TREE THE GUEST IS GIVEN. It is checked by diffing
 * the published tree against the same bring-up with the un-match list cleared,
 * because a count would pass just as happily if the wrong byte moved.
 */
static void test_app_settings_reach_the_guest(const uint8_t *kernel,
                                              size_t kernel_len,
                                              const uint8_t *tree,
                                              size_t tree_len) {
    /*
     * `multitouch` left this list on 2026-07-30 -- "only until that is fixed",
     * said the entry that used to be here, and run163 plus r181/r182 fixed it:
     * the bootload completes and a slide-to-unlock reaches the home screen. It
     * costs no display either, matched and un-matched both rendering 273,206
     * bytes. test_vmbootoptions asserts the positive half (still applied, now
     * effective, absent from the un-match list); here it simply stops being a
     * node the app strikes from the tree.
     */
    static const char *const NUBS[] = {
        "mbx", "sha1", "baseband", "spi2", "usb-otg"
    };
    const unsigned want = (unsigned)(sizeof NUBS / sizeof NUBS[0]);
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t options;
    s5l_bringup_request_t request;
    s5l_bringup_result_t result;
    s5l8900_t machine;
    uint8_t *plain = NULL;
    uint32_t published = 0u;

    printf("the app's settings reach the guest\n");

    /* The baseline: identical bring-up, un-match list explicitly empty. */
    CHECK(s5l8900_init(&machine, S5L_BRINGUP_PHYS_BASE, S5L_BRINGUP_RAM_SIZE),
          "s5l8900_init failed");
    memset(&request, 0, sizeof request);
    request.kernel = kernel;
    request.kernel_size = kernel_len;
    request.devicetree = tree;
    request.devicetree_size = tree_len;
    CHECK(s5l_bringup(&machine, &request, NULL, &result) == S5L_BRINGUP_OK,
          "the baseline bring-up was refused: %s", result.detail);
    published = result.devicetree_size;
    plain = (uint8_t *)malloc(published ? published : 1u);
    CHECK(plain != NULL, "out of memory");
    if (plain)
        memcpy(plain, machine.ram + (result.devicetree_pa - 0x08000000u),
               published);
    s5l8900_free(&machine);

    /*
     * Now the app's defaults, mapped by the app's own code. Nothing here names
     * a device-tree path: if the mapping stops reaching bring-up again, this
     * fails, which is the whole point.
     */
    for (unsigned i = 0; i < vm_option_count() && i < VM_BOOT_OPTION_MAX; i++)
        values[i] = vm_option_at(i)->def;

    CHECK(s5l8900_init(&machine, S5L_BRINGUP_PHYS_BASE, S5L_BRINGUP_RAM_SIZE),
          "s5l8900_init failed");
    memset(&request, 0, sizeof request);
    vm_boot_options_apply(values, vm_option_count(), &request, &options);
    request.kernel = kernel;
    request.kernel_size = kernel_len;
    request.devicetree = tree;
    request.devicetree_size = tree_len;

    CHECK(request.unmatch_count == want,
          "the app's defaults asked for %u un-matches, expected %u",
          request.unmatch_count, want);

    CHECK(s5l_bringup(&machine, &request, NULL, &result) == S5L_BRINGUP_OK,
          "the app's own configuration was refused: %s", result.detail);
    CHECK(result.devicetree_unmatched == want,
          "%u nodes struck, expected %u", result.devicetree_unmatched, want);
    CHECK(result.devicetree_size == published,
          "the app's configuration resized the device tree");

    if (plain && result.devicetree_size == published) {
        const uint8_t *now = machine.ram + (result.devicetree_pa - 0x08000000u);
        unsigned differs = 0u;
        for (uint32_t i = 0; i < published; i++) {
            if (plain[i] == now[i]) continue;
            differs++;
            CHECK(now[i] == (uint8_t)'x',
                  "byte %u became 0x%02x, not 'x'", i, now[i]);
        }
        CHECK(differs == want,
              "the app's defaults changed %u bytes of the tree, expected %u -- "
              "if this is 0, the settings screen is again deciding nothing",
              differs, want);
    }
    s5l8900_free(&machine);

    /*
     * And the other direction, which is what makes it a test of the seam
     * rather than of a constant: turning the nubs ON must leave the tree
     * untouched. A mapping hard-wired to strike five nodes would pass
     * everything above and fail here.
     */
    for (unsigned i = 0; i < vm_option_count() && i < VM_BOOT_OPTION_MAX; i++)
        values[i] = vm_option_at(i)->def;
    for (unsigned n = 0; n < want; n++)
        for (unsigned i = 0; i < vm_option_count(); i++)
            if (!strcmp(vm_option_at(i)->name, NUBS[n])) values[i] = true;

    CHECK(s5l8900_init(&machine, S5L_BRINGUP_PHYS_BASE, S5L_BRINGUP_RAM_SIZE),
          "s5l8900_init failed");
    memset(&request, 0, sizeof request);
    vm_boot_options_apply(values, vm_option_count(), &request, &options);
    request.kernel = kernel;
    request.kernel_size = kernel_len;
    request.devicetree = tree;
    request.devicetree_size = tree_len;
    CHECK(request.unmatch_count == 0u,
          "matching every nub still asked for %u un-matches",
          request.unmatch_count);
    CHECK(s5l_bringup(&machine, &request, NULL, &result) == S5L_BRINGUP_OK,
          "bring-up with every nub matched was refused: %s", result.detail);
    CHECK(result.devicetree_unmatched == 0u,
          "%u nodes struck with every nub matched",
          result.devicetree_unmatched);
    if (plain && result.devicetree_size == published) {
        const uint8_t *now = machine.ram + (result.devicetree_pa - 0x08000000u);
        CHECK(memcmp(plain, now, published) == 0,
              "matching every nub still changed the tree");
    }
    s5l8900_free(&machine);
    free(plain);
}

/*
 * Optional artifact-backed seam test.  CI cannot carry Apple's firmware or a
 * 588 MB checkpoint fixture, so the ordinary run states the skip.  A maintainer
 * can point S5LBOX_APP_RESTORE_FIXTURE at a disposable directory containing:
 *
 *   rootfs-work.img, state.snap, state.snap.mdstate,
 *   state.snap.restore-once
 *
 * Optional nonempty host-policy controls in the same directory are honored,
 * including clock.active-host-off when that policy is compiled into the app.
 *
 * This reaches the same VMFirmwareBoot entry point as the phone.  It is not a
 * core-only snapshot test: the app must open the external disk, create both md
 * bridges, apply their sidecar, load the machine, and consume the marker.
 */
static void test_saved_state_restore_fixture(void) {
    const char *fixture = getenv("S5LBOX_APP_RESTORE_FIXTURE");
    if (!fixture || !*fixture) {
        printf("SKIP: app saved-state restore needs "
               "S5LBOX_APP_RESTORE_FIXTURE\n");
        return;
    }

    vm_firmware_boot_paths_t paths;
    CHECK(vm_firmware_boot_paths_split(&paths, S5LBOX_FIRMWARE_DIR, fixture),
          "restore fixture paths were refused");

    char interpreter_marker[VM_FW_BOOT_PATH_CAPACITY + 64u];
    snprintf(interpreter_marker, sizeof interpreter_marker, "%s/%s", fixture,
             VM_FW_BOOT_INTERPRETER_FILE);
    FILE *interpreter_file = fopen(interpreter_marker, "rb");
    bool expect_interpreter = interpreter_file != NULL;
    if (interpreter_file) fclose(interpreter_file);
    char user_only_marker[VM_FW_BOOT_PATH_CAPACITY + 64u];
    snprintf(user_only_marker, sizeof user_only_marker, "%s/%s", fixture,
             VM_FW_BOOT_COMPACT_USER_ONLY_FILE);
    FILE *user_only_file = fopen(user_only_marker, "rb");
    bool expect_user_only = user_only_file != NULL;
    if (user_only_file) fclose(user_only_file);
    char window_refill_off_marker[VM_FW_BOOT_PATH_CAPACITY + 64u];
    snprintf(window_refill_off_marker, sizeof window_refill_off_marker,
             "%s/%s", fixture,
             VM_FW_BOOT_COMPACT_WINDOW_REFILL_OFF_FILE);
    FILE *window_refill_off_file = fopen(window_refill_off_marker, "rb");
    bool expect_window_refill_off = window_refill_off_file != NULL;
    if (window_refill_off_file) fclose(window_refill_off_file);
    char privileged_window_refill_marker[VM_FW_BOOT_PATH_CAPACITY + 64u];
    snprintf(privileged_window_refill_marker,
             sizeof privileged_window_refill_marker, "%s/%s", fixture,
             VM_FW_BOOT_COMPACT_PRIVILEGED_WINDOW_REFILL_FILE);
    FILE *privileged_window_refill_file =
        fopen(privileged_window_refill_marker, "rb");
    bool expect_privileged_window_refill =
        privileged_window_refill_file != NULL;
    if (privileged_window_refill_file)
        fclose(privileged_window_refill_file);
    char active_clock_off_marker[VM_FW_BOOT_PATH_CAPACITY + 64u];
    snprintf(active_clock_off_marker, sizeof active_clock_off_marker,
             "%s/%s", fixture, VM_FW_BOOT_ACTIVE_CLOCK_OFF_FILE);
    FILE *active_clock_off_file = fopen(active_clock_off_marker, "rb");
    bool expect_active_clock_off = active_clock_off_file != NULL;
    if (active_clock_off_file) fclose(active_clock_off_file);

    bool values[VM_BOOT_OPTION_MAX];
    for (unsigned i = 0; i < VM_BOOT_OPTION_MAX; i++) values[i] = false;
    for (unsigned i = 0; i < vm_option_count() && i < VM_BOOT_OPTION_MAX; i++)
        values[i] = vm_option_at(i)->def;
    static const char *const ON[] = {
        "mbx", "usb-otg", "multitouch", "vram", "lcd-panel-id",
        "memory-reg", "rtc-patch", "activate", "nat"
    };
    for (unsigned n = 0; n < sizeof ON / sizeof ON[0]; n++) {
        int i = vm_option_index(ON[n]);
        CHECK(i >= 0, "restore option %s is absent", ON[n]);
        if (i >= 0) values[(unsigned)i] = true;
    }
    int ca = vm_option_index("ca-software-render");
    CHECK(ca >= 0, "ca-software-render option is absent");
    if (ca >= 0) values[(unsigned)ca] = false;

    s5l8900_t machine;
    vm_firmware_boot_report_t report;
    vm_firmware_boot_t *boot = vm_firmware_boot_create();
    memset(&machine, 0, sizeof machine);
    CHECK(boot != NULL, "could not create restore boot context");
    CHECK(s5l8900_init(&machine, S5L_BRINGUP_PHYS_BASE,
                       S5L_BRINGUP_RAM_SIZE),
          "restore s5l8900_init failed");
    if (!boot || !machine.ram) {
        if (machine.ram) s5l8900_free(&machine);
        vm_firmware_boot_destroy(&boot);
        return;
    }

    bool ok = vm_firmware_boot_start(boot, &machine, &paths, values,
                                     vm_option_count(), &report);
    CHECK(ok, "app restore was refused: %s", report.detail);
    CHECK(report.ok == ok, "restore report.ok disagrees with return value");
    if (ok) {
        CHECK(machine.cpu.cycles == UINT64_C(7320000000),
              "restored at %llu instructions, expected 7320000000",
              (unsigned long long)machine.cpu.cycles);
        CHECK(s5l_clcd_active_window(&machine.clcd) != CLCD_WIN_NONE,
              "restored active scene has no CLCD window");
        CHECK(mentions(report.summary, "restored"),
              "restore summary hides what happened: %s", report.summary);
#if defined(S5LBOX_IOS_ACTIVE_REALTIME_CLOCK)
        CHECK((machine.active_host_now == NULL) == expect_active_clock_off,
              "active-clock marker policy disagrees with the machine");
        CHECK(mentions(report.summary, "active-clock-off control") ==
                  expect_active_clock_off,
              "active-clock marker is hidden or falsely reported: %s",
              report.summary);
#else
        (void)expect_active_clock_off;
#endif
#if defined(S5LBOX_STATIC_A64_ENGINE)
        if (expect_interpreter)
            CHECK(mentions(report.summary, "interpreter control"),
                  "interpreter marker did not reach the engine: %s",
                  report.summary);
#if defined(S5LBOX_STATIC_A64_DEFAULT_COMPACT_RAW)
        if (!expect_interpreter && expect_user_only)
            CHECK(mentions(report.summary, "User-only control"),
                  "User-only marker did not reach the engine: %s",
                  report.summary);
        if (!expect_interpreter && expect_window_refill_off)
            CHECK(mentions(report.summary, "window-refill-off"),
                  "window-refill marker did not reach the engine: %s",
                  report.summary);
        if (!expect_interpreter && expect_privileged_window_refill)
            CHECK(mentions(report.summary,
                           "privileged-window experiment"),
                  "privileged window marker did not reach the engine: %s",
                  report.summary);
#else
        (void)expect_user_only;
        (void)expect_window_refill_off;
        (void)expect_privileged_window_refill;
#endif
#else
        (void)expect_interpreter;
        (void)expect_user_only;
        (void)expect_window_refill_off;
        (void)expect_privileged_window_refill;
#endif

        char marker[VM_FW_BOOT_PATH_CAPACITY + 64u];
        snprintf(marker, sizeof marker, "%s/%s", fixture,
                 VM_FW_BOOT_RESTORE_ONCE_FILE);
        FILE *still_there = fopen(marker, "rb");
        CHECK(still_there == NULL,
              "successful restore did not consume its one-shot marker");
        if (still_there) fclose(still_there);
    }

    s5l8900_free(&machine);
    vm_firmware_boot_destroy(&boot);
}

int main(void) {
    vm_firmware_boot_state_t state;

    printf("== vm firmware boot ==\n");

    /* This is a persisted ABI, not an in-memory convenience.  r446 and every
     * external-md checkpoint written by bootkernel v1 carry exactly 131,248
     * bytes here; an architecture-dependent layout change must fail before a
     * phone reads the wrong offsets as bridge counters or coherent tail data. */
    CHECK(sizeof(external_md_sidecar_t) == 131248u,
          "external-md sidecar ABI is %zu bytes, expected 131248",
          sizeof(external_md_sidecar_t));
    CHECK(EXTERNAL_MD_SIDECAR_MAGIC == UINT32_C(0x3144534d) &&
          EXTERNAL_MD_SIDECAR_VERSION == UINT32_C(1),
          "external-md sidecar identity drifted");

    test_saved_state_restore_fixture();

    {
        size_t klen = 0, tlen = 0;
        uint8_t *k = slurp_firmware("kernel.macho", &klen);
        uint8_t *t = k ? slurp_firmware("devicetree.bin", &tlen) : NULL;
        if (k && t) {
            test_app_settings_reach_the_guest(k, klen, t, tlen);
        } else {
            printf("SKIP: the app-to-guest seam needs the kernelcache and "
                   "device tree in %s\n", S5LBOX_FIRMWARE_DIR);
        }
        free(k);
        free(t);
    }

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
        CHECK(!vm_firmware_boot_provision(NULL, NULL, 0u, NULL, NULL, detail, sizeof detail),
              "provisioning a NULL directory succeeded");
        CHECK(detail[0] != '\0', "provisioning must explain its refusal");
        /* An existing work image is never replaced, so a second boot cannot
         * silently discard the guest's writes from the first. */
        write_file(VM_FW_BOOT_WORK_FILE, 8192);
        CHECK(!vm_firmware_boot_provision(&SHARED, NULL, 0u, NULL, NULL,
                                          detail, sizeof detail),
              "provisioning overwrote an existing work image");
        printf("  (existing work image refused: %s)\n", detail);

        /* And with the destination clear, the fixture's rootfs.img -- 4 KB of
         * 0x5a, not an HFS volume -- is refused on its own merits. */
        remove_file(VM_FW_BOOT_WORK_FILE);
        CHECK(!vm_firmware_boot_provision(&SHARED, NULL, 0u, NULL, NULL,
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
        CHECK(!vm_firmware_boot_provision(&missing, NULL, 0u, NULL, NULL,
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
