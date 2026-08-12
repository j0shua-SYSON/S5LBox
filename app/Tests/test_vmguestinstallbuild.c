/* S5LBox -- end-to-end guest install builder policy tests. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "VMGuestInstallBuild.h"

#include "VMSnapshotStore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <unistd.h>
#endif

#define FIXTURE_DIR "vmguestinstallbuild-fixture"
#define HFS_FIXTURE_BLOCK_SIZE 512u
#define HFS_FIXTURE_BLOCKS 16u
#define HFS_FIXTURE_SIZE (HFS_FIXTURE_BLOCK_SIZE * HFS_FIXTURE_BLOCKS)
#define HFS_FIXTURE_BITMAP_OFFSET (4u * HFS_FIXTURE_BLOCK_SIZE)
#define HFS_VOLUME_HEADER_OFFSET 1024u
#define HFS_VOLUME_HEADER_SIZE 512u

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

static bool write_buffer(const char *path, const uint8_t *bytes,
                         size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = size == 0u || fwrite(bytes, 1u, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool read_buffer(const char *path, uint8_t *bytes, size_t size) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = size == 0u || fread(bytes, 1u, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static uint64_t file_size_or_zero(const char *path) {
#ifdef _WIN32
    struct _stat64 st;
    return path && _stat64(path, &st) == 0 &&
           (st.st_mode & _S_IFREG) != 0 && st.st_size > 0
               ? (uint64_t)st.st_size : 0u;
#else
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_size > 0 ? (uint64_t)st.st_size : 0u;
#endif
}

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void write_be64(uint8_t *bytes, uint64_t value) {
    write_be32(bytes, (uint32_t)(value >> 32));
    write_be32(bytes + 4u, (uint32_t)value);
}

static void hfs_fixture_bitmap_set(uint8_t *image, uint32_t bit) {
    uint8_t *byte = image + HFS_FIXTURE_BITMAP_OFFSET + (bit >> 3);
    *byte |= (uint8_t)(1u << (7u - (bit & 7u)));
}

static void make_dirty_hfs_fixture(uint8_t image[HFS_FIXTURE_SIZE]) {
    static const uint32_t USED_BLOCKS[] = {0u, 1u, 2u, 4u, 14u, 15u};
    memset(image, 0, HFS_FIXTURE_SIZE);
    uint8_t *header = image + HFS_VOLUME_HEADER_OFFSET;
    write_be16(header, 0x4858u); /* HFSX */
    write_be16(header + 2u, 5u);
    write_be32(header + 4u, 0u); /* not cleanly unmounted */
    write_be32(header + 40u, HFS_FIXTURE_BLOCK_SIZE);
    write_be32(header + 44u, HFS_FIXTURE_BLOCKS);
    write_be32(header + 48u,
               HFS_FIXTURE_BLOCKS -
                   (uint32_t)(sizeof USED_BLOCKS / sizeof USED_BLOCKS[0]));
    write_be32(header + 52u, 1u);
    write_be64(header + 112u, 8u); /* 64 allocation-bitmap bits */
    write_be32(header + 124u, 1u);
    write_be32(header + 128u, 4u);
    write_be32(header + 132u, 1u);
    for (size_t i = 0u; i < sizeof USED_BLOCKS / sizeof USED_BLOCKS[0]; i++)
        hfs_fixture_bitmap_set(image, USED_BLOCKS[i]);
    memcpy(image + HFS_FIXTURE_SIZE - HFS_VOLUME_HEADER_OFFSET, header,
           HFS_VOLUME_HEADER_SIZE);
}

static bool read_hfs_geometry(const char *path, uint32_t *block_size,
                              uint32_t *total_blocks,
                              uint32_t *free_blocks) {
    if (block_size) *block_size = 0u;
    if (total_blocks) *total_blocks = 0u;
    if (free_blocks) *free_blocks = 0u;
    if (!path || !block_size || !total_blocks || !free_blocks) return false;

    FILE *file = fopen(path, "rb");
    if (!file) return false;
    uint8_t header[512];
    bool ok = fseek(file, 1024L, SEEK_SET) == 0 &&
              fread(header, 1u, sizeof header, file) == sizeof header;
    if (fclose(file) != 0) ok = false;
    if (!ok || !((header[0] == 'H' && header[1] == '+') ||
                 (header[0] == 'H' && header[1] == 'X')))
        return false;

    uint32_t block = read_be32(header + 40u);
    uint32_t total = read_be32(header + 44u);
    uint32_t free = read_be32(header + 48u);
    if (block == 0u || total == 0u || free > total) return false;
    *block_size = block;
    *total_blocks = total;
    *free_blocks = free;
    return true;
}

