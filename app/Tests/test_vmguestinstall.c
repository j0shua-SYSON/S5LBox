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

static bool storage_stage_path_for(char *out, size_t capacity,
                                   const char *name) {
    char stage[1400];
    return path_for(stage, sizeof stage, VM_GUEST_STORAGE_STAGE_DIRECTORY) &&
           join_path(out, capacity, stage, name);
}

static bool privilege_stage_path_for(char *out, size_t capacity,
                                     const char *name) {
    char stage[1400];
    return path_for(stage, sizeof stage,
                    VM_GUEST_PRIVILEGE_STAGE_DIRECTORY) &&
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
        VM_GUEST_STORAGE_BACKUP_FILE,
        VM_GUEST_STORAGE_MARKER_FILE,
        VM_GUEST_STORAGE_MARKER_TMP,
        VM_GUEST_STORAGE_JOURNAL_FILE,
        VM_GUEST_STORAGE_JOURNAL_TMP,
        VM_GUEST_PRIVILEGE_BACKUP_FILE,
        VM_GUEST_PRIVILEGE_MARKER_FILE,
        VM_GUEST_PRIVILEGE_MARKER_TMP,
        VM_GUEST_PRIVILEGE_JOURNAL_FILE,
        VM_GUEST_PRIVILEGE_JOURNAL_TMP,
        VM_GUEST_INSTALL_RESUME_ONCE_FILE,
        VM_GUEST_INSTALL_RESUME_ONCE_TMP,
    };
    char path[1400];
    if (stage_path_for(path, sizeof path, VM_GUEST_INSTALL_NEXT_FILE))
        (void)remove(path);
    if (path_for(path, sizeof path, VM_GUEST_INSTALL_STAGE_DIRECTORY))
        (void)remove_directory(path);
    if (storage_stage_path_for(path, sizeof path,
                               VM_GUEST_INSTALL_NEXT_FILE))
        (void)remove(path);
    if (path_for(path, sizeof path, VM_GUEST_STORAGE_STAGE_DIRECTORY))
        (void)remove_directory(path);
    if (privilege_stage_path_for(path, sizeof path,
                                 VM_GUEST_INSTALL_NEXT_FILE))
        (void)remove(path);
    if (path_for(path, sizeof path, VM_GUEST_PRIVILEGE_STAGE_DIRECTORY))
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

static void test_stage_preparation(void) {
    remove_fixture_artifacts();
    char live[1400];
    char stage[1400];
    char next[1400];
    char marker_tmp[1400];
    char unexpected[1400];
    CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE) &&
          path_for(stage, sizeof stage, VM_GUEST_INSTALL_STAGE_DIRECTORY) &&
          stage_path_for(next, sizeof next, VM_GUEST_INSTALL_NEXT_FILE) &&
          path_for(marker_tmp, sizeof marker_tmp,
                   VM_GUEST_INSTALL_MARKER_TMP) &&
          join_path(unexpected, sizeof unexpected, stage, "unexpected"),
          "stage-preparation path overflow");
    CHECK(write_bytes(live, "old-rootfs") && make_directory(stage) &&
          write_bytes(next, "inert-incomplete-image") &&
          write_bytes(marker_tmp, "inert partial record"),
          "could not seed an interrupted builder");

    vm_guest_install_result_t result;
    char detail[256];
    vm_guest_install_status_t status = vm_guest_install_prepare_stage(
        FIXTURE_DIR, &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && !result.committed &&
          exists(stage) && !exists(next) && !exists(marker_tmp),
          "interrupted builder did not converge to an empty stage: %s / %s",
          vm_guest_install_status_text(status), detail);

    status = vm_guest_install_prepare_stage(FIXTURE_DIR, &result,
                                            detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && exists(stage) && !exists(next),
          "preparing an already empty stage was not idempotent: %s",
          detail);
    CHECK(write_bytes(unexpected, "preserve me"),
          "could not seed an unexpected stage file");
    status = vm_guest_install_prepare_stage(FIXTURE_DIR, &result,
                                            detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_ERR_STATE && exists(unexpected),
          "an unexpected stage file was removed or accepted: %s / %s",
          vm_guest_install_status_text(status), detail);
    CHECK(remove(unexpected) == 0,
          "could not remove unexpected stage fixture");

    status = vm_guest_install_prepare_stage(FIXTURE_DIR, &result,
                                            detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && write_bytes(next, "new-rootfs"),
          "could not prepare a publishable stage: %s", detail);
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest, 0x51u);
    status = vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed,
          "prepared stage did not publish: %s / %s",
          vm_guest_install_status_text(status), detail);
    status = vm_guest_install_prepare_stage(FIXTURE_DIR, &result,
                                            detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed && !exists(stage),
          "prepare did not preserve an existing committed install: %s",
          detail);
    check_committed_files(digest);
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
    char obstruction[1400];
    CHECK(join_path(obstruction, sizeof obstruction, backup, "keep") &&
          write_bytes(obstruction, "not removable as a file"),
          "could not make cleanup obstruction non-empty");
    status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed &&
          !result.cleanup_complete,
          "cleanup failure lost committed state: %s", detail);
    (void)remove(obstruction);
    CHECK(remove_directory(backup), "could not remove cleanup obstruction");
    status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                      detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed &&
          result.cleanup_complete,
          "cleanup retry did not converge: %s", detail);
    check_committed_files(digest);
}

