/* See VMFirmwareBoot.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMFirmwareBoot.h"

#include "file_block.h"
#include "md_snapshot.h"
#include "snapshot.h"
#include "VMFrameTelemetry.h"
#include "VMGuestInstall.h"
#include "VMNetworkSession.h"
#include "VMSnapshotCow.h"
#include "VMFirmwareHLE.h"
#include "VMResumeCheckpoint.h"
#include "ios3_bringup_gate.h"
#include "rootfs_work.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(S5LBOX_IOS_WFI_REALTIME_PACING)
#include <errno.h>
#endif
#if defined(S5LBOX_IOS_WFI_REALTIME_PACING) || \
    defined(S5LBOX_IOS_ACTIVE_REALTIME_CLOCK)
#include <time.h>
#endif

struct vm_firmware_boot {
    file_block_t     *media;
    s5l_bringup_md_t *bridges;   /* ~300 KiB; heap, never a thread stack */
    /*
     * Copy-on-write recording, armed only when a snapshot exists to
     * protect. A machine with no saved states pays nothing: no overlay is
     * opened, no block is read before it is written, and the descriptor
     * handed to the bringup is the file adapter's own.
     */
    vm_cow_t         *cow;
    char              overlay[VM_FW_BOOT_PATH_CAPACITY];
    bool              overlay_armed;
    char              work_directory[VM_FW_BOOT_PATH_CAPACITY];
    uint64_t          media_size;
    vm_network_session_t *network;
#if defined(S5LBOX_IOS3_HLE_EXPERIMENT)
    /* Pointer identity only. VMEngine frees the machine before this owner;
     * vm_firmware_hle_release() deliberately never dereferences it. */
    const s5l8900_t   *hle_machine;
#endif
};

#if defined(S5LBOX_IOS_WFI_REALTIME_PACING)
/*
 * The portable machine names the exact idle interval; the iOS frontend owns
 * the wall-clock wait. nanosleep() uses no private API, entitlement or
 * executable-memory facility, and it runs only on the emulator thread.
 *
 * EINTR is not success. Advancing the whole guest interval after a shortened
 * host wait would reintroduce the clock acceleration this callback exists to
 * prevent, so resume the remainder and report false on every other error.
 */
static bool vm_firmware_wfi_sleep(void *ctx, uint64_t nanoseconds) {
    (void)ctx;
    struct timespec request;
    request.tv_sec = (time_t)(nanoseconds / UINT64_C(1000000000));
    request.tv_nsec = (long)(nanoseconds % UINT64_C(1000000000));

    for (;;) {
        struct timespec remaining = {0};
        if (nanosleep(&request, &remaining) == 0) return true;
        if (errno != EINTR) return false;
        request = remaining;
    }
}
#endif

#if defined(S5LBOX_IOS_ACTIVE_REALTIME_CLOCK)
/*
 * Active guest time is a property of the emulated board, but the portable
 * core cannot select a host clock. CLOCK_MONOTONIC is public, unaffected by
 * wall-clock changes, and available on every iOS version this app supports.
 */
static bool vm_firmware_active_now(void *ctx, uint64_t *nanoseconds) {
    (void)ctx;
    if (!nanoseconds) return false;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L)
        return false;

    uint64_t seconds = (uint64_t)now.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)now.tv_nsec) /
                      UINT64_C(1000000000))
        return false;
    *nanoseconds = seconds * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return true;
}
#endif

/* -------------------------------------------------------------------------- */

static bool join_path(char *out, size_t capacity, const char *directory,
                      const char *name) {
    if (!out || !capacity || !directory || !name) return false;
    size_t n = strlen(directory);
    /* Both separators are accepted so the same code works against a Windows
     * host path in the test suite and a POSIX one on the phone. */
    bool has_separator = n > 0u && (directory[n - 1u] == '/' ||
                                    directory[n - 1u] == '\\');
    int written = snprintf(out, capacity, "%s%s%s", directory,
                           has_separator ? "" : "/", name);
    return written > 0 && (size_t)written < capacity;
}

/* Size of a regular file, or 0 for absent/empty/unreadable. Deliberately does
 * not distinguish those: to this layer they are the same answer, "you cannot
 * boot this", and a size is all the caller shows. */
static uint64_t file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0u;
    int64_t size = 0;
#ifdef _WIN32
    /* MinGW's C `long` is 32-bit even on a 64-bit host. The app now supports
     * an exact 2 GiB work image, whose first unrepresentable byte made ftell()
     * return -1 and the Windows seam test call a prepared disk absent. */
    if (_fseeki64(f, 0, SEEK_END) == 0) size = _ftelli64(f);
#else
    /* iOS is LP64, so the ordinary C interface covers every supported image. */
    if (fseek(f, 0, SEEK_END) == 0) size = (int64_t)ftell(f);
#endif
    fclose(f);
    return size > 0 ? (uint64_t)size : 0u;
}

static uint8_t *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buffer = (uint8_t *)malloc((size_t)size);
    if (!buffer) { fclose(f); return NULL; }
    size_t got = fread(buffer, 1u, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buffer); return NULL; }
    *out_size = got;
    return buffer;
}

#if defined(S5LBOX_IOS_ACTIVE_REALTIME_CLOCK)
/* Parse the tiny same-binary calibration control without accepting signs,
 * overflow, suffixes or locale-specific whitespace. Empty/absent means the
 * conservative product default; malformed nonempty input fails the boot so a
 * physical result can never be attributed to a budget that was not applied. */
static bool read_active_clock_budget_control(const char *path,
                                             uint32_t *budget,
                                             bool *controlled) {
    if (!path || !budget || !controlled) return false;
    *budget = S5L8900_ACTIVE_CLOCK_DEFAULT_WORK_TICKS;
    *controlled = false;

    uint64_t file_bytes = file_size(path);
    if (file_bytes == 0u) return true;
    if (file_bytes > 32u) return false;

    size_t size = 0u;
    uint8_t *bytes = slurp(path, &size);
    if (!bytes || size == 0u) {
        free(bytes);
        return false;
    }

    size_t i = 0u;
    while (i < size && (bytes[i] == ' ' || bytes[i] == '\t' ||
                        bytes[i] == '\r' || bytes[i] == '\n'))
        i++;
    bool have_digit = false;
    uint32_t value = 0u;
    while (i < size && bytes[i] >= '0' && bytes[i] <= '9') {
        have_digit = true;
        unsigned digit = (unsigned)(bytes[i] - '0');
        if (value > (S5L8900_ACTIVE_CLOCK_MAX_WORK_TICKS - digit) / 10u) {
            free(bytes);
            return false;
        }
        value = value * 10u + digit;
        i++;
    }
    while (i < size && (bytes[i] == ' ' || bytes[i] == '\t' ||
                        bytes[i] == '\r' || bytes[i] == '\n'))
        i++;
    bool valid = have_digit && i == size && value > 0u &&
                 value <= S5L8900_ACTIVE_CLOCK_MAX_WORK_TICKS;
    free(bytes);
    if (!valid) return false;

    *budget = value;
    *controlled = true;
    return true;
}
#endif

static void set_detail(char *out, size_t capacity, const char *text) {
    if (!out || !capacity) return;
    (void)snprintf(out, capacity, "%s", text ? text : "");
    out[capacity - 1u] = '\0';
}

