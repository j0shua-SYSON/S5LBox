/* S5LBox -- end-to-end guest install builder policy tests. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "VMGuestInstallBuild.h"

#include "VMFirmwareBoot.h"
#include "VMResumeCheckpoint.h"
#include "VMSnapshotStore.h"
#include "bringup.h"
#include "soc.h"

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
#define CHECKPOINT_TEST_RAM_SIZE UINT32_C(0x00100000)

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

static bool seek_file(FILE *file, uint64_t offset) {
#ifdef _WIN32
    return file && _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return file && fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static bool write_hfs_free_blocks(const char *path, uint64_t image_size,
                                  uint32_t free_blocks) {
    if (!path || image_size < 2u * HFS_VOLUME_HEADER_OFFSET) return false;
    FILE *file = fopen(path, "r+b");
    if (!file) return false;
    uint8_t bytes[4];
    write_be32(bytes, free_blocks);
    bool ok = seek_file(file, HFS_VOLUME_HEADER_OFFSET + 48u) &&
              fwrite(bytes, 1u, sizeof bytes, file) == sizeof bytes &&
              seek_file(file,
                        image_size - HFS_VOLUME_HEADER_OFFSET + 48u) &&
              fwrite(bytes, 1u, sizeof bytes, file) == sizeof bytes &&
              fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    return ok;
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

static void make_clean_free_count_mismatch_fixture(
    uint8_t image[HFS_FIXTURE_SIZE]) {
    make_dirty_hfs_fixture(image);
    uint8_t *primary = image + HFS_VOLUME_HEADER_OFFSET;
    uint8_t *alternate =
        image + HFS_FIXTURE_SIZE - HFS_VOLUME_HEADER_OFFSET;
    write_be32(primary + 4u, 1u << 8); /* cleanly unmounted */
    write_be32(alternate + 4u, 1u << 8);
    uint32_t actual_free = read_be32(primary + 48u);
    write_be32(primary + 48u, actual_free - 1u);
    write_be32(alternate + 48u, actual_free - 1u);
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

static bool read_hfs_attributes(const char *path, uint32_t *attributes) {
    if (attributes) *attributes = 0u;
    if (!path || !attributes) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    uint8_t bytes[4];
    bool ok = fseek(file, (long)(HFS_VOLUME_HEADER_OFFSET + 4u), SEEK_SET) == 0 &&
              fread(bytes, 1u, sizeof bytes, file) == sizeof bytes;
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    *attributes = read_be32(bytes);
    return true;
}

static bool probe_exact_bigboss_keyring(
    const char *live, rootfs_work_result_t *out) {
    size_t size = 0u;
    const uint8_t *keyring = vm_guest_rootfs_bigboss_keyring(&size);
    rootfs_work_file_repair_t probe;
    memset(&probe, 0, sizeof probe);
    probe.path = VM_GUEST_ROOTFS_TRUSTED_KEYRING_PATH;
    probe.expected_size = (uint64_t)size;
    probe.expected_permissions = 0644u;
    probe.desired_permissions = 0644u;
    rootfs_work_file_repair_state_t state =
        ROOTFS_WORK_FILE_REPAIR_MISSING;
    return keyring && size == 1164u &&
           ios3_sha256(keyring, size, probe.expected_sha256) &&
           rootfs_work_probe_file_repair(live, &probe, &state, out) ==
               ROOTFS_WORK_OK &&
           state == ROOTFS_WORK_FILE_REPAIR_SATISFIED;
}

