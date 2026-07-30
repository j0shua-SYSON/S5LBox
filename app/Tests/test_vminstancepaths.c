/*
 * Host-side tests for app/Sources/VMInstancePaths.c — which files belong to
 * which machine.
 *
 * Two properties, and they fail in opposite directions:
 *
 *   1. TWO MACHINES MUST NOT SHARE A WORK IMAGE. Sharing it is not a cosmetic
 *      bug; it is two guests writing one HFS+ volume. So every derivation is
 *      asserted to produce a DIFFERENT work-image path per id, and the same
 *      shared path for the read-only artefacts.
 *   2. AN EXISTING 450 MB IMPORT MUST NOT VANISH. The pre-instance work image
 *      is adopted by rename, exactly once, and a failed adoption must leave it
 *      where it was and say where that is.
 *
 * Every fixture here is a file this test writes itself; it needs no firmware
 * and copies nothing large.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMInstancePaths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

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

/* The test's own scratch tree, created beside the test binary. */
static const char *ROOT     = "vminstpaths-fixture";
static const char *FIRMWARE = "vminstpaths-fixture/firmware";
static const char *MACHINES = "vminstpaths-fixture/Machines";

static const char *ID_A = "00112233445566aa";
static const char *ID_B = "ffeeddccbbaa9988";

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

static void write_file(const char *path, size_t bytes) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  cannot create %s\n", path); return; }
    for (size_t i = 0; i < bytes; i++) fputc(0x5a, f);
    fclose(f);
}

static uint64_t size_of(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0u;
    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    fclose(f);
    return size > 0 ? (uint64_t)size : 0u;
}

static bool exists(const char *path) { return size_of(path) > 0u; }

static bool ends_with(const char *text, const char *suffix) {
    size_t n = strlen(text), m = strlen(suffix);
    return n >= m && strcmp(text + (n - m), suffix) == 0;
}

/* ------------------------------------------------------------------------- */

static void test_ids_that_must_be_refused(void) {
    vm_instance_paths_t paths;

    /*
     * The id becomes a path component. Every one of these would escape the
     * machines directory or collide with another machine's files, and the only
     * thing standing between a corrupted machines.txt and a wrong directory is
     * this check.
     */
    static const char *const BAD[] = {
        "", "..", "../../etc", "0011223344556/77", "0011223344556\\77",
        "00112233445566AA",            /* upper case is not the stored form */
        "00112233445566a",             /* fifteen                            */
        "00112233445566aaa",           /* seventeen                          */
        "00112233445566a.", "0011223344556 aa", "zzzzzzzzzzzzzzzz"
    };
    for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        vm_instance_paths_status_t s =
            vm_instance_paths_derive(FIRMWARE, MACHINES, BAD[i], &paths);
        CHECK(s == VM_INSTANCE_PATHS_ERR_ID,
              "identifier \"%s\" was accepted (status %d)", BAD[i], (int)s);
        CHECK(paths.machine[0] == '\0' && paths.work_image[0] == '\0',
              "a refused identifier still produced paths");
    }

    CHECK(vm_instance_paths_derive(NULL, MACHINES, ID_A, &paths) ==
              VM_INSTANCE_PATHS_ERR_NULL,
          "a missing firmware directory was accepted");
    CHECK(vm_instance_paths_derive(FIRMWARE, NULL, ID_A, &paths) ==
              VM_INSTANCE_PATHS_ERR_NULL,
          "a missing machines directory was accepted");
    CHECK(vm_instance_paths_derive(FIRMWARE, MACHINES, NULL, &paths) ==
              VM_INSTANCE_PATHS_ERR_NULL,
          "a missing identifier was accepted");
    CHECK(vm_instance_paths_derive(FIRMWARE, "", ID_A, &paths) ==
              VM_INSTANCE_PATHS_ERR_NULL,
          "an empty machines directory was accepted");
    CHECK(vm_instance_paths_derive(FIRMWARE, MACHINES, ID_A, NULL) ==
              VM_INSTANCE_PATHS_ERR_NULL,
          "a NULL output was accepted");

    /* Every refusal has words. */
    for (int s = VM_INSTANCE_PATHS_OK; s <= VM_INSTANCE_PATHS_ERR_TOO_LONG; s++) {
        const char *text =
            vm_instance_paths_status_text((vm_instance_paths_status_t)s);
        CHECK(text && text[0], "status %d has no text", s);
    }

    /* A directory long enough that the join cannot fit is a refusal, not a
     * truncated path pointing at somebody else's file. */
    char huge[VM_FW_BOOT_PATH_CAPACITY + 8u];
    memset(huge, 'x', sizeof huge - 1u);
    huge[sizeof huge - 1u] = '\0';
    CHECK(vm_instance_paths_derive(FIRMWARE, huge, ID_A, &paths) ==
              VM_INSTANCE_PATHS_ERR_TOO_LONG,
          "an over-long machines directory was accepted");
    CHECK(paths.machine[0] == '\0',
          "an over-long path still produced a machine directory");
}