static bool resize_sparse(const char *path, uint64_t size) {
#ifdef _WIN32
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD ignored = 0u;
    bool ok = DeviceIoControl(file, FSCTL_SET_SPARSE, NULL, 0u, NULL, 0u,
                              &ignored, NULL) != 0;
    LARGE_INTEGER end;
    end.QuadPart = (LONGLONG)size;
    if (ok && !SetFilePointerEx(file, end, NULL, FILE_BEGIN)) ok = false;
    if (ok && !SetEndOfFile(file)) ok = false;
    if (!CloseHandle(file)) ok = false;
    return ok;
#else
    FILE *file = fopen(path, "r+b");
    if (!file) return false;
    bool ok = ftruncate(fileno(file), (off_t)size) == 0;
    if (fclose(file) != 0) ok = false;
    return ok;
#endif
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
    (void)join_path(stage, sizeof stage, FIXTURE_DIR,
                    VM_GUEST_STORAGE_STAGE_DIRECTORY);
    (void)join_path(next, sizeof next, stage, VM_GUEST_INSTALL_NEXT_FILE);
    (void)remove(next);
    (void)remove_directory(stage);
    static const char *const LEAVES[] = {
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
    bool staging_seen;
} progress_log_t;

static void capture_progress(void *opaque,
                             vm_guest_install_build_phase_t phase,
                             uint64_t completed, uint64_t total) {
    progress_log_t *log = (progress_log_t *)opaque;
    if (!log) return;
    if (log->calls == 0u) log->first = phase;
    log->last = phase;
    if (phase == VM_GUEST_INSTALL_BUILD_STAGING) log->staging_seen = true;
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
    CHECK(resize_sparse(live, VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES),
          "could not make the committed fixture represent a 2 GiB disk");

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            FIXTURE_DIR, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && result.already_installed &&
          !result.storage_upgraded &&
          result.transaction.committed &&
          memcmp(result.manifest_sha256, digest, sizeof digest) == 0,
          "existing install was not idempotent: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    CHECK(progress.calls >= 3u &&
          progress.first == VM_GUEST_INSTALL_BUILD_RECOVERING &&
          progress.last == VM_GUEST_INSTALL_BUILD_COMPLETE,
          "idempotent progress did not reach completion");
}

