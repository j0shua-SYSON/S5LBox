/*
 * S5LBox -- crash-safe guest filesystem transaction tests.
 *
 * Every fixture is a few bytes. The point is rename ordering, strict records,
 * and recovery at each durable boundary; copying a 445 MiB HFS image would
 * make these tests slower without testing one additional state transition.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMGuestInstall.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#define FIXTURE_DIR "vmguestinstall-fixture"

static unsigned checks;
static unsigned failures;

#define CHECK(condition, ...) do {                                         \
    checks++;                                                              \
    if (!(condition)) {                                                    \
        failures++;                                                        \
        printf("  FAIL %s:%d: ", __func__, __LINE__);                    \
        printf(__VA_ARGS__);                                               \
        printf("\n");                                                     \
    }                                                                      \
} while (0)

static bool join_path(char *out, size_t capacity, const char *directory,
                      const char *name) {
    int written = snprintf(out, capacity, "%s/%s", directory, name);
    return written >= 0 && (size_t)written < capacity;
}

static bool make_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

static bool remove_directory(const char *path) {
#ifdef _WIN32
    return _rmdir(path) == 0 || errno == ENOENT;
#else
    return rmdir(path) == 0 || errno == ENOENT;
#endif
}

static bool write_bytes(const char *path, const char *bytes) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t size = strlen(bytes);
    bool ok = fwrite(bytes, 1u, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool path_for(char *out, size_t capacity, const char *name) {
    return join_path(out, capacity, FIXTURE_DIR, name);
}

static bool stage_path_for(char *out, size_t capacity, const char *name) {
    char stage[1400];
    return path_for(stage, sizeof stage, VM_GUEST_INSTALL_STAGE_DIRECTORY) &&
           join_path(out, capacity, stage, name);
}

static bool file_equals(const char *path, const char *wanted) {
    char bytes[64];
    memset(bytes, 0, sizeof bytes);
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t got = fread(bytes, 1u, sizeof bytes, file);
    bool ok = ferror(file) == 0 && fclose(file) == 0;
    size_t wanted_size = strlen(wanted);
    return ok && got == wanted_size &&
           memcmp(bytes, wanted, wanted_size) == 0;
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void remove_fixture_artifacts(void) {
    static const char *const names[] = {
        VM_GUEST_INSTALL_LIVE_FILE,
        VM_GUEST_INSTALL_BACKUP_FILE,
        VM_GUEST_INSTALL_MARKER_FILE,
        VM_GUEST_INSTALL_MARKER_TMP,
        VM_GUEST_INSTALL_JOURNAL_FILE,
        VM_GUEST_INSTALL_JOURNAL_TMP,
        VM_GUEST_INSTALL_RESUME_ONCE_FILE,
        VM_GUEST_INSTALL_RESUME_ONCE_TMP,
    };
    char path[1400];
    if (stage_path_for(path, sizeof path, VM_GUEST_INSTALL_NEXT_FILE))
        (void)remove(path);
    if (path_for(path, sizeof path, VM_GUEST_INSTALL_STAGE_DIRECTORY))
        (void)remove_directory(path);
    for (size_t i = 0u; i < sizeof names / sizeof names[0]; i++) {
        if (!path_for(path, sizeof path, names[i])) continue;
        (void)remove(path);
        (void)remove_directory(path);
    }
}

static bool prepare_pair(void) {
    remove_fixture_artifacts();
    char stage[1400];
    char live[1400];
    char next[1400];
    char resume[1400];
    return path_for(stage, sizeof stage, VM_GUEST_INSTALL_STAGE_DIRECTORY) &&
           make_directory(stage) &&
           path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE) &&
           stage_path_for(next, sizeof next, VM_GUEST_INSTALL_NEXT_FILE) &&
           path_for(resume, sizeof resume,
                    VM_GUEST_INSTALL_RESUME_ONCE_FILE) &&
           write_bytes(live, "old-rootfs") &&
           write_bytes(next, "new-rootfs") &&
           write_bytes(resume, "resume old disk\n");
}

static void fill_digest(uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE],
                        unsigned seed) {
    for (size_t i = 0u; i < VM_GUEST_INSTALL_SHA256_SIZE; i++)
        digest[i] = (uint8_t)(seed + (unsigned)i * 7u);
}

static void check_committed_files(const uint8_t digest[
                                      VM_GUEST_INSTALL_SHA256_SIZE]) {
    char live[1400];
    char backup[1400];
    char journal[1400];
    char stage[1400];
    char resume[1400];
    CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE),
          "live path overflow");
    CHECK(file_equals(live, "new-rootfs"), "new live bytes are wrong");
    CHECK(path_for(backup, sizeof backup, VM_GUEST_INSTALL_BACKUP_FILE),
          "backup path overflow");
    CHECK(!exists(backup), "backup survived successful cleanup");
    CHECK(path_for(journal, sizeof journal, VM_GUEST_INSTALL_JOURNAL_FILE),
          "journal path overflow");
    CHECK(!exists(journal), "journal survived successful cleanup");
    CHECK(path_for(stage, sizeof stage, VM_GUEST_INSTALL_STAGE_DIRECTORY),
          "stage path overflow");
    CHECK(!exists(stage), "stage directory survived successful cleanup");
    CHECK(path_for(resume, sizeof resume,
                   VM_GUEST_INSTALL_RESUME_ONCE_FILE),
          "automatic-resume path overflow");
    CHECK(!exists(resume),
          "old-disk automatic resume survived filesystem replacement");

    uint8_t read_digest[VM_GUEST_INSTALL_SHA256_SIZE];
    char detail[256];
    CHECK(vm_guest_install_probe(FIXTURE_DIR, read_digest,
                                 detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_VALID,
          "committed marker was not strict-valid: %s", detail);
    CHECK(memcmp(read_digest, digest, sizeof read_digest) == 0,
          "committed marker digest changed");
}

static void test_normal_and_idempotent(void) {
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    uint8_t other[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest, 1u);
    fill_digest(other, 2u);
    CHECK(prepare_pair(), "could not prepare normal transaction");

    vm_guest_install_result_t result;
    char detail[256];
    vm_guest_install_status_t status =
        vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                 detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK, "publish returned %s: %s",
          vm_guest_install_status_text(status), detail);
    CHECK(result.committed && !result.rolled_back &&
          result.cleanup_complete && result.has_manifest,
          "normal publish flags are wrong");
    check_committed_files(digest);

    status = vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed,
          "same-manifest retry was not idempotent: %s", detail);
    status = vm_guest_install_publish(FIXTURE_DIR, other, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_ERR_STATE && !result.committed,
          "different manifest overwrote or accepted a committed install");

    /* Once transaction debris is gone, a checkpoint saved against the new
     * disk is legitimate and ordinary startup recovery must preserve it. */
    char resume[1400];
    CHECK(path_for(resume, sizeof resume,
                   VM_GUEST_INSTALL_RESUME_ONCE_FILE),
          "new automatic-resume path overflow");
    CHECK(write_bytes(resume, "resume new disk\n"),
          "could not seed post-install automatic resume");
    status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed && exists(resume),
          "clean committed startup deleted a new-disk resume point");
    (void)remove(resume);
}

