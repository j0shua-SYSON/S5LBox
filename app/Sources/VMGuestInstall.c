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
#if defined(__APPLE__)
#include <sys/clonefile.h>
#endif
#include <unistd.h>
#endif

#define VM_GUEST_INSTALL_WRITE_RETRIES 64u
#define VM_GUEST_INSTALL_RECORD_CAPACITY 160u
#define VM_GUEST_RECOVERY_COPY_BUFFER (1024u * 1024u)

static const char VM_GUEST_MARKER_PREFIX[] =
    "s5lbox-guest-install 1\nmanifest-sha256 ";
static const char VM_GUEST_JOURNAL_PREFIX[] =
    "s5lbox-guest-install-transaction 1\nmanifest-sha256 ";
static const char VM_GUEST_STORAGE_MARKER_PREFIX[] =
    "s5lbox-guest-storage 1\ninstall-manifest-sha256 ";
static const char VM_GUEST_STORAGE_JOURNAL_PREFIX[] =
    "s5lbox-guest-storage-transaction 1\ninstall-manifest-sha256 ";
static const char VM_GUEST_PRIVILEGE_MARKER_PREFIX[] =
    "s5lbox-guest-cydia-privileges 1\ninstall-manifest-sha256 ";
static const char VM_GUEST_PRIVILEGE_JOURNAL_PREFIX[] =
    "s5lbox-guest-cydia-privileges-transaction 1\ninstall-manifest-sha256 ";
static const char VM_GUEST_SOURCES_MARKER_PREFIX[] =
    "s5lbox-guest-cydia-sources 1\ninstall-manifest-sha256 ";
static const char VM_GUEST_SOURCES_JOURNAL_PREFIX[] =
    "s5lbox-guest-cydia-sources-transaction 1\ninstall-manifest-sha256 ";
static const char VM_GUEST_SOURCES_V2_MARKER_PREFIX[] =
    "s5lbox-guest-cydia-sources 2\ninstall-manifest-sha256 ";
static const char VM_GUEST_SOURCES_V2_JOURNAL_PREFIX[] =
    "s5lbox-guest-cydia-sources-transaction 2\ninstall-manifest-sha256 ";
static const char VM_GUEST_APT_TRUST_MARKER_PREFIX[] =
    "s5lbox-guest-apt-trust 1\ninstall-manifest-sha256 ";
static const char VM_GUEST_APT_TRUST_JOURNAL_PREFIX[] =
    "s5lbox-guest-apt-trust-transaction 1\ninstall-manifest-sha256 ";
static const char VM_GUEST_RECOVERY_MARKER_PREFIX[] =
    "s5lbox-guest-recovery 1\nrecovery-id ";
static const char VM_GUEST_RECOVERY_JOURNAL_PREFIX[] =
    "s5lbox-guest-recovery-transaction 1\nrecovery-id ";
static const uint8_t VM_GUEST_RECOVERY_IDENTITY[
    VM_GUEST_INSTALL_SHA256_SIZE] = {
    0x95u, 0x3bu, 0xa3u, 0xa2u, 0xafu, 0x51u, 0x75u, 0x2du,
    0xccu, 0x45u, 0x05u, 0x20u, 0x54u, 0xf1u, 0xcbu, 0x33u,
    0xf8u, 0xc8u, 0x55u, 0x8fu, 0x54u, 0xc3u, 0x30u, 0x25u,
    0x89u, 0x4eu, 0x22u, 0x09u, 0x1au, 0xafu, 0x3au, 0x63u
};

typedef struct {
    const char *backup_file;
    const char *stage_directory;
    const char *marker_file;
    const char *marker_tmp;
    const char *journal_file;
    const char *journal_tmp;
    const char *marker_prefix;
    const char *journal_prefix;
    const char *diagnostic_name;
    bool ephemeral_marker;
} guest_transaction_spec_t;

static const guest_transaction_spec_t VM_GUEST_INSTALL_SPEC = {
    VM_GUEST_INSTALL_BACKUP_FILE,
    VM_GUEST_INSTALL_STAGE_DIRECTORY,
    VM_GUEST_INSTALL_MARKER_FILE,
    VM_GUEST_INSTALL_MARKER_TMP,
    VM_GUEST_INSTALL_JOURNAL_FILE,
    VM_GUEST_INSTALL_JOURNAL_TMP,
    VM_GUEST_MARKER_PREFIX,
    VM_GUEST_JOURNAL_PREFIX,
    "guest-install",
    false
};

static const guest_transaction_spec_t VM_GUEST_STORAGE_SPEC = {
    VM_GUEST_STORAGE_BACKUP_FILE,
    VM_GUEST_STORAGE_STAGE_DIRECTORY,
    VM_GUEST_STORAGE_MARKER_FILE,
    VM_GUEST_STORAGE_MARKER_TMP,
    VM_GUEST_STORAGE_JOURNAL_FILE,
    VM_GUEST_STORAGE_JOURNAL_TMP,
    VM_GUEST_STORAGE_MARKER_PREFIX,
    VM_GUEST_STORAGE_JOURNAL_PREFIX,
    "guest-storage",
    false
};

static const guest_transaction_spec_t VM_GUEST_PRIVILEGE_SPEC = {
    VM_GUEST_PRIVILEGE_BACKUP_FILE,
    VM_GUEST_PRIVILEGE_STAGE_DIRECTORY,
    VM_GUEST_PRIVILEGE_MARKER_FILE,
    VM_GUEST_PRIVILEGE_MARKER_TMP,
    VM_GUEST_PRIVILEGE_JOURNAL_FILE,
    VM_GUEST_PRIVILEGE_JOURNAL_TMP,
    VM_GUEST_PRIVILEGE_MARKER_PREFIX,
    VM_GUEST_PRIVILEGE_JOURNAL_PREFIX,
    "guest-cydia-privileges",
    false
};

static const guest_transaction_spec_t VM_GUEST_SOURCES_SPEC = {
    VM_GUEST_SOURCES_BACKUP_FILE,
    VM_GUEST_SOURCES_STAGE_DIRECTORY,
    VM_GUEST_SOURCES_MARKER_FILE,
    VM_GUEST_SOURCES_MARKER_TMP,
    VM_GUEST_SOURCES_JOURNAL_FILE,
    VM_GUEST_SOURCES_JOURNAL_TMP,
    VM_GUEST_SOURCES_MARKER_PREFIX,
    VM_GUEST_SOURCES_JOURNAL_PREFIX,
    "guest-cydia-sources",
    false
};