static bool write_ppp_marker(const vm_firmware_boot_paths_t *paths,
                             bool enabled, char *detail,
                             size_t detail_capacity) {
    char marker[VM_FW_BOOT_PATH_CAPACITY + 64u];
    char temporary[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!paths ||
        !join_path(marker, sizeof marker, paths->work,
                   VM_FW_BOOT_PPP_FILE) ||
        !join_path(temporary, sizeof temporary, paths->work,
                   VM_FW_BOOT_PPP_TMP)) {
        set_detail(detail, detail_capacity,
                   "The guest-network record path is too long to use.");
        return false;
    }

    /* This is called only while making a new/incomplete work image. A stale
     * record from an interrupted attempt must follow the current request. */
    (void)remove(temporary);
    (void)remove(marker);
    if (!enabled) return true;

    static const char contents[] = "s5lbox-ppp-provision 1\n";
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        set_detail(detail, detail_capacity,
                   "The guest-network record could not be created.");
        return false;
    }
    bool ok = fwrite(contents, 1u, sizeof contents - 1u, file) ==
              sizeof contents - 1u;
    if (fflush(file) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        (void)remove(temporary);
        set_detail(detail, detail_capacity,
                   "The guest-network record could not be written.");
        return false;
    }
    if (rename(temporary, marker) != 0) {
        (void)remove(temporary);
        set_detail(detail, detail_capacity,
                   "The guest-network record could not be published.");
        return false;
    }
    return true;
}

/*
 * Restore a checkpoint only when an explicit, non-empty one-shot marker is
 * present.  The state pair on its own is inert: that prevents an interrupted
 * copy, an obsolete experiment, or a future snapshot-list entry from silently
 * replacing a cold boot.
 *
 * The caller has already opened the live work image and completed bring-up.
 * That establishes the correct RAM geometry, bridge callbacks and host-owned
 * pointers.  snapshot_load() then replaces only their contents, exactly as the
 * desktop harness does.  The marker is consumed by the caller only after every
 * post-restore hook has also succeeded, so a failed start remains retryable.
 */
static bool restore_once(vm_firmware_boot_t *boot, s5l8900_t *machine,
                         const vm_firmware_boot_paths_t *paths,
                         uint64_t work_size, bool *restored,
                         char *marker_path, size_t marker_capacity,
                         char *detail, size_t detail_capacity) {
    if (restored) *restored = false;
    if (marker_path && marker_capacity) marker_path[0] = '\0';
    if (!boot || !boot->bridges || !machine || !paths || !restored ||
        !marker_path || !marker_capacity) {
        set_detail(detail, detail_capacity,
                   "The saved-state restore request was incomplete.");
        return false;
    }

    char state_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    char md_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(marker_path, marker_capacity, paths->work,
                   VM_FW_BOOT_RESTORE_ONCE_FILE) ||
        !join_path(state_path, sizeof state_path, paths->work,
                   VM_FW_BOOT_STATE_FILE) ||
        !join_path(md_path, sizeof md_path, paths->work,
                   VM_FW_BOOT_STATE_MD_FILE)) {
        set_detail(detail, detail_capacity,
                   "The saved-state path is too long to use.");
        return false;
    }

    if (file_size(marker_path) == 0u) {
        marker_path[0] = '\0';
        return true;
    }

    uint64_t state_bytes = file_size(state_path);
    uint64_t md_bytes = file_size(md_path);
    if (state_bytes == 0u || md_bytes != (uint64_t)sizeof(external_md_sidecar_t)) {
        (void)snprintf(detail, detail_capacity,
                       "The one-shot restore needs a complete %s and an exact "
                       "%zu-byte %s; found %llu and %llu bytes.",
                       VM_FW_BOOT_STATE_FILE, sizeof(external_md_sidecar_t),
                       VM_FW_BOOT_STATE_MD_FILE,
                       (unsigned long long)state_bytes,
                       (unsigned long long)md_bytes);
        if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
        return false;
    }

    external_md_sidecar_t sidecar = {0};
    FILE *md = fopen(md_path, "rb");
    bool read_ok = md && fread(&sidecar, sizeof sidecar, 1u, md) == 1u;
    if (md) fclose(md);
    if (!read_ok) {
        set_detail(detail, detail_capacity,
                   "The saved memory-disk bridge state could not be read.");
        return false;
    }
    if (sidecar.magic != EXTERNAL_MD_SIDECAR_MAGIC ||
        sidecar.version != EXTERNAL_MD_SIDECAR_VERSION) {
        (void)snprintf(detail, detail_capacity,
                       "The saved memory-disk bridge state is format %08x/%u; "
                       "this build requires %08x/%u.",
                       sidecar.magic, sidecar.version,
                       EXTERNAL_MD_SIDECAR_MAGIC,
                       EXTERNAL_MD_SIDECAR_VERSION);
        if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
        return false;
    }
    if (sidecar.media_size != work_size || sidecar.image_bytes != work_size) {
        (void)snprintf(detail, detail_capacity,
                       "The saved state describes a %llu-byte disk image "
                       "(%llu bytes copied), but this machine has %llu bytes.",
                       (unsigned long long)sidecar.media_size,
                       (unsigned long long)sidecar.image_bytes,
                       (unsigned long long)work_size);
        if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
        return false;
    }
    if (!boot->bridges->installed) {
        set_detail(detail, detail_capacity,
                   "The saved state needs the memory-disk bridges, but bring-up "
                   "did not install them.");
        return false;
    }

    memcpy(boot->bridges->raw.guard_tail, sidecar.guard_tail,
           sizeof sidecar.guard_tail);
    boot->bridges->raw.stats = sidecar.raw_stats;
    boot->bridges->strategy.stats = sidecar.strategy_stats;

    snapshot_status_t status = snapshot_load(machine, state_path);
    if (status != SNAP_OK) {
        (void)snprintf(detail, detail_capacity,
                       "The saved machine state was refused: %s.",
                       snapshot_strerror(status));
        if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
        return false;
    }

    *restored = true;
    return true;
}

/* -------------------------------------------------------------------------- */

/* Copy one directory into a fixed field, refusing anything that does not fit
 * rather than truncating: a truncated directory is a real path somewhere else,
 * and the one thing worse than not finding the work image is finding a
 * different one. */
static bool set_directory(char *field, const char *value) {
    int written = snprintf(field, VM_FW_BOOT_PATH_CAPACITY, "%s",
                           value ? value : "");
    return written >= 0 && (size_t)written < VM_FW_BOOT_PATH_CAPACITY;
}

bool vm_firmware_boot_paths_shared(vm_firmware_boot_paths_t *out,
                                   const char *directory) {
    return vm_firmware_boot_paths_split(out, directory, directory);
}

bool vm_firmware_boot_paths_split(vm_firmware_boot_paths_t *out,
                                  const char *firmware_directory,
                                  const char *work_directory) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!set_directory(out->firmware, firmware_directory) ||
        !set_directory(out->work, work_directory)) {
        memset(out, 0, sizeof *out);
        return false;
    }
    return true;
}