static void test_storage_recovery_preserves_install_authority(void) {
    for (unsigned boundary = 1u; boundary <= 4u; boundary++) {
        uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
        fill_digest(digest, 71u);
        CHECK(prepare_pair(),
              "could not prepare install before storage boundary %u", boundary);
        vm_guest_install_result_t result;
        char detail[256];
        CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                       detail, sizeof detail) ==
                  VM_GUEST_INSTALL_OK && result.committed,
              "could not commit install before storage boundary %u: %s",
              boundary, detail);

        CHECK(vm_guest_storage_prepare_stage(FIXTURE_DIR, &result,
                                             detail, sizeof detail) ==
                  VM_GUEST_INSTALL_OK && !result.committed,
              "could not prepare storage boundary %u: %s", boundary, detail);
        char next[1400];
        char resume[1400];
        CHECK(vm_guest_storage_stage_image_path(next, sizeof next,
                                                FIXTURE_DIR) &&
              path_for(resume, sizeof resume,
                       VM_GUEST_INSTALL_RESUME_ONCE_FILE) &&
              write_bytes(next, "grown-rootfs") &&
              write_bytes(resume, "resume pre-growth disk\n"),
              "could not seed storage boundary %u", boundary);

        vm_guest_install_test_interrupt_after(boundary);
        vm_guest_install_status_t status = vm_guest_storage_publish(
            FIXTURE_DIR, digest, &result, detail, sizeof detail);
        vm_guest_install_test_interrupt_after(0u);
        CHECK(status == VM_GUEST_INSTALL_ERR_INTERRUPTED,
              "storage boundary %u returned %s, not interruption",
              boundary, vm_guest_install_status_text(status));

        uint8_t install_digest[VM_GUEST_INSTALL_SHA256_SIZE];
        CHECK(vm_guest_install_probe(FIXTURE_DIR, install_digest,
                                     detail, sizeof detail) ==
                  VM_GUEST_INSTALL_PROBE_VALID &&
              memcmp(install_digest, digest, sizeof install_digest) == 0,
              "storage boundary %u removed or changed install authority",
              boundary);

        status = vm_guest_storage_recover(FIXTURE_DIR, &result,
                                          detail, sizeof detail);
        CHECK(status == VM_GUEST_INSTALL_OK && result.committed &&
              result.cleanup_complete && result.has_manifest &&
              memcmp(result.manifest_sha256, digest, sizeof digest) == 0,
              "storage boundary %u did not recover to a clean commit: %s",
              boundary, detail);
        char live[1400];
        CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE) &&
              file_equals(live, "grown-rootfs") && !exists(resume),
              "storage boundary %u published wrong bytes or kept resume",
              boundary);
        status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                          detail, sizeof detail);
        CHECK(status == VM_GUEST_INSTALL_OK && result.committed &&
              memcmp(result.manifest_sha256, digest, sizeof digest) == 0,
              "install authority failed after storage boundary %u: %s",
              boundary, detail);
    }
}

