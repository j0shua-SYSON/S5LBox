/* See VMResumeCheckpoint.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMResumeCheckpoint.h"

#include "VMFirmwareBoot.h"
#include "snapshot.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define VM_RESUME_PATH_EXTRA 64u
#define VM_RESUME_WRITE_RETRIES 64u

static void resume_detail(char *out, size_t capacity, const char *text) {
    if (!out || capacity == 0u) return;
    (void)snprintf(out, capacity, "%s", text ? text : "");
    out[capacity - 1u] = '\0';
}

static bool resume_path(char *out, size_t capacity, const char *directory,
                        const char *name) {
    if (!out || capacity == 0u || !directory || !*directory || !name || !*name)
        return false;
    size_t n = strlen(directory);
    const char *separator = (n > 0u && (directory[n - 1u] == '/' ||
                                       directory[n - 1u] == '\\')) ? "" : "/";
    int written = snprintf(out, capacity, "%s%s%s", directory, separator, name);
    return written >= 0 && (size_t)written < capacity;
}

static bool resume_remove_if_present(const char *path) {
    if (remove(path) == 0) return true;
    return errno == ENOENT;
}

static bool resume_replace(const char *source, const char *destination) {
#ifdef _WIN32
    return MoveFileExA(source, destination,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(source, destination) == 0;
#endif
}

static bool resume_write_all(int descriptor, const uint8_t *bytes, size_t size) {
    size_t done = 0u;
    unsigned no_progress = 0u;
    while (done < size && no_progress < VM_RESUME_WRITE_RETRIES) {
#ifdef _WIN32
        size_t remaining = size - done;
        unsigned request = remaining > (size_t)INT_MAX
                         ? (unsigned)INT_MAX : (unsigned)remaining;
        int wrote = _write(descriptor, bytes + done, request);
#else
        ssize_t wrote = write(descriptor, bytes + done, size - done);
#endif
        if (wrote > 0) {
            done += (size_t)wrote;
            no_progress = 0u;
            continue;
        }
        if (wrote < 0 && errno == EINTR) {
            no_progress++;
            continue;
        }
        return false;
    }
    return done == size;
}

static bool resume_write_durable(const char *path, const void *bytes,
                                 size_t size) {
    if (!path || !bytes || size == 0u) return false;
#ifdef _WIN32
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
#else
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#endif
    if (descriptor < 0) return false;

    bool ok = resume_write_all(descriptor, (const uint8_t *)bytes, size);
#ifdef _WIN32
    if (ok && _commit(descriptor) != 0) ok = false;
    if (_close(descriptor) != 0) ok = false;
#else
    if (ok && fsync(descriptor) != 0) ok = false;
    if (close(descriptor) != 0) ok = false;
#endif
    if (!ok) (void)remove(path);
    return ok;
}

static bool resume_sync_existing(const char *path) {
#ifdef _WIN32
    int descriptor = _open(path, _O_RDWR | _O_BINARY);
#else
    int descriptor = open(path, O_RDWR);
#endif
    if (descriptor < 0) return false;
#ifdef _WIN32
    bool ok = _commit(descriptor) == 0;
    if (_close(descriptor) != 0) ok = false;
#else
    bool ok = fsync(descriptor) == 0;
    if (close(descriptor) != 0) ok = false;
#endif
    return ok;
}

bool vm_resume_checkpoint_save(const s5l8900_t *machine,
                               const external_md_sidecar_t *sidecar,
                               const char *work_directory,
                               char *detail, size_t detail_capacity) {
    resume_detail(detail, detail_capacity, "");
    if (!machine || !sidecar || !work_directory || !*work_directory) {
        resume_detail(detail, detail_capacity,
                      "The running machine did not provide a complete checkpoint.");
        return false;
    }
    if (sidecar->magic != EXTERNAL_MD_SIDECAR_MAGIC ||
        sidecar->version != EXTERNAL_MD_SIDECAR_VERSION ||
        sidecar->media_size == 0u ||
        sidecar->image_bytes != sidecar->media_size) {
        resume_detail(detail, detail_capacity,
                      "The running disk bridge provided invalid checkpoint state.");
        return false;
    }

    char state[VM_FW_BOOT_PATH_CAPACITY + VM_RESUME_PATH_EXTRA];
    char state_partial[VM_FW_BOOT_PATH_CAPACITY + VM_RESUME_PATH_EXTRA];
    char bridge[VM_FW_BOOT_PATH_CAPACITY + VM_RESUME_PATH_EXTRA];
    char bridge_partial[VM_FW_BOOT_PATH_CAPACITY + VM_RESUME_PATH_EXTRA];
    char marker[VM_FW_BOOT_PATH_CAPACITY + VM_RESUME_PATH_EXTRA];
    char marker_partial[VM_FW_BOOT_PATH_CAPACITY + VM_RESUME_PATH_EXTRA];
    if (!resume_path(state, sizeof state, work_directory,
                     VM_FW_BOOT_STATE_FILE) ||
        !resume_path(state_partial, sizeof state_partial, work_directory,
                     VM_FW_BOOT_STATE_TMP) ||
        !resume_path(bridge, sizeof bridge, work_directory,
                     VM_FW_BOOT_STATE_MD_FILE) ||
        !resume_path(bridge_partial, sizeof bridge_partial, work_directory,
                     VM_FW_BOOT_STATE_MD_TMP) ||
        !resume_path(marker, sizeof marker, work_directory,
                     VM_FW_BOOT_RESTORE_ONCE_FILE) ||
        !resume_path(marker_partial, sizeof marker_partial, work_directory,
                     VM_FW_BOOT_RESTORE_ONCE_TMP)) {
        resume_detail(detail, detail_capacity,
                      "The machine checkpoint path is too long to use.");
        return false;
    }

    /* Invalidate the old transaction before touching either payload. */
    if (!resume_remove_if_present(marker) ||
        !resume_remove_if_present(marker_partial)) {
        resume_detail(detail, detail_capacity,
                      "The previous resume request could not be cleared safely.");
        return false;
    }

    if (!resume_write_durable(bridge_partial, sidecar, sizeof *sidecar)) {
        resume_detail(detail, detail_capacity,
                      "The disk-bridge checkpoint could not be written.");
        return false;
    }

    snapshot_status_t saved = snapshot_save(machine, state_partial);
    if (saved != SNAP_OK) {
        (void)remove(bridge_partial);
        (void)snprintf(detail, detail_capacity,
                       "The machine checkpoint could not be written: %s.",
                       snapshot_strerror(saved));
        if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
        return false;
    }
    if (!resume_sync_existing(state_partial)) {
        (void)remove(state_partial);
        (void)remove(bridge_partial);
        resume_detail(detail, detail_capacity,
                      "The machine checkpoint could not be made durable.");
        return false;
    }

    /* No marker exists yet, so either replacement may happen first safely. */
    if (!resume_replace(bridge_partial, bridge) ||
        !resume_replace(state_partial, state)) {
        (void)remove(state_partial);
        (void)remove(bridge_partial);
        resume_detail(detail, detail_capacity,
                      "The completed checkpoint files could not be installed.");
        return false;
    }

    static const char request[] = "S5LBox automatic resume 1\n";
    if (!resume_write_durable(marker_partial, request, sizeof request - 1u) ||
        !resume_replace(marker_partial, marker)) {
        (void)remove(marker_partial);
        resume_detail(detail, detail_capacity,
                      "The checkpoint was written, but automatic resume could not be armed.");
        return false;
    }

    return true;
}