void vm_firmware_boot_probe(const vm_firmware_boot_paths_t *paths,
                            vm_firmware_boot_state_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->readiness = VM_FW_BOOT_INCOMPLETE;

    if (!paths || !paths->firmware[0]) {
        set_detail(out->detail, sizeof out->detail,
                   "This app has no directory to look for firmware in.");
        return;
    }

    const char *directory = paths->firmware;
    char path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (join_path(path, sizeof path, directory, VM_FW_BOOT_KERNEL_FILE))
        out->kernel_size = file_size(path);
    if (join_path(path, sizeof path, directory, VM_FW_BOOT_DEVICETREE_FILE))
        out->devicetree_size = file_size(path);
    if (join_path(path, sizeof path, directory, VM_FW_BOOT_ROOTFS_FILE))
        out->rootfs_size = file_size(path);
    /* The work image is the one file that belongs to a single machine, so it
     * is looked for in the machine's own directory and nowhere else. An empty
     * work directory reports "no work image", which is the truthful answer for
     * a caller that has not named a machine. */
    if (paths->work[0] &&
        join_path(path, sizeof path, paths->work, VM_FW_BOOT_WORK_FILE))
        out->work_size = file_size(path);

    out->kernel_present     = out->kernel_size > 0u;
    out->devicetree_present = out->devicetree_size > 0u;
    out->rootfs_present     = out->rootfs_size > 0u;
    /*
     * A WORK IMAGE HAS TO BE AT LEAST AS BIG AS WHAT IT WAS COPIED FROM, and
     * "not empty" is not that test.
     *
     * rootfs_work_create() copies the whole root filesystem and then grows it,
     * so a finished one is always larger than the source -- 466,825,216 octets
     * against 433,274,880 for 7E18. A run that was interrupted, or refused
     * part way through, leaves a SHORT file behind, and `> 0` calls that
     * prepared. The machine then boots a truncated HFS+ volume: it mounts, and
     * then launchd cannot create anything on it and the boot stops dead
     * immediately after AppleMultitouchZ2SPI with a black screen and no error
     * anywhere. That is docs/BOOTLOG.md's "fstab wall" arriving by a different
     * road, and it is what a user actually hit.
     *
     * Requiring >= the source size is the honest guard rather than an exact
     * formula: the provisioner rounds the growth to allocation blocks, so the
     * final size is not source + GROWTH exactly, but it is never smaller than
     * the source.
     */
    out->work_present       = out->work_size > 0u &&
                              out->rootfs_size > 0u &&
                              out->work_size >= out->rootfs_size;

    if (!out->kernel_present || !out->devicetree_present ||
        !out->rootfs_present) {
        /* Name every missing file at once rather than one per attempt: a user
         * who imported two of three should not have to boot three times to
         * learn that. */
        char missing[192];
        missing[0] = '\0';
        size_t used = 0u;
        static const struct { const char *name; size_t offset; } files[] = {
            { VM_FW_BOOT_KERNEL_FILE,
              offsetof(vm_firmware_boot_state_t, kernel_present) },
            { VM_FW_BOOT_DEVICETREE_FILE,
              offsetof(vm_firmware_boot_state_t, devicetree_present) },
            { VM_FW_BOOT_ROOTFS_FILE,
              offsetof(vm_firmware_boot_state_t, rootfs_present) },
        };
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
            const bool *present =
                (const bool *)((const char *)out + files[i].offset);
            if (*present) continue;
            int written = snprintf(missing + used, sizeof missing - used,
                                   "%s%s", used ? ", " : "", files[i].name);
            if (written < 0 || (size_t)written >= sizeof missing - used) break;
            used += (size_t)written;
        }
        (void)snprintf(out->detail, sizeof out->detail,
                       "Missing firmware: %s. Import an IPSW from Settings.",
                       missing);
        out->detail[sizeof out->detail - 1u] = '\0';
        return;
    }

    if (!out->work_present) {
        out->readiness = VM_FW_BOOT_NEEDS_WORK_IMAGE;
        if (out->work_size > 0u && out->work_size < out->rootfs_size)
            /* Distinguish "not started" from "started and left short", because
             * the second needs the stale file removed and the first does not. */
            set_detail(out->detail, sizeof out->detail,
                       "The writable root filesystem is incomplete: it is "
                       "smaller than the one it was copied from, so an earlier "
                       "attempt was interrupted. It will be made again.");
        else
            set_detail(out->detail, sizeof out->detail,
                       "The writable root filesystem has not been prepared yet.");
        return;
    }

    out->readiness = VM_FW_BOOT_READY;
}

/* -------------------------------------------------------------------------- */

vm_firmware_boot_t *vm_firmware_boot_create(void) {
    vm_firmware_boot_t *boot =
        (vm_firmware_boot_t *)calloc(1u, sizeof *boot);
    if (!boot) return NULL;
    boot->bridges = (s5l_bringup_md_t *)calloc(1u, sizeof *boot->bridges);
    boot->media = file_block_create();
    if (!boot->bridges || !boot->media) {
        vm_firmware_boot_destroy(&boot);
        return NULL;
    }
    return boot;
}

bool vm_firmware_boot_arm_overlay(vm_firmware_boot_t *boot,
                                  const char *overlay_path) {
    if (!boot) return false;
    if (!overlay_path || !*overlay_path) {
        boot->overlay[0] = '\0';
        boot->overlay_armed = false;
        return true;
    }
    size_t n = strlen(overlay_path);
    if (n >= sizeof boot->overlay) return false;
    memcpy(boot->overlay, overlay_path, n + 1u);
    boot->overlay_armed = true;
    return true;
}

const vm_block_t *vm_firmware_boot_media(const vm_firmware_boot_t *boot) {
    if (!boot || !boot->media) return NULL;
    /* The RAW descriptor, deliberately: replay writes historical contents back
     * over the image, and routing that through the overlay would record the
     * blocks it is restoring as if the guest had just changed them. */
    return file_block_get(boot->media);
}

bool vm_firmware_boot_rearm_overlay(vm_firmware_boot_t *boot,
                                    const char *overlay_path,
                                    char *detail, size_t capacity) {
    if (!boot || !boot->media) return false;
    /* Close first. Two adapters over one file would each hold a private
     * bitmap, and the second would re-save blocks the first already had --
     * appending a newer, wrong copy that replay would apply last. */
    if (boot->cow) (void)vm_cow_close(&boot->cow);
    if (!overlay_path || !*overlay_path) {
        boot->overlay[0] = '\0';
        boot->overlay_armed = false;
        return true;
    }
    if (!vm_firmware_boot_arm_overlay(boot, overlay_path)) return false;
    vm_cow_status_t st = vm_cow_open(&boot->cow, file_block_get(boot->media),
                                     boot->overlay, detail, capacity);
    if (st != VM_COW_OK && detail && capacity && !detail[0])
        (void)snprintf(detail, capacity, "%s", vm_cow_status_text(st));
    return st == VM_COW_OK;
}

bool vm_firmware_boot_flush_overlay(vm_firmware_boot_t *boot) {
    if (!boot) return false;
    if (!boot->cow)
        return file_block_flush(boot->media) == FILE_BLOCK_STATUS_OK;
    return vm_cow_flush(boot->cow) == VM_COW_OK;
}