static const guest_transaction_spec_t VM_GUEST_SOURCES_V2_SPEC = {
    VM_GUEST_SOURCES_V2_BACKUP_FILE,
    VM_GUEST_SOURCES_V2_STAGE_DIRECTORY,
    VM_GUEST_SOURCES_V2_MARKER_FILE,
    VM_GUEST_SOURCES_V2_MARKER_TMP,
    VM_GUEST_SOURCES_V2_JOURNAL_FILE,
    VM_GUEST_SOURCES_V2_JOURNAL_TMP,
    VM_GUEST_SOURCES_V2_MARKER_PREFIX,
    VM_GUEST_SOURCES_V2_JOURNAL_PREFIX,
    "guest-cydia-sources-v2",
    false
};

static const guest_transaction_spec_t VM_GUEST_APT_TRUST_SPEC = {
    VM_GUEST_APT_TRUST_BACKUP_FILE,
    VM_GUEST_APT_TRUST_STAGE_DIRECTORY,
    VM_GUEST_APT_TRUST_MARKER_FILE,
    VM_GUEST_APT_TRUST_MARKER_TMP,
    VM_GUEST_APT_TRUST_JOURNAL_FILE,
    VM_GUEST_APT_TRUST_JOURNAL_TMP,
    VM_GUEST_APT_TRUST_MARKER_PREFIX,
    VM_GUEST_APT_TRUST_JOURNAL_PREFIX,
    "guest-apt-trust",
    false
};

static const guest_transaction_spec_t VM_GUEST_RECOVERY_SPEC = {
    VM_GUEST_RECOVERY_BACKUP_FILE,
    VM_GUEST_RECOVERY_STAGE_DIRECTORY,
    VM_GUEST_RECOVERY_MARKER_FILE,
    VM_GUEST_RECOVERY_MARKER_TMP,
    VM_GUEST_RECOVERY_JOURNAL_FILE,
    VM_GUEST_RECOVERY_JOURNAL_TMP,
    VM_GUEST_RECOVERY_MARKER_PREFIX,
    VM_GUEST_RECOVERY_JOURNAL_PREFIX,
    "guest-recovery",
    true
};

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

