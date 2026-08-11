/* See VMGuestInstall.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "VMGuestInstall.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define VM_GUEST_INSTALL_WRITE_RETRIES 64u
#define VM_GUEST_INSTALL_RECORD_CAPACITY 160u

static const char VM_GUEST_MARKER_PREFIX[] =
    "s5lbox-guest-install 1\nmanifest-sha256 ";
static const char VM_GUEST_JOURNAL_PREFIX[] =
    "s5lbox-guest-install-transaction 1\nmanifest-sha256 ";

typedef enum {
    GUEST_NODE_ABSENT = 0,
    GUEST_NODE_REGULAR,
    GUEST_NODE_EMPTY,
    GUEST_NODE_DIRECTORY,
    GUEST_NODE_OTHER,
    GUEST_NODE_IO_ERROR
} guest_node_t;

typedef struct {
    char work[VM_GUEST_INSTALL_PATH_CAPACITY];
    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    char backup[VM_GUEST_INSTALL_PATH_CAPACITY];
    char stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    char next[VM_GUEST_INSTALL_PATH_CAPACITY];
    char marker[VM_GUEST_INSTALL_PATH_CAPACITY];
    char marker_tmp[VM_GUEST_INSTALL_PATH_CAPACITY];
    char journal[VM_GUEST_INSTALL_PATH_CAPACITY];
    char journal_tmp[VM_GUEST_INSTALL_PATH_CAPACITY];
    char resume_once[VM_GUEST_INSTALL_PATH_CAPACITY];
    char resume_once_tmp[VM_GUEST_INSTALL_PATH_CAPACITY];
} guest_paths_t;

#if defined(S5LBOX_GUEST_INSTALL_TESTING)
static unsigned guest_test_interrupt_boundary;

void vm_guest_install_test_interrupt_after(unsigned boundary) {
    guest_test_interrupt_boundary = boundary;
}

static bool guest_test_interrupt(unsigned boundary) {
    return guest_test_interrupt_boundary == boundary;
}
#else
static bool guest_test_interrupt(unsigned boundary) {
    (void)boundary;
    return false;
}
#endif

static void guest_detail(char *out, size_t capacity, const char *text) {
    if (!out || capacity == 0u) return;
    (void)snprintf(out, capacity, "%s", text ? text : "");
    out[capacity - 1u] = '\0';
}

static void guest_result_clear(vm_guest_install_result_t *result) {
    if (!result) return;
    memset(result, 0, sizeof *result);
    result->cleanup_complete = true;
}

static bool guest_join(char *out, size_t capacity, const char *directory,
                       const char *name) {
    if (!out || capacity == 0u || !directory || !*directory ||
        !name || !*name)
        return false;
    size_t n = strlen(directory);
    const char *separator =
        (directory[n - 1u] == '/' || directory[n - 1u] == '\\') ? "" : "/";
    int written = snprintf(out, capacity, "%s%s%s", directory, separator,
                           name);
    return written >= 0 && (size_t)written < capacity;
}

static bool guest_paths_init(guest_paths_t *paths,
                             const char *work_directory) {
    if (!paths) return false;
    memset(paths, 0, sizeof *paths);
    if (!work_directory || !*work_directory) return false;
    int copied = snprintf(paths->work, sizeof paths->work, "%s",
                          work_directory);
    if (copied < 0 || (size_t)copied >= sizeof paths->work) return false;
    return guest_join(paths->live, sizeof paths->live, paths->work,
                      VM_GUEST_INSTALL_LIVE_FILE) &&
           guest_join(paths->backup, sizeof paths->backup, paths->work,
                      VM_GUEST_INSTALL_BACKUP_FILE) &&
           guest_join(paths->stage, sizeof paths->stage, paths->work,
                      VM_GUEST_INSTALL_STAGE_DIRECTORY) &&
           guest_join(paths->next, sizeof paths->next, paths->stage,
                      VM_GUEST_INSTALL_NEXT_FILE) &&
           guest_join(paths->marker, sizeof paths->marker, paths->work,
                      VM_GUEST_INSTALL_MARKER_FILE) &&
           guest_join(paths->marker_tmp, sizeof paths->marker_tmp, paths->work,
                      VM_GUEST_INSTALL_MARKER_TMP) &&
           guest_join(paths->journal, sizeof paths->journal, paths->work,
                      VM_GUEST_INSTALL_JOURNAL_FILE) &&
           guest_join(paths->journal_tmp, sizeof paths->journal_tmp,
                      paths->work, VM_GUEST_INSTALL_JOURNAL_TMP) &&
           guest_join(paths->resume_once, sizeof paths->resume_once,
                      paths->work, VM_GUEST_INSTALL_RESUME_ONCE_FILE) &&
           guest_join(paths->resume_once_tmp, sizeof paths->resume_once_tmp,
                      paths->work, VM_GUEST_INSTALL_RESUME_ONCE_TMP);
}

bool vm_guest_install_stage_image_path(char *out, size_t capacity,
                                       const char *work_directory) {
    guest_paths_t paths;
    if (!out || capacity == 0u) return false;
    out[0] = '\0';
    if (!guest_paths_init(&paths, work_directory)) return false;
    int written = snprintf(out, capacity, "%s", paths.next);
    if (written < 0 || (size_t)written >= capacity) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static guest_node_t guest_node(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return GUEST_NODE_ABSENT;
        return GUEST_NODE_IO_ERROR;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        return GUEST_NODE_OTHER;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u)
        return GUEST_NODE_DIRECTORY;
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return GUEST_NODE_IO_ERROR;
    if ((st.st_mode & _S_IFREG) == 0) return GUEST_NODE_OTHER;
    return st.st_size > 0 ? GUEST_NODE_REGULAR : GUEST_NODE_EMPTY;
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return GUEST_NODE_ABSENT;
        return GUEST_NODE_IO_ERROR;
    }
    if (S_ISLNK(st.st_mode)) return GUEST_NODE_OTHER;
    if (S_ISDIR(st.st_mode)) return GUEST_NODE_DIRECTORY;
    if (!S_ISREG(st.st_mode)) return GUEST_NODE_OTHER;
    return st.st_size > 0 ? GUEST_NODE_REGULAR : GUEST_NODE_EMPTY;
#endif
}

static bool guest_work_directory_ok(const guest_paths_t *paths) {
    return paths && guest_node(paths->work) == GUEST_NODE_DIRECTORY;
}

static bool guest_sync_directory(const char *path);

static bool guest_remove_if_present(const char *path) {
    if (remove(path) == 0) return true;
    return errno == ENOENT;
}

static bool guest_invalidate_resume(const guest_paths_t *paths) {
    if (!paths) return false;
    if (!guest_remove_if_present(paths->resume_once) ||
        !guest_remove_if_present(paths->resume_once_tmp))
        return false;
    return guest_sync_directory(paths->work);
}

static bool guest_sync_directory(const char *path) {
#ifdef _WIN32
    /* MoveFileEx(..., WRITE_THROUGH) supplies rename durability on Windows;
     * directory handles do not support _commit(). */
    (void)path;
    return true;
