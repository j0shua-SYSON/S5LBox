/* S5LBox -- end-to-end guest install builder policy tests. */
#include "VMGuestInstallBuild.h"

#include "VMSnapshotStore.h"

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

#define FIXTURE_DIR "vmguestinstallbuild-fixture"

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
                      const char *leaf) {
    if (!out || !directory || !leaf) return false;
    size_t directory_size = strlen(directory);
    size_t leaf_size = strlen(leaf);
    if (directory_size == 0u || leaf_size == 0u ||
        directory_size > capacity || leaf_size > capacity - directory_size ||
        2u > capacity - directory_size - leaf_size)
        return false;
    memcpy(out, directory, directory_size);
    out[directory_size] = '/';
    memcpy(out + directory_size + 1u, leaf, leaf_size + 1u);
    return true;
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

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void remove_file(const char *directory, const char *leaf) {
    char path[VM_GUEST_INSTALL_PATH_CAPACITY];
    if (join_path(path, sizeof path, directory, leaf)) (void)remove(path);
}

static void clear_fixture(void) {
    char stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    char next[VM_GUEST_INSTALL_PATH_CAPACITY];
    char snapshots[VM_GUEST_INSTALL_PATH_CAPACITY];
    char snapshot[VM_GUEST_INSTALL_PATH_CAPACITY];
    char meta[VM_GUEST_INSTALL_PATH_CAPACITY];
    (void)join_path(stage, sizeof stage, FIXTURE_DIR,
                    VM_GUEST_INSTALL_STAGE_DIRECTORY);
    (void)join_path(next, sizeof next, stage, VM_GUEST_INSTALL_NEXT_FILE);
    (void)vm_snapshot_dir(FIXTURE_DIR, snapshots, sizeof snapshots);
    (void)vm_snapshot_path(snapshots, "20260811T000000Z", false,
                           snapshot, sizeof snapshot);
    (void)vm_snapshot_member_path(snapshots, "20260811T000000Z", false,
                                  VM_SNAPSHOT_META_FILE, meta, sizeof meta);
    (void)remove(meta);
    (void)remove_directory(snapshot);
    (void)remove_directory(snapshots);
    (void)remove(next);
    (void)remove_directory(stage);
    static const char *const LEAVES[] = {
        VM_GUEST_INSTALL_LIVE_FILE,
        VM_GUEST_INSTALL_BACKUP_FILE,
        VM_GUEST_INSTALL_MARKER_FILE,
        VM_GUEST_INSTALL_MARKER_TMP,
        VM_GUEST_INSTALL_JOURNAL_FILE,
        VM_GUEST_INSTALL_JOURNAL_TMP,
        VM_GUEST_INSTALL_RESUME_ONCE_FILE,
        VM_GUEST_INSTALL_RESUME_ONCE_TMP
    };
    for (size_t i = 0u; i < sizeof LEAVES / sizeof LEAVES[0]; i++)
        remove_file(FIXTURE_DIR, LEAVES[i]);
}

typedef struct {
    unsigned calls;
    vm_guest_install_build_phase_t first;
    vm_guest_install_build_phase_t last;
} progress_log_t;

static void capture_progress(void *opaque,
                             vm_guest_install_build_phase_t phase,
                             uint64_t completed, uint64_t total) {
    progress_log_t *log = (progress_log_t *)opaque;
    if (!log) return;
    if (log->calls == 0u) log->first = phase;
    log->last = phase;
    log->calls++;
    CHECK(total == 0u || completed <= total,
          "progress exceeds its phase total");
}

static void test_argument_and_package_refusals(void) {
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_guest_install_build_result_t result;
    CHECK(vm_guest_install_build_from_directory(
              NULL, NULL, NULL, NULL, &result, detail, sizeof detail) ==
              VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT,
          "missing work directory was accepted");
    CHECK(vm_guest_install_build_from_directory(
              "does-not-exist", NULL, NULL, NULL, &result,
              detail, sizeof detail) == VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION,
          "missing machine directory was not a recovery refusal");

    clear_fixture();
    CHECK(make_directory(FIXTURE_DIR), "could not create builder fixture");
    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, FIXTURE_DIR,
                    VM_GUEST_INSTALL_LIVE_FILE) &&
          write_bytes(live, "old-rootfs"),
          "could not seed builder live disk");
    CHECK(vm_guest_install_build_from_directory(
              FIXTURE_DIR, NULL, NULL, NULL, &result,
              detail, sizeof detail) == VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT,
          "missing package directory was accepted");
    CHECK(vm_guest_install_build_from_directory(
              FIXTURE_DIR, "packages-do-not-exist", NULL, NULL, &result,
              detail, sizeof detail) == VM_GUEST_INSTALL_BUILD_ERR_PACKAGES,
          "missing package files were not refused by the package layer");
}