static void test_dirty_existing_install_refuses_before_stage(void) {
    clear_fixture();
    CHECK(make_directory(FIXTURE_DIR), "could not create dirty fixture");
    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    char next[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, FIXTURE_DIR,
                    VM_GUEST_INSTALL_LIVE_FILE) &&
          write_bytes(live, "old-rootfs"),
          "could not seed dirty committed fixture");

    vm_guest_install_result_t transaction;
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    CHECK(vm_guest_install_prepare_stage(FIXTURE_DIR, &transaction,
                                         detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK &&
          vm_guest_install_stage_image_path(next, sizeof next, FIXTURE_DIR) &&
          write_bytes(next, "installed-rootfs"),
          "could not prepare dirty committed fixture: %s", detail);
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest);
    CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &transaction,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed,
          "could not commit dirty fixture: %s", detail);

    uint8_t fixture[HFS_FIXTURE_SIZE];
    uint8_t observed[HFS_FIXTURE_SIZE];
    make_dirty_hfs_fixture(fixture);
    CHECK(write_buffer(live, fixture, sizeof fixture),
          "could not write dirty HFS fixture");

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            FIXTURE_DIR, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN &&
          result.already_installed && result.transaction.committed &&
          !result.storage_upgraded && !result.storage_transaction.committed &&
          result.rootfs.status == ROOTFS_WORK_HFS_INVALID &&
          result.rootfs.stage == ROOTFS_WORK_STAGE_SOURCE_VALIDATE &&
          strstr(detail, "clean guest shutdown") != NULL,
          "dirty install returned %s/%s: %s",
          vm_guest_install_build_status_text(status),
          rootfs_work_stage_name(result.rootfs.stage), detail);
    CHECK(!progress.staging_seen,
          "dirty install emitted staging progress before refusing");
    CHECK(read_buffer(live, observed, sizeof observed) &&
          memcmp(observed, fixture, sizeof fixture) == 0 &&
          file_size_or_zero(live) == HFS_FIXTURE_SIZE,
          "dirty install changed its live disk");

    uint8_t after_digest[VM_GUEST_INSTALL_SHA256_SIZE];
    CHECK(vm_guest_install_probe(FIXTURE_DIR, after_digest,
                                 detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_VALID &&
          memcmp(after_digest, digest, sizeof digest) == 0,
          "dirty refusal changed install authority: %s", detail);
    static const char *const STORAGE_LEAVES[] = {
        VM_GUEST_STORAGE_BACKUP_FILE,
        VM_GUEST_STORAGE_STAGE_DIRECTORY,
        VM_GUEST_STORAGE_MARKER_FILE,
        VM_GUEST_STORAGE_MARKER_TMP,
        VM_GUEST_STORAGE_JOURNAL_FILE,
        VM_GUEST_STORAGE_JOURNAL_TMP
    };
    for (size_t i = 0u;
         i < sizeof STORAGE_LEAVES / sizeof STORAGE_LEAVES[0]; i++) {
        char path[VM_GUEST_INSTALL_PATH_CAPACITY];
        CHECK(join_path(path, sizeof path, FIXTURE_DIR, STORAGE_LEAVES[i]) &&
              !exists(path),
              "dirty refusal left storage artifact %s", STORAGE_LEAVES[i]);
    }
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
    vm_guest_install_build_result_t retry;
    status = vm_guest_install_build_from_directory(
        machine, NULL, capture_progress, &progress,
        &retry, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && retry.already_installed &&
          !retry.storage_upgraded && retry.transaction.committed &&
          retry.rootfs.final_size == 0u,
          "2 GiB install was rewritten or not idempotent: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    printf("real-install-build base=%llu final=%llu entries=%u reused=%u\n",
           (unsigned long long)base.final_size,
           (unsigned long long)result.rootfs.final_size,
           result.rootfs.provision_entries,
           result.rootfs.provision_reused_entries);
}