vm_firmware_checkpoint_status_t vm_firmware_boot_save_resume(
        vm_firmware_boot_t *boot, const s5l8900_t *machine,
        char *detail, size_t detail_capacity) {
    set_detail(detail, detail_capacity, "");
    if (!boot || !machine || !boot->bridges || !boot->media ||
        !boot->bridges->installed || !boot->work_directory[0] ||
        boot->media_size == 0u) {
        set_detail(detail, detail_capacity,
                   "This machine does not have a live firmware checkpoint path.");
        return VM_FW_CHECKPOINT_ERROR;
    }

    /* The entry SVC may leave a host continuation live until the guest reaches
     * its completion SVC. That continuation owns scratch spans which the small
     * sidecar deliberately does not serialize, so run forward and retry rather
     * than writing a checkpoint that can never complete the operation. */
    for (unsigned i = 0; i < MD_RAW_BRIDGE_MAX_BOUNCE_SLOTS; i++) {
        if (boot->bridges->raw.pending[i].active) {
            set_detail(detail, detail_capacity,
                       "Waiting for the current disk operation to finish.");
            return VM_FW_CHECKPOINT_BUSY;
        }
    }

    if (!vm_firmware_boot_flush_overlay(boot)) {
        set_detail(detail, detail_capacity,
                   "The writable root filesystem could not be flushed.");
        return VM_FW_CHECKPOINT_ERROR;
    }

    external_md_sidecar_t sidecar;
    memset(&sidecar, 0, sizeof sidecar);
    sidecar.magic = EXTERNAL_MD_SIDECAR_MAGIC;
    sidecar.version = EXTERNAL_MD_SIDECAR_VERSION;
    sidecar.media_size = boot->media_size;
    sidecar.image_bytes = boot->media_size;
    sidecar.strategy_stats = boot->bridges->strategy.stats;
    sidecar.raw_stats = boot->bridges->raw.stats;
    memcpy(sidecar.guard_tail, boot->bridges->raw.guard_tail,
           sizeof sidecar.guard_tail);

    if (!vm_resume_checkpoint_save(machine, &sidecar,
                                   boot->work_directory,
                                   detail, detail_capacity))
        return VM_FW_CHECKPOINT_ERROR;
    return VM_FW_CHECKPOINT_OK;
}

void vm_firmware_boot_destroy(vm_firmware_boot_t **slot) {
    if (!slot || !*slot) return;
    vm_firmware_boot_t *boot = *slot;
    vm_network_session_destroy(&boot->network);
#if defined(S5LBOX_IOS3_HLE_EXPERIMENT)
    if (boot->hle_machine)
        vm_firmware_hle_release(boot->hle_machine);
#endif
    /*
     * The overlay closes BEFORE the media, and that order is not tidiness. The
     * adapter holds a copy of the file descriptor and its close is where a
     * buffered record finally reaches disk; letting the media go first would
     * flush an image whose history is still in a stdio buffer, which is the
     * one state a later restore would trust and be wrong about.
     */
    if (boot->cow) (void)vm_cow_close(&boot->cow);
    if (boot->media) {
        /* The bridges borrowed this descriptor, so every borrow must have
         * ended -- which it has, because the caller frees the machine first. */
        (void)file_block_flush(boot->media);
        (void)file_block_close(boot->media);
        (void)file_block_destroy(&boot->media);
    }
    free(boot->bridges);
    free(boot);
    *slot = NULL;
}

/*
 * A powered-off checkpoint is loaded only long enough to identify its PMU
 * state, then the machine is rebuilt for a fresh boot. s5l8900_init() restores
 * the product build's default engine, so replay the already-validated marker
 * controls before bring-up writes the kernel into the new RAM allocation.
 *
 * This is deliberately smaller than the marker parser below. Every conflict,
 * pathname and availability check has already succeeded once in this call;
 * this helper only reapplies host policy lost with the old machine object.
 */
static bool reapply_engine_controls_after_reset(
        s5l8900_t *machine, bool forced_interpreter,
        bool compact_user_only, bool compact_window_refill_off,
        bool compact_window_cache, bool compact_privileged_window_refill) {
    (void)machine;
    (void)forced_interpreter;
    (void)compact_user_only;
    (void)compact_window_refill_off;
    (void)compact_window_cache;
    (void)compact_privileged_window_refill;
#if defined(S5LBOX_STATIC_A64_ENGINE)
    if (forced_interpreter &&
        !s5l8900_static_a64_set_enabled(machine, false))
        return false;
#if defined(S5LBOX_STATIC_A64_DEFAULT_COMPACT_RAW)
    if (!forced_interpreter && compact_user_only &&
        !s5l8900_static_a64_set_compact_raw_privileged(machine, false))
        return false;
    if (!forced_interpreter && compact_window_refill_off &&
        !s5l8900_static_a64_set_compact_raw_window_refill(machine, false))
        return false;
    if (compact_window_cache &&
        !s5l8900_static_a64_set_compact_raw_window_cache(machine, true))
        return false;
    if (!forced_interpreter && compact_privileged_window_refill &&
        !s5l8900_static_a64_set_compact_raw_privileged_window_refill(
            machine, true))
        return false;
#endif
#endif
    return true;
}