static void test_two_machines_are_two_machines(void) {
    vm_instance_paths_t a, b;

    CHECK(vm_instance_paths_derive(FIRMWARE, MACHINES, ID_A, &a) ==
              VM_INSTANCE_PATHS_OK, "deriving machine A failed");
    CHECK(vm_instance_paths_derive(FIRMWARE, MACHINES, ID_B, &b) ==
              VM_INSTANCE_PATHS_OK, "deriving machine B failed");

    /* THE PROPERTY. Different disks. */
    CHECK(strcmp(a.work_image, b.work_image) != 0,
          "two machines share one work image: %s", a.work_image);
    CHECK(strcmp(a.machine, b.machine) != 0,
          "two machines share one directory: %s", a.machine);

    /* And the same read-only originals, because copying 450 MB per machine
     * would buy nothing: nothing writes to them. */
    CHECK(strcmp(a.firmware, b.firmware) == 0,
          "the imported artefacts are not shared");
    CHECK(strcmp(a.legacy_work_image, b.legacy_work_image) == 0,
          "the pre-instance work image resolves differently per machine");

    /* Shape. The id names the directory, the file name is the same one the
     * boot path already looks for. */
    CHECK(strstr(a.machine, ID_A) != NULL,
          "the machine directory is not named after the id: %s", a.machine);
    CHECK(ends_with(a.work_image, VM_FW_BOOT_WORK_FILE),
          "the work image is not %s: %s", VM_FW_BOOT_WORK_FILE, a.work_image);
    CHECK(ends_with(a.legacy_work_image, VM_FW_BOOT_WORK_FILE) &&
          strstr(a.legacy_work_image, "firmware") != NULL,
          "the pre-instance work image is not in the firmware directory: %s",
          a.legacy_work_image);
    CHECK(strstr(a.work_image, ID_A) != NULL,
          "the work image is not inside the machine's directory: %s",
          a.work_image);

    /*
     * THE SNAPSHOT PAIR, held to the same property as the disk: one machine's
     * suspended state must never resolve to another's. Restoring machine B's
     * RAM on top of machine A's filesystem would corrupt A's volume silently,
     * and a resume never runs fsck to notice.
     */
    CHECK(strcmp(a.state, b.state) != 0,
          "two machines share one snapshot: %s", a.state);
    CHECK(strcmp(a.state_md, b.state_md) != 0,
          "two machines share one bridge state: %s", a.state_md);
    CHECK(strstr(a.state, ID_A) != NULL && strstr(a.state_md, ID_A) != NULL,
          "the snapshot is not inside the machine's directory: %s", a.state);
    CHECK(ends_with(a.state, VM_FW_BOOT_STATE_FILE),
          "the snapshot is not %s: %s", VM_FW_BOOT_STATE_FILE, a.state);
    /*
     * And the partial names must differ from the final ones, or the atomic
     * rename degrades into writing straight over the good snapshot -- which
     * is the failure the temp file exists to prevent, so it is worth a check
     * rather than a comment.
     */
    CHECK(strcmp(a.state, a.state_tmp) != 0,
          "the partial snapshot has the same name as the finished one: %s",
          a.state);
    CHECK(strcmp(a.state_md, a.state_md_tmp) != 0,
          "the partial bridge state has the same name as the finished one: %s",
          a.state_md);
    CHECK(strcmp(a.state, a.state_md) != 0,
          "the snapshot and the bridge state are the same file: %s", a.state);

    /* A trailing separator on the caller's directory must not double up. */
    vm_instance_paths_t slashed;
    char with_slash[256];
    snprintf(with_slash, sizeof with_slash, "%s/", MACHINES);
    CHECK(vm_instance_paths_derive(FIRMWARE, with_slash, ID_A, &slashed) ==
              VM_INSTANCE_PATHS_OK, "a trailing separator was refused");
    CHECK(strstr(slashed.machine, "//") == NULL,
          "a trailing separator produced a doubled one: %s", slashed.machine);

    /* What the boot path is handed: shared firmware, private work directory. */
    vm_firmware_boot_paths_t boot;
    vm_instance_paths_to_boot(&a, &boot);
    CHECK(strcmp(boot.firmware, a.firmware) == 0,
          "the boot paths lost the firmware directory");
    CHECK(strcmp(boot.work, a.machine) == 0,
          "the boot paths do not point at this machine's directory");
    CHECK(strcmp(boot.firmware, boot.work) != 0,
          "the boot paths collapsed back to one directory");

    vm_instance_paths_to_boot(NULL, &boot);
    CHECK(boot.firmware[0] == '\0' && boot.work[0] == '\0',
          "NULL paths produced a usable boot layout");
}