static bool save_automatic_checkpoint(bool powered_off, uint64_t media_size,
                                      char *detail,
                                      size_t detail_capacity) {
    s5l8900_t machine;
    if (!s5l8900_init(&machine, S5L_BRINGUP_PHYS_BASE,
                      CHECKPOINT_TEST_RAM_SIZE))
        return false;
    machine.cpu.r[15] = S5L_BRINGUP_PHYS_BASE;
    if (powered_off) {
        machine.pmu.written[PCF50635_OOCSHDWN] = 1u;
        machine.pmu.regs[PCF50635_OOCSHDWN] =
            PCF50635_OOCSHDWN_GO_STANDBY;
    }
    external_md_sidecar_t sidecar;
    memset(&sidecar, 0, sizeof sidecar);
    sidecar.magic = EXTERNAL_MD_SIDECAR_MAGIC;
    sidecar.version = EXTERNAL_MD_SIDECAR_VERSION;
    sidecar.media_size = media_size;
    sidecar.image_bytes = media_size;
    bool saved = vm_resume_checkpoint_save(
        &machine, &sidecar, FIXTURE_DIR, detail, detail_capacity);
    s5l8900_free(&machine);
    return saved;
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
    if (join_path(path, sizeof path, directory, leaf)) {
        (void)remove(path);
        (void)remove_directory(path);
    }
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
    (void)join_path(stage, sizeof stage, FIXTURE_DIR,
                    VM_GUEST_PRIVILEGE_STAGE_DIRECTORY);
    (void)join_path(next, sizeof next, stage, VM_GUEST_INSTALL_NEXT_FILE);
    (void)remove(next);
    (void)remove_directory(stage);
    (void)join_path(stage, sizeof stage, FIXTURE_DIR,
                    VM_GUEST_SOURCES_STAGE_DIRECTORY);
    (void)join_path(next, sizeof next, stage, VM_GUEST_INSTALL_NEXT_FILE);
    (void)remove(next);
    (void)remove_directory(stage);
    (void)join_path(stage, sizeof stage, FIXTURE_DIR,
                    VM_GUEST_SOURCES_V2_STAGE_DIRECTORY);
    (void)join_path(next, sizeof next, stage, VM_GUEST_INSTALL_NEXT_FILE);
    (void)remove(next);
    (void)remove_directory(stage);
    (void)join_path(stage, sizeof stage, FIXTURE_DIR,
                    VM_GUEST_APT_TRUST_STAGE_DIRECTORY);
    (void)join_path(next, sizeof next, stage, VM_GUEST_INSTALL_NEXT_FILE);
    (void)remove(next);
    (void)remove_directory(stage);
    (void)join_path(stage, sizeof stage, FIXTURE_DIR,
                    VM_GUEST_RECOVERY_STAGE_DIRECTORY);
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
        VM_GUEST_PRIVILEGE_BACKUP_FILE,
        VM_GUEST_PRIVILEGE_MARKER_FILE,
        VM_GUEST_PRIVILEGE_MARKER_TMP,
        VM_GUEST_PRIVILEGE_JOURNAL_FILE,
        VM_GUEST_PRIVILEGE_JOURNAL_TMP,
        VM_GUEST_SOURCES_BACKUP_FILE,
        VM_GUEST_SOURCES_MARKER_FILE,
        VM_GUEST_SOURCES_MARKER_TMP,
        VM_GUEST_SOURCES_JOURNAL_FILE,
        VM_GUEST_SOURCES_JOURNAL_TMP,
        VM_GUEST_SOURCES_V2_BACKUP_FILE,
        VM_GUEST_SOURCES_V2_MARKER_FILE,
        VM_GUEST_SOURCES_V2_MARKER_TMP,
        VM_GUEST_SOURCES_V2_JOURNAL_FILE,
        VM_GUEST_SOURCES_V2_JOURNAL_TMP,
        VM_GUEST_APT_TRUST_BACKUP_FILE,
        VM_GUEST_APT_TRUST_MARKER_FILE,
        VM_GUEST_APT_TRUST_MARKER_TMP,
        VM_GUEST_APT_TRUST_JOURNAL_FILE,
        VM_GUEST_APT_TRUST_JOURNAL_TMP,
        VM_GUEST_RECOVERY_BACKUP_FILE,
        VM_GUEST_RECOVERY_MARKER_FILE,
        VM_GUEST_RECOVERY_MARKER_TMP,
        VM_GUEST_RECOVERY_JOURNAL_FILE,
        VM_GUEST_RECOVERY_JOURNAL_TMP,
        VM_FW_BOOT_STATE_FILE,
        VM_FW_BOOT_STATE_MD_FILE,
        VM_FW_BOOT_STATE_TMP,
        VM_FW_BOOT_STATE_MD_TMP,
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

static void test_allocation_repair_publication_proof(void) {
    const uint64_t size = UINT64_C(2) * 1024u * 1024u * 1024u;
    rootfs_work_result_t probe;
    rootfs_work_result_t repair;

    memset(&probe, 0, sizeof probe);
    probe.status = ROOTFS_WORK_HFS_INVALID;
    probe.source_size = size;
    probe.source_cleanly_unmounted = true;
    probe.source_allocation_free_count_mismatch = true;
    probe.source_allocation_bitmap_used = 113816u;
    probe.source_allocation_header_used = 113817u;

    memset(&repair, 0, sizeof repair);
    repair.status = ROOTFS_WORK_OK;
    repair.source_size = size;
    repair.final_size = size;
    repair.source_cleanly_unmounted = true;
    repair.source_allocation_free_count_mismatch = true;
    repair.source_allocation_bitmap_used = 113816u;
    repair.source_allocation_header_used = 113817u;
    repair.allocation_free_count_repairable = 1u;
    repair.allocation_free_count_repaired = 1u;
    CHECK(vm_guest_install_build_test_allocation_repair_proven(
              &probe, &repair, size),
          "an exact stale free-count repair was not publishable");

    repair.allocation_free_count_repairable = 0u;
    repair.allocation_free_count_repaired = 0u;
    repair.catalog_topology_nodes_repairable = 20u;
    repair.catalog_topology_nodes_repaired = 20u;
    repair.catalog_extent_records_repairable = 1u;
    repair.catalog_extent_records_repaired = 1u;
    repair.allocation_bits_repairable = 1u;
    repair.allocation_bits_repaired = 1u;
    repair.allocation_missing_blocks = 1u;
    repair.allocation_orphan_blocks = 1u;
    repair.allocation_extent_collisions = 1u;
    CHECK(vm_guest_install_build_test_allocation_repair_proven(
              &probe, &repair, size),
          "a fully applied bitmap reconciliation was not publishable");

    rootfs_work_result_t bad = repair;
    bad.allocation_bits_repaired = 0u;
    CHECK(!vm_guest_install_build_test_allocation_repair_proven(
              &probe, &bad, size),
          "a partially applied bitmap plan was publishable");
    bad = repair;
    bad.catalog_extent_records_repaired = 0u;
    CHECK(!vm_guest_install_build_test_allocation_repair_proven(
              &probe, &bad, size),
          "a partially applied fork repair was publishable");
    bad = repair;
    bad.catalog_topology_nodes_repaired = 19u;
    CHECK(!vm_guest_install_build_test_allocation_repair_proven(
              &probe, &bad, size),
          "a partially applied topology repair was publishable");
    bad = repair;
    bad.source_allocation_bitmap_used++;
    CHECK(!vm_guest_install_build_test_allocation_repair_proven(
              &probe, &bad, size),
          "a clone that did not match preflight accounting was publishable");
    bad = repair;
    bad.published = true;
    CHECK(!vm_guest_install_build_test_allocation_repair_proven(
              &probe, &bad, size),
          "a scanner-owned published image was accepted as an unpublished clone");
    bad = repair;
    bad.final_size--;
    CHECK(!vm_guest_install_build_test_allocation_repair_proven(
              &probe, &bad, size),
          "a repair with changed geometry was publishable");
    bad = repair;
    bad.allocation_bits_repairable = 0u;
    bad.allocation_bits_repaired = 0u;
    bad.catalog_extent_records_repairable = 0u;
    bad.catalog_extent_records_repaired = 0u;
    CHECK(!vm_guest_install_build_test_allocation_repair_proven(
              &probe, &bad, size),
          "a topology-only pass was allowed to waive an allocation mismatch");
}

static void test_bigboss_cache_plan_avoids_stashed_system_paths(void) {
    static const char helper[] =
        "/private/var/lib/s5lbox/cydia-index-cache-v2";
    rootfs_work_entry_t entries[8];
    rootfs_work_entry_t sentinel[7];
    memset(entries, 0, sizeof entries);
    memset(sentinel, 0xa5, sizeof sentinel);

    CHECK(vm_guest_install_build_test_bigboss_source_entries(
              sentinel, 7u, true) == 8u &&
          ((const unsigned char *)sentinel)[0] == 0xa5u,
          "a short cache-plan buffer was modified");
    size_t count = vm_guest_install_build_test_bigboss_source_entries(
        entries, 8u, true);
    CHECK(count == 8u, "cache plan has %zu entries, expected 8", count);

    const rootfs_work_entry_t *helper_entry = NULL;
    const rootfs_work_entry_t *plist_entry = NULL;
    for (size_t i = 0u; i < count; i++) {
        CHECK(entries[i].path != NULL,
              "cache plan entry %zu has no path", i);
        if (!entries[i].path) continue;
        CHECK(strcmp(entries[i].path, "/usr/libexec") != 0 &&
                  strncmp(entries[i].path, "/usr/libexec/", 13u) != 0,
              "cache plan still depends on a stashed path: %s",
              entries[i].path);
        if (strcmp(entries[i].path, helper) == 0)
            helper_entry = &entries[i];
        if (strcmp(entries[i].path,
                   "/System/Library/LaunchDaemons/"
                   "com.j0shua.s5lbox.cydia-index-cache-v2.plist") == 0)
            plist_entry = &entries[i];
    }
    CHECK(helper_entry && helper_entry->kind == ROOTFS_WORK_ENTRY_FILE &&
              helper_entry->permissions == 0755u &&
              helper_entry->content_size != 0u,
          "cache cleanup helper is absent from the private state directory");
    CHECK(plist_entry && plist_entry->kind == ROOTFS_WORK_ENTRY_FILE &&
              plist_entry->content_size != 0u &&
              strstr((const char *)plist_entry->content, helper) != NULL,
          "cache cleanup launchd job does not call the private helper");

    memset(entries, 0, sizeof entries);
    count = vm_guest_install_build_test_bigboss_source_entries(
        entries, 8u, false);
    CHECK(count == 7u,
          "cache plan without source creation has %zu entries, expected 7",
          count);
}

static void test_apt_trust_plan_is_exact_and_private(void) {
    rootfs_work_entry_t entries[8];
    rootfs_work_entry_t sentinel[7];
    memset(entries, 0, sizeof entries);
    memset(sentinel, 0xa5, sizeof sentinel);
    CHECK(vm_guest_install_build_test_apt_trust_entries(
              sentinel, 7u, true) == 8u &&
          ((const unsigned char *)sentinel)[0] == 0xa5u,
          "a short APT-trust plan buffer was modified");
    size_t count = vm_guest_install_build_test_apt_trust_entries(
        entries, 8u, true);
    CHECK(count == 8u, "APT-trust plan has %zu entries, expected 8", count);

    const rootfs_work_entry_t *keyring_entry = NULL;
    const rootfs_work_entry_t *helper_entry = NULL;
    const rootfs_work_entry_t *plist_entry = NULL;
    for (size_t i = 0u; i < count; i++) {
        CHECK(entries[i].path != NULL,
              "APT-trust plan entry %zu has no path", i);
        if (!entries[i].path) continue;
        CHECK(strcmp(entries[i].path, "/usr/libexec") != 0 &&
                  strncmp(entries[i].path, "/usr/libexec/", 13u) != 0,
              "APT-trust plan depends on a stashed path: %s",
              entries[i].path);
        if (strcmp(entries[i].path,
                   VM_GUEST_ROOTFS_TRUSTED_KEYRING_PATH) == 0)
            keyring_entry = &entries[i];
        if (strcmp(entries[i].path,
                   "/private/var/lib/s5lbox/apt-trust-v1") == 0)
            helper_entry = &entries[i];
        if (strcmp(entries[i].path,
                   "/System/Library/LaunchDaemons/"
                   "com.j0shua.s5lbox.apt-trust-v1.plist") == 0)
            plist_entry = &entries[i];
    }
    size_t expected_size = 0u;
    const uint8_t *expected =
        vm_guest_rootfs_bigboss_keyring(&expected_size);
    uint8_t digest[IOS3_SHA256_DIGEST_SIZE];
    static const uint8_t EXPECTED_DIGEST[IOS3_SHA256_DIGEST_SIZE] = {
        0x0du, 0x01u, 0xddu, 0x89u, 0x07u, 0x22u, 0xaeu, 0x15u,
        0x91u, 0x6cu, 0x77u, 0x30u, 0xe8u, 0x9au, 0xbau, 0x8cu,
        0x41u, 0xa8u, 0xceu, 0xaeu, 0xa5u, 0x02u, 0x93u, 0xf8u,
        0xbbu, 0x4au, 0xc9u, 0xd2u, 0x79u, 0x5fu, 0xecu, 0xb5u
    };
    CHECK(keyring_entry && expected && expected_size == 1164u &&
              keyring_entry->kind == ROOTFS_WORK_ENTRY_FILE &&
              keyring_entry->permissions == 0644u &&
              keyring_entry->content_size == expected_size &&
              memcmp(keyring_entry->content, expected, expected_size) == 0 &&
              ios3_sha256(keyring_entry->content,
                          keyring_entry->content_size, digest) &&
              memcmp(digest, EXPECTED_DIGEST, sizeof digest) == 0,
          "APT-trust plan does not carry the exact pinned BigBoss key");
    CHECK(helper_entry && helper_entry->kind == ROOTFS_WORK_ENTRY_FILE &&
              helper_entry->permissions == 0755u &&
              helper_entry->content_size != 0u,
          "APT-trust cache helper is absent from private state");
    CHECK(plist_entry && plist_entry->kind == ROOTFS_WORK_ENTRY_FILE &&
              plist_entry->content_size != 0u &&
              strstr((const char *)plist_entry->content,
                     "/private/var/lib/s5lbox/apt-trust-v1") != NULL,
          "APT-trust launchd job does not call the private helper");

    memset(entries, 0, sizeof entries);
    count = vm_guest_install_build_test_apt_trust_entries(
        entries, 8u, false);
    CHECK(count == 7u,
          "APT-trust plan without key creation has %zu entries, expected 7",
          count);
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
    CHECK(vm_guest_privilege_confirm(FIXTURE_DIR, digest, &transaction,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed,
          "could not seed an already-completed privilege migration: %s",
          detail);
    CHECK(vm_guest_sources_confirm(FIXTURE_DIR, digest, &transaction,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed,
          "could not seed an already-completed source migration: %s",
          detail);
    CHECK(vm_guest_sources_v2_confirm(FIXTURE_DIR, digest, &transaction,
                                      detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed,
          "could not seed an already-completed source-v2 migration: %s",
          detail);
    CHECK(vm_guest_apt_trust_confirm(FIXTURE_DIR, digest, &transaction,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed,
          "could not seed an already-completed APT-trust migration: %s",
          detail);
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

static void test_snapshot_blocks_free_count_recovery(void) {
    clear_fixture();
    CHECK(make_directory(FIXTURE_DIR),
          "could not create free-count snapshot fixture");
    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    char next[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, FIXTURE_DIR,
                    VM_GUEST_INSTALL_LIVE_FILE) &&
          write_bytes(live, "old-rootfs"),
          "could not seed free-count snapshot fixture");

    vm_guest_install_result_t transaction;
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    CHECK(vm_guest_install_prepare_stage(FIXTURE_DIR, &transaction,
                                         detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK &&
          vm_guest_install_stage_image_path(next, sizeof next, FIXTURE_DIR) &&
          write_bytes(next, "installed-rootfs"),
          "could not prepare free-count snapshot fixture: %s", detail);
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest);
    CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &transaction,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_privilege_confirm(FIXTURE_DIR, digest, &transaction,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_sources_confirm(FIXTURE_DIR, digest, &transaction,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_sources_v2_confirm(FIXTURE_DIR, digest, &transaction,
                                      detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_apt_trust_confirm(FIXTURE_DIR, digest, &transaction,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed,
          "could not commit free-count snapshot fixture: %s", detail);

    uint8_t fixture[HFS_FIXTURE_SIZE];
    uint8_t observed[HFS_FIXTURE_SIZE];
    make_clean_free_count_mismatch_fixture(fixture);
    CHECK(write_buffer(live, fixture, sizeof fixture),
          "could not write free-count HFS fixture");
    rootfs_work_result_t probe;
    CHECK(rootfs_work_validate_source(live, &probe) ==
              ROOTFS_WORK_HFS_INVALID &&
          probe.source_cleanly_unmounted &&
          probe.source_allocation_free_count_mismatch &&
          probe.source_allocation_bitmap_used == 6u &&
          probe.source_allocation_header_used == 7u,
          "free-count fixture did not expose structured mismatch evidence: %s",
          probe.detail);

    char snapshots[VM_GUEST_INSTALL_PATH_CAPACITY];
    char snapshot[VM_GUEST_INSTALL_PATH_CAPACITY];
    char meta[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(vm_snapshot_dir(FIXTURE_DIR, snapshots, sizeof snapshots) ==
              VM_SNAPSHOT_OK &&
          make_directory(snapshots) &&
          vm_snapshot_path(snapshots, "20260811T000000Z", false,
                           snapshot, sizeof snapshot) == VM_SNAPSHOT_OK &&
          make_directory(snapshot) &&
          vm_snapshot_member_path(snapshots, "20260811T000000Z", false,
                                  VM_SNAPSHOT_META_FILE,
                                  meta, sizeof meta) == VM_SNAPSHOT_OK,
          "could not prepare free-count historical snapshot");
    vm_snapshot_info_t info;
    memset(&info, 0, sizeof info);
    (void)snprintf(info.id, sizeof info.id, "%s", "20260811T000000Z");
    info.created_unix = UINT64_C(1786383600);
    info.retired = UINT64_C(7654321);
    char snapshot_detail[128];
    CHECK(vm_snapshot_meta_write(meta, &info, snapshot_detail,
                                 sizeof snapshot_detail) == VM_SNAPSHOT_OK,
          "could not write free-count snapshot metadata: %s", snapshot_detail);

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            FIXTURE_DIR, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    char recovery_stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(status == VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS &&
          result.historical_snapshots == 1u &&
          result.rootfs.source_allocation_free_count_mismatch &&
          !result.filesystem_repaired &&
          !result.filesystem_recovery_transaction.committed &&
          !progress.staging_seen &&
          vm_guest_recovery_stage_image_path(
              recovery_stage, sizeof recovery_stage, FIXTURE_DIR) &&
          !exists(recovery_stage),
          "snapshot gate did not precede free-count recovery: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    CHECK(read_buffer(live, observed, sizeof observed) &&
          memcmp(observed, fixture, sizeof fixture) == 0 &&
          file_size_or_zero(live) == HFS_FIXTURE_SIZE,
          "snapshot-gated free-count recovery changed the live disk");
}

static void test_committed_maintenance_cleanup_blocks_new_transaction(void) {
    clear_fixture();
    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    char next[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, FIXTURE_DIR,
                    VM_GUEST_INSTALL_LIVE_FILE) &&
          write_bytes(live, "old-rootfs"),
          "could not seed cleanup-residue fixture");
    vm_guest_install_result_t transaction;
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    CHECK(vm_guest_install_prepare_stage(FIXTURE_DIR, &transaction,
                                         detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK &&
          vm_guest_install_stage_image_path(next, sizeof next, FIXTURE_DIR) &&
          write_bytes(next, "installed-rootfs"),
          "could not prepare cleanup-residue fixture: %s", detail);
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest);
    CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &transaction,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_privilege_confirm(FIXTURE_DIR, digest, &transaction,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_sources_confirm(FIXTURE_DIR, digest, &transaction,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_sources_v2_confirm(FIXTURE_DIR, digest, &transaction,
                                      detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_apt_trust_confirm(FIXTURE_DIR, digest, &transaction,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          resize_sparse(live, VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES),
          "could not commit cleanup-residue fixture: %s", detail);

    char residue[VM_GUEST_INSTALL_PATH_CAPACITY];
    char residue_member[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(residue, sizeof residue, FIXTURE_DIR,
                    VM_GUEST_PRIVILEGE_BACKUP_FILE) &&
          make_directory(residue) &&
          join_path(residue_member, sizeof residue_member, residue,
                    "blocks-cleanup") &&
          write_bytes(residue_member, "not empty\n"),
          "could not seed committed cleanup residue");
    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            FIXTURE_DIR, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION &&
          result.already_installed && result.transaction.committed &&
          result.privilege_transaction.committed &&
          !result.privilege_transaction.cleanup_complete &&
          strstr(detail, "cleanup residue") != NULL &&
          !progress.staging_seen && exists(residue),
          "cleanup residue did not block new disk maintenance: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    (void)remove(residue_member);
    (void)remove_directory(residue);
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
    static const char *const MAINTENANCE_LEAVES[] = {
        VM_GUEST_STORAGE_BACKUP_FILE,
        VM_GUEST_STORAGE_STAGE_DIRECTORY,
        VM_GUEST_STORAGE_MARKER_FILE,
        VM_GUEST_STORAGE_MARKER_TMP,
        VM_GUEST_STORAGE_JOURNAL_FILE,
        VM_GUEST_STORAGE_JOURNAL_TMP,
        VM_GUEST_PRIVILEGE_BACKUP_FILE,
        VM_GUEST_PRIVILEGE_STAGE_DIRECTORY,
        VM_GUEST_PRIVILEGE_MARKER_FILE,
        VM_GUEST_PRIVILEGE_MARKER_TMP,
        VM_GUEST_PRIVILEGE_JOURNAL_FILE,
        VM_GUEST_PRIVILEGE_JOURNAL_TMP,
        VM_GUEST_SOURCES_BACKUP_FILE,
        VM_GUEST_SOURCES_STAGE_DIRECTORY,
        VM_GUEST_SOURCES_MARKER_FILE,
        VM_GUEST_SOURCES_MARKER_TMP,
        VM_GUEST_SOURCES_JOURNAL_FILE,
        VM_GUEST_SOURCES_JOURNAL_TMP,
        VM_GUEST_SOURCES_V2_BACKUP_FILE,
        VM_GUEST_SOURCES_V2_STAGE_DIRECTORY,
        VM_GUEST_SOURCES_V2_MARKER_FILE,
        VM_GUEST_SOURCES_V2_MARKER_TMP,
        VM_GUEST_SOURCES_V2_JOURNAL_FILE,
        VM_GUEST_SOURCES_V2_JOURNAL_TMP,
        VM_GUEST_APT_TRUST_BACKUP_FILE,
        VM_GUEST_APT_TRUST_STAGE_DIRECTORY,
        VM_GUEST_APT_TRUST_MARKER_FILE,
        VM_GUEST_APT_TRUST_MARKER_TMP,
        VM_GUEST_APT_TRUST_JOURNAL_FILE,
        VM_GUEST_APT_TRUST_JOURNAL_TMP,
        VM_GUEST_RECOVERY_BACKUP_FILE,
        VM_GUEST_RECOVERY_STAGE_DIRECTORY,
        VM_GUEST_RECOVERY_MARKER_FILE,
        VM_GUEST_RECOVERY_MARKER_TMP,
        VM_GUEST_RECOVERY_JOURNAL_FILE,
        VM_GUEST_RECOVERY_JOURNAL_TMP
    };
    for (size_t i = 0u;
         i < sizeof MAINTENANCE_LEAVES / sizeof MAINTENANCE_LEAVES[0]; i++) {
        char path[VM_GUEST_INSTALL_PATH_CAPACITY];
        CHECK(join_path(path, sizeof path, FIXTURE_DIR,
                        MAINTENANCE_LEAVES[i]) &&
              !exists(path),
              "dirty refusal left maintenance artifact %s",
              MAINTENANCE_LEAVES[i]);
    }
}

static void test_powered_off_checkpoint_allows_only_dirty_bit(void) {
    clear_fixture();
    CHECK(make_directory(FIXTURE_DIR),
          "could not create checkpoint-gate fixture");
    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    char next[VM_GUEST_INSTALL_PATH_CAPACITY];
    char marker[VM_GUEST_INSTALL_PATH_CAPACITY];
    char state[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, FIXTURE_DIR,
                    VM_GUEST_INSTALL_LIVE_FILE) &&
          join_path(marker, sizeof marker, FIXTURE_DIR,
                    VM_FW_BOOT_RESTORE_ONCE_FILE) &&
          join_path(state, sizeof state, FIXTURE_DIR,
                    VM_FW_BOOT_STATE_FILE) &&
          write_bytes(live, "old-rootfs"),
          "could not seed checkpoint-gate fixture");

    vm_guest_install_result_t transaction;
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    CHECK(vm_guest_install_prepare_stage(FIXTURE_DIR, &transaction,
                                         detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK &&
          vm_guest_install_stage_image_path(next, sizeof next, FIXTURE_DIR) &&
          write_bytes(next, "installed-rootfs"),
          "could not prepare checkpoint-gate fixture: %s", detail);
    uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE];
    fill_digest(digest);
    CHECK(vm_guest_install_publish(FIXTURE_DIR, digest, &transaction,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_privilege_confirm(FIXTURE_DIR, digest, &transaction,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_sources_confirm(FIXTURE_DIR, digest, &transaction,
                                   detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_sources_v2_confirm(FIXTURE_DIR, digest, &transaction,
                                      detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed &&
          vm_guest_apt_trust_confirm(FIXTURE_DIR, digest, &transaction,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && transaction.committed,
          "could not commit checkpoint-gate fixture: %s", detail);

    uint8_t fixture[HFS_FIXTURE_SIZE];
    make_dirty_hfs_fixture(fixture);
    CHECK(write_buffer(live, fixture, sizeof fixture),
          "could not write checkpoint-gate HFS fixture");
    CHECK(save_automatic_checkpoint(false, HFS_FIXTURE_SIZE,
                                    detail, sizeof detail),
          "could not save running checkpoint: %s", detail);

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            FIXTURE_DIR, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN &&
          !result.powered_off_checkpoint_witnessed &&
          !result.storage_upgraded && !progress.staging_seen &&
          exists(marker),
          "running checkpoint authorized dirty maintenance: %s / %s",
          vm_guest_install_build_status_text(status), detail);

    CHECK(save_automatic_checkpoint(true, HFS_FIXTURE_SIZE,
                                    detail, sizeof detail),
          "could not save powered-off checkpoint: %s", detail);
    memset(&progress, 0, sizeof progress);
    status = vm_guest_install_build_from_directory(
        FIXTURE_DIR, NULL, capture_progress, &progress,
        &result, detail, sizeof detail);
    uint32_t attributes = 0u;
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK &&
          result.powered_off_checkpoint_witnessed &&
          result.storage_upgraded && result.storage_transaction.committed &&
          result.rootfs.published && result.rootfs.source_unclean_accepted &&
          progress.staging_seen &&
          file_size_or_zero(live) >=
              VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES &&
          read_hfs_attributes(live, &attributes) &&
          (attributes & (1u << 8)) == 0u &&
          (attributes & (1u << 11)) == 0u &&
          !exists(marker) && exists(state),
          "powered-off checkpoint did not publish the preserved-dirty clone: %s / %s (attrs=0x%08x)",
          vm_guest_install_build_status_text(status), detail, attributes);

    uint64_t upgraded_size = file_size_or_zero(live);
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t retry;
    status = vm_guest_install_build_from_directory(
        FIXTURE_DIR, NULL, capture_progress, &progress,
        &retry, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && retry.already_installed &&
          !retry.powered_off_checkpoint_witnessed &&
          !retry.storage_upgraded && !progress.staging_seen &&
          retry.rootfs.final_size == 0u &&
          file_size_or_zero(live) == upgraded_size && !exists(marker),
          "checkpoint-gated maintenance retry was not idempotent: %s / %s",
          vm_guest_install_build_status_text(status), detail);
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

static void test_real_privilege_repair_when_supplied(void) {
    const char *machine = getenv("S5LBOX_EXISTING_PRIVILEGE_MACHINE_DIR");
    if (!machine || !*machine) {
        printf("real-cydia-privilege-repair SKIP (existing machine path unset)\n");
        return;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, machine,
                    VM_GUEST_INSTALL_LIVE_FILE),
          "real privilege-repair live path overflow");
    const char *base = getenv("S5LBOX_PRIVILEGE_REPAIR_BASE_IMAGE");
    const char *executable = getenv("S5LBOX_PRIVILEGE_REPAIR_EXECUTABLE");
    bool setup_any = (base && *base) || (executable && *executable);
    bool setup_all = base && *base && executable && *executable;
    uint8_t executable_sha256[IOS3_SHA256_DIGEST_SIZE];
    bool executable_sha256_valid = false;
    memset(executable_sha256, 0, sizeof executable_sha256);
    CHECK(!setup_any || setup_all,
          "repair fixture setup needs both base image and executable");
    if (setup_any && !setup_all) return;
    if (setup_all) {
        uint64_t executable_size = file_size_or_zero(executable);
        uint8_t *bytes = executable_size <= SIZE_MAX
            ? (uint8_t *)malloc((size_t)executable_size) : NULL;
        bool payload_ok = !exists(live) &&
            executable_size == UINT64_C(320704) && bytes &&
            read_buffer(executable, bytes, (size_t)executable_size);
        if (payload_ok)
            executable_sha256_valid = ios3_sha256(
                bytes, (size_t)executable_size, executable_sha256);
        CHECK(payload_ok,
              "could not prepare the exact Cydia executable fixture");
        CHECK(executable_sha256_valid,
              "could not hash the exact Cydia executable fixture");
        if (!payload_ok || !executable_sha256_valid) {
            free(bytes);
            return;
        }
        rootfs_work_entry_t entries[2];
        memset(entries, 0, sizeof entries);
        entries[0].kind = ROOTFS_WORK_ENTRY_DIRECTORY;
        entries[0].path = "/Applications/Cydia.app";
        entries[0].permissions = 0755u;
        entries[0].owner_id = 0u;
        entries[0].group_id = 0u;
        entries[0].existing_policy = ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
        entries[1].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[1].path = "/Applications/Cydia.app/Cydia_";
        entries[1].content = bytes;
        entries[1].content_size = (size_t)executable_size;
        entries[1].permissions = 0755u;
        entries[1].owner_id = 0u;
        entries[1].group_id = 0u;
        rootfs_work_options_t setup;
        memset(&setup, 0, sizeof setup);
        setup.preserve_fstab = true;
        setup.entries = entries;
        setup.entry_count = 2u;
        rootfs_work_result_t setup_result;
        rootfs_work_status_t setup_status = rootfs_work_create(
            base, live, &setup, &setup_result);
        free(bytes);
        CHECK(setup_status == ROOTFS_WORK_OK && setup_result.published &&
              setup_result.provision_entries >= 1u,
              "exact Cydia repair fixture refused: %s at %s (%s)",
              rootfs_work_status_name(setup_status),
              rootfs_work_stage_name(setup_result.stage),
              setup_result.detail);
        if (setup_status != ROOTFS_WORK_OK) return;
    }
    uint64_t before = file_size_or_zero(live);
    CHECK(before > 0u,
          "real privilege-repair source has no live disk (%llu bytes)",
          (unsigned long long)before);
    if (before == 0u) return;
    bool expect_storage_growth =
        before < VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES;

    uint8_t manifest[VM_GUEST_INSTALL_SHA256_SIZE];
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    CHECK(vm_guest_install_probe(machine, manifest, detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_VALID,
          "real privilege-repair source has no valid install marker: %s",
          detail);

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            machine, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && result.already_installed &&
          result.storage_upgraded == expect_storage_growth &&
          result.cydia_privileges_repaired &&
          result.cydia_privileges_verified &&
          result.cydia_sources_added &&
          result.cydia_sources_verified &&
          result.apt_trust_installed &&
          result.apt_trust_verified &&
          result.rootfs.file_repairs_applied == 1u &&
          result.rootfs.provision_entries >= 4u &&
          result.rootfs.provision_entries +
                  result.rootfs.provision_reused_entries >=
              9u &&
          result.rootfs.provision_entries +
                  result.rootfs.provision_reused_entries <=
              11u &&
          result.privilege_transaction.committed &&
          result.sources_v2_transaction.committed &&
          result.apt_trust_transaction.committed,
          "combined Cydia repair/source/trust migration refused or did not apply: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    uint64_t after = file_size_or_zero(live);
    CHECK(after == (expect_storage_growth
                        ? VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES : before),
          "combined repair published %llu bytes from a %llu-byte source",
          (unsigned long long)after, (unsigned long long)before);
    if (executable_sha256_valid) {
        rootfs_work_file_repair_t verify;
        memset(&verify, 0, sizeof verify);
        verify.path = "/Applications/Cydia.app/Cydia_";
        verify.expected_size = UINT64_C(320704);
        memcpy(verify.expected_sha256, executable_sha256,
               sizeof verify.expected_sha256);
        verify.expected_owner_id = 0u;
        verify.expected_group_id = 0u;
        verify.expected_permissions = 0755u;
        verify.desired_owner_id = 0u;
        verify.desired_group_id = 0u;
        verify.desired_permissions = 06755u;
        rootfs_work_file_repair_state_t state =
            ROOTFS_WORK_FILE_REPAIR_MISSING;
        rootfs_work_result_t probe;
        CHECK(rootfs_work_probe_file_repair(live, &verify, &state, &probe) ==
                  ROOTFS_WORK_OK &&
              state == ROOTFS_WORK_FILE_REPAIR_SATISFIED,
              "published exact Cydia executable is not root:root 06755: %s at %s (%s)",
              rootfs_work_status_name(probe.status),
              rootfs_work_stage_name(probe.stage), probe.detail);
    }
    static const uint8_t expected_source[] =
        VM_GUEST_ROOTFS_BIGBOSS_SOURCE_LINE;
    rootfs_work_file_repair_t source_probe;
    memset(&source_probe, 0, sizeof source_probe);
    source_probe.path = VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH;
    source_probe.expected_size = sizeof expected_source - 1u;
    source_probe.expected_permissions = 0644u;
    source_probe.desired_permissions = 0644u;
    rootfs_work_file_repair_state_t source_state =
        ROOTFS_WORK_FILE_REPAIR_MISSING;
    rootfs_work_result_t source_result;
    memset(&source_result, 0, sizeof source_result);
    CHECK(ios3_sha256(expected_source, sizeof expected_source - 1u,
                      source_probe.expected_sha256) &&
          rootfs_work_probe_file_repair(
              live, &source_probe, &source_state, &source_result) ==
              ROOTFS_WORK_OK &&
          source_state == ROOTFS_WORK_FILE_REPAIR_SATISFIED,
          "published BigBoss source is not exact root:root 0644 data: %s at %s (%s)",
          rootfs_work_status_name(source_result.status),
          rootfs_work_stage_name(source_result.stage),
          source_result.detail);
    vm_guest_install_result_t privilege;
    CHECK(vm_guest_privilege_recover(machine, &privilege,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && privilege.committed &&
          privilege.has_manifest &&
          memcmp(privilege.manifest_sha256, manifest, sizeof manifest) == 0,
          "real privilege-repair record is not recoverable: %s", detail);

    vm_guest_install_build_result_t retry;
    status = vm_guest_install_build_from_directory(
        machine, NULL, capture_progress, &progress,
        &retry, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && retry.already_installed &&
          !retry.storage_upgraded && !retry.cydia_privileges_repaired &&
          retry.cydia_privileges_verified && !retry.cydia_sources_added &&
          retry.cydia_sources_verified && !retry.apt_trust_installed &&
          retry.apt_trust_verified && retry.rootfs.final_size == 0u,
          "real privilege-repair retry rewrote the disk: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    printf("real-cydia-privilege-repair before=%llu after=%llu applied=%u\n",
           (unsigned long long)before, (unsigned long long)after,
           result.rootfs.file_repairs_applied);
}

static void test_real_cache_recovery_when_supplied(void) {
    const char *machine = getenv("S5LBOX_EXISTING_CACHE_MACHINE_DIR");
    if (!machine || !*machine) {
        printf("real-cydia-cache-recovery SKIP (existing machine path unset)\n");
        return;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, machine,
                    VM_GUEST_INSTALL_LIVE_FILE),
          "real cache-recovery live path overflow");
    uint64_t before = file_size_or_zero(live);
    CHECK(before > 0u, "real cache-recovery source has no live disk");
    if (before == 0u) return;
    bool expect_storage_growth =
        before < VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES;

    uint8_t manifest[VM_GUEST_INSTALL_SHA256_SIZE];
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    CHECK(vm_guest_install_probe(machine, manifest, detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_VALID,
          "real cache-recovery source has no valid install marker: %s",
          detail);

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            machine, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && result.already_installed &&
          result.storage_upgraded == expect_storage_growth &&
          result.cydia_privileges_verified &&
          result.cydia_sources_added && result.cydia_sources_verified &&
          result.apt_trust_installed && result.apt_trust_verified &&
          result.rootfs.provision_entries >= 4u &&
          result.rootfs.provision_entries +
                  result.rootfs.provision_reused_entries >= 9u &&
          result.rootfs.provision_entries +
                  result.rootfs.provision_reused_entries <= 11u &&
          result.sources_transaction.committed &&
          result.sources_v2_transaction.committed &&
          result.apt_trust_transaction.committed,
          "real cache recovery refused or omitted its payload: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    uint64_t after = file_size_or_zero(live);
    CHECK(after == (expect_storage_growth
                        ? VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES : before),
          "cache recovery published %llu bytes from a %llu-byte source",
          (unsigned long long)after, (unsigned long long)before);

    vm_guest_install_result_t recovered;
    CHECK(vm_guest_sources_v2_recover(machine, &recovered,
                                      detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && recovered.committed &&
          recovered.has_manifest &&
          memcmp(recovered.manifest_sha256, manifest, sizeof manifest) == 0,
          "cache-recovery transaction is not recoverable: %s", detail);

    vm_guest_install_build_result_t retry;
    status = vm_guest_install_build_from_directory(
        machine, NULL, capture_progress, &progress,
        &retry, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && retry.already_installed &&
          !retry.storage_upgraded && !retry.cydia_privileges_repaired &&
          retry.cydia_privileges_verified && !retry.cydia_sources_added &&
          retry.cydia_sources_verified && !retry.apt_trust_installed &&
          retry.apt_trust_verified && retry.rootfs.final_size == 0u,
          "cache-recovery retry rewrote the disk: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    printf("real-cydia-cache-recovery before=%llu after=%llu entries=%u\n",
           (unsigned long long)before, (unsigned long long)after,
           result.rootfs.provision_entries);
}

static void test_real_apt_trust_when_supplied(void) {
    const char *machine = getenv("S5LBOX_EXISTING_APT_TRUST_MACHINE_DIR");
    if (!machine || !*machine) {
        printf("real-apt-trust SKIP (existing machine path unset)\n");
        return;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, machine,
                    VM_GUEST_INSTALL_LIVE_FILE),
          "real APT-trust live path overflow");
    uint64_t before = file_size_or_zero(live);
    CHECK(before >= VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES,
          "real APT-trust source is smaller than the 2 GiB migration floor");
    if (before < VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES) return;

    uint8_t manifest[VM_GUEST_INSTALL_SHA256_SIZE];
    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    CHECK(vm_guest_install_probe(machine, manifest, detail, sizeof detail) ==
              VM_GUEST_INSTALL_PROBE_VALID,
          "real APT-trust source has no valid install marker: %s", detail);
    vm_guest_install_result_t before_transaction;
    CHECK(vm_guest_apt_trust_recover(
              machine, &before_transaction, detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK,
          "real APT-trust transaction could not be recovered: %s", detail);

    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            machine, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && result.already_installed &&
          !result.storage_upgraded && !result.cydia_privileges_repaired &&
          !result.cydia_sources_added && result.apt_trust_verified &&
          result.apt_trust_transaction.committed &&
          file_size_or_zero(live) == before,
          "real APT-trust migration refused or changed unrelated state: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    if (!before_transaction.committed) {
        CHECK(result.rootfs.published &&
              result.rootfs.provision_entries >= 2u &&
              result.rootfs.provision_entries +
                      result.rootfs.provision_reused_entries == 8u,
              "real APT-trust migration omitted its bounded payload: %s",
              result.rootfs.detail);
    }
    rootfs_work_result_t keyring_probe;
    memset(&keyring_probe, 0, sizeof keyring_probe);
    CHECK(probe_exact_bigboss_keyring(live, &keyring_probe),
          "published trusted.gpg is not the exact pinned BigBoss key: %s at %s (%s)",
          rootfs_work_status_name(keyring_probe.status),
          rootfs_work_stage_name(keyring_probe.stage), keyring_probe.detail);

    vm_guest_install_result_t recovered;
    CHECK(vm_guest_apt_trust_recover(machine, &recovered,
                                     detail, sizeof detail) ==
              VM_GUEST_INSTALL_OK && recovered.committed &&
          recovered.cleanup_complete && recovered.has_manifest &&
          memcmp(recovered.manifest_sha256, manifest, sizeof manifest) == 0,
          "real APT-trust record is not recoverable: %s", detail);

    vm_guest_install_build_result_t retry;
    memset(&progress, 0, sizeof progress);
    status = vm_guest_install_build_from_directory(
        machine, NULL, capture_progress, &progress,
        &retry, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && retry.already_installed &&
          !retry.storage_upgraded && !retry.cydia_privileges_repaired &&
          !retry.cydia_sources_added && !retry.apt_trust_installed &&
          retry.apt_trust_verified && retry.rootfs.final_size == 0u &&
          file_size_or_zero(live) == before,
          "real APT-trust retry rewrote the disk: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    printf("real-apt-trust before=%llu after=%llu entries=%u\n",
           (unsigned long long)before,
           (unsigned long long)file_size_or_zero(live),
           result.rootfs.provision_entries);
}

static void test_real_allocation_recovery_when_supplied(void) {
    const char *machine = getenv("S5LBOX_FREE_COUNT_RECOVERY_MACHINE_DIR");
    if (!machine || !*machine) {
        printf("real-allocation-recovery SKIP (disposable machine path unset)\n");
        return;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    CHECK(join_path(live, sizeof live, machine,
                    VM_GUEST_INSTALL_LIVE_FILE),
          "real free-count recovery live path overflow");
    uint64_t size = file_size_or_zero(live);
    CHECK(size > 2u * HFS_VOLUME_HEADER_OFFSET,
          "real free-count recovery source has no usable live disk");
    if (size <= 2u * HFS_VOLUME_HEADER_OFFSET) return;

    uint32_t block_size = 0u, total_blocks = 0u, header_free_blocks = 0u;
    CHECK(read_hfs_geometry(live, &block_size, &total_blocks,
                            &header_free_blocks) &&
          header_free_blocks > 0u,
          "real free-count recovery source has invalid HFS geometry");
    if (block_size == 0u || total_blocks == 0u ||
        header_free_blocks == 0u) return;
    rootfs_work_result_t before;
    rootfs_work_status_t before_status =
        rootfs_work_validate_source(live, &before);
    if (before_status == ROOTFS_WORK_OK) {
        CHECK(write_hfs_free_blocks(live, size, header_free_blocks - 1u),
              "could not inject the disposable free-count mismatch");
    } else if (before_status == ROOTFS_WORK_HFS_INVALID &&
               before.source_cleanly_unmounted &&
               before.source_allocation_free_count_mismatch &&
               before.source_allocation_header_used ==
                   before.source_allocation_bitmap_used + 1u) {
        /* A timed-out external harness may have stopped after injecting the
         * mismatch but before publication. Continue the same transaction test
         * instead of requiring a fresh 2 GiB copy. */
    } else {
        CHECK(0,
              "real free-count source was not a valid or exact stale-count disk: %s at %s (%s)",
              rootfs_work_status_name(before.status),
              rootfs_work_stage_name(before.stage), before.detail);
        return;
    }
    rootfs_work_result_t mismatch;
    uint32_t mismatch_free_blocks = 0u;
    CHECK(rootfs_work_validate_source(live, &mismatch) ==
              ROOTFS_WORK_HFS_INVALID &&
          mismatch.source_cleanly_unmounted &&
          mismatch.source_allocation_free_count_mismatch &&
          mismatch.source_allocation_header_used ==
              mismatch.source_allocation_bitmap_used + 1u &&
          read_hfs_geometry(live, &block_size, &total_blocks,
                            &mismatch_free_blocks),
          "real free-count mismatch was not diagnosed structurally: %s",
          mismatch.detail);

    char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    progress_log_t progress;
    memset(&progress, 0, sizeof progress);
    vm_guest_install_build_result_t result;
    vm_guest_install_build_status_t status =
        vm_guest_install_build_from_directory(
            machine, NULL, capture_progress, &progress,
            &result, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && result.already_installed &&
          result.filesystem_repaired &&
          result.filesystem_recovery_transaction.committed &&
          result.filesystem_recovery_transaction.cleanup_complete &&
          vm_guest_install_build_test_allocation_repair_proven(
              &mismatch, &result.filesystem_recovery, size) &&
          !result.storage_upgraded && !result.cydia_privileges_repaired &&
          !result.cydia_sources_added,
          "real allocation recovery refused or did not publish its proven repair: %s / %s",
          vm_guest_install_build_status_text(status), detail);

    rootfs_work_result_t verified;
    uint32_t repaired_block_size = 0u, repaired_total_blocks = 0u,
             repaired_free_blocks = 0u;
    uint32_t expected_free_blocks = mismatch_free_blocks +
        (result.filesystem_recovery.allocation_free_count_repaired != 0u
             ? 1u : 0u);
    CHECK(rootfs_work_validate_source(live, &verified) == ROOTFS_WORK_OK &&
          read_hfs_geometry(live, &repaired_block_size,
                            &repaired_total_blocks,
                            &repaired_free_blocks) &&
          file_size_or_zero(live) == size &&
          repaired_block_size == block_size &&
          repaired_total_blocks == total_blocks &&
          repaired_free_blocks == expected_free_blocks,
          "published allocation repair did not restore exact geometry: %s",
          verified.detail);

    vm_guest_install_build_result_t retry;
    memset(&progress, 0, sizeof progress);
    status = vm_guest_install_build_from_directory(
        machine, NULL, capture_progress, &progress,
        &retry, detail, sizeof detail);
    CHECK(status == VM_GUEST_INSTALL_BUILD_OK && retry.already_installed &&
          !retry.filesystem_repaired && !retry.storage_upgraded &&
          !retry.cydia_privileges_repaired && !retry.cydia_sources_added &&
          file_size_or_zero(live) == size,
          "real free-count recovery retry was not idempotent: %s / %s",
          vm_guest_install_build_status_text(status), detail);
    printf("real-allocation-recovery size=%llu used=%u header=%u bits=%u free=%u\n",
           (unsigned long long)size,
           mismatch.source_allocation_bitmap_used,
           mismatch.source_allocation_header_used,
           result.filesystem_recovery.allocation_bits_repaired,
           result.filesystem_recovery.allocation_free_count_repaired);
}

int main(void) {
    printf("== guest install builder ==\n");
    if (!make_directory(FIXTURE_DIR)) {
        printf("could not create %s\n", FIXTURE_DIR);
        return 2;
    }
    test_argument_and_package_refusals();
    test_allocation_repair_publication_proof();
    test_bigboss_cache_plan_avoids_stashed_system_paths();
    test_apt_trust_plan_is_exact_and_private();
    test_historical_snapshot_gate();
    test_existing_install_is_idempotent();
    test_snapshot_blocks_free_count_recovery();
    test_committed_maintenance_cleanup_blocks_new_transaction();
    test_dirty_existing_install_refuses_before_stage();
    test_powered_off_checkpoint_allows_only_dirty_bit();
    test_real_storage_upgrade_when_supplied();
    test_real_privilege_repair_when_supplied();
    test_real_cache_recovery_when_supplied();
    test_real_apt_trust_when_supplied();
    test_real_allocation_recovery_when_supplied();
    test_real_build_when_supplied();
    clear_fixture();
    (void)remove_directory(FIXTURE_DIR);
    printf("== guest install builder: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