static void guest_named_detail(char *out, size_t capacity,
                               const guest_transaction_spec_t *spec,
                               const char *suffix) {
    if (!out || capacity == 0u) return;
    (void)snprintf(out, capacity, "The %s%s",
                   spec ? spec->diagnostic_name : "guest-disk",
                   suffix ? suffix : "");
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

static bool guest_paths_init_for(guest_paths_t *paths,
                                 const char *work_directory,
                                 const guest_transaction_spec_t *spec) {
    if (!paths || !spec) return false;
    memset(paths, 0, sizeof *paths);
    if (!work_directory || !*work_directory) return false;
    int copied = snprintf(paths->work, sizeof paths->work, "%s",
                          work_directory);
    if (copied < 0 || (size_t)copied >= sizeof paths->work) return false;
    return guest_join(paths->live, sizeof paths->live, paths->work,
                      VM_GUEST_INSTALL_LIVE_FILE) &&
           guest_join(paths->backup, sizeof paths->backup, paths->work,
                      spec->backup_file) &&
           guest_join(paths->stage, sizeof paths->stage, paths->work,
                      spec->stage_directory) &&
           guest_join(paths->next, sizeof paths->next, paths->stage,
                      VM_GUEST_INSTALL_NEXT_FILE) &&
           guest_join(paths->marker, sizeof paths->marker, paths->work,
                      spec->marker_file) &&
           guest_join(paths->marker_tmp, sizeof paths->marker_tmp, paths->work,
                      spec->marker_tmp) &&
           guest_join(paths->journal, sizeof paths->journal, paths->work,
                      spec->journal_file) &&
           guest_join(paths->journal_tmp, sizeof paths->journal_tmp,
                      paths->work, spec->journal_tmp) &&
           guest_join(paths->resume_once, sizeof paths->resume_once,
                      paths->work, VM_GUEST_INSTALL_RESUME_ONCE_FILE) &&
           guest_join(paths->resume_once_tmp, sizeof paths->resume_once_tmp,
                      paths->work, VM_GUEST_INSTALL_RESUME_ONCE_TMP);
}

static bool guest_paths_init(guest_paths_t *paths,
                             const char *work_directory) {
    return guest_paths_init_for(paths, work_directory,
                                &VM_GUEST_INSTALL_SPEC);
}

static bool guest_stage_image_path_for(
    char *out, size_t capacity, const char *work_directory,
    const guest_transaction_spec_t *spec) {
    guest_paths_t paths;
    if (!out || capacity == 0u) return false;
    out[0] = '\0';
    if (!guest_paths_init_for(&paths, work_directory, spec)) return false;
    int written = snprintf(out, capacity, "%s", paths.next);
    if (written < 0 || (size_t)written >= capacity) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool vm_guest_install_stage_image_path(char *out, size_t capacity,
                                       const char *work_directory) {
    return guest_stage_image_path_for(out, capacity, work_directory,
                                      &VM_GUEST_INSTALL_SPEC);
}

bool vm_guest_storage_stage_image_path(char *out, size_t capacity,
                                       const char *work_directory) {
    return guest_stage_image_path_for(out, capacity, work_directory,
                                      &VM_GUEST_STORAGE_SPEC);
}

bool vm_guest_privilege_stage_image_path(char *out, size_t capacity,
                                         const char *work_directory) {
    return guest_stage_image_path_for(out, capacity, work_directory,
                                      &VM_GUEST_PRIVILEGE_SPEC);
}

bool vm_guest_sources_stage_image_path(char *out, size_t capacity,
                                       const char *work_directory) {
    return guest_stage_image_path_for(out, capacity, work_directory,
                                      &VM_GUEST_SOURCES_SPEC);
}

bool vm_guest_sources_v2_stage_image_path(char *out, size_t capacity,
                                          const char *work_directory) {
    return guest_stage_image_path_for(out, capacity, work_directory,
                                      &VM_GUEST_SOURCES_V2_SPEC);
}

bool vm_guest_apt_trust_stage_image_path(char *out, size_t capacity,
                                         const char *work_directory) {
    return guest_stage_image_path_for(out, capacity, work_directory,
                                      &VM_GUEST_APT_TRUST_SPEC);
}

bool vm_guest_recovery_stage_image_path(char *out, size_t capacity,
                                        const char *work_directory) {
    return guest_stage_image_path_for(out, capacity, work_directory,
                                      &VM_GUEST_RECOVERY_SPEC);
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

static bool guest_remove_directory_if_present(const char *path) {
#ifdef _WIN32
    if (_rmdir(path) == 0) return true;
#else
    if (rmdir(path) == 0) return true;
#endif
    return errno == ENOENT;
}

static bool guest_make_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0700) == 0;
#endif
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
    if (!guest_remove_directory_if_present(paths->stage)) ok = false;
    if (!guest_sync_directory(paths->work)) ok = false;
    return ok;
}

static bool guest_finish_committed_cleanup(
    const guest_paths_t *paths, const guest_transaction_spec_t *spec) {
    if (!guest_remove_committed_artifacts(paths)) return false;
    if (!spec || !spec->ephemeral_marker) return true;
    if (!guest_remove_if_present(paths->marker)) return false;
    return guest_sync_directory(paths->work);
}

static bool guest_remove_rolled_back_artifacts(const guest_paths_t *paths) {
    bool ok = true;
    if (!guest_remove_if_present(paths->journal)) ok = false;
    if (!guest_remove_if_present(paths->journal_tmp)) ok = false;
    if (!guest_remove_if_present(paths->marker_tmp)) ok = false;
    if (!guest_remove_if_present(paths->next)) ok = false;
    if (!guest_remove_directory_if_present(paths->stage)) ok = false;
    if (!guest_sync_directory(paths->work)) ok = false;
    return ok;
}

static vm_guest_install_status_t
guest_continue(const guest_paths_t *paths,
               const guest_transaction_spec_t *spec,
               const uint8_t digest[VM_GUEST_INSTALL_SHA256_SIZE],
               vm_guest_install_result_t *result,
               char *detail, size_t detail_capacity) {
    guest_node_t live = guest_node(paths->live);
    guest_node_t backup = guest_node(paths->backup);
    guest_node_t next = guest_node(paths->next);
    if ((live != GUEST_NODE_ABSENT && live != GUEST_NODE_REGULAR) ||
        (backup != GUEST_NODE_ABSENT && backup != GUEST_NODE_REGULAR) ||
        (next != GUEST_NODE_ABSENT && next != GUEST_NODE_REGULAR)) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk replacement contains an invalid file type or empty image.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }

    bool have_live = live == GUEST_NODE_REGULAR;
    bool have_backup = backup == GUEST_NODE_REGULAR;
    bool have_next = next == GUEST_NODE_REGULAR;
    bool resume_invalidated = false;

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
        if (!guest_invalidate_resume(paths)) {
            guest_detail(detail, detail_capacity,
                         "The stale automatic-resume request could not be cleared after installing the prepared guest disk.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
        resume_invalidated = true;
        if (guest_test_interrupt(3u))
            return VM_GUEST_INSTALL_ERR_INTERRUPTED;
    }

    if (have_live && !have_next && have_backup) {
        if (!resume_invalidated && !guest_invalidate_resume(paths)) {
            guest_detail(detail, detail_capacity,
                         "The installed guest disk is awaiting automatic-resume invalidation.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
        if (!guest_publish_record(paths->marker_tmp, paths->marker,
                                  paths->work, spec->marker_prefix,
                                  digest)) {
            guest_detail(detail, detail_capacity,
                         "The new guest disk is installed, but its commit record could not be published.");
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
        bool cleaned = guest_finish_committed_cleanup(paths, spec);
        if (result) result->cleanup_complete = cleaned;
        if (!cleaned)
            guest_detail(detail, detail_capacity,
                         "The guest-disk replacement is committed, but inert transaction files could not all be removed.");
        return VM_GUEST_INSTALL_OK;
    }

    if (!have_live && !have_next && have_backup) {
        if (!guest_rename_new(paths->backup, paths->live) ||
            !guest_sync_directory(paths->work)) {
            guest_detail(detail, detail_capacity,
                         "The interrupted guest-disk replacement could not restore the original disk.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
        if (result) result->rolled_back = true;
        bool cleaned = guest_remove_rolled_back_artifacts(paths);
        if (result) result->cleanup_complete = cleaned;
        guest_detail(detail, detail_capacity,
                     cleaned
                         ? "An incomplete guest-disk replacement was rolled back to the original disk."
                         : "The original guest disk was restored, but inert transaction files remain.");
        return VM_GUEST_INSTALL_OK;
    }

    guest_detail(detail, detail_capacity,
                 "The guest-disk replacement transaction is contradictory; no file was changed.");
    return VM_GUEST_INSTALL_ERR_STATE;
}

static vm_guest_install_status_t
guest_recover_for(const char *work_directory,
                  const guest_transaction_spec_t *spec,
                  vm_guest_install_result_t *result,
                  char *detail, size_t detail_capacity) {
    guest_result_clear(result);
    guest_detail(detail, detail_capacity, "");
    guest_paths_t paths;
    if (!guest_paths_init_for(&paths, work_directory, spec)) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk transaction path is too long to use.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }
    if (!guest_work_directory_ok(&paths)) {
        guest_detail(detail, detail_capacity,
                     "The machine work directory is missing or is not a real directory.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }

    uint8_t marker_digest[VM_GUEST_INSTALL_SHA256_SIZE];
    vm_guest_install_probe_t marker =
        guest_record_probe(paths.marker, spec->marker_prefix,
                           marker_digest);
    if (marker == VM_GUEST_INSTALL_PROBE_INVALID) {
        guest_named_detail(detail, detail_capacity, spec,
                           " record is malformed; no recovery was attempted.");
        return VM_GUEST_INSTALL_ERR_RECORD;
    }
    if (marker == VM_GUEST_INSTALL_PROBE_IO_ERROR) {
        guest_named_detail(detail, detail_capacity, spec,
                           " record could not be read safely.");
        return VM_GUEST_INSTALL_ERR_IO;
    }

    uint8_t journal_digest[VM_GUEST_INSTALL_SHA256_SIZE];
    vm_guest_install_probe_t journal =
        guest_record_probe(paths.journal, spec->journal_prefix,
                           journal_digest);
    if (journal == VM_GUEST_INSTALL_PROBE_INVALID) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk recovery journal is malformed; no disk was changed.");
        return VM_GUEST_INSTALL_ERR_RECORD;
    }
    if (journal == VM_GUEST_INSTALL_PROBE_IO_ERROR) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk recovery journal could not be read safely.");
        return VM_GUEST_INSTALL_ERR_IO;
    }

    if (marker == VM_GUEST_INSTALL_PROBE_VALID) {
        if (guest_node(paths.live) != GUEST_NODE_REGULAR) {
            guest_detail(detail, detail_capacity,
                         "A guest-disk record exists without a valid live guest disk.");
            return VM_GUEST_INSTALL_ERR_STATE;
        }
        if (journal == VM_GUEST_INSTALL_PROBE_VALID &&
            !guest_digest_equal(marker_digest, journal_digest)) {
            guest_detail(detail, detail_capacity,
                         "The committed guest-disk record and recovery journal name different identities.");
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
        bool cleaned = guest_finish_committed_cleanup(&paths, spec);
        if (result) result->cleanup_complete = cleaned;
        if (!cleaned)
            guest_detail(detail, detail_capacity,
                         "The guest-disk replacement is committed, but inert transaction files could not all be removed.");
        return VM_GUEST_INSTALL_OK;
    }

    if (journal == VM_GUEST_INSTALL_PROBE_VALID)
        return guest_continue(&paths, spec, journal_digest, result,
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
vm_guest_install_recover(const char *work_directory,
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity) {
    return guest_recover_for(work_directory, &VM_GUEST_INSTALL_SPEC, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_storage_recover(const char *work_directory,
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity) {
    return guest_recover_for(work_directory, &VM_GUEST_STORAGE_SPEC, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_privilege_recover(const char *work_directory,
                           vm_guest_install_result_t *result,
                           char *detail, size_t detail_capacity) {
    return guest_recover_for(work_directory, &VM_GUEST_PRIVILEGE_SPEC, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_sources_recover(const char *work_directory,
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity) {
    return guest_recover_for(work_directory, &VM_GUEST_SOURCES_SPEC, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_sources_v2_recover(const char *work_directory,
                            vm_guest_install_result_t *result,
                            char *detail, size_t detail_capacity) {
    return guest_recover_for(work_directory, &VM_GUEST_SOURCES_V2_SPEC,
                             result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_apt_trust_recover(const char *work_directory,
                           vm_guest_install_result_t *result,
                           char *detail, size_t detail_capacity) {
    return guest_recover_for(work_directory, &VM_GUEST_APT_TRUST_SPEC,
                             result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_recovery_recover(const char *work_directory,
                          vm_guest_install_result_t *result,
                          char *detail, size_t detail_capacity) {
    return guest_recover_for(work_directory, &VM_GUEST_RECOVERY_SPEC, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_maintenance_recover(const char *work_directory,
                             vm_guest_install_result_t *privilege_result,
                             vm_guest_install_result_t *storage_result,
                             vm_guest_install_result_t *sources_result,
                             char *detail, size_t detail_capacity) {
    enum { MAINTENANCE_COUNT = 6 };
    const guest_transaction_spec_t *specs[MAINTENANCE_COUNT] = {
        &VM_GUEST_RECOVERY_SPEC,
        &VM_GUEST_PRIVILEGE_SPEC,
        &VM_GUEST_STORAGE_SPEC,
        &VM_GUEST_SOURCES_SPEC,
        &VM_GUEST_SOURCES_V2_SPEC,
        &VM_GUEST_APT_TRUST_SPEC
    };
    guest_paths_t paths[MAINTENANCE_COUNT];
    vm_guest_install_result_t local[MAINTENANCE_COUNT];
    vm_guest_install_result_t *results[MAINTENANCE_COUNT] = {
        &local[0],
        privilege_result ? privilege_result : &local[1],
        storage_result ? storage_result : &local[2],
        sources_result ? sources_result : &local[3],
        &local[4],
        &local[5]
    };
    bool journal[MAINTENANCE_COUNT] = {false};
    size_t owner = MAINTENANCE_COUNT;
    size_t owner_count = 0u;

    for (size_t i = 0u; i < MAINTENANCE_COUNT; i++)
        guest_result_clear(results[i]);
    guest_detail(detail, detail_capacity, "");
    for (size_t i = 0u; i < MAINTENANCE_COUNT; i++) {
        if (!guest_paths_init_for(&paths[i], work_directory, specs[i])) {
            guest_detail(detail, detail_capacity,
                         "A guest-maintenance recovery path is too long to use.");
            return VM_GUEST_INSTALL_ERR_PATH;
        }
        journal[i] = guest_node(paths[i].journal) != GUEST_NODE_ABSENT;
        if (journal[i]) {
            owner = i;
            owner_count++;
        }
    }
    if (owner_count > 1u) {
        guest_detail(detail, detail_capacity,
                     "Multiple guest-disk maintenance journals claim the shared live disk; none was guessed through.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }

    /* A journal names the transaction that is genuinely in flight. A backup
     * without a journal can instead be inert cleanup residue from an already
     * committed marker, so it must not steal priority from that journal. Only
     * when the shared live name is absent and there is no journal does one
     * lone orphan backup identify the recovery owner. */
    if (owner_count == 0u &&
        guest_node(paths[0].live) == GUEST_NODE_ABSENT) {
        for (size_t i = 0u; i < MAINTENANCE_COUNT; i++) {
            if (guest_node(paths[i].backup) != GUEST_NODE_ABSENT) {
                owner = i;
                owner_count++;
            }
        }
        if (owner_count > 1u) {
            guest_detail(detail, detail_capacity,
                         "Multiple orphaned guest-disk maintenance backups exist without a live disk; none was guessed through.");
            return VM_GUEST_INSTALL_ERR_STATE;
        }
    }

    if (owner < MAINTENANCE_COUNT) {
        vm_guest_install_status_t status = guest_recover_for(
            work_directory, specs[owner], results[owner],
            detail, detail_capacity);
        if (status != VM_GUEST_INSTALL_OK) return status;
    }
    for (size_t i = 0u; i < MAINTENANCE_COUNT; i++) {
        if (i == owner) continue;
        vm_guest_install_status_t status = guest_recover_for(
            work_directory, specs[i], results[i],
            detail, detail_capacity);
        if (status != VM_GUEST_INSTALL_OK) return status;
    }
    return VM_GUEST_INSTALL_OK;
}

static vm_guest_install_status_t
guest_prepare_stage_for(const char *work_directory,
                        const guest_transaction_spec_t *spec,
                        vm_guest_install_result_t *result,
                        char *detail, size_t detail_capacity) {
    vm_guest_install_result_t local_result;
    vm_guest_install_result_t *recovered = result ? result : &local_result;
    vm_guest_install_status_t status = guest_recover_for(
        work_directory, spec, recovered, detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK || recovered->committed) return status;

    guest_paths_t paths;
    if (!guest_paths_init_for(&paths, work_directory, spec)) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk stage path is too long to use.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }
    if (guest_node(paths.live) != GUEST_NODE_REGULAR ||
        guest_node(paths.backup) != GUEST_NODE_ABSENT) {
        guest_detail(detail, detail_capacity,
                     "Preparing an install needs one live guest disk and no backup.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }

    const char *partials[] = {paths.marker_tmp, paths.journal_tmp};
    for (size_t i = 0u; i < sizeof partials / sizeof partials[0]; i++) {
        guest_node_t node = guest_node(partials[i]);
        if (node == GUEST_NODE_REGULAR || node == GUEST_NODE_EMPTY) {
            if (!guest_remove_if_present(partials[i])) {
                guest_detail(detail, detail_capacity,
                             "An inert guest-disk partial record could not be removed.");
                return VM_GUEST_INSTALL_ERR_IO;
            }
        } else if (node != GUEST_NODE_ABSENT) {
            guest_detail(detail, detail_capacity,
                         "A guest-disk partial record has an unsafe file type.");
            return node == GUEST_NODE_IO_ERROR ? VM_GUEST_INSTALL_ERR_IO
                                               : VM_GUEST_INSTALL_ERR_STATE;
        }
    }

    guest_node_t stage = guest_node(paths.stage);
    if (stage == GUEST_NODE_DIRECTORY) {
        guest_node_t next = guest_node(paths.next);
        if (next == GUEST_NODE_REGULAR || next == GUEST_NODE_EMPTY) {
            if (!guest_remove_if_present(paths.next)) {
                guest_detail(detail, detail_capacity,
                             "The inert staged guest disk could not be removed.");
                return VM_GUEST_INSTALL_ERR_IO;
            }
        } else if (next != GUEST_NODE_ABSENT) {
            guest_detail(detail, detail_capacity,
                         "The staged guest-disk path has an unsafe file type.");
            return next == GUEST_NODE_IO_ERROR ? VM_GUEST_INSTALL_ERR_IO
                                               : VM_GUEST_INSTALL_ERR_STATE;
        }
        if (!guest_remove_directory_if_present(paths.stage)) {
            if (errno == ENOTEMPTY || errno == EEXIST) {
                guest_detail(detail, detail_capacity,
                             "The guest-disk stage contains unexpected files and was not replaced.");
                return VM_GUEST_INSTALL_ERR_STATE;
            }
            guest_detail(detail, detail_capacity,
                         "The inert guest-disk stage could not be removed.");
            return VM_GUEST_INSTALL_ERR_IO;
        }
    } else if (stage != GUEST_NODE_ABSENT) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk stage is not a real directory.");
        return stage == GUEST_NODE_IO_ERROR ? VM_GUEST_INSTALL_ERR_IO
                                            : VM_GUEST_INSTALL_ERR_STATE;
    }

    if (!guest_make_directory(paths.stage) ||
        !guest_sync_directory(paths.work)) {
        (void)guest_remove_directory_if_present(paths.stage);
        guest_detail(detail, detail_capacity,
                     "The empty guest-disk stage could not be created durably.");
        return VM_GUEST_INSTALL_ERR_IO;
    }
    if (guest_node(paths.stage) != GUEST_NODE_DIRECTORY ||
        guest_node(paths.next) != GUEST_NODE_ABSENT) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk stage changed while it was being prepared.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    guest_detail(detail, detail_capacity, "");
    return VM_GUEST_INSTALL_OK;
}

vm_guest_install_status_t
vm_guest_install_prepare_stage(const char *work_directory,
                               vm_guest_install_result_t *result,
                               char *detail, size_t detail_capacity) {
    return guest_prepare_stage_for(work_directory, &VM_GUEST_INSTALL_SPEC,
                                   result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_storage_prepare_stage(const char *work_directory,
                               vm_guest_install_result_t *result,
                               char *detail, size_t detail_capacity) {
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;
    vm_guest_install_result_t sources;
    vm_guest_install_status_t status = vm_guest_maintenance_recover(
        work_directory, &privilege, &storage, &sources,
        detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK)
        return status;
    if ((privilege.committed || privilege.rolled_back) &&
        !privilege.cleanup_complete) {
        guest_detail(detail, detail_capacity,
                     "Cydia privilege-repair residue must be cleaned before storage maintenance starts.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if ((sources.committed || sources.rolled_back) &&
        !sources.cleanup_complete) {
        guest_detail(detail, detail_capacity,
                     "Cydia source-maintenance residue must be cleaned before storage maintenance starts.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if (storage.committed) {
        if (result) *result = storage;
        return VM_GUEST_INSTALL_OK;
    }
    return guest_prepare_stage_for(work_directory, &VM_GUEST_STORAGE_SPEC,
                                   result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_privilege_prepare_stage(const char *work_directory,
                                 vm_guest_install_result_t *result,
                                 char *detail, size_t detail_capacity) {
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;
    vm_guest_install_result_t sources;
    vm_guest_install_status_t status = vm_guest_maintenance_recover(
        work_directory, &privilege, &storage, &sources,
        detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK)
        return status;
    if ((storage.committed || storage.rolled_back) &&
        !storage.cleanup_complete) {
        guest_detail(detail, detail_capacity,
                     "Storage-maintenance residue must be cleaned before the Cydia privilege repair starts.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if ((sources.committed || sources.rolled_back) &&
        !sources.cleanup_complete) {
        guest_detail(detail, detail_capacity,
                     "Cydia source-maintenance residue must be cleaned before the Cydia privilege repair starts.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if (privilege.committed) {
        if (result) *result = privilege;
        return VM_GUEST_INSTALL_OK;
    }
    return guest_prepare_stage_for(work_directory, &VM_GUEST_PRIVILEGE_SPEC,
                                   result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_sources_prepare_stage(const char *work_directory,
                               vm_guest_install_result_t *result,
                               char *detail, size_t detail_capacity) {
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;
    vm_guest_install_result_t sources;
    vm_guest_install_status_t status = vm_guest_maintenance_recover(
        work_directory, &privilege, &storage, &sources,
        detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK)
        return status;
    if ((storage.committed || storage.rolled_back) &&
        !storage.cleanup_complete) {
        guest_detail(detail, detail_capacity,
                     "Storage-maintenance residue must be cleaned before Cydia source maintenance starts.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if ((privilege.committed || privilege.rolled_back) &&
        !privilege.cleanup_complete) {
        guest_detail(detail, detail_capacity,
                     "Cydia privilege-repair residue must be cleaned before Cydia source maintenance starts.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if (sources.committed) {
        if (result) *result = sources;
        return VM_GUEST_INSTALL_OK;
    }
    return guest_prepare_stage_for(work_directory, &VM_GUEST_SOURCES_SPEC,
                                   result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_sources_v2_prepare_stage(const char *work_directory,
                                  vm_guest_install_result_t *result,
                                  char *detail, size_t detail_capacity) {
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;
    vm_guest_install_result_t sources;
    vm_guest_install_result_t sources_v2;
    vm_guest_install_status_t status = vm_guest_maintenance_recover(
        work_directory, &privilege, &storage, &sources,
        detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK)
        return status;
    status = vm_guest_sources_v2_recover(
        work_directory, &sources_v2, detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK)
        return status;
    if (sources_v2.committed) {
        if (result) *result = sources_v2;
        return VM_GUEST_INSTALL_OK;
    }
    return guest_prepare_stage_for(work_directory, &VM_GUEST_SOURCES_V2_SPEC,
                                   result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_apt_trust_prepare_stage(const char *work_directory,
                                 vm_guest_install_result_t *result,
                                 char *detail, size_t detail_capacity) {
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;
    vm_guest_install_result_t sources;
    vm_guest_install_result_t apt_trust;
    vm_guest_install_status_t status = vm_guest_maintenance_recover(
        work_directory, &privilege, &storage, &sources,
        detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK)
        return status;
    status = vm_guest_apt_trust_recover(
        work_directory, &apt_trust, detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK)
        return status;
    if (apt_trust.committed) {
        if (result) *result = apt_trust;
        return VM_GUEST_INSTALL_OK;
    }
    return guest_prepare_stage_for(work_directory, &VM_GUEST_APT_TRUST_SPEC,
                                   result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_recovery_prepare_stage(const char *work_directory,
                                vm_guest_install_result_t *result,
                                char *detail, size_t detail_capacity) {
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;
    vm_guest_install_result_t sources;
    vm_guest_install_status_t status = vm_guest_maintenance_recover(
        work_directory, &privilege, &storage, &sources,
        detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK)
        return status;
    if (((privilege.committed || privilege.rolled_back) &&
         !privilege.cleanup_complete) ||
        ((storage.committed || storage.rolled_back) &&
         !storage.cleanup_complete) ||
        ((sources.committed || sources.rolled_back) &&
         !sources.cleanup_complete)) {
        guest_detail(detail, detail_capacity,
                     "Guest-maintenance residue must be cleaned before filesystem recovery starts.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    return guest_prepare_stage_for(work_directory, &VM_GUEST_RECOVERY_SPEC,
                                   result, detail, detail_capacity);
}

static bool guest_copy_new(const char *source, const char *destination) {
#if defined(__APPLE__)
    if (clonefile(source, destination, 0u) == 0) return true;
    (void)remove(destination);
    return false;
#else
    uint8_t *buffer = NULL;
    bool okay = false;
#ifdef _WIN32
    int input = _open(source, _O_RDONLY | _O_BINARY);
    int output = -1;
#else
    int input = open(source, O_RDONLY);
    int output = -1;
#endif
    if (input < 0) return false;
    buffer = (uint8_t *)malloc(VM_GUEST_RECOVERY_COPY_BUFFER);
    if (!buffer) goto done;
#ifdef _WIN32
    output = _open(destination,
                   _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
    output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
    if (output < 0) goto done;
    for (;;) {
#ifdef _WIN32
        int got = _read(input, buffer,
                        (unsigned)VM_GUEST_RECOVERY_COPY_BUFFER);
#else
        ssize_t got = read(input, buffer, VM_GUEST_RECOVERY_COPY_BUFFER);
#endif
        if (got > 0) {
            if (!guest_write_all(output, buffer, (size_t)got)) goto done;
            continue;
        }
        if (got == 0) break;
        if (errno != EINTR) goto done;
    }
#ifdef _WIN32
    if (_commit(output) != 0) goto done;
#else
    if (fsync(output) != 0) goto done;
#endif
    okay = true;

done:
    free(buffer);
#ifdef _WIN32
    if (output >= 0 && _close(output) != 0) okay = false;
    if (_close(input) != 0) okay = false;
#else
    if (output >= 0 && close(output) != 0) okay = false;
    if (close(input) != 0) okay = false;
#endif
    if (!okay) (void)remove(destination);
    return okay;
#endif
}

vm_guest_install_status_t
vm_guest_recovery_clone_live_to_stage(const char *work_directory,
                                      char *detail, size_t detail_capacity) {
    guest_paths_t paths;

    guest_detail(detail, detail_capacity, "");
    if (!guest_paths_init_for(&paths, work_directory,
                              &VM_GUEST_RECOVERY_SPEC)) {
        guest_detail(detail, detail_capacity,
                     "The filesystem-recovery path is too long to use.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }
    if (!guest_work_directory_ok(&paths) ||
        guest_node(paths.stage) != GUEST_NODE_DIRECTORY ||
        guest_node(paths.live) != GUEST_NODE_REGULAR ||
        guest_node(paths.next) != GUEST_NODE_ABSENT ||
        guest_node(paths.backup) != GUEST_NODE_ABSENT ||
        guest_node(paths.marker) != GUEST_NODE_ABSENT ||
        guest_node(paths.journal) != GUEST_NODE_ABSENT) {
        guest_detail(detail, detail_capacity,
                     "Filesystem recovery needs one live disk and one empty prepared stage.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if (!guest_copy_new(paths.live, paths.next) ||
        guest_node(paths.next) != GUEST_NODE_REGULAR ||
        !guest_sync_directory(paths.stage)) {
        (void)guest_remove_if_present(paths.next);
        guest_detail(detail, detail_capacity,
                     "The live guest disk could not be cloned into the recovery stage.");
        return VM_GUEST_INSTALL_ERR_IO;
    }
    return VM_GUEST_INSTALL_OK;
}

vm_guest_install_status_t
vm_guest_recovery_discard_stage(const char *work_directory,
                                char *detail, size_t detail_capacity) {
    guest_paths_t paths;

    guest_detail(detail, detail_capacity, "");
    if (!guest_paths_init_for(&paths, work_directory,
                              &VM_GUEST_RECOVERY_SPEC)) {
        guest_detail(detail, detail_capacity,
                     "The filesystem-recovery path is too long to use.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }
    if (guest_node(paths.journal) != GUEST_NODE_ABSENT ||
        guest_node(paths.backup) != GUEST_NODE_ABSENT ||
        guest_node(paths.marker) != GUEST_NODE_ABSENT) {
        guest_detail(detail, detail_capacity,
                     "A published filesystem-recovery transaction cannot be discarded as an inert stage.");
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    guest_node_t stage = guest_node(paths.stage);
    if (stage == GUEST_NODE_ABSENT) return VM_GUEST_INSTALL_OK;
    if (stage != GUEST_NODE_DIRECTORY) {
        guest_detail(detail, detail_capacity,
                     "The filesystem-recovery stage has an unsafe file type.");
        return stage == GUEST_NODE_IO_ERROR ? VM_GUEST_INSTALL_ERR_IO
                                            : VM_GUEST_INSTALL_ERR_STATE;
    }
    guest_node_t next = guest_node(paths.next);
    if (next != GUEST_NODE_ABSENT && next != GUEST_NODE_REGULAR &&
        next != GUEST_NODE_EMPTY) {
        guest_detail(detail, detail_capacity,
                     "The staged recovery disk has an unsafe file type.");
        return next == GUEST_NODE_IO_ERROR ? VM_GUEST_INSTALL_ERR_IO
                                           : VM_GUEST_INSTALL_ERR_STATE;
    }
    if (!guest_remove_if_present(paths.next) ||
        !guest_remove_directory_if_present(paths.stage) ||
        !guest_sync_directory(paths.work)) {
        guest_detail(detail, detail_capacity,
                     "The inert filesystem-recovery stage could not be removed durably.");
        return VM_GUEST_INSTALL_ERR_IO;
    }
    return VM_GUEST_INSTALL_OK;
}

static vm_guest_install_status_t
guest_publish_for(const char *work_directory,
                  const guest_transaction_spec_t *spec,
                  const uint8_t manifest_sha256[
                      VM_GUEST_INSTALL_SHA256_SIZE],
                  vm_guest_install_result_t *result,
                  char *detail, size_t detail_capacity) {
    guest_result_clear(result);
    guest_detail(detail, detail_capacity, "");
    if (!manifest_sha256) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk transaction identity is missing.");
        return VM_GUEST_INSTALL_ERR_ARGUMENT;
    }

    guest_paths_t paths;
    if (!guest_paths_init_for(&paths, work_directory, spec)) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk transaction path is too long to use.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }
    if (!guest_work_directory_ok(&paths)) {
        guest_detail(detail, detail_capacity,
                     "The machine work directory is missing or is not a real directory.");
        return VM_GUEST_INSTALL_ERR_PATH;
    }

    uint8_t existing_digest[VM_GUEST_INSTALL_SHA256_SIZE];
    vm_guest_install_probe_t marker =
        guest_record_probe(paths.marker, spec->marker_prefix,
                           existing_digest);
    if (marker == VM_GUEST_INSTALL_PROBE_VALID) {
        if (!guest_digest_equal(existing_digest, manifest_sha256)) {
            guest_detail(detail, detail_capacity,
                         "This machine already has a different committed guest-disk identity.");
            return VM_GUEST_INSTALL_ERR_STATE;
        }
        return guest_recover_for(work_directory, spec, result,
                                 detail, detail_capacity);
    }
    if (marker == VM_GUEST_INSTALL_PROBE_INVALID) {
        guest_detail(detail, detail_capacity,
                     "The existing guest-disk record is malformed; it was not overwritten.");
        return VM_GUEST_INSTALL_ERR_RECORD;
    }
    if (marker == VM_GUEST_INSTALL_PROBE_IO_ERROR) {
        guest_detail(detail, detail_capacity,
                     "The existing guest-disk record could not be read safely.");
        return VM_GUEST_INSTALL_ERR_IO;
    }

    vm_guest_install_probe_t journal =
        guest_record_probe(paths.journal, spec->journal_prefix,
                           existing_digest);
    if (journal == VM_GUEST_INSTALL_PROBE_VALID) {
        if (!guest_digest_equal(existing_digest, manifest_sha256)) {
            guest_detail(detail, detail_capacity,
                         "An unfinished guest-disk replacement names a different identity.");
            return VM_GUEST_INSTALL_ERR_STATE;
        }
        return guest_recover_for(work_directory, spec, result,
                                 detail, detail_capacity);
    }
    if (journal == VM_GUEST_INSTALL_PROBE_INVALID) {
        guest_detail(detail, detail_capacity,
                     "The existing guest-disk recovery journal is malformed.");
        return VM_GUEST_INSTALL_ERR_RECORD;
    }
    if (journal == VM_GUEST_INSTALL_PROBE_IO_ERROR) {
        guest_detail(detail, detail_capacity,
                     "The existing guest-disk recovery journal could not be read safely.");
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
                              spec->journal_prefix, manifest_sha256)) {
        guest_detail(detail, detail_capacity,
                     "The guest-disk recovery journal could not be published.");
        return VM_GUEST_INSTALL_ERR_IO;
    }
    if (guest_test_interrupt(1u))
        return VM_GUEST_INSTALL_ERR_INTERRUPTED;
    return guest_continue(&paths, spec, manifest_sha256, result,
                          detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_install_publish(const char *work_directory,
                         const uint8_t manifest_sha256[
                             VM_GUEST_INSTALL_SHA256_SIZE],
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity) {
    return guest_publish_for(work_directory, &VM_GUEST_INSTALL_SPEC,
                             manifest_sha256, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_storage_publish(const char *work_directory,
                         const uint8_t manifest_sha256[
                             VM_GUEST_INSTALL_SHA256_SIZE],
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity) {
    return guest_publish_for(work_directory, &VM_GUEST_STORAGE_SPEC,
                             manifest_sha256, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_privilege_publish(const char *work_directory,
                           const uint8_t manifest_sha256[
                               VM_GUEST_INSTALL_SHA256_SIZE],
                           vm_guest_install_result_t *result,
                           char *detail, size_t detail_capacity) {
    return guest_publish_for(work_directory, &VM_GUEST_PRIVILEGE_SPEC,
                             manifest_sha256, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_sources_publish(const char *work_directory,
                         const uint8_t manifest_sha256[
                             VM_GUEST_INSTALL_SHA256_SIZE],
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity) {
    return guest_publish_for(work_directory, &VM_GUEST_SOURCES_SPEC,
                             manifest_sha256, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_sources_v2_publish(const char *work_directory,
                            const uint8_t manifest_sha256[
                                VM_GUEST_INSTALL_SHA256_SIZE],
                            vm_guest_install_result_t *result,
                            char *detail, size_t detail_capacity) {
    return guest_publish_for(work_directory, &VM_GUEST_SOURCES_V2_SPEC,
                             manifest_sha256, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_apt_trust_publish(const char *work_directory,
                           const uint8_t manifest_sha256[
                               VM_GUEST_INSTALL_SHA256_SIZE],
                           vm_guest_install_result_t *result,
                           char *detail, size_t detail_capacity) {
    return guest_publish_for(work_directory, &VM_GUEST_APT_TRUST_SPEC,
                             manifest_sha256, result,
                             detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_recovery_publish(const char *work_directory,
                          vm_guest_install_result_t *result,
                          char *detail, size_t detail_capacity) {
    return guest_publish_for(work_directory, &VM_GUEST_RECOVERY_SPEC,
                             VM_GUEST_RECOVERY_IDENTITY, result,
                             detail, detail_capacity);
}

static vm_guest_install_status_t
guest_confirm_for(const char *work_directory,
                  const guest_transaction_spec_t *spec,
                  const char *label,
                  const uint8_t manifest_sha256[
                      VM_GUEST_INSTALL_SHA256_SIZE],
                  vm_guest_install_result_t *result,
                  char *detail, size_t detail_capacity) {
    vm_guest_install_result_t recovered;
    guest_paths_t paths;
    guest_paths_t install_paths;
    uint8_t install_digest[VM_GUEST_INSTALL_SHA256_SIZE];

    guest_result_clear(result);
    guest_detail(detail, detail_capacity, "");
    if (!spec || !label || !manifest_sha256) {
        if (detail && detail_capacity) {
            (void)snprintf(detail, detail_capacity,
                           "The %s identity is missing.",
                           label ? label : "guest-maintenance");
            detail[detail_capacity - 1u] = '\0';
        }
        return VM_GUEST_INSTALL_ERR_ARGUMENT;
    }
    vm_guest_install_status_t status = guest_recover_for(
        work_directory, spec, &recovered,
        detail, detail_capacity);
    if (status != VM_GUEST_INSTALL_OK)
        return status;
    if (!guest_paths_init_for(&paths, work_directory, spec) ||
        !guest_paths_init_for(&install_paths, work_directory,
                              &VM_GUEST_INSTALL_SPEC)) {
        if (detail && detail_capacity) {
            (void)snprintf(detail, detail_capacity,
                           "The %s path is too long to use.", label);
            detail[detail_capacity - 1u] = '\0';
        }
        return VM_GUEST_INSTALL_ERR_PATH;
    }
    vm_guest_install_probe_t install = guest_record_probe(
        install_paths.marker, VM_GUEST_MARKER_PREFIX, install_digest);
    if (install == VM_GUEST_INSTALL_PROBE_INVALID) {
        guest_detail(detail, detail_capacity,
                     "The guest-install record is malformed; the repair was not recorded.");
        return VM_GUEST_INSTALL_ERR_RECORD;
    }
    if (install == VM_GUEST_INSTALL_PROBE_IO_ERROR) {
        guest_detail(detail, detail_capacity,
                     "The guest-install record could not be read safely.");
        return VM_GUEST_INSTALL_ERR_IO;
    }
    if (install != VM_GUEST_INSTALL_PROBE_VALID ||
        !guest_digest_equal(install_digest, manifest_sha256)) {
        if (detail && detail_capacity) {
            (void)snprintf(detail, detail_capacity,
                           "The %s does not match this committed guest installation.",
                           label);
            detail[detail_capacity - 1u] = '\0';
        }
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if (recovered.committed) {
        if (!recovered.has_manifest ||
            !guest_digest_equal(recovered.manifest_sha256,
                                manifest_sha256)) {
            if (detail && detail_capacity) {
                (void)snprintf(
                    detail, detail_capacity,
                    "This machine already has a different %s identity.",
                    label);
                detail[detail_capacity - 1u] = '\0';
            }
            return VM_GUEST_INSTALL_ERR_STATE;
        }
        if (result) *result = recovered;
        return VM_GUEST_INSTALL_OK;
    }
    if (guest_node(paths.live) != GUEST_NODE_REGULAR ||
        guest_node(paths.marker) != GUEST_NODE_ABSENT ||
        guest_node(paths.backup) != GUEST_NODE_ABSENT ||
        guest_node(paths.stage) != GUEST_NODE_ABSENT ||
        guest_node(paths.journal) != GUEST_NODE_ABSENT ||
        guest_node(paths.marker_tmp) != GUEST_NODE_ABSENT ||
        guest_node(paths.journal_tmp) != GUEST_NODE_ABSENT) {
        if (detail && detail_capacity) {
            (void)snprintf(
                detail, detail_capacity,
                "The %s namespace is not clean enough to confirm.", label);
            detail[detail_capacity - 1u] = '\0';
        }
        return VM_GUEST_INSTALL_ERR_STATE;
    }
    if (!guest_publish_record(paths.marker_tmp, paths.marker, paths.work,
                              spec->marker_prefix,
                              manifest_sha256)) {
        if (detail && detail_capacity) {
            (void)snprintf(detail, detail_capacity,
                           "The %s record could not be published.", label);
            detail[detail_capacity - 1u] = '\0';
        }
        return VM_GUEST_INSTALL_ERR_IO;
    }
    if (result) {
        result->committed = true;
        result->cleanup_complete = true;
        result->has_manifest = true;
        memcpy(result->manifest_sha256, manifest_sha256,
               VM_GUEST_INSTALL_SHA256_SIZE);
    }
    return VM_GUEST_INSTALL_OK;
}

vm_guest_install_status_t
vm_guest_privilege_confirm(const char *work_directory,
                           const uint8_t manifest_sha256[
                               VM_GUEST_INSTALL_SHA256_SIZE],
                           vm_guest_install_result_t *result,
                           char *detail, size_t detail_capacity) {
    return guest_confirm_for(work_directory, &VM_GUEST_PRIVILEGE_SPEC,
                             "Cydia privilege repair", manifest_sha256,
                             result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_sources_confirm(const char *work_directory,
                         const uint8_t manifest_sha256[
                             VM_GUEST_INSTALL_SHA256_SIZE],
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity) {
    return guest_confirm_for(work_directory, &VM_GUEST_SOURCES_SPEC,
                             "Cydia source maintenance", manifest_sha256,
                             result, detail, detail_capacity);
}

vm_guest_install_status_t
vm_guest_sources_v2_confirm(const char *work_directory,
                            const uint8_t manifest_sha256[
                                VM_GUEST_INSTALL_SHA256_SIZE],
                            vm_guest_install_result_t *result,
                            char *detail, size_t detail_capacity) {
    return guest_confirm_for(work_directory, &VM_GUEST_SOURCES_V2_SPEC,
                             "Cydia repository-cache maintenance",
                             manifest_sha256, result, detail,
                             detail_capacity);
}

vm_guest_install_status_t
vm_guest_apt_trust_confirm(const char *work_directory,
                           const uint8_t manifest_sha256[
                               VM_GUEST_INSTALL_SHA256_SIZE],
                           vm_guest_install_result_t *result,
                           char *detail, size_t detail_capacity) {
    return guest_confirm_for(work_directory, &VM_GUEST_APT_TRUST_SPEC,
                             "APT trust-store maintenance",
                             manifest_sha256, result, detail,
                             detail_capacity);
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