/*
 * The plan, which is the decision that costs either nothing, a rename, or
 * 450 MB. Driven entirely off files this test creates and removes.
 */
static void test_work_image_plan(void) {
    vm_instance_paths_t a;
    if (vm_instance_paths_derive(FIRMWARE, MACHINES, ID_A, &a) !=
        VM_INSTANCE_PATHS_OK) return;
    if (!make_dir(a.machine)) { printf("  SKIP: cannot create %s\n", a.machine); return; }

    remove(a.work_image);
    remove(a.legacy_work_image);

    /* Nothing anywhere: the expensive answer. */
    CHECK(vm_instance_work_plan(&a) == VM_INSTANCE_WORK_PROVISION,
          "an empty installation did not ask to provision");

    /* A pre-instance shared image and no private one: adopt. */
    write_file(a.legacy_work_image, 4096);
    CHECK(vm_instance_work_plan(&a) == VM_INSTANCE_WORK_ADOPT,
          "a pre-instance work image was not offered for adoption");

    /* Both: the machine's own wins, and adoption must not fire. Sharing is
     * how the old build behaved and it must not be reachable by accident. */
    write_file(a.work_image, 2048);
    CHECK(vm_instance_work_plan(&a) == VM_INSTANCE_WORK_PRIVATE,
          "a machine with its own image was told to adopt");

    /* A zero-byte file is not a file: an interrupted copy leaves exactly that,
     * and treating it as this machine's disk hands the guest an empty volume. */
    remove(a.work_image);
    write_file(a.work_image, 0);
    CHECK(vm_instance_work_plan(&a) == VM_INSTANCE_WORK_ADOPT,
          "a zero-byte work image was accepted as a root filesystem");

    /* And adoption must actually go through over it. rename() succeeds over an
     * existing entry on POSIX and fails with EEXIST on Windows, so this is the
     * case where the phone and the test host would otherwise disagree. */
    {
        char detail[512];
        CHECK(vm_instance_work_adopt(&a, detail, sizeof detail),
              "adoption over a zero-byte work image failed: %s", detail);
        CHECK(size_of(a.work_image) == 4096u,
              "the adopted image is %llu bytes, expected 4096",
              (unsigned long long)size_of(a.work_image));
    }
    remove(a.work_image);
    write_file(a.legacy_work_image, 4096);

    remove(a.legacy_work_image);
    write_file(a.legacy_work_image, 0);
    CHECK(vm_instance_work_plan(&a) == VM_INSTANCE_WORK_PROVISION,
          "a zero-byte pre-instance image was offered for adoption");
    remove(a.legacy_work_image);

    CHECK(vm_instance_work_plan(NULL) == VM_INSTANCE_WORK_PROVISION,
          "NULL paths did not fall back to provisioning");
}