static void test_privilege_recovery_preserves_install_authority(void) {
    for (unsigned boundary = 1u; boundary <= 4u; boundary++) {
        uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
        fill_digest(digest, 93u);
        CHECK(prepare_pair(),
              "could not prepare install before privilege boundary %u",
              boundary);
        vm_guest_install_result_t result;
        char detail[256];
        CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                       detail, sizeof detail) ==
                  VM_GUEST_INSTALL_OK && result.committed,
              "could not commit install before privilege boundary %u: %s",
              boundary, detail);

        CHECK(vm_guest_privilege_prepare_stage(FIXTURE_DIR, &result,
                                               detail, sizeof detail) ==
                  VM_GUEST_INSTALL_OK && !result.committed,
              "could not prepare privilege boundary %u: %s",
              boundary, detail);
        char next[1400];
        char resume[1400];
        CHECK(vm_guest_privilege_stage_image_path(next, sizeof next,
                                                  FIXTURE_DIR) &&
              path_for(resume, sizeof resume,
                       VM_GUEST_INSTALL_RESUME_ONCE_FILE) &&
              write_bytes(next, "privileged-rootfs") &&
              write_bytes(resume, "resume pre-repair disk\n"),
              "could not seed privilege boundary %u", boundary);

        vm_guest_install_test_interrupt_after(boundary);
        vm_guest_install_status_t status = vm_guest_privilege_publish(
            FIXTURE_DIR, digest, &result, detail, sizeof detail);
        vm_guest_install_test_interrupt_after(0u);
        CHECK(status == VM_GUEST_INSTALL_ERR_INTERRUPTED,
              "privilege boundary %u returned %s, not interruption",
              boundary, vm_guest_install_status_text(status));

        uint8_t install_digest[VM_GUEST_INSTALL_SHA256_SIZE];
        CHECK(vm_guest_install_probe(FIXTURE_DIR, install_digest,
                                     detail, sizeof detail) ==
                  VM_GUEST_INSTALL_PROBE_VALID &&
              memcmp(install_digest, digest, sizeof install_digest) == 0,
              "privilege boundary %u removed or changed install authority",
              boundary);

        status = vm_guest_privilege_recover(FIXTURE_DIR, &result,
                                             detail, sizeof detail);
        CHECK(status == VM_GUEST_INSTALL_OK && result.committed &&
              result.cleanup_complete && result.has_manifest &&
              memcmp(result.manifest_sha256, digest, sizeof digest) == 0,
              "privilege boundary %u did not recover to a clean commit: %s",
              boundary, detail);
        char live[1400];
        CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE) &&
              file_equals(live, "privileged-rootfs") && !exists(resume),
              "privilege boundary %u published wrong bytes or kept resume",
              boundary);
        status = vm_guest_install_recover(FIXTURE_DIR, &result,
                                          detail, sizeof detail);
        CHECK(status == VM_GUEST_INSTALL_OK && result.committed &&
              memcmp(result.manifest_sha256, digest, sizeof digest) == 0,
              "install authority failed after privilege boundary %u: %s",
              boundary, detail);
    }
}