static void test_every_durable_boundary(void) {
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest, 11u);
    for (unsigned boundary = 1u; boundary <= 4u; boundary++) {
        CHECK(prepare_pair(), "could not prepare boundary %u", boundary);
        vm_guest_install_result_t result;
        char detail[256];
        vm_guest_install_test_interrupt_after(boundary);
        vm_guest_install_status_t status =
            vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                     detail, sizeof detail);
        vm_guest_install_test_interrupt_after(0u);
        CHECK(status == VM_GUEST_INSTALL_ERR_INTERRUPTED,
              "boundary %u returned %s, not interruption", boundary,
              vm_guest_install_status_text(status));
        if (boundary == 4u)
            CHECK(result.committed,
                  "marker boundary did not report an already committed disk");

        status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                          detail, sizeof detail);
        CHECK(status == VM_GUEST_INSTALL_OK,
              "boundary %u recovery returned %s: %s", boundary,
              vm_guest_install_status_text(status), detail);
        CHECK(result.committed && result.cleanup_complete,
              "boundary %u did not recover to a clean commit", boundary);
        check_committed_files(digest);
    }
}

static void test_missing_stage_rolls_back(void) {
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest, 21u);
    CHECK(prepare_pair(), "could not prepare rollback transaction");
    vm_guest_install_result_t result;
    char detail[256];
    vm_guest_install_test_interrupt_after(2u);
    vm_guest_install_status_t status =
        vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                 detail, sizeof detail);
    vm_guest_install_test_interrupt_after(0u);
    CHECK(status == VM_GUEST_INSTALL_ERR_INTERRUPTED,
          "backup-boundary interruption failed");
    char next[1400];
    CHECK(stage_path_for(next, sizeof next, VM_GUEST_INSTALL_NEXT_FILE),
          "next path overflow");
    CHECK(remove(next) == 0, "could not simulate a lost stage image");

    status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.rolled_back &&
          !result.committed,
          "lost stage was not rolled back: %s", detail);
    char live[1400];
    CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE),
          "live path overflow");
    CHECK(file_equals(live, "old-rootfs"),
          "rollback did not restore original bytes");
    CHECK(vm_guest_install_probe(FIXTURE_DIR, NULL, detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_ABSENT,
          "rollback published an install marker");
}