bool vm_firmware_boot_start(vm_firmware_boot_t *boot,
                            s5l8900_t *machine,
                            const vm_firmware_boot_paths_t *paths,
                            const bool *options, unsigned option_count,
                            vm_firmware_boot_report_t *report) {
    if (!report) return false;
    memset(report, 0, sizeof *report);
    set_detail(report->summary, sizeof report->summary, "not started");

    if (!boot || !boot->bridges || !boot->media || !machine || !paths) {
        set_detail(report->detail, sizeof report->detail,
                   "Internal error: the firmware boot path was not set up.");
        set_detail(report->summary, sizeof report->summary, "internal error");
        return false;
    }

    /* Capacity and Cydia-metadata maintenance have disjoint journals but
     * replace the same live image. Their recovery layer inspects which one
     * currently owns that pathname and runs it first; a fixed order is wrong
     * once either transaction can be interrupted between the two renames. */
    vm_guest_install_result_t guest_privilege;
    vm_guest_install_result_t guest_storage;
    char guest_maintenance_detail[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
    vm_guest_install_status_t guest_maintenance_status =
        vm_guest_maintenance_recover(paths->work, &guest_privilege,
                                     &guest_storage,
                                     guest_maintenance_detail,
                                     sizeof guest_maintenance_detail);
    if (guest_maintenance_status != VM_GUEST_INSTALL_OK) {
        (void)snprintf(report->detail, sizeof report->detail,
                       "Guest-maintenance recovery refused (%s): %.180s",
                       vm_guest_install_status_text(guest_maintenance_status),
                       guest_maintenance_detail[0]
                           ? guest_maintenance_detail
                           : "no safe recovery exists");
        report->detail[sizeof report->detail - 1u] = '\0';
        set_detail(report->summary, sizeof report->summary,
                   "guest-maintenance recovery required");
        return false;
    }

    /* Finish or roll back an interrupted install BEFORE probing or opening the
     * live image. The strict marker is also the only authority for the paired
     * code-signing policy below: malformed evidence stops here instead of
     * producing a half-installed, policy-off boot. */
    vm_guest_install_result_t guest_install;
    char guest_install_detail[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
    vm_guest_install_status_t guest_install_status =
        vm_guest_install_recover(paths->work, &guest_install,
                                 guest_install_detail,
                                 sizeof guest_install_detail);
    if (guest_install_status != VM_GUEST_INSTALL_OK) {
        (void)snprintf(report->detail, sizeof report->detail,
                       "Guest-install recovery refused (%s): %.180s",
                       vm_guest_install_status_text(guest_install_status),
                       guest_install_detail[0] ? guest_install_detail
                                               : "no safe recovery exists");
        report->detail[sizeof report->detail - 1u] = '\0';
        set_detail(report->summary, sizeof report->summary,
                   "guest-install recovery required");
        return false;
    }
    bool guest_install_committed = guest_install.committed;

    bool forced_interpreter = false;
    bool compact_user_only = false;
    bool compact_window_refill_off = false;
    bool compact_window_cache = false;
    bool compact_privileged_window_refill = false;
    bool compact_pc_profile = false;
    bool active_clock_off = false;
    bool active_clock_budget_control = false;
    uint32_t active_clock_work_budget =
        S5L8900_ACTIVE_CLOCK_DEFAULT_WORK_TICKS;
#if defined(S5LBOX_STATIC_A64_ENGINE)
    /*
     * One signed binary supplies both halves of the physical A/B.  The marker
     * only turns an already compiled engine OFF; it cannot create executable
     * memory or enable a facility absent from the build.  Keeping the choice
     * outside the core snapshot is intentional: static_a64_state is host-owned
     * derived state and snapshot_load() never serializes it.
     */
    char interpreter_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(interpreter_path, sizeof interpreter_path, paths->work,
                   VM_FW_BOOT_INTERPRETER_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The engine-control path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }
    forced_interpreter = file_size(interpreter_path) > 0u;
    if (forced_interpreter &&
        !s5l8900_static_a64_set_enabled(machine, false)) {
        set_detail(report->detail, sizeof report->detail,
                   "The interpreter control could not disable the signed "
                   "static engine.");
        set_detail(report->summary, sizeof report->summary,
                   "interpreter control unavailable");
        return false;
    }
#if defined(S5LBOX_STATIC_A64_DEFAULT_COMPACT_RAW)
    char compact_user_only_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(compact_user_only_path, sizeof compact_user_only_path,
                   paths->work, VM_FW_BOOT_COMPACT_USER_ONLY_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The compact-control path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }
    compact_user_only = file_size(compact_user_only_path) > 0u;
    if (!forced_interpreter && compact_user_only &&
        !s5l8900_static_a64_set_compact_raw_privileged(machine, false)) {
        set_detail(report->detail, sizeof report->detail,
                   "The User-only control could not disable privileged "
                   "compact entries.");
        set_detail(report->summary, sizeof report->summary,
                   "User-only control unavailable");
        return false;
    }
    char compact_window_refill_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(compact_window_refill_path,
                   sizeof compact_window_refill_path, paths->work,
                   VM_FW_BOOT_COMPACT_WINDOW_REFILL_OFF_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The compact window-control path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }
    compact_window_refill_off =
        file_size(compact_window_refill_path) > 0u;
    if (!forced_interpreter && compact_window_refill_off &&
        !s5l8900_static_a64_set_compact_raw_window_refill(machine, false)) {
        set_detail(report->detail, sizeof report->detail,
                   "The compact window control could not disable lookup-only "
                   "resident refills.");
        set_detail(report->summary, sizeof report->summary,
                   "window-refill control unavailable");
        return false;
    }
    char compact_window_cache_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(compact_window_cache_path,
                   sizeof compact_window_cache_path, paths->work,
                   VM_FW_BOOT_COMPACT_WINDOW_CACHE_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The compact window-cache path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }
    compact_window_cache = file_size(compact_window_cache_path) > 0u;
    if (compact_window_cache &&
        (forced_interpreter || compact_window_refill_off)) {
        set_detail(report->detail, sizeof report->detail,
                   "The compact window-cache experiment conflicts with an "
                   "interpreter or window-refill-off control.");
        set_detail(report->summary, sizeof report->summary,
                   "conflicting compact controls");
        return false;
    }
    if (compact_window_cache &&
        !s5l8900_static_a64_set_compact_raw_window_cache(machine, true)) {
        set_detail(report->detail, sizeof report->detail,
                   "The compact window-cache experiment could not be "
                   "enabled.");
        set_detail(report->summary, sizeof report->summary,
                   "window-cache experiment unavailable");
        return false;
    }
    char compact_privileged_window_refill_path[
        VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(compact_privileged_window_refill_path,
                   sizeof compact_privileged_window_refill_path,
                   paths->work,
                   VM_FW_BOOT_COMPACT_PRIVILEGED_WINDOW_REFILL_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The privileged window-rollout path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }
    compact_privileged_window_refill =
        file_size(compact_privileged_window_refill_path) > 0u;
    if (!forced_interpreter && compact_privileged_window_refill &&
        (compact_user_only || compact_window_refill_off)) {
        set_detail(report->detail, sizeof report->detail,
                   "The privileged window experiment conflicts with an "
                   "active User-only or all-window-refill control.");
        set_detail(report->summary, sizeof report->summary,
                   "conflicting compact controls");
        return false;
    }
    if (!forced_interpreter && compact_privileged_window_refill &&
        !s5l8900_static_a64_set_compact_raw_privileged_window_refill(
            machine, true)) {
        set_detail(report->detail, sizeof report->detail,
                   "The privileged compact window experiment could not be "
                   "enabled.");
        set_detail(report->summary, sizeof report->summary,
                   "privileged window experiment unavailable");
        return false;
    }
#endif
    char compact_pc_profile_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(compact_pc_profile_path,
                   sizeof compact_pc_profile_path, paths->work,
                   VM_FW_BOOT_COMPACT_PC_PROFILE_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The compact CPU-profile path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }
    compact_pc_profile = file_size(compact_pc_profile_path) > 0u;
    if (compact_pc_profile && forced_interpreter) {
        set_detail(report->detail, sizeof report->detail,
                   "The compact CPU profile conflicts with the interpreter "
                   "control.");
        set_detail(report->summary, sizeof report->summary,
                   "conflicting engine controls");
        return false;
    }
#endif
    (void)compact_user_only;
    (void)compact_window_refill_off;
    (void)compact_window_cache;
    (void)compact_privileged_window_refill;
    (void)compact_pc_profile;

#if defined(S5LBOX_IOS_ACTIVE_REALTIME_CLOCK)
    char active_clock_off_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(active_clock_off_path, sizeof active_clock_off_path,
                   paths->work, VM_FW_BOOT_ACTIVE_CLOCK_OFF_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The active-clock control path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }
    active_clock_off = file_size(active_clock_off_path) > 0u;
    char active_clock_budget_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(active_clock_budget_path, sizeof active_clock_budget_path,
                   paths->work, VM_FW_BOOT_ACTIVE_CLOCK_BUDGET_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The active-clock work-budget path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }
    if (!read_active_clock_budget_control(
            active_clock_budget_path, &active_clock_work_budget,
            &active_clock_budget_control)) {
        set_detail(report->detail, sizeof report->detail,
                   "The active-clock work budget must contain one decimal "
                   "integer from 1 through 64.");
        set_detail(report->summary, sizeof report->summary,
                   "invalid active-clock work budget");
        return false;
    }
    if (active_clock_off && active_clock_budget_control) {
        set_detail(report->detail, sizeof report->detail,
                   "The active-clock-off and work-budget controls conflict.");
        set_detail(report->summary, sizeof report->summary,
                   "conflicting active-clock controls");
        return false;
    }
#else
    (void)active_clock_off;
    (void)active_clock_budget_control;
    (void)active_clock_work_budget;
#endif

    vm_firmware_boot_state_t state;
    vm_firmware_boot_probe(paths, &state);
    if (state.readiness != VM_FW_BOOT_READY) {
        set_detail(report->detail, sizeof report->detail, state.detail);
        set_detail(report->summary, sizeof report->summary,
                   state.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE
                       ? "root filesystem not prepared"
                       : "firmware incomplete");
        return false;
    }

    char kernel_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    char tree_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    char work_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(kernel_path, sizeof kernel_path, paths->firmware,
                   VM_FW_BOOT_KERNEL_FILE) ||
        !join_path(tree_path, sizeof tree_path, paths->firmware,
                   VM_FW_BOOT_DEVICETREE_FILE) ||
        !join_path(work_path, sizeof work_path, paths->work,
                   VM_FW_BOOT_WORK_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The firmware directory path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }

    /* The work image first: it is the only step that can fail because another
     * machine is already using the file, and discovering that after reading
     * 8 MB of kernel wastes the user's time for no reason. */
    file_block_status_t opened =
        file_block_open(boot->media, work_path, state.work_size);
    if (opened != FILE_BLOCK_STATUS_OK) {
        (void)snprintf(report->detail, sizeof report->detail,
                       "Cannot open the root filesystem work image: %s.",
                       file_block_strerror(opened));
        report->detail[sizeof report->detail - 1u] = '\0';
        set_detail(report->summary, sizeof report->summary,
                   "root filesystem unavailable");
        return false;
    }
    int work_written = snprintf(boot->work_directory,
                                sizeof boot->work_directory, "%s", paths->work);
    if (work_written < 0 ||
        (size_t)work_written >= sizeof boot->work_directory) {
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   "The machine work directory is too long to checkpoint.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }
    boot->media_size = state.work_size;

    size_t kernel_size = 0u, tree_size = 0u;
    uint8_t *kernel = slurp(kernel_path, &kernel_size);
    uint8_t *tree = kernel ? slurp(tree_path, &tree_size) : NULL;
    if (!kernel || !tree) {
        free(kernel);
        free(tree);
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   kernel ? "Cannot read devicetree.bin."
                          : "Cannot read kernel.macho.");
        set_detail(report->summary, sizeof report->summary,
                   "firmware unreadable");
        return false;
    }

    s5l_bringup_request_t request;
    memset(&request, 0, sizeof request);
    request.kernel = kernel;
    request.kernel_size = kernel_size;
    request.devicetree = tree;
    request.devicetree_size = tree_size;
    /*
     * The one seam copy-on-write needs. With no snapshot to protect the
     * bringup gets the file adapter's own descriptor and nothing changes; with
     * one armed it gets the overlay's facade instead, and every write from
     * here on saves what it is about to destroy.
     *
     * A failure to arm is FATAL rather than a fallback to the bare descriptor.
     * Carrying on would run a machine whose writes are not being recorded
     * while its snapshots still claim to be restorable, and the user would
     * find out at the moment they most needed it to be true.
     */
    request.root_media = file_block_get(boot->media);
    if (boot->overlay_armed) {
        char why[192] = {0};
        vm_cow_status_t st = vm_cow_open(&boot->cow, request.root_media,
                                         boot->overlay, why, sizeof why);
        if (st != VM_COW_OK) {
            free(kernel);
            free(tree);
            (void)file_block_close(boot->media);
            (void)snprintf(report->detail, sizeof report->detail,
                           "Cannot record changes for the saved states: %s.",
                           why[0] ? why : vm_cow_status_text(st));
            report->detail[sizeof report->detail - 1u] = '\0';
            set_detail(report->summary, sizeof report->summary,
                       "snapshot history unavailable");
            return false;
        }
        request.root_media = vm_cow_block(boot->cow);
    }
    /* One place decides both which kernel is acceptable and where its
     * memory-disk call sites are, so the bridges can never be armed against a
     * kernel that was authorized somewhere else. */
    ios3_bringup_gate_configure(&request, NULL);
    /*
     * The user's switches, resolved into the request AFTER the fields above so
     * that a switch can never overwrite the media or the gate, and BEFORE
     * s5l_bringup() so the report describes the request that actually ran.
     * Everything the mapping refuses is recorded in report->options, which the
     * caller shows: this is where a switch that does not reach the machine
     * becomes a sentence instead of a silence.
     */
    vm_boot_options_apply(options, option_count, &request, &report->options);

    char ppp_marker[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!join_path(ppp_marker, sizeof ppp_marker, paths->work,
                   VM_FW_BOOT_PPP_FILE)) {
        free(kernel);
        free(tree);
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   "The guest-network record path is too long to use.");
        set_detail(report->summary, sizeof report->summary,
                   "network path unavailable");
        return false;
    }
    bool ppp_provisioned = file_size(ppp_marker) > 0u;
    vm_boot_options_reconcile_network(&report->options, &request,
                                      ppp_provisioned);

    vm_boot_options_reconcile_jailbreak(
        &report->options, &request, guest_install_committed);

    s5l_bringup_status_t status =
        s5l_bringup(machine, &request, boot->bridges, &report->bringup);

    if (status != S5L_BRINGUP_OK) {
        free(kernel);
        free(tree);
        (void)file_block_close(boot->media);
        (void)snprintf(report->detail, sizeof report->detail,
                       "Could not start Apple's kernel (%s at %s): %s",
                       s5l_bringup_status_name(status),
                       s5l_bringup_stage_name(report->bringup.stage),
                       report->bringup.detail);
        report->detail[sizeof report->detail - 1u] = '\0';
        (void)snprintf(report->summary, sizeof report->summary,
                       "firmware boot failed: %s",
                       s5l_bringup_status_name(status));
        report->summary[sizeof report->summary - 1u] = '\0';
        return false;
    }

    bool restored = false;
    char restore_marker[VM_FW_BOOT_PATH_CAPACITY + 64u];
    if (!restore_once(boot, machine, paths, state.work_size, &restored,
                      restore_marker, sizeof restore_marker,
                      report->detail, sizeof report->detail)) {
        free(kernel);
        free(tree);
        (void)file_block_close(boot->media);
        set_detail(report->summary, sizeof report->summary,
                   "saved state unavailable");
        return false;
    }

    bool checkpoint_loaded = restored;
    bool fresh_boot_after_poweroff = false;

    /* OOCSHDWN.GO_STANDBY is the guest's full power-off command, not an
     * ordinary suspend point. A prior implementation reset directly into
     * XNU's retained-RAM page and delivered PMU ONKEY. Exact replay proved
     * that it reaches and exits the GPIO handler, but the saved shutdown never
     * re-enables the PMU child or LCD and remains black indefinitely. Real
     * hardware starts through its boot chain after full power-off; this
     * emulator's supported boot boundary starts at the kernel instead.
     *
     * The guest flushed and unmounted the same work image before issuing this
     * command, so retain that disk but discard the powered-off CPU/RAM image.
     * Ordinary running checkpoints never enter this branch and still restore
     * exactly. Keeping the kernel and device-tree buffers alive until here is
     * what lets the fallback rebuild without a second file read. */
    if (restored && s5l_pcf50635_in_standby(&machine->pmu)) {
        uint32_t ram_base = machine->ram_base;
        uint32_t ram_size = machine->ram_size;
        s5l8900_free(machine);
        if (!s5l8900_init(machine, ram_base, ram_size)) {
            free(kernel);
            free(tree);
            (void)file_block_close(boot->media);
            set_detail(report->detail, sizeof report->detail,
                       "The saved machine was fully powered off, but memory "
                       "for its required fresh boot could not be allocated.");
            set_detail(report->summary, sizeof report->summary,
                       "powered-off checkpoint could not restart");
            return false;
        }
        if (!reapply_engine_controls_after_reset(
                machine, forced_interpreter, compact_user_only,
                compact_window_refill_off, compact_window_cache,
                compact_privileged_window_refill)) {
            free(kernel);
            free(tree);
            (void)file_block_close(boot->media);
            set_detail(report->detail, sizeof report->detail,
                       "The saved machine was fully powered off, but its "
                       "engine controls could not be reapplied for a fresh "
                       "boot.");
            set_detail(report->summary, sizeof report->summary,
                       "powered-off checkpoint controls unavailable");
            return false;
        }
        status = s5l_bringup(machine, &request, boot->bridges,
                             &report->bringup);
        if (status != S5L_BRINGUP_OK) {
            free(kernel);
            free(tree);
            (void)file_block_close(boot->media);
            (void)snprintf(
                report->detail, sizeof report->detail,
                "The saved machine was fully powered off, and its required "
                "fresh boot failed (%.32s at %.32s): %.100s",
                s5l_bringup_status_name(status),
                s5l_bringup_stage_name(report->bringup.stage),
                report->bringup.detail);
            report->detail[sizeof report->detail - 1u] = '\0';
            set_detail(report->summary, sizeof report->summary,
                       "powered-off checkpoint could not restart");
            return false;
        }
        restored = false;
        fresh_boot_after_poweroff = true;
    }

    /* Guest DRAM now holds its own copy of everything. */
    free(kernel);
    free(tree);

#if defined(S5LBOX_IOS_ACTIVE_REALTIME_CLOCK)
    /*
     * Arm this after restore so the first host sample anchors the restored
     * guest instant. The portable default remains deterministic; this product
     * policy treats monotonic wall time as an upper bound, then limits it by
     * retired work so timer/display deadlines cannot overwhelm a slow host.
     * Failure is explicit because silently reverting would recreate the
     * device-dependent navigation cadence this build is intended to measure.
     */
    if (!active_clock_off &&
        !s5l8900_set_active_clock_work_budget(
            machine, active_clock_work_budget)) {
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   "The interactive guest clock rejected its bounded work "
                   "budget.");
        set_detail(report->summary, sizeof report->summary,
                   "active clock budget unavailable");
        return false;
    }
    if (!active_clock_off &&
        !s5l8900_set_active_host_clock(
            machine, vm_firmware_active_now, NULL)) {
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   "The interactive guest clock could not enable bounded "
                   "active-time synchronization.");
        set_detail(report->summary, sizeof report->summary,
                   "active clock unavailable");
        return false;
    }
#endif

#if defined(S5LBOX_IOS_WFI_REALTIME_PACING)
    /*
     * Arm this AFTER the optional snapshot load. It is host policy, not saved
     * guest state, and the loaded timer phase is now the one whose future WFI
     * edges will be paced. A failure stops startup instead of silently falling
     * back to the phone-speed-dependent fast-forward that made guest time run
     * several times faster than wall time during idle navigation.
     */
    if (!s5l8900_set_wfi_host_pacing(
            machine, vm_firmware_wfi_sleep, NULL)) {
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   "The interactive guest clock could not enable bounded "
                   "idle pacing.");
        set_detail(report->summary, sizeof report->summary,
                   "idle pacing unavailable");
        return false;
    }