static void test_real_storage_upgrade_when_supplied(void) {
    const char *machine = getenv("S5LBOX_EXISTING_INSTALL_MACHINE_DIR");
    if (!machine || !*machine) {
        printf("real-storage-upgrade SKIP (existing machine path unset)\n");
        return;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, machine,
                    VM_GUEST_INSTALL_LIVE_FILE),
          "real storage-upgrade live path overflow");
    uint64_t before = file_size_or_zero(live);
    CHECK(before > 0u && before < VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES,
          "real storage-upgrade source is %llu bytes, not an older small disk",
          (unsigned long long)before);
    if (before == 0u || before >= VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES)
        return;

    uint32_t old_block_size = 0u, old_total_blocks = 0u,
             old_free_blocks = 0u;
    CHECK(read_hfs_geometry(live, &old_block_size, &old_total_blocks,
                            &old_free_blocks),
          "real storage-upgrade source has no readable HFS geometry");
    if (old_block_size == 0u || old_total_blocks == 0u) return;

    uint8_t manifest[VM_GUEST_INSTALL_SHA256_SIZE];
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    CHECK(vm_guest_install_probe(machine, manifest, detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_VALID,
          "real storage-upgrade source has no valid install marker: %s",
          detail);

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            machine, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && result.already_installed &&
          result.storage_upgraded && result.transaction.committed &&
          result.storage_transaction.committed,
          "real storage upgrade refused: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    uint64_t after = file_size_or_zero(live);
    CHECK(after == VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES &&
          result.rootfs.final_size == after,
          "real storage upgrade published %llu bytes, result says %llu",
          (unsigned long long)after,
          (unsigned long long)result.rootfs.final_size);

    uint32_t new_block_size = 0u, new_total_blocks = 0u,
             new_free_blocks = 0u;
    CHECK(read_hfs_geometry(live, &new_block_size, &new_total_blocks,
                            &new_free_blocks) &&
          new_block_size == old_block_size &&
          new_total_blocks > old_total_blocks &&
          new_free_blocks > old_free_blocks,
          "real storage upgrade did not publish larger usable HFS geometry");

    uint8_t after_manifest[VM_GUEST_INSTALL_SHA256_SIZE];
    CHECK(vm_guest_install_probe(machine, after_manifest,
                                 detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_VALID &&
          memcmp(after_manifest, manifest, sizeof manifest) == 0,
          "real storage upgrade changed install authority: %s", detail);
    vm_guest_install_result_t storage;
    CHECK(vm_guest_storage_recover(machine, &storage,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && storage.committed &&
          memcmp(storage.manifest_sha256, manifest, sizeof manifest) == 0,
          "real storage-upgrade record is not recoverable: %s", detail);

    vm_guest_install_build_result_t retry;
    status = vm_guest_install_build_from_directory(
        machine, NULL, capture_progress, &progress,
        &retry, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && retry.already_installed &&
          !retry.storage_upgraded && retry.rootfs.final_size == 0u,
          "real storage-upgrade retry rewrote the disk: %s / %s",
          vm_guest_install_build_status_text(status), detail);

    const char *capacity_proof = getenv("S5LBOX_STORAGE_CAPACITY_PROOF");
    if (capacity_proof && *capacity_proof) {
        CHECK(!exists(capacity_proof),
              "capacity-proof destination already exists; refusing to replace it");
        size_t payload_size = ROOTFS_WORK_MAX_ENTRY_BYTES;
        uint8_t *payload = (uint8_t *)malloc(payload_size);
        CHECK(payload != NULL, "capacity-proof payload allocation failed");
        if (payload && !exists(capacity_proof)) {
            for (size_t i = 0u; i < payload_size; i++)
                payload[i] = (uint8_t)(i * 131u + 17u);
            rootfs_work_entry_t entries[3];
            memset(entries, 0, sizeof entries);
            static const char *const PATHS[3] = {
                "/private/var/tmp/.s5lbox-capacity-proof-1",
                "/private/var/tmp/.s5lbox-capacity-proof-2",
                "/private/var/tmp/.s5lbox-capacity-proof-3"
            };
            for (size_t i = 0u; i < 3u; i++) {
                entries[i].kind = ROOTFS_WORK_ENTRY_FILE;
                entries[i].path = PATHS[i];
                entries[i].content = payload;
                entries[i].content_size = payload_size;
                entries[i].permissions = 0600u;
                entries[i].owner_id = 0u;
                entries[i].group_id = 0u;
            }
            rootfs_work_options_t proof_options;
            memset(&proof_options, 0, sizeof proof_options);
            proof_options.preserve_fstab = true;
            proof_options.entries = entries;
            proof_options.entry_count = 3u;
            rootfs_work_result_t proof;
            rootfs_work_status_t proof_status = rootfs_work_create(
                live, capacity_proof, &proof_options, &proof);
            uint64_t old_free_bytes =
                (uint64_t)old_free_blocks * old_block_size;
            uint64_t requested_bytes = (uint64_t)payload_size * 3u;
            CHECK(requested_bytes > old_free_bytes,
                  "capacity proof requested %llu bytes but old disk had %llu free",
                  (unsigned long long)requested_bytes,
                  (unsigned long long)old_free_bytes);
            CHECK(proof_status == ROOTFS_WORK_OK && proof.published &&
                  proof.provision_entries == 3u &&
                  proof.provision_blocks > old_free_blocks,
                  "post-growth allocation did not exceed the old free-block ceiling: %s/%s entries=%u blocks=%u old-free=%u",
                  rootfs_work_status_name(proof_status),
                  proof.detail, proof.provision_entries,
                  proof.provision_blocks, old_free_blocks);
            printf("real-storage-capacity old-free=%u blocks allocated=%u "
                   "bytes=%llu proof=%s\n",
                   old_free_blocks, proof.provision_blocks,
                   (unsigned long long)requested_bytes, capacity_proof);
        }
        free(payload);
    } else {
        printf("real-storage-capacity SKIP (proof path unset)\n");
    }
    printf("real-storage-upgrade before=%llu final=%llu blocks=%u->%u "
           "free=%u->%u\n",
           (unsigned long long)before, (unsigned long long)after,
           old_total_blocks, new_total_blocks,
           old_free_blocks, new_free_blocks);
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
    test_dirty_existing_install_refuses_before_stage();
    test_real_storage_upgrade_when_supplied();
    test_real_build_when_supplied();
    clear_fixture();
    (void)remove_directory(FIXTURE_DIR);
    printf("== guest install builder: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