static void test_orphan_and_ambiguous_backup(void) {
    remove_fixture_artifacts();
    char live[1400];
    char backup[1400];
    CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE),
          "live path overflow");
    CHECK(path_for(backup, sizeof backup, VM_GUEST_INSTALL_BACKUP_FILE),
          "backup path overflow");
    CHECK(write_bytes(backup, "old-rootfs"), "could not write orphan backup");
    vm_guest_install_result_t result;
    char detail[256];
    vm_guest_install_status_t status =
        vm_guest_install_recover(FIXTURE_DIR, &result,
                                 detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.rolled_back,
          "unambiguous orphan backup was not restored: %s", detail);
    CHECK(file_equals(live, "old-rootfs"),
          "orphan recovery changed backup bytes");

    CHECK(write_bytes(backup, "ambiguous-backup"),
          "could not write ambiguous backup");
    status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_ERR_STATE,
          "live plus journal-free backup was guessed through");
    CHECK(file_equals(live, "old-rootfs") &&
          file_equals(backup, "ambiguous-backup"),
          "ambiguous recovery mutated a disk");
}

static void test_malformed_records_fail_closed(void) {
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest, 31u);
    CHECK(prepare_pair(), "could not prepare malformed-journal test");
    vm_guest_install_result_t result;
    char detail[256];
    vm_guest_install_test_interrupt_after(1u);
    vm_guest_install_status_t status =
        vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                 detail, sizeof detail);
    vm_guest_install_test_interrupt_after(0u);
    CHECK(status == VM_GUEST_INSTALL_ERR_INTERRUPTED,
          "journal interruption failed");
    char journal[1400];
    CHECK(path_for(journal, sizeof journal, VM_GUEST_INSTALL_JOURNAL_FILE),
          "journal path overflow");
    CHECK(write_bytes(journal, "not a transaction\n"),
          "could not corrupt journal");
    status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_ERR_RECORD && !result.committed,
          "malformed journal was accepted");
    char live[1400];
    char next[1400];
    CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE),
          "live path overflow");
    CHECK(stage_path_for(next, sizeof next, VM_GUEST_INSTALL_NEXT_FILE),
          "next path overflow");
    CHECK(file_equals(live, "old-rootfs") && file_equals(next, "new-rootfs"),
          "malformed journal caused a disk mutation");

    remove_fixture_artifacts();
    char marker[1400];
    CHECK(path_for(marker, sizeof marker, VM_GUEST_INSTALL_MARKER_FILE),
          "marker path overflow");
    CHECK(write_bytes(live, "unchanged-rootfs") &&
          write_bytes(marker, "nonempty but malformed\n"),
          "could not create malformed marker fixture");
    CHECK(vm_guest_install_probe(FIXTURE_DIR, NULL, detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_INVALID,
          "nonempty malformed marker counted as installed");
    status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_ERR_RECORD &&
          file_equals(live, "unchanged-rootfs"),
          "malformed committed marker was guessed through");
}

static void test_committed_cleanup_failure_is_distinct(void) {
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest, 41u);
    CHECK(prepare_pair(), "could not prepare cleanup test");
    vm_guest_install_result_t result;
    char detail[256];
    vm_guest_install_test_interrupt_after(4u);
    vm_guest_install_status_t status =
        vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                 detail, sizeof detail);
    vm_guest_install_test_interrupt_after(0u);
    CHECK(status == VM_GUEST_INSTALL_ERR_INTERRUPTED && result.committed,
          "marker interruption did not leave a committed transaction");

    char backup[1400];
    CHECK(path_for(backup, sizeof backup, VM_GUEST_INSTALL_BACKUP_FILE),
          "backup path overflow");
    CHECK(remove(backup) == 0 && make_directory(backup),
          "could not create cleanup obstruction");
    status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed &&
          !result.cleanup_complete,
          "cleanup failure lost committed state: %s", detail);
    CHECK(remove_directory(backup), "could not remove cleanup obstruction");
    status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed &&
          result.cleanup_complete,
          "cleanup retry did not converge: %s", detail);
    check_committed_files(digest);
}

int main(void) {
    printf("== guest install transaction ==\n");
    if (!make_directory(FIXTURE_DIR)) {
        printf("could not create %s\n", FIXTURE_DIR);
        return 2;
    }
    remove_fixture_artifacts();

    char stage_image[1400];
    CHECK(vm_guest_install_stage_image_path(stage_image, sizeof stage_image,
                                            FIXTURE_DIR),
          "stage-image path was refused");
    CHECK(strstr(stage_image, VM_GUEST_INSTALL_STAGE_DIRECTORY) != NULL &&
          strstr(stage_image, VM_GUEST_INSTALL_NEXT_FILE) != NULL,
          "stage-image path names the wrong file: %s", stage_image);

    test_normal_and_idempotent();
    test_every_durable_boundary();
    test_missing_stage_rolls_back();
    test_orphan_and_ambiguous_backup();
    test_malformed_records_fail_closed();
    test_committed_cleanup_failure_is_distinct();

    vm_guest_install_test_interrupt_after(0u);
    remove_fixture_artifacts();
    (void)remove_directory(FIXTURE_DIR);
    printf("== guest install transaction: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