#endif

    if (ppp_provisioned) {
        int nat_index = vm_option_index("nat");
        bool nat_enabled = nat_index >= 0 &&
            (unsigned)nat_index < report->options.count &&
            report->options.row[nat_index].effective;
        char network_detail[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
        boot->network = vm_network_session_create(
            machine, nat_enabled, network_detail, sizeof network_detail);
        if (!boot->network) {
            (void)file_block_close(boot->media);
            (void)snprintf(report->detail, sizeof report->detail,
                           "Guest networking could not start: %.220s",
                           network_detail[0] ? network_detail :
                               "the host peer was unavailable");
            report->detail[sizeof report->detail - 1u] = '\0';
            set_detail(report->summary, sizeof report->summary,
                       "guest networking unavailable");
            return false;
        }
    }

#if defined(S5LBOX_IOS3_HLE_EXPERIMENT)
    /*
     * This build exists to measure the native raster replacements on a real
     * phone. Quietly falling back to the ordinary interpreter would produce a
     * perfectly plausible but false control result, so inability to install
     * the exact-PC boundary is a boot failure in this build only.
     */
    if (!vm_firmware_hle_enable(machine)) {
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   "The experimental native raster hook could not be armed; "
                   "the ordinary app build is unaffected.");
        set_detail(report->summary, sizeof report->summary,
                   "experimental raster HLE unavailable");
        return false;
    }
    boot->hle_machine = machine;