/*
 * ADOPTION. The user's existing 450 MB guest disk is moved, not copied and not
 * deleted, and exactly one machine can have it.
 */
static void test_adoption_moves_and_happens_once(void) {
    vm_instance_paths_t a, b;
    char detail[512];

    if (vm_instance_paths_derive(FIRMWARE, MACHINES, ID_A, &a) !=
            VM_INSTANCE_PATHS_OK ||
        vm_instance_paths_derive(FIRMWARE, MACHINES, ID_B, &b) !=
            VM_INSTANCE_PATHS_OK) return;
    if (!make_dir(a.machine) || !make_dir(b.machine)) return;

    remove(a.work_image);
    remove(b.work_image);
    remove(a.legacy_work_image);

    /* A recognisable size, so "moved" can be told from "made". */
    write_file(a.legacy_work_image, 12345);
    CHECK(exists(a.legacy_work_image), "the fixture did not create the legacy image");

    detail[0] = '\0';
    CHECK(vm_instance_work_adopt(&a, detail, sizeof detail),
          "adoption failed: %s", detail);
    CHECK(size_of(a.work_image) == 12345u,
          "the adopted image is %llu bytes, expected 12345",
          (unsigned long long)size_of(a.work_image));
    CHECK(!exists(a.legacy_work_image),
          "the pre-instance image is still in the shared directory");
    CHECK(detail[0] != '\0', "a successful adoption said nothing");
    printf("  (adopted: %s)\n", detail);

    /* Once. The second machine finds nothing to adopt and is told so rather
     * than being handed the first machine's disk. */
    detail[0] = '\0';
    CHECK(!vm_instance_work_adopt(&b, detail, sizeof detail),
          "a second machine adopted an image that had already been taken");
    CHECK(detail[0] != '\0', "a refused adoption said nothing");
    CHECK(!exists(b.work_image),
          "a refused adoption still created a work image");
    CHECK(vm_instance_work_plan(&b) == VM_INSTANCE_WORK_PROVISION,
          "the second machine was not told to provision its own");

    /* And it must never overwrite a machine's own image, even if a legacy one
     * reappears -- which it can, because the importer writes to that
     * directory and a user can put files there. */
    write_file(a.legacy_work_image, 999);
    detail[0] = '\0';
    CHECK(!vm_instance_work_adopt(&a, detail, sizeof detail),
          "adoption overwrote a machine's own work image");
    CHECK(size_of(a.work_image) == 12345u,
          "the machine's own image was replaced: %llu bytes",
          (unsigned long long)size_of(a.work_image));
    CHECK(exists(a.legacy_work_image),
          "a refused adoption removed the file it refused to move");

    /* A NULL detail buffer must not be a crash: the engine passes one, a
     * caller that only wants the answer may not. */
    CHECK(!vm_instance_work_adopt(&a, NULL, 0u),
          "adoption with no detail buffer reported success");
    CHECK(!vm_instance_work_adopt(NULL, detail, sizeof detail),
          "adoption of NULL paths reported success");

    remove(a.work_image);
    remove(b.work_image);
    remove(a.legacy_work_image);
}

