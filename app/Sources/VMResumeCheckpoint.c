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
#include <sys/stat.h>
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

static bool resume_regular_file_size(const char *path, uint64_t *out_size) {
    if (out_size) *out_size = 0u;
    if (!path || !*path || !out_size) return false;
    errno = 0;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return false;
    if ((st.st_mode & _S_IFREG) == 0 || st.st_size < 0) {
        errno = EINVAL;
        return false;
    }
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
        errno = EINVAL;
        return false;
    }
#endif
    *out_size = (uint64_t)st.st_size;
    return true;
}

static bool resume_read_exact(const char *path, void *bytes, size_t size) {
    if (!path || !bytes || size == 0u) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fread(bytes, 1u, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

vm_resume_checkpoint_state_t vm_resume_checkpoint_probe_state(
    const char *work_directory, uint64_t media_size,
    uint32_t ram_base, uint32_t ram_size,
    char *detail, size_t detail_capacity) {
    static const char request[] = "S5LBox automatic resume 1\n";
    char state[VM_FW_BOOT_PATH_CAPACITY + VM_RESUME_PATH_EXTRA];
    char bridge[VM_FW_BOOT_PATH_CAPACITY + VM_RESUME_PATH_EXTRA];
    char marker[VM_FW_BOOT_PATH_CAPACITY + VM_RESUME_PATH_EXTRA];
    uint64_t marker_size = 0u;
    uint64_t state_size = 0u;
    uint64_t bridge_size = 0u;
    char marker_bytes[sizeof request - 1u];
    external_md_sidecar_t sidecar;
    s5l8900_t machine;

    resume_detail(detail, detail_capacity, "");
    if (!work_directory || !*work_directory || media_size == 0u ||
        ram_size == 0u ||
        !resume_path(state, sizeof state, work_directory,
                     VM_FW_BOOT_STATE_FILE) ||
        !resume_path(bridge, sizeof bridge, work_directory,
                     VM_FW_BOOT_STATE_MD_FILE) ||
        !resume_path(marker, sizeof marker, work_directory,
                     VM_FW_BOOT_RESTORE_ONCE_FILE)) {
        resume_detail(detail, detail_capacity,
                      "The automatic checkpoint probe is incomplete.");
        return VM_RESUME_CHECKPOINT_INVALID;
    }

    if (!resume_regular_file_size(marker, &marker_size)) {
        if (errno == ENOENT) {
            resume_detail(detail, detail_capacity,
                          "No automatic checkpoint is armed.");
            return VM_RESUME_CHECKPOINT_ABSENT;
        }
        resume_detail(detail, detail_capacity,
                      "The automatic checkpoint marker could not be inspected.");
        return VM_RESUME_CHECKPOINT_INVALID;
    }
    if (marker_size == 0u) {
        resume_detail(detail, detail_capacity,
                      "No automatic checkpoint is armed.");
        return VM_RESUME_CHECKPOINT_ABSENT;
    }
    if (marker_size != sizeof request - 1u ||
        !resume_read_exact(marker, marker_bytes, sizeof marker_bytes) ||
        memcmp(marker_bytes, request, sizeof marker_bytes) != 0) {
        resume_detail(detail, detail_capacity,
                      "The automatic checkpoint marker is not exact.");
        return VM_RESUME_CHECKPOINT_INVALID;
    }
    if (!resume_regular_file_size(state, &state_size) || state_size == 0u ||
        !resume_regular_file_size(bridge, &bridge_size) ||
        bridge_size != sizeof sidecar ||
        !resume_read_exact(bridge, &sidecar, sizeof sidecar)) {
        resume_detail(detail, detail_capacity,
                      "The automatic checkpoint payload pair is incomplete.");
        return VM_RESUME_CHECKPOINT_INVALID;
    }
    if (sidecar.magic != EXTERNAL_MD_SIDECAR_MAGIC ||
        sidecar.version != EXTERNAL_MD_SIDECAR_VERSION ||
        sidecar.media_size != media_size ||
        sidecar.image_bytes != media_size) {
        resume_detail(detail, detail_capacity,
                      "The automatic checkpoint belongs to a different guest disk.");
        return VM_RESUME_CHECKPOINT_INVALID;
    }

    memset(&machine, 0, sizeof machine);
    if (!s5l8900_init(&machine, ram_base, ram_size)) {
        resume_detail(detail, detail_capacity,
                      "Memory for checkpoint verification is unavailable.");
        return VM_RESUME_CHECKPOINT_INVALID;
    }
    snapshot_status_t loaded = snapshot_load(&machine, state);
    if (loaded != SNAP_OK) {
        char failure[VM_FW_BOOT_DETAIL_CAPACITY];
        (void)snprintf(failure, sizeof failure,
                       "The automatic checkpoint was refused: %s.",
                       snapshot_strerror(loaded));
        failure[sizeof failure - 1u] = '\0';
        resume_detail(detail, detail_capacity, failure);
        s5l8900_free(&machine);
        return VM_RESUME_CHECKPOINT_INVALID;
    }
    bool powered_off = s5l_pcf50635_in_standby(&machine.pmu);
    s5l8900_free(&machine);
    resume_detail(detail, detail_capacity,
                  powered_off
                      ? "The automatic checkpoint proves full guest power-off."
                      : "The automatic checkpoint is a resumable running state.");
    return powered_off ? VM_RESUME_CHECKPOINT_POWERED_OFF
                       : VM_RESUME_CHECKPOINT_RUNNING;
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