#endif

#if defined(S5LBOX_STATIC_A64_ENGINE)
    /* Start diagnostic state only after the firmware, saved state and every
     * ordinary host policy have passed validation. Marker-free boots never
     * create the dedicated sampler thread. */
    if (compact_pc_profile &&
        !s5l8900_static_a64_enable_compact_raw_pc_profile(machine)) {
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   "The compact CPU profile is unavailable on this host or "
                   "its target-thread sampler could not be started.");
        set_detail(report->summary, sizeof report->summary,
                   "compact CPU profile unavailable");
        return false;
    }
#endif

    /* Consume only the request, not the checkpoint.  The state remains useful
     * for a controlled repeat, but it cannot roll a disk forward or backward a
     * second time unless the caller explicitly re-arms it after reinstalling
     * the matching image. */
    if (checkpoint_loaded && remove(restore_marker) != 0) {
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   "The saved state loaded, but its one-shot marker could not "
                   "be consumed. The machine was stopped to prevent an unsafe "
                   "second restore after its disk changes.");
        set_detail(report->summary, sizeof report->summary,
                   "saved state not safely consumed");
        return false;
    }

    report->ok = true;
#if defined(S5LBOX_IOS3_HLE_EXPERIMENT)
    (void)snprintf(report->summary, sizeof report->summary,
                   "iPhone OS 3.1.3 + EXPERIMENTAL native raster HLE");
#else
    const char *engine_mode = "interpreter";
    if (forced_interpreter) {
        engine_mode = "interpreter control";
    } else {
#if defined(S5LBOX_STATIC_A64_DEFAULT_COMPACT_RAW)
        if (compact_user_only && compact_window_refill_off)
            engine_mode =
                "compact raw, User-only control + window-refill-off control";
        else if (compact_user_only)
            engine_mode = "compact raw, User-only control";
        else if (compact_window_refill_off)
            engine_mode = "compact raw, window-refill-off control";
        else if (compact_privileged_window_refill)
            engine_mode =
                "compact raw, privileged prefix + privileged-window experiment";
        else
            engine_mode = "compact raw, privileged prefix";
#elif defined(S5LBOX_STATIC_A64_DEFAULT_GRAPH)
        engine_mode = "static graph";
#endif
    }
    char active_clock_budget_suffix[48] = {0};
    if (active_clock_budget_control)
        (void)snprintf(active_clock_budget_suffix,
                       sizeof active_clock_budget_suffix,
                       ", active-clock-budget-%u control",
                       active_clock_work_budget);
    if (restored) {
        (void)snprintf(report->summary, sizeof report->summary,
                       "iPhone OS 3.1.3 restored at %.1f M insn "
                       "(%s%s%s%s%s)",
                       (double)machine->cpu.cycles / 1000000.0, engine_mode,
                       compact_window_cache ? ", window-cache experiment" : "",
                       compact_pc_profile ? ", compact-PC profile" : "",
                       active_clock_off ? ", active-clock-off control" : "",
                       active_clock_budget_suffix);
    } else if (fresh_boot_after_poweroff) {
        (void)snprintf(report->summary, sizeof report->summary,
                       "iPhone OS 3.1.3 fresh boot after powered-off "
                       "checkpoint (%s%s%s%s%s)", engine_mode,
                       compact_window_cache ? ", window-cache experiment" : "",
                       compact_pc_profile ? ", compact-PC profile" : "",
                       active_clock_off ? ", active-clock-off control" : "",
                       active_clock_budget_suffix);
    } else {
        (void)snprintf(report->summary, sizeof report->summary,
                       "iPhone OS 3.1.3 kernel, root on /dev/md0 "
                       "(%s%s%s%s%s)",
                       engine_mode,
                       compact_window_cache ? ", window-cache experiment" : "",
                       compact_pc_profile ? ", compact-PC profile" : "",
                       active_clock_off ? ", active-clock-off control" : "",
                       active_clock_budget_suffix);
    }
