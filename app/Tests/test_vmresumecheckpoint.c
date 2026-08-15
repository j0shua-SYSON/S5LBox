/*
 * S5LBox -- automatic resume checkpoint transaction tests.
 *
 * These tests need no firmware. They pin the property the UI depends on: the
 * restore marker appears only after both complete payloads are installed, and
 * a failed replacement leaves no request that startup could mistake for a
 * valid resume.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareBoot.h"
#include "VMResumeCheckpoint.h"
#include "snapshot.h"
#include "soc.h"

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

#define FIXTURE_DIR "vmresumecheckpoint-fixture"
#define TEST_RAM_SIZE (1u << 20)

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

static bool path_for(char *out, size_t capacity, const char *name) {
    int written = snprintf(out, capacity, "%s/%s", FIXTURE_DIR, name);
    return written >= 0 && (size_t)written < capacity;
}

static uint64_t file_bytes(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) return 0u;
    return (uint64_t)st.st_size;
}

static void remove_checkpoint_files(void) {
    static const char *const names[] = {
        VM_FW_BOOT_STATE_FILE,
        VM_FW_BOOT_STATE_MD_FILE,
        VM_FW_BOOT_STATE_TMP,
        VM_FW_BOOT_STATE_MD_TMP,
        VM_FW_BOOT_RESTORE_ONCE_FILE,
        VM_FW_BOOT_RESTORE_ONCE_TMP,
    };
    char path[1200];
    for (size_t i = 0u; i < sizeof names / sizeof names[0]; i++) {
        if (path_for(path, sizeof path, names[i])) (void)remove(path);
    }
}

static bool write_marker(void) {
    char path[1200];
    if (!path_for(path, sizeof path, VM_FW_BOOT_RESTORE_ONCE_FILE)) return false;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite("old\n", 1u, 4u, file) == 4u;
    if (fclose(file) != 0) ok = false;
    return ok;
}

int main(void) {
#ifdef _WIN32
    if (_mkdir(FIXTURE_DIR) != 0 && errno != EEXIST) {
#else
    if (mkdir(FIXTURE_DIR, 0700) != 0 && errno != EEXIST) {
#endif
        printf("could not create %s\n", FIXTURE_DIR);
        return 2;
    }
    remove_checkpoint_files();

    s5l8900_t source;
    s5l8900_t restored;
    CHECK(s5l8900_init(&source, 0u, TEST_RAM_SIZE), "source init failed");
    CHECK(s5l8900_init(&restored, 0u, TEST_RAM_SIZE), "restore init failed");

    source.cpu.r[0] = 0x13579bdfu;
    source.cpu.r[15] = 0x00004000u;
    source.cpu.cycles = UINT64_C(123456789);
    source.ram[17] = 0xa5u;

    external_md_sidecar_t sidecar;
    memset(&sidecar, 0, sizeof sidecar);
    sidecar.magic = EXTERNAL_MD_SIDECAR_MAGIC;
    sidecar.version = EXTERNAL_MD_SIDECAR_VERSION;
    sidecar.media_size = UINT64_C(4096);
    sidecar.image_bytes = UINT64_C(4096);
    sidecar.strategy_stats.successful_reads = 7u;
    sidecar.raw_stats.successful_reads = 11u;
    sidecar.guard_tail[3] = 0x6cu;

    char detail[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
    CHECK(vm_resume_checkpoint_save(&source, &sidecar, FIXTURE_DIR,
                                    detail, sizeof detail),
          "first save failed: %s", detail);

    char state[1200], bridge[1200], marker[1200];
    char state_partial[1200], bridge_partial[1200], marker_partial[1200];
    CHECK(path_for(state, sizeof state, VM_FW_BOOT_STATE_FILE), "state path");
    CHECK(path_for(bridge, sizeof bridge, VM_FW_BOOT_STATE_MD_FILE), "bridge path");
    CHECK(path_for(marker, sizeof marker, VM_FW_BOOT_RESTORE_ONCE_FILE),
          "marker path");
    CHECK(path_for(state_partial, sizeof state_partial, VM_FW_BOOT_STATE_TMP),
          "state partial path");
    CHECK(path_for(bridge_partial, sizeof bridge_partial,
                   VM_FW_BOOT_STATE_MD_TMP), "bridge partial path");
    CHECK(path_for(marker_partial, sizeof marker_partial,
                   VM_FW_BOOT_RESTORE_ONCE_TMP), "marker partial path");

    CHECK(file_bytes(state) > 0u, "machine state was not installed");
    CHECK(file_bytes(bridge) == sizeof sidecar,
          "bridge sidecar is %llu bytes, expected %zu",
          (unsigned long long)file_bytes(bridge), sizeof sidecar);
    CHECK(file_bytes(marker) > 0u, "non-empty restore marker was not installed");
    CHECK(file_bytes(state_partial) == 0u, "state partial survived success");
    CHECK(file_bytes(bridge_partial) == 0u, "bridge partial survived success");
    CHECK(file_bytes(marker_partial) == 0u, "marker partial survived success");

    uint64_t running_state_bytes = file_bytes(state);
    uint64_t running_bridge_bytes = file_bytes(bridge);
    uint64_t running_marker_bytes = file_bytes(marker);
    CHECK(vm_resume_checkpoint_probe_state(
              FIXTURE_DIR, sidecar.media_size, 0u, TEST_RAM_SIZE,
              detail, sizeof detail) == VM_RESUME_CHECKPOINT_RUNNING,
          "running checkpoint probe failed: %s", detail);
    CHECK(file_bytes(state) == running_state_bytes &&
          file_bytes(bridge) == running_bridge_bytes &&
          file_bytes(marker) == running_marker_bytes,
          "running checkpoint probe consumed or changed its transaction");

    snapshot_status_t loaded = snapshot_load(&restored, state);
    CHECK(loaded == SNAP_OK, "saved state did not load: %s",
          snapshot_strerror(loaded));
    CHECK(restored.cpu.r[0] == source.cpu.r[0] &&
          restored.cpu.r[15] == source.cpu.r[15] &&
          restored.cpu.cycles == source.cpu.cycles &&
          restored.ram[17] == source.ram[17],
          "restored machine does not match the saved boundary");

    external_md_sidecar_t read_sidecar;
    memset(&read_sidecar, 0, sizeof read_sidecar);
    FILE *bridge_file = fopen(bridge, "rb");
    bool bridge_read = bridge_file &&
        fread(&read_sidecar, sizeof read_sidecar, 1u, bridge_file) == 1u;
    if (bridge_file) fclose(bridge_file);
    CHECK(bridge_read &&
          memcmp(&read_sidecar, &sidecar, sizeof sidecar) == 0,
          "installed bridge sidecar changed bytes");

    /* A second save replaces the single resume point, rather than keeping a
     * stale first marker or restoring the first machine by accident. */
    (void)remove(marker);
    source.cpu.r[0] = 0x2468ace0u;
    CHECK(vm_resume_checkpoint_save(&source, &sidecar, FIXTURE_DIR,
                                    detail, sizeof detail),
          "replacement save failed: %s", detail);
    CHECK(snapshot_load(&restored, state) == SNAP_OK,
          "replacement state did not load");
    CHECK(restored.cpu.r[0] == 0x2468ace0u,
          "replacement left the first checkpoint installed");

    source.pmu.written[PCF50635_OOCSHDWN] = 1u;
    source.pmu.regs[PCF50635_OOCSHDWN] =
        PCF50635_OOCSHDWN_GO_STANDBY;
    CHECK(vm_resume_checkpoint_save(&source, &sidecar, FIXTURE_DIR,
                                    detail, sizeof detail),
          "powered-off save failed: %s", detail);
    uint64_t powered_state_bytes = file_bytes(state);
    uint64_t powered_bridge_bytes = file_bytes(bridge);
    uint64_t powered_marker_bytes = file_bytes(marker);
    CHECK(vm_resume_checkpoint_probe_state(
              FIXTURE_DIR, sidecar.media_size, 0u, TEST_RAM_SIZE,
              detail, sizeof detail) == VM_RESUME_CHECKPOINT_POWERED_OFF,
          "powered-off checkpoint probe failed: %s", detail);
    CHECK(file_bytes(state) == powered_state_bytes &&
          file_bytes(bridge) == powered_bridge_bytes &&
          file_bytes(marker) == powered_marker_bytes,
          "powered-off checkpoint probe consumed or changed its transaction");
    CHECK(vm_resume_checkpoint_probe_state(
              FIXTURE_DIR, sidecar.media_size + 1u, 0u, TEST_RAM_SIZE,
              NULL, 0u) == VM_RESUME_CHECKPOINT_INVALID,
          "wrong-disk checkpoint was accepted without a detail buffer");
    CHECK(write_marker(), "could not tamper with the restore marker");
    CHECK(vm_resume_checkpoint_probe_state(
              FIXTURE_DIR, sidecar.media_size, 0u, TEST_RAM_SIZE,
              detail, sizeof detail) == VM_RESUME_CHECKPOINT_INVALID,
          "inexact restore marker was accepted: %s", detail);
    CHECK(remove(marker) == 0, "could not remove restore marker");
    CHECK(vm_resume_checkpoint_probe_state(
              FIXTURE_DIR, sidecar.media_size, 0u, TEST_RAM_SIZE,
              detail, sizeof detail) == VM_RESUME_CHECKPOINT_ABSENT,
          "missing restore marker was not absent: %s", detail);
    CHECK(vm_resume_checkpoint_save(&source, &sidecar, FIXTURE_DIR,
                                    detail, sizeof detail),
          "could not restore a valid powered-off transaction: %s", detail);

    /* The transaction invalidates the request BEFORE writing. Make snapshot
     * validation fail after that point and prove startup has no marker with
     * which it could consume either the old or partial payload pair. */
    CHECK(write_marker(), "could not seed the prior marker");
    uint32_t ram_size = source.ram_size;
    source.ram_size = 0u;
    CHECK(!vm_resume_checkpoint_save(&source, &sidecar, FIXTURE_DIR,
                                     detail, sizeof detail),
          "an invalid machine produced a checkpoint");
    source.ram_size = ram_size;
    CHECK(file_bytes(marker) == 0u,
          "failed save left a restore marker armed");
    CHECK(detail[0] != '\0', "failed save did not explain itself");

    s5l8900_free(&source);
    s5l8900_free(&restored);
    remove_checkpoint_files();
#ifdef _WIN32
    (void)_rmdir(FIXTURE_DIR);
#else
    (void)rmdir(FIXTURE_DIR);
#endif

    printf("== automatic resume checkpoint: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