static void test_historical_snapshot_gate(void) {
    clear_fixture();
    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    char snapshots[VM_GUEST_INSTALL_PATH_CAPACITY];
    char snapshot[VM_GUEST_INSTALL_PATH_CAPACITY];
    char meta[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, FIXTURE_DIR,
                    VM_GUEST_INSTALL_LIVE_FILE) &&
          write_bytes(live, "old-rootfs") &&
          vm_snapshot_dir(FIXTURE_DIR, snapshots, sizeof snapshots) ==
              VM_SNAPSHOT_OK &&
          make_directory(snapshots) &&
          vm_snapshot_path(snapshots, "20260811T000000Z", false,
                           snapshot, sizeof snapshot) == VM_SNAPSHOT_OK &&
          make_directory(snapshot) &&
          vm_snapshot_member_path(snapshots, "20260811T000000Z", false,
                                  VM_SNAPSHOT_META_FILE,
                                  meta, sizeof meta) == VM_SNAPSHOT_OK,
          "could not prepare historical snapshot paths");
    vm_snapshot_info_t info;
    memset(&info, 0, sizeof info);
    (void)snprintf(info.id, sizeof info.id, "%s", "20260811T000000Z");
    info.created_unix = UINT64_C(1786383600);
    info.retired = UINT64_C(1234567);
    char snapshot_detail[128];
    CHECK(vm_snapshot_meta_write(meta, &info, snapshot_detail,
                                 sizeof snapshot_detail) == VM_SNAPSHOT_OK,
          "could not write snapshot meta: %s", snapshot_detail);

    vm_guest_install_build_result_t result;
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            FIXTURE_DIR, "packages-do-not-exist", NULL, NULL,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS &&
          result.historical_snapshots == 1u && exists(meta),
          "historical snapshot was ignored or changed: %s / %s",
          vm_guest_install_build_status_text(status), detail);
}

static void fill_digest(uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE]) {
    for (size_t i = 0u; i < VM_GUEST_INSTALL_SHA256_SIZE; i++)
        digest[i] = (uint8_t)(0x31u + (unsigned)i);
}

static void test_existing_install_is_idempotent(void) {
    clear_fixture();
    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    char next[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, FIXTURE_DIR,
                    VM_GUEST_INSTALL_LIVE_FILE) &&
          write_bytes(live, "old-rootfs"),
          "could not seed committed fixture");
    vm_guest_install_result_t transaction;
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    CHECK(vm_guest_install_prepare_stage(FIXTURE_DIR, &transaction,
                                         detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK &&
          vm_guest_install_stage_image_path(next, sizeof next, FIXTURE_DIR) &&
          write_bytes(next, "installed-rootfs"),
          "could not prepare committed fixture: %s", detail);
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest);
    CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &transaction,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed,
          "could not commit fixture: %s", detail);

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            FIXTURE_DIR, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && result.already_installed &&
          result.transaction.committed &&
          memcmp(result.manifest_sha256, digest, sizeof digest) == 0,
          "existing install was not idempotent: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    CHECK(progress.calls >= 3u &&
          progress.first == VM_GUEST_INSTALL_BUILD_RECOVERING &&
          progress.last == VM_GUEST_INSTALL_BUILD_COMPLETE,
          "idempotent progress did not reach completion");
}

static void test_real_build_when_supplied(void) {
    const char *source = getenv("S5LBOX_ROOTFS_SOURCE");
    const char *packages = getenv("S5LBOX_GUEST_PACKAGE_DIR");
    const char *machine = getenv("S5LBOX_INSTALL_MACHINE_DIR");
    bool any = (source && *source) || (packages && *packages) ||
               (machine && *machine);
    bool all = source && *source && packages && *packages &&
               machine && *machine;
    CHECK(!any || all,
          "real builder test needs all three S5LBOX integration paths");
    if (!all) {
        printf("real-install-build SKIP (integration paths unset)\n");
        return;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, machine,
                    VM_GUEST_INSTALL_LIVE_FILE),
          "real live path overflow");
    rootfs_work_options_t base_options;
    memset(&base_options, 0, sizeof base_options);
    base_options.growth_bytes = UINT64_C(32) * 1024u * 1024u;
    rootfs_work_result_t base;
    rootfs_work_status_t base_status = rootfs_work_create(
        source, live, &base_options, &base);
    CHECK(base_status == ROOTFS_WORK_OK && base.published,
          "base work image refused at %s: %s",
          rootfs_work_stage_name(base.stage), base.detail);
    if (base_status != ROOTFS_WORK_OK) return;

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            machine, packages, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK &&
          result.transaction.committed && !result.already_installed,
          "real install build refused: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    CHECK(result.rootfs.final_size > base.final_size &&
          result.plan.packages == 28u &&
          result.plan.foundation_packages == 14u,
          "real build result is incomplete");
    CHECK(result.rootfs.final_size ==
              VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES,
          "real install volume is %llu bytes, expected the 2 GiB minimum",
          (unsigned long long)result.rootfs.final_size);
    uint8_t marker[VM_GUEST_INSTALL_SHA256_SIZE];
    CHECK(vm_guest_install_probe(machine, marker, detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_VALID &&
          memcmp(marker, result.manifest_sha256, sizeof marker) == 0,
          "real transaction marker does not match the built plan: %s", detail);
    printf("real-install-build base=%llu final=%llu entries=%u reused=%u\n",
           (unsigned long long)base.final_size,
           (unsigned long long)result.rootfs.final_size,
           result.rootfs.provision_entries,
           result.rootfs.provision_reused_entries);
}

int main(void) {
    printf("== guest install builder ==\n");
    if (!make_directory(FIXTURE_DIR)) {
        printf("could not create %s\n", FIXTURE_DIR);
        return 2;
    }
    test_argument_and_package_refusals();
    test_historical_snapshot_gate();
    test_existing_install_is_idempotent();
    test_real_build_when_supplied();
    clear_fixture();
    (void)remove_directory(FIXTURE_DIR);
    printf("== guest install builder: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