#else
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return false;
    bool ok = fsync(descriptor) == 0;
    if (close(descriptor) != 0) ok = false;
    return ok;
#endif
}

static bool guest_rename_new(const char *source, const char *destination) {
    if (guest_node(destination) != GUEST_NODE_ABSENT) return false;
#ifdef _WIN32
    return MoveFileExA(source, destination, MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(source, destination) == 0;
#endif
}

static bool guest_write_all(int descriptor, const uint8_t *bytes,
                            size_t size) {
    size_t done = 0u;
    unsigned no_progress = 0u;
    while (done < size && no_progress < VM_GUEST_INSTALL_WRITE_RETRIES) {
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

static bool guest_write_durable(const char *path, const void *bytes,
                                size_t size) {
    if (!path || !bytes || size == 0u) return false;
#ifdef _WIN32
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
#else
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#endif
    if (descriptor < 0) return false;
    bool ok = guest_write_all(descriptor, (const uint8_t *)bytes, size);
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

static bool guest_sync_existing(const char *path) {
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

static char guest_hex_digit(unsigned value) {
    return (char)(value < 10u ? ('0' + value) : ('a' + value - 10u));
}

static int guest_hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool guest_record_format(char *out, size_t capacity,
                                const char *prefix,
                                const uint8_t digest[
                                    VM_GUEST_INSTALL_SHA256_SIZE],
                                size_t *out_size) {
    if (!out || !capacity || !prefix || !digest || !out_size) return false;
    size_t prefix_size = strlen(prefix);
    size_t wanted = prefix_size + VM_GUEST_INSTALL_SHA256_SIZE * 2u + 1u;
    if (wanted >= capacity) return false;
    memcpy(out, prefix, prefix_size);
    for (size_t i = 0u; i < VM_GUEST_INSTALL_SHA256_SIZE; i++) {
        out[prefix_size + i * 2u] = guest_hex_digit(digest[i] >> 4u);
        out[prefix_size + i * 2u + 1u] =
            guest_hex_digit(digest[i] & 0x0fu);
    }
    out[wanted - 1u] = '\n';
    out[wanted] = '\0';
    *out_size = wanted;
    return true;
}

static vm_guest_install_probe_t
guest_record_probe(const char *path, const char *prefix,
                   uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE]) {
    if (digest) memset(digest, 0, VM_GUEST_INSTALL_SHA256_SIZE);
    guest_node_t node = guest_node(path);
    if (node == GUEST_NODE_ABSENT) return VM_GUEST_INSTALL_PROBE_ABSENT;
    if (node == GUEST_NODE_IO_ERROR) return VM_GUEST_INSTALL_PROBE_IO_ERROR;
    if (node != GUEST_NODE_REGULAR) return VM_GUEST_INSTALL_PROBE_INVALID;

    char record[VM_GUEST_INSTALL_RECORD_CAPACITY];
    FILE *file = fopen(path, "rb");
    if (!file) return VM_GUEST_INSTALL_PROBE_IO_ERROR;
    size_t got = fread(record, 1u, sizeof record, file);
    bool io_error = ferror(file) != 0;
    int extra = EOF;
    if (!io_error && got == sizeof record) extra = fgetc(file);
    if (fclose(file) != 0) io_error = true;
    if (io_error) return VM_GUEST_INSTALL_PROBE_IO_ERROR;
    if (got == sizeof record || extra != EOF)
        return VM_GUEST_INSTALL_PROBE_INVALID;

    size_t prefix_size = strlen(prefix);
    size_t expected = prefix_size + VM_GUEST_INSTALL_SHA256_SIZE * 2u + 1u;
    if (got != expected || memcmp(record, prefix, prefix_size) != 0 ||
        record[expected - 1u] != '\n')
        return VM_GUEST_INSTALL_PROBE_INVALID;
    for (size_t i = 0u; i < VM_GUEST_INSTALL_SHA256_SIZE; i++) {
        int high = guest_hex_value(record[prefix_size + i * 2u]);
        int low = guest_hex_value(record[prefix_size + i * 2u + 1u]);
        if (high < 0 || low < 0) {
            if (digest) memset(digest, 0, VM_GUEST_INSTALL_SHA256_SIZE);
            return VM_GUEST_INSTALL_PROBE_INVALID;
        }
        if (digest) digest[i] = (uint8_t)((unsigned)high << 4u |
                                          (unsigned)low);
    }
    return VM_GUEST_INSTALL_PROBE_VALID;
}

vm_guest_install_probe_t
vm_guest_install_probe(const char *work_directory,
                       uint8_t manifest_sha256[VM_GUEST_INSTALL_SHA256_SIZE],
                       char *detail, size_t detail_capacity) {
    guest_detail(detail, detail_capacity, "");
    if (manifest_sha256)
        memset(manifest_sha256, 0, VM_GUEST_INSTALL_SHA256_SIZE);
    guest_paths_t paths;
    if (!guest_paths_init(&paths, work_directory)) {
        guest_detail(detail, detail_capacity,
                     "The guest-install record path is too long to use.");
        return VM_GUEST_INSTALL_PROBE_IO_ERROR;
    }
    vm_guest_install_probe_t probe =
        guest_record_probe(paths.marker, VM_GUEST_MARKER_PREFIX,
                           manifest_sha256);
    if (probe == VM_GUEST_INSTALL_PROBE_INVALID)
        guest_detail(detail, detail_capacity,
                     "The guest-install record is malformed; the machine was not started.");
    else if (probe == VM_GUEST_INSTALL_PROBE_IO_ERROR)
        guest_detail(detail, detail_capacity,
                     "The guest-install record could not be read safely.");
    return probe;
}

static bool guest_digest_equal(
    const uint8_t a[VM_GUEST_INSTALL_SHA256_SIZE],
    const uint8_t b[VM_GUEST_INSTALL_SHA256_SIZE]) {
    return memcmp(a, b, VM_GUEST_INSTALL_SHA256_SIZE) == 0;
}

static bool guest_publish_record(const char *temporary,
                                 const char *destination,
                                 const char *directory, const char *prefix,
                                 const uint8_t digest[
                                     VM_GUEST_INSTALL_SHA256_SIZE]) {
    char record[VM_GUEST_INSTALL_RECORD_CAPACITY];
    size_t size = 0u;
    if (!guest_record_format(record, sizeof record, prefix, digest, &size))
        return false;
    if (!guest_remove_if_present(temporary) ||
        guest_node(destination) != GUEST_NODE_ABSENT ||
        !guest_write_durable(temporary, record, size) ||
        !guest_rename_new(temporary, destination)) {
        (void)remove(temporary);
        return false;
    }
    return guest_sync_directory(directory);
}

static bool guest_remove_committed_artifacts(const guest_paths_t *paths) {
    bool ok = true;
    if (!guest_remove_if_present(paths->backup)) ok = false;
    if (!guest_remove_if_present(paths->journal)) ok = false;
    if (!guest_remove_if_present(paths->journal_tmp)) ok = false;
    if (!guest_remove_if_present(paths->marker_tmp)) ok = false;
    if (!guest_remove_if_present(paths->next)) ok = false;
#ifdef _WIN32
    if (_rmdir(paths->stage) != 0 && errno != ENOENT) ok = false;
#else
    if (rmdir(paths->stage) != 0 && errno != ENOENT) ok = false;
#endif
    if (!guest_sync_directory(paths->work)) ok = false;
    return ok;
}

static bool guest_remove_rolled_back_artifacts(const guest_paths_t *paths) {
    bool ok = true;
    if (!guest_remove_if_present(paths->journal)) ok = false;
    if (!guest_remove_if_present(paths->journal_tmp)) ok = false;
    if (!guest_remove_if_present(paths->marker_tmp)) ok = false;
    if (!guest_remove_if_present(paths->next)) ok = false;
#ifdef _WIN32
    if (_rmdir(paths->stage) != 0 && errno != ENOENT) ok = false;
#else
    if (rmdir(paths->stage) != 0 && errno != ENOENT) ok = false;
#endif
    if (!guest_sync_directory(paths->work)) ok = false;
    return ok;
}

static vm_guest_install_status_t
guest_continue(const guest_paths_t *paths,
               const uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE],
               vm_guest_install_result_t *result,
               char *detail, size_t detail_capacity) {
    if (!guest_invalidate_resume(paths)) {
        guest_detail(detail, detail_capacity,
                     "The stale automatic-resume request could not be cleared before replacing the guest disk.");
        return VM_GUEST_INSTALL_ERR_IO;
    }
    guest_node_t live = guest_node(paths->live);
    guest_node_t backup = guest_node(paths->backup);
    guest_node_t next = guest_node(paths->next);
    if ((live != GUEST_NODE_ABSENT && live != GUEST_NODE_REGULAR) ||
        (backup != GUEST_NODE_ABSENT && backup != GUEST_NODE_REGULAR) ||
        (next != GUEST_NODE_ABSENT && next != GUEST_NODE_REGULAR)) {
        guest_detail(detail, detail_capacity,
                     "The guest-install disk transaction contains an invalid file type or empty image.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }

    bool have_live = live == GUEST_NODE_REGULAR;
    bool have_backup = backup == GUEST_NODE_REGULAR;
    bool have_next = next == GUEST_NODE_REGULAR;

    if (have_live && have_next && !have_backup) {
        if (!guest_rename_new(paths->live, paths->backup) ||
            !guest_sync_directory(paths->work)) {
            guest_detail(detail, detail_capacity,
                         "The current guest disk could not be preserved before installation.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
        have_live = false;
        have_backup = true;
        if (guest_test_interrupt(2u))
            return VM_GUEST_INSTALL_ERR_INTERRUPTED;
    }

    if (!have_live && have_next && have_backup) {
        if (!guest_rename_new(paths->next, paths->live) ||
            !guest_sync_directory(paths->stage) ||
            !guest_sync_directory(paths->work)) {
            guest_detail(detail, detail_capacity,
                         "The prepared guest disk could not be installed.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
        have_live = true;
        have_next = false;
        if (guest_test_interrupt(3u))
            return VM_GUEST_INSTALL_ERR_INTERRUPTED;
    }

    if (have_live && !have_next && have_backup) {
        if (!guest_publish_record(paths->marker_tmp, paths->marker,
                                  paths->work, VM_GUEST_MARKER_PREFIX,
                                  digest)) {
            guest_detail(detail, detail_capacity,
                         "The new guest disk is installed, but its boot-policy record could not be published.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
        if (result) {
            result->committed = true;
            result->has_manifest = true;
            memcpy(result->manifest_sha256, digest,
                   VM_GUEST_INSTALL_SHA256_SIZE);
        }
        if (guest_test_interrupt(4u))
            return VM_GUEST_INSTALL_ERR_INTERRUPTED;
        bool cleaned = guest_remove_committed_artifacts(paths);
        if (result) result->cleanup_complete = cleaned;
        if (!cleaned)
            guest_detail(detail, detail_capacity,
                         "The guest install is committed, but inert transaction files could not all be removed.");
        return VM_GUEST_INSTALL_OK;
    }

    if (!have_live && !have_next && have_backup) {
        if (!guest_rename_new(paths->backup, paths->live) ||
            !guest_sync_directory(paths->work)) {
            guest_detail(detail, detail_capacity,
                         "The interrupted guest install could not restore the original disk.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
        if (result) result->rolled_back = true;
        bool cleaned = guest_remove_rolled_back_artifacts(paths);
        if (result) result->cleanup_complete = cleaned;
        guest_detail(detail, detail_capacity,
                     cleaned
                         ? "An incomplete guest install was rolled back to the original disk."
                         : "The original guest disk was restored, but inert transaction files remain.");
        return VM_GUEST_INSTALL_OK;
    }

    guest_detail(detail, detail_capacity,
                 "The guest-install disk transaction is contradictory; no file was changed.");
    return VM_GUEST_INSTALL_ERR_STATE;
}

vm_guest_install_status_t
vm_guest_install_recover(const char *work_directory,
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity) {
    guest_result_clear(result);
    guest_detail(detail, detail_capacity, "");
    guest_paths_t paths;
    if (!guest_paths_init(&paths, work_directory)) {
        guest_detail(detail, detail_capacity,
                     "The guest-install transaction path is too long to use.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }
    if (!guest_work_directory_ok(&paths)) {
        guest_detail(detail, detail_capacity,
                     "The machine work directory is missing or is not a real directory.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }

    uint8_t marker_digest[VM_GUEST_INSTALL_SHA256_SIZE];
    vm_guest_install_probe_t marker =
        guest_record_probe(paths.marker, VM_GUEST_MARKER_PREFIX,
                           marker_digest);
    if (marker == VM_GUEST_INSTALL_PROBE_INVALID) {
        guest_detail(detail, detail_capacity,
                     "The committed guest-install record is malformed; no recovery was attempted.");
        return VM_GUEST_INSTALL_ERR_RECORD;
    }
    if (marker == VM_GUEST_INSTALL_PROBE_IO_ERROR) {
        guest_detail(detail, detail_capacity,
                     "The committed guest-install record could not be read safely.");
        return VM_GUEST_INSTALL_ERR_IO;
    }

    uint8_t journal_digest[VM_GUEST_INSTALL_SHA256_SIZE];
    vm_guest_install_probe_t journal =
        guest_record_probe(paths.journal, VM_GUEST_JOURNAL_PREFIX,
                           journal_digest);
    if (journal == VM_GUEST_INSTALL_PROBE_INVALID) {
        guest_detail(detail, detail_capacity,
                     "The guest-install recovery journal is malformed; no disk was changed.");
        return VM_GUEST_INSTALL_ERR_RECORD;
    }
    if (journal == VM_GUEST_INSTALL_PROBE_IO_ERROR) {
        guest_detail(detail, detail_capacity,
                     "The guest-install recovery journal could not be read safely.");
        return VM_GUEST_INSTALL_ERR_IO;
    }

    if (marker == VM_GUEST_INSTALL_PROBE_VALID) {
        if (guest_node(paths.live) != GUEST_NODE_REGULAR) {
            guest_detail(detail, detail_capacity,
                         "A guest-install record exists without a valid live guest disk.");
            return VM_GUEST_INSTALL_ERR_STATE;
        }
        if (journal == VM_GUEST_INSTALL_PROBE_VALID &&
            !guest_digest_equal(marker_digest, journal_digest)) {
            guest_detail(detail, detail_capacity,
                         "The committed guest-install record and recovery journal name different manifests.");
            return VM_GUEST_INSTALL_ERR_STATE;
        }
        if (result) {
            result->committed = true;
            result->has_manifest = true;
            memcpy(result->manifest_sha256, marker_digest,
                   VM_GUEST_INSTALL_SHA256_SIZE);
        }
        guest_node_t residue_backup = guest_node(paths.backup);
        guest_node_t residue_next = guest_node(paths.next);
        guest_node_t residue_stage = guest_node(paths.stage);
        bool transaction_residue =
            journal == VM_GUEST_INSTALL_PROBE_VALID ||
            residue_backup != GUEST_NODE_ABSENT ||
            residue_next != GUEST_NODE_ABSENT ||
            residue_stage != GUEST_NODE_ABSENT;
        if (transaction_residue && !guest_invalidate_resume(&paths)) {
            guest_detail(detail, detail_capacity,
                         "The guest disk is committed, but its stale automatic-resume request could not be cleared.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
        bool cleaned = guest_remove_committed_artifacts(&paths);
        if (result) result->cleanup_complete = cleaned;
        if (!cleaned)
            guest_detail(detail, detail_capacity,
                         "The guest install is committed, but inert transaction files could not all be removed.");
        return VM_GUEST_INSTALL_OK;
    }

    if (journal == VM_GUEST_INSTALL_PROBE_VALID)
        return guest_continue(&paths, journal_digest, result,
                              detail, detail_capacity);

    guest_node_t live = guest_node(paths.live);
    guest_node_t backup = guest_node(paths.backup);
    guest_node_t next = guest_node(paths.next);
    if (backup == GUEST_NODE_REGULAR && live == GUEST_NODE_ABSENT &&
        next == GUEST_NODE_ABSENT) {
        if (!guest_invalidate_resume(&paths) ||
            !guest_rename_new(paths.backup, paths.live) ||
            !guest_sync_directory(paths.work)) {
            guest_detail(detail, detail_capacity,
                         "The orphaned original guest disk could not be restored.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
        if (result) result->rolled_back = true;
        bool cleaned = guest_remove_rolled_back_artifacts(&paths);
        if (result) result->cleanup_complete = cleaned;
        guest_detail(detail, detail_capacity,
                     "An orphaned guest-disk backup was restored; no install was committed.");
        return VM_GUEST_INSTALL_OK;
    }
    if (backup != GUEST_NODE_ABSENT) {
        guest_detail(detail, detail_capacity,
                     "A guest-disk backup exists without a recovery journal; no file was changed.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    return VM_GUEST_INSTALL_OK;
}

vm_guest_install_status_t
vm_guest_install_publish(const char *work_directory,
                         const uint8_t manifest_sha256[
                             VM_GUEST_INSTALL_SHA256_SIZE],
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity) {
    guest_result_clear(result);
    guest_detail(detail, detail_capacity, "");
    if (!manifest_sha256) {
        guest_detail(detail, detail_capacity,
                     "The guest-install manifest digest is missing.");
        return VM_GUEST_INSTALL_ERR_ARGUMENT;
    }

    guest_paths_t paths;
    if (!guest_paths_init(&paths, work_directory)) {
        guest_detail(detail, detail_capacity,
                     "The guest-install transaction path is too long to use.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }
    if (!guest_work_directory_ok(&paths)) {
        guest_detail(detail, detail_capacity,
                     "The machine work directory is missing or is not a real directory.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }

    uint8_t existing_digest[VM_GUEST_INSTALL_SHA256_SIZE];
    vm_guest_install_probe_t marker =
        guest_record_probe(paths.marker, VM_GUEST_MARKER_PREFIX,
                           existing_digest);
    if (marker == VM_GUEST_INSTALL_PROBE_VALID) {
        if (!guest_digest_equal(existing_digest, manifest_sha256)) {
            guest_detail(detail, detail_capacity,
                         "This machine already has a different committed guest-install manifest.");
            return VM_GUEST_INSTALL_ERR_STATE;
        }
        return vm_guest_install_recover(work_directory, result,
                                        detail, detail_capacity);
    }
    if (marker == VM_GUEST_INSTALL_PROBE_INVALID) {
        guest_detail(detail, detail_capacity,
                     "The existing guest-install record is malformed; it was not overwritten.");
        return VM_GUEST_INSTALL_ERR_RECORD;
    }
    if (marker == VM_GUEST_INSTALL_PROBE_IO_ERROR) {
        guest_detail(detail, detail_capacity,
                     "The existing guest-install record could not be read safely.");
        return VM_GUEST_INSTALL_ERR_IO;
    }

    vm_guest_install_probe_t journal =
        guest_record_probe(paths.journal, VM_GUEST_JOURNAL_PREFIX,
                           existing_digest);
    if (journal == VM_GUEST_INSTALL_PROBE_VALID) {
        if (!guest_digest_equal(existing_digest, manifest_sha256)) {
            guest_detail(detail, detail_capacity,
                         "An unfinished guest install names a different manifest.");
            return VM_GUEST_INSTALL_ERR_STATE;
        }
        return vm_guest_install_recover(work_directory, result,
                                        detail, detail_capacity);
    }
    if (journal == VM_GUEST_INSTALL_PROBE_INVALID) {
        guest_detail(detail, detail_capacity,
                     "The existing guest-install recovery journal is malformed.");
        return VM_GUEST_INSTALL_ERR_RECORD;
    }
    if (journal == VM_GUEST_INSTALL_PROBE_IO_ERROR) {
        guest_detail(detail, detail_capacity,
                     "The existing guest-install recovery journal could not be read safely.");
        return VM_GUEST_INSTALL_ERR_IO;
    }

    if (guest_node(paths.stage) != GUEST_NODE_DIRECTORY ||
        guest_node(paths.live) != GUEST_NODE_REGULAR ||
        guest_node(paths.next) != GUEST_NODE_REGULAR ||
        guest_node(paths.backup) != GUEST_NODE_ABSENT) {
        guest_detail(detail, detail_capacity,
                     "Publishing needs one live disk, one complete staged disk, and no prior backup.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if (!guest_sync_existing(paths.next)) {
        guest_detail(detail, detail_capacity,
                     "The prepared guest disk could not be made durable.");
        return VM_GUEST_INSTALL_ERR_IO;
    }
    if (!guest_publish_record(paths.journal_tmp, paths.journal, paths.work,
                              VM_GUEST_JOURNAL_PREFIX, manifest_sha256)) {
        guest_detail(detail, detail_capacity,
                     "The guest-install recovery journal could not be published.");
        return VM_GUEST_INSTALL_ERR_IO;
    }
    if (guest_test_interrupt(1u))
        return VM_GUEST_INSTALL_ERR_INTERRUPTED;
    return guest_continue(&paths, manifest_sha256, result,
                          detail, detail_capacity);
}

const char *vm_guest_install_status_text(vm_guest_install_status_t status) {
    switch (status) {
        case VM_GUEST_INSTALL_OK:              return "ok";
        case VM_GUEST_INSTALL_ERR_ARGUMENT:    return "invalid argument";
        case VM_GUEST_INSTALL_ERR_PATH:        return "path unavailable";
        case VM_GUEST_INSTALL_ERR_RECORD:      return "invalid record";
        case VM_GUEST_INSTALL_ERR_STATE:       return "contradictory state";
        case VM_GUEST_INSTALL_ERR_IO:          return "I/O failure";
        case VM_GUEST_INSTALL_ERR_INTERRUPTED: return "test interruption";
        default:                               return "unknown status";
    }
}