#endif
    if (ppp_provisioned) {
        int nat_index = vm_option_index("nat");
        bool nat_enabled = nat_index >= 0 &&
            (unsigned)nat_index < report->options.count &&
            report->options.row[nat_index].effective;
        size_t used = strlen(report->summary);
        if (used < sizeof report->summary)
            (void)snprintf(report->summary + used,
                           sizeof report->summary - used,
                           "%s", nat_enabled ? ", PPP/NAT" : ", PPP");
    }
    report->summary[sizeof report->summary - 1u] = '\0';
    /* No failure remains after this point. Execution observations begin only
     * when the caller enters its run loop, so this is the exact boundary at
     * which a restarted machine must stop inheriting the preceding machine's
     * counter baselines and worst-gap witness. */
    vm_frame_telemetry_begin_machine();
    return true;
}

/* -------------------------------------------------------------------------- */

bool vm_firmware_boot_provision(const vm_firmware_boot_paths_t *paths,
                                const bool *values, unsigned value_count,
                                void (*progress)(void *ctx, uint64_t done,
                                                 uint64_t total),
                                void *progress_ctx,
                                char *detail, size_t detail_capacity) {
    char source_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    char work_path[VM_FW_BOOT_PATH_CAPACITY + 64u];
    rootfs_work_options_t options;
    rootfs_work_result_t result;
    vm_boot_provision_options_t wanted;

    set_detail(detail, detail_capacity, "");

    if (!paths || !paths->firmware[0] || !paths->work[0] ||
        !join_path(source_path, sizeof source_path, paths->firmware,
                   VM_FW_BOOT_ROOTFS_FILE) ||
        !join_path(work_path, sizeof work_path, paths->work,
                   VM_FW_BOOT_WORK_FILE)) {
        set_detail(detail, detail_capacity,
                   "The firmware directory path is unusable.");
        return false;
    }

    vm_boot_options_for_provisioning(values, value_count, &wanted);

    memset(&options, 0, sizeof options);
    memset(&result, 0, sizeof result);
    /* The stock image names /dev/disk0s1, which is NAND this machine does not
     * model; without this line launchd fails fsck and halts. */
    options.fstab_line = ROOTFS_WORK_DEFAULT_FSTAB;
    options.growth_bytes = VM_FW_BOOT_GROWTH_BYTES;
    /*
     * The QuartzCore software renderer used to be forced on here regardless of
     * the switch, which meant the settings screen could show it OFF while
     * every work image this app made had it ON. It now follows the switch --
     * and because it is written into the image rather than decided at boot,
     * changing it later does nothing until the image is remade. VMBootOptions
     * is where that is said to the user.
     */
    options.ca_software_render = wanted.ca_software_render;
    options.ppp_launchd_job = wanted.ppp;

    /*
     * ACTIVATION, which the app used to describe as unimplemented while the
     * library it already links has always been able to do it. Without these two
     * catalog objects lockdownd finds no data_ark.plist, and a SpringBoard that
     * renders perfectly still sits at "connect to iTunes" forever.
     *
     * The entries point at static storage inside rootfs_work and stay valid for
     * the life of the process, so this array only has to outlive the create
     * call below -- which it does, being in the same frame.
     *
     * Offline provisioning of a file on this machine's own writable image: no
     * Apple record is applied, none is verified, and the canonical firmware is
     * not touched.
     */
    rootfs_work_entry_t provision[VM_FW_BOOT_PROVISION_ENTRIES];
    size_t provision_count = rootfs_work_standard_entries(
        wanted.activate, wanted.ppp, NULL, 0u);
    memset(provision, 0, sizeof provision);
    /*
     * The phone used to set only ppp_launchd_job. That made pppd appear and
     * negotiate a link, but silently omitted /etc/ppp/options, resolv.conf and
     * the matching SystemConfiguration service that bootkernel already
     * provisions. A green core test therefore described a desktop image, not
     * the image users created in the app. Merge the same authoritative table
     * here, after activation, exactly as bootkernel does.
     */
    if (provision_count > sizeof provision / sizeof provision[0] ||
        rootfs_work_standard_entries(wanted.activate, wanted.ppp, provision,
                                     sizeof provision / sizeof provision[0]) !=
            provision_count) {
        set_detail(detail, detail_capacity,
                   "The standard provisioning table no longer fits; refusing "
                   "a partial work image.");
        return false;
    }
    options.entries = provision_count ? provision : NULL;
    options.entry_count = provision_count;

    /*
     * CLEAR AN INCOMPLETE ONE FIRST, AND ONLY AN INCOMPLETE ONE.
     *
     * rootfs_work_create() opens the destination O_CREAT|O_EXCL, so it refuses
     * outright if anything is already there -- which is the right behaviour and
     * is what stops it walking over a machine that already exists. But an
     * interrupted or refused run leaves a SHORT file behind, and from then on
     * every retry fails with EEXIST while the probe keeps asking for one. The
     * user sits on "Preparing" for ever, or worse, boots the truncated volume.
     *
     * So a file smaller than the source is removed and remade. A file at least
     * as large as the source is NOT: that is a finished work image, it is the
     * user's machine -- everything installed in it, every setting -- and
     * deleting it because provisioning happened to be asked again would be
     * data loss dressed up as repair. The size test is the same one
     * vm_firmware_boot_probe() uses to decide the image is real.
     */
    uint64_t source_size = file_size(source_path);
    uint64_t existing = file_size(work_path);
    bool creating = existing == 0u;
    if (existing > 0u) {
        if (source_size > 0u && existing < source_size) {
            if (remove(work_path) != 0) {
                (void)snprintf(detail, detail_capacity,
                               "An incomplete root filesystem is in the way "
                               "and could not be removed. Delete %s and try "
                               "again.", VM_FW_BOOT_WORK_FILE);
                return false;
            }
            creating = true;
        }
    }

    if (creating &&
        !write_ppp_marker(paths, wanted.ppp, detail, detail_capacity))
        return false;

    /* The caller's bar. NULL is fine and means nobody is watching. */
    options.progress     = progress;
    options.progress_ctx = progress_ctx;

    rootfs_work_status_t status =
        rootfs_work_create(source_path, work_path, &options, &result);
    if (status != ROOTFS_WORK_OK) {
        (void)snprintf(detail, detail_capacity,
                       "Could not prepare the root filesystem (%s at %s): %s",
                       rootfs_work_status_name(status),
                       rootfs_work_stage_name(result.stage),
                       result.detail);
        if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
        return false;
    }

    /*
     * Say what was applied even on success, and take it from the RESULT rather
     * than from the request: ca_plist_offset stays UINT64_MAX unless the
     * rewrite actually found and replaced its pattern, so this cannot report a
     * transformation that did not happen. The value is fixed in the image from
     * here on, which is why it is worth a line at all.
     */
    (void)snprintf(detail, detail_capacity,
                   "Prepared %.1f MB. QuartzCore software renderer: %s.",
                   (double)result.final_size / 1048576.0,
                   result.ca_plist_offset != UINT64_MAX ? "on" : "off");
    if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
    return true;
}