/*
 * The whole point, end to end: after adoption the two machines see two
 * different root filesystems through the boot layer, and the shared originals
 * through the same one.
 */
static void test_boot_layer_sees_the_split(void) {
    vm_instance_paths_t a, b;
    vm_firmware_boot_paths_t boot_a, boot_b;
    vm_firmware_boot_state_t state_a, state_b;

    if (vm_instance_paths_derive(FIRMWARE, MACHINES, ID_A, &a) !=
            VM_INSTANCE_PATHS_OK ||
        vm_instance_paths_derive(FIRMWARE, MACHINES, ID_B, &b) !=
            VM_INSTANCE_PATHS_OK) return;
    if (!make_dir(a.machine) || !make_dir(b.machine)) return;

    char path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    snprintf(path, sizeof path, "%s/%s", FIRMWARE, VM_FW_BOOT_KERNEL_FILE);
    write_file(path, 64);
    snprintf(path, sizeof path, "%s/%s", FIRMWARE, VM_FW_BOOT_DEVICETREE_FILE);
    write_file(path, 64);
    snprintf(path, sizeof path, "%s/%s", FIRMWARE, VM_FW_BOOT_ROOTFS_FILE);
    write_file(path, 4096);

    /* A has a disk, B does not. */
    write_file(a.work_image, 8192);
    remove(b.work_image);

    vm_instance_paths_to_boot(&a, &boot_a);
    vm_instance_paths_to_boot(&b, &boot_b);
    vm_firmware_boot_probe(&boot_a, &state_a);
    vm_firmware_boot_probe(&boot_b, &state_b);

    CHECK(state_a.readiness == VM_FW_BOOT_READY,
          "the machine with a work image is not ready (%d): %s",
          (int)state_a.readiness, state_a.detail);
    CHECK(state_b.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE,
          "the machine without a work image reported %d: %s",
          (int)state_b.readiness, state_b.detail);
    CHECK(state_a.rootfs_size == state_b.rootfs_size &&
          state_a.kernel_size == state_b.kernel_size,
          "the two machines see different imported artefacts");
    CHECK(state_a.work_size == 8192u && state_b.work_size == 0u,
          "the work images are not per machine (%llu vs %llu)",
          (unsigned long long)state_a.work_size,
          (unsigned long long)state_b.work_size);

    /* An empty work directory reports "no work image" rather than falling back
     * to the shared one, which is how a stale caller would resurrect sharing. */
    write_file(a.legacy_work_image, 4096);
    vm_firmware_boot_paths_t nowhere;
    vm_firmware_boot_paths_split(&nowhere, FIRMWARE, "");
    vm_firmware_boot_state_t state_none;
    vm_firmware_boot_probe(&nowhere, &state_none);
    CHECK(state_none.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE,
          "an unnamed machine found a work image anyway (%d)",
          (int)state_none.readiness);

    remove(a.work_image);
    remove(a.legacy_work_image);
    snprintf(path, sizeof path, "%s/%s", FIRMWARE, VM_FW_BOOT_KERNEL_FILE);
    remove(path);
    snprintf(path, sizeof path, "%s/%s", FIRMWARE, VM_FW_BOOT_DEVICETREE_FILE);
    remove(path);
    snprintf(path, sizeof path, "%s/%s", FIRMWARE, VM_FW_BOOT_ROOTFS_FILE);
    remove(path);
}

int main(void) {
    printf("== vm instance paths ==\n");

    if (!make_dir(ROOT) || !make_dir(FIRMWARE) || !make_dir(MACHINES)) {
        printf("SKIP: cannot create the fixture directories\n");
        return 0;
    }

    test_ids_that_must_be_refused();
    test_two_machines_are_two_machines();
    test_work_image_plan();
    test_adoption_moves_and_happens_once();
    test_boot_layer_sees_the_split();

    printf("== vm instance paths: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