static void test_privilege_confirmation_is_marker_only(void) {
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    uint8_t other[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest, 117u);
    fill_digest(other, 118u);
    CHECK(prepare_pair(), "could not prepare marker-only confirmation");
    vm_guest_install_result_t result;
    char detail[256];
    CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &result,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && result.committed,
          "could not commit install before marker-only confirmation: %s",
          detail);
    char resume[1400];
    CHECK(path_for(resume, sizeof resume,
                   VM_GUEST_INSTALL_RESUME_ONCE_FILE) &&
          write_bytes(resume, "resume current disk\n"),
          "could not seed current-disk resume authority");

    vm_guest_install_status_t status = vm_guest_privilege_confirm(
        FIXTURE_DIR, digest, &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed &&
          result.cleanup_complete && result.has_manifest &&
          memcmp(result.manifest_sha256, digest, sizeof digest) == 0,
          "marker-only confirmation failed: %s / %s",
          vm_guest_install_status_text(status), detail);
    char live[1400];
    CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE) &&
          file_equals(live, "new-rootfs") &&
          file_equals(resume, "resume current disk\n"),
          "marker-only confirmation changed the disk or invalidated resume");

    status = vm_guest_privilege_confirm(FIXTURE_DIR, digest, &result,
                                        detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_OK && result.committed,
          "marker-only confirmation was not idempotent: %s", detail);
    status = vm_guest_privilege_confirm(FIXTURE_DIR, other, &result,
                                        detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_ERR_STATE,
          "different confirmation identity replaced the committed marker");
}

static void test_maintenance_recovery_chooses_the_active_owner(void) {
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest, 139u);

    /* A committed privilege marker must not inspect the temporarily missing
     * live path before the active storage journal puts that path back. */
    CHECK(prepare_pair(), "could not prepare active-storage ordering case");
    vm_guest_install_result_t install;
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;
    char detail[256];
    CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &install,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && install.committed &&
          vm_guest_privilege_confirm(FIXTURE_DIR, digest, &privilege,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && privilege.committed &&
          vm_guest_storage_prepare_stage(FIXTURE_DIR, &storage,
                                         detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK,
          "could not seed active storage with committed privilege: %s",
          detail);
    char next[1400];
    CHECK(vm_guest_storage_stage_image_path(next, sizeof next, FIXTURE_DIR) &&
          write_bytes(next, "grown-rootfs"),
          "could not write active-storage candidate");
    vm_guest_install_test_interrupt_after(2u);
    CHECK(vm_guest_storage_publish(FIXTURE_DIR, digest, &storage,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_ERR_INTERRUPTED,
          "storage transaction did not stop with live temporarily absent");
    vm_guest_install_test_interrupt_after(0u);
    CHECK(vm_guest_maintenance_recover(FIXTURE_DIR, &privilege, &storage,
                                       detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && privilege.committed &&
          storage.committed,
          "dynamic recovery did not run active storage first: %s", detail);
    char live[1400];
    CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE) &&
          file_equals(live, "grown-rootfs"),
          "active-storage ordering published the wrong live disk");

    /* The converse: a committed storage marker must wait while the active
     * privilege journal restores the shared live path. */
    CHECK(prepare_pair(), "could not prepare active-privilege ordering case");
    CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &install,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && install.committed &&
          vm_guest_storage_prepare_stage(FIXTURE_DIR, &storage,
                                         detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK &&
          vm_guest_storage_stage_image_path(next, sizeof next, FIXTURE_DIR) &&
          write_bytes(next, "grown-rootfs") &&
          vm_guest_storage_publish(FIXTURE_DIR, digest, &storage,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && storage.committed &&
          vm_guest_privilege_prepare_stage(FIXTURE_DIR, &privilege,
                                           detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK &&
          vm_guest_privilege_stage_image_path(next, sizeof next,
                                              FIXTURE_DIR) &&
          write_bytes(next, "privileged-rootfs"),
          "could not seed active privilege with committed storage: %s",
          detail);
    vm_guest_install_test_interrupt_after(2u);
    CHECK(vm_guest_privilege_publish(FIXTURE_DIR, digest, &privilege,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_ERR_INTERRUPTED,
          "privilege transaction did not stop with live temporarily absent");
    vm_guest_install_test_interrupt_after(0u);
    CHECK(vm_guest_maintenance_recover(FIXTURE_DIR, &privilege, &storage,
                                       detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && privilege.committed &&
          storage.committed,
          "dynamic recovery did not run active privilege first: %s", detail);
    CHECK(file_equals(live, "privileged-rootfs"),
          "active-privilege ordering published the wrong live disk");
}

static void test_maintenance_recovery_refuses_competing_owners(void) {
    char live[1400];
    char privilege_journal[1400];
    char storage_journal[1400];
    char privilege_backup[1400];
    char storage_backup[1400];
    char detail[256];
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;

    remove_fixture_artifacts();
    CHECK(path_for(live, sizeof live, VM_GUEST_INSTALL_LIVE_FILE) &&
          path_for(privilege_journal, sizeof privilege_journal,
                   VM_GUEST_PRIVILEGE_JOURNAL_FILE) &&
          path_for(storage_journal, sizeof storage_journal,
                   VM_GUEST_STORAGE_JOURNAL_FILE) &&
          write_bytes(live, "original-rootfs") &&
          write_bytes(privilege_journal, "claimed\n") &&
          write_bytes(storage_journal, "claimed\n"),
          "could not seed two competing maintenance journals");
    CHECK(vm_guest_maintenance_recover(FIXTURE_DIR, &privilege, &storage,
                                       detail, sizeof detail) ==
              VM_GUEST_INSTALL_ERR_STATE,
          "two maintenance journals were guessed through: %s", detail);
    CHECK(file_equals(live, "original-rootfs") &&
          file_equals(privilege_journal, "claimed\n") &&
          file_equals(storage_journal, "claimed\n"),
          "journal conflict changed the live disk or either owner record");

    remove_fixture_artifacts();
    CHECK(path_for(privilege_backup, sizeof privilege_backup,
                   VM_GUEST_PRIVILEGE_BACKUP_FILE) &&
          path_for(storage_backup, sizeof storage_backup,
                   VM_GUEST_STORAGE_BACKUP_FILE) &&
          write_bytes(privilege_backup, "privilege-original") &&
          write_bytes(storage_backup, "storage-original"),
          "could not seed two orphaned maintenance backups");
    CHECK(vm_guest_maintenance_recover(FIXTURE_DIR, &privilege, &storage,
                                       detail, sizeof detail) ==
              VM_GUEST_INSTALL_ERR_STATE,
          "two orphaned maintenance backups were guessed through: %s",
          detail);
    CHECK(!exists(live) &&
          file_equals(privilege_backup, "privilege-original") &&
          file_equals(storage_backup, "storage-original"),
          "orphan-backup conflict restored an arbitrary owner");
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
    CHECK(vm_guest_storage_stage_image_path(stage_image, sizeof stage_image,
                                            FIXTURE_DIR) &&
          strstr(stage_image, VM_GUEST_STORAGE_STAGE_DIRECTORY) != NULL &&
          strstr(stage_image, VM_GUEST_INSTALL_NEXT_FILE) != NULL,
          "storage stage-image path names the wrong file: %s", stage_image);
    CHECK(vm_guest_privilege_stage_image_path(stage_image,
                                              sizeof stage_image,
                                              FIXTURE_DIR) &&
          strstr(stage_image, VM_GUEST_PRIVILEGE_STAGE_DIRECTORY) != NULL &&
          strstr(stage_image, VM_GUEST_INSTALL_NEXT_FILE) != NULL,
          "privilege stage-image path names the wrong file: %s", stage_image);

    test_stage_preparation();
    test_normal_and_idempotent();
    test_every_durable_boundary();
    test_missing_stage_rolls_back();
    test_orphan_and_ambiguous_backup();
    test_malformed_records_fail_closed();
    test_committed_cleanup_failure_is_distinct();
    test_storage_recovery_preserves_install_authority();
    test_privilege_recovery_preserves_install_authority();
    test_privilege_confirmation_is_marker_only();
    test_maintenance_recovery_chooses_the_active_owner();
    test_maintenance_recovery_refuses_competing_owners();

    vm_guest_install_test_interrupt_after(0u);
    remove_fixture_artifacts();
    (void)remove_directory(FIXTURE_DIR);
    printf("== guest install transaction: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
