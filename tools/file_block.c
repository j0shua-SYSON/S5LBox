/*
 * S5LBox -- bounded descriptor-backed adapter for a writable vm_block_t.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#define _POSIX_C_SOURCE 200809L
#endif

#include "file_block.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

/* Older MSVC errno.h versions omit POSIX EOVERFLOW. */
#ifdef EOVERFLOW
#define FILE_BLOCK_OVERFLOW_ERROR EOVERFLOW
#else
#define FILE_BLOCK_OVERFLOW_ERROR ERANGE
#endif

typedef struct file_block_session file_block_session_t;

#define FILE_BLOCK_READ_CACHE_WAYS UINT32_C(4)
#define FILE_BLOCK_READ_CACHE_ENTRY_COUNT \
    (FILE_BLOCK_READ_CACHE_BYTES / FILE_BLOCK_READ_CACHE_LINE_BYTES)
#define FILE_BLOCK_READ_CACHE_SET_COUNT \
    (FILE_BLOCK_READ_CACHE_ENTRY_COUNT / FILE_BLOCK_READ_CACHE_WAYS)

typedef struct {
    uint64_t offset;
    uint64_t last_use;
    size_t valid_bytes;
    bool valid;
} file_block_read_cache_entry_t;

#ifdef _WIN32
typedef struct {
    DWORD volume_serial;
    DWORD file_index_high;
    DWORD file_index_low;
} file_block_identity_t;
#else
typedef struct {
    dev_t device;
    ino_t inode;
} file_block_identity_t;
#endif

typedef enum {
    SESSION_VALIDATION_OK = 0,
    SESSION_VALIDATION_RETRY,
    SESSION_VALIDATION_ERROR
} session_validation_t;

struct file_block {
    file_block_session_t *current;
    file_block_session_t *retired;
    int last_system_error;
    bool read_cache_enabled;
};

struct file_block_session {
    file_block_t *owner;
    file_block_session_t *next;
    vm_block_t block;
    uint64_t expected_size;
    int descriptor;
    int last_system_error;
    bool active;
    bool sticky_error;
    file_block_identity_t initial_identity;
    uint8_t *read_cache_data;
    file_block_read_cache_entry_t
        read_cache_entries[FILE_BLOCK_READ_CACHE_ENTRY_COUNT];
    uint64_t read_cache_clock;
    bool read_cache_enabled;
    file_block_read_cache_stats_t read_cache_stats;
};

#ifdef _WIN32
typedef struct _stat64 file_block_stat_t;

static bool host_offset_supported(uint64_t offset) {
    return offset <= (uint64_t)INT64_MAX;
}

static int host_open_exclusive(const char *path, int *error_out) {
    int descriptor = -1;
    int flags = _O_RDWR | _O_BINARY | _O_NOINHERIT;
    unsigned retry_count = 0;

    for (;;) {
        int saved_error;

        descriptor = -1;
#ifdef _MSC_VER
        {
            errno_t open_error = _sopen_s(&descriptor, path, flags,
                                          _SH_DENYRW,
                                          _S_IREAD | _S_IWRITE);
            if (open_error == 0) {
                *error_out = 0;
                return descriptor;
            }
            saved_error = (int)open_error;
        }
#else
        errno = 0;
        descriptor = _sopen(path, flags, _SH_DENYRW,
                            _S_IREAD | _S_IWRITE);
        if (descriptor >= 0) {
            *error_out = 0;
            return descriptor;
        }
        saved_error = errno;
#endif
        retry_count++;
        if (saved_error != EINTR ||
            retry_count >= VM_BLOCK_NO_PROGRESS_LIMIT) {
            *error_out = saved_error != 0 ? saved_error : EIO;
            return -1;
        }
    }
}

static int host_fstat(int descriptor, file_block_stat_t *status) {
    return _fstat64(descriptor, status);
}

static bool host_is_regular(const file_block_stat_t *status) {
    return (status->st_mode & _S_IFMT) == _S_IFREG;
}

static int host_sync(int descriptor) {
    return _commit(descriptor);
}

static int host_close(int descriptor) {
    return _close(descriptor);
}

static int windows_system_error(void) {
    DWORD error_number = GetLastError();

    if (error_number == ERROR_SUCCESS || error_number > (DWORD)INT_MAX)
        return EIO;
    return (int)error_number;
}

static bool host_capture_identity(int descriptor,
                                  const file_block_stat_t *status,
                                  file_block_identity_t *identity,
                                  bool *has_links,
                                  int *error_out) {
    BY_HANDLE_FILE_INFORMATION information;
    intptr_t native_handle;

    (void)status;
    errno = 0;
    native_handle = _get_osfhandle(descriptor);
    if (native_handle == (intptr_t)-1) {
        *error_out = errno != 0 ? errno : EBADF;
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    if (!GetFileInformationByHandle((HANDLE)native_handle, &information)) {
        *error_out = windows_system_error();
        return false;
    }

    identity->volume_serial = information.dwVolumeSerialNumber;
    identity->file_index_high = information.nFileIndexHigh;
    identity->file_index_low = information.nFileIndexLow;
    *has_links = information.nNumberOfLinks != 0;
    *error_out = 0;
    return true;
}
#else
typedef struct stat file_block_stat_t;

static bool host_offset_supported(uint64_t offset) {
    off_t converted = (off_t)offset;

    return converted >= (off_t)0 && (uint64_t)converted == offset;
}

static int posix_open_bounded(const char *path, int flags, int *error_out) {
    unsigned retry_count = 0;

    for (;;) {
        int descriptor;
        int saved_error;

        errno = 0;
        descriptor = open(path, flags);
        if (descriptor >= 0) {
            *error_out = 0;
            return descriptor;
        }
        saved_error = errno;
        retry_count++;
        if (saved_error != EINTR ||
            retry_count >= VM_BLOCK_NO_PROGRESS_LIMIT) {
            *error_out = saved_error != 0 ? saved_error : EIO;
            return -1;
        }
    }
}

static int set_close_on_exec(int descriptor) {
    int flags;
    unsigned retry_count;

    retry_count = 0;
    do {
        errno = 0;
        flags = fcntl(descriptor, F_GETFD);
        if (flags >= 0)
            break;
        retry_count++;
    } while (errno == EINTR && retry_count < VM_BLOCK_NO_PROGRESS_LIMIT);
    if (flags < 0)
        return -1;
    if ((flags & FD_CLOEXEC) != 0)
        return 0;

    retry_count = 0;
    for (;;) {
        int result;

        errno = 0;
        result = fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
        if (result == 0)
            return 0;
        retry_count++;
        if (errno != EINTR || retry_count >= VM_BLOCK_NO_PROGRESS_LIMIT)
            return -1;
    }
}

static int host_open_exclusive(const char *path, int *error_out) {
    int descriptor;
    int saved_error;

#ifdef O_CLOEXEC
    descriptor = posix_open_bounded(path, O_RDWR | O_CLOEXEC, &saved_error);
    if (descriptor >= 0) {
        *error_out = 0;
        return descriptor;
    }
    if (saved_error != EINVAL) {
        *error_out = saved_error;
        return -1;
    }
#endif

    descriptor = posix_open_bounded(path, O_RDWR, &saved_error);
    if (descriptor < 0) {
        *error_out = saved_error;
        return -1;
    }
    if (set_close_on_exec(descriptor) != 0) {
        saved_error = errno;
        (void)close(descriptor);
        *error_out = saved_error;
        return -1;
    }
    *error_out = 0;
    return descriptor;
}

static int host_fstat(int descriptor, file_block_stat_t *status) {
    return fstat(descriptor, status);
}

static bool host_is_regular(const file_block_stat_t *status) {
    return S_ISREG(status->st_mode);
}

static int host_lock_whole_file(int descriptor) {
    struct flock lock;
    unsigned retry_count = 0;

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = (off_t)0;
    lock.l_len = (off_t)0;
    lock.l_pid = (pid_t)0;
    for (;;) {
        int result;

        errno = 0;
        result = fcntl(descriptor, F_SETLK, &lock);
        if (result == 0)
            return 0;
        retry_count++;
        if (errno != EINTR || retry_count >= VM_BLOCK_NO_PROGRESS_LIMIT)
            return -1;
    }
}

static int host_sync(int descriptor) {
    return fsync(descriptor);
}

static int host_close(int descriptor) {
    return close(descriptor);
}

static bool host_capture_identity(int descriptor,
                                  const file_block_stat_t *status,
                                  file_block_identity_t *identity,
                                  bool *has_links,
                                  int *error_out) {
    (void)descriptor;
    identity->device = status->st_dev;
    identity->inode = status->st_ino;
    *has_links = status->st_nlink != 0;
    *error_out = 0;
    return true;
}
#endif

static int host_fstat_bounded(int descriptor,
                              file_block_stat_t *status,
                              int *error_out) {
    unsigned retry_count = 0;

    for (;;) {
        int saved_error;

        errno = 0;
        if (host_fstat(descriptor, status) == 0) {
            *error_out = 0;
            return 0;
        }
        saved_error = errno;
        retry_count++;
        if (saved_error != EINTR ||
            retry_count >= VM_BLOCK_NO_PROGRESS_LIMIT) {
            *error_out = saved_error != 0 ? saved_error : EIO;
            return -1;
        }
    }
}

static int synthesized_error(int error_number) {
    return error_number != 0 ? error_number : EIO;
}

static void adapter_remember_error(file_block_t *adapter, int error_number) {
    if (adapter)
        adapter->last_system_error = synthesized_error(error_number);
}

static void session_remember_error(file_block_session_t *session,
                                   int error_number) {
    int saved_error;

    if (!session)
        return;
    saved_error = synthesized_error(error_number);
    session->sticky_error = true;
    if (session->last_system_error == 0)
        session->last_system_error = saved_error;
    if (session->owner)
        session->owner->last_system_error = session->last_system_error;
}

static bool identities_equal(const file_block_identity_t *left,
                             const file_block_identity_t *right) {
#ifdef _WIN32
    return left->volume_serial == right->volume_serial &&
           left->file_index_high == right->file_index_high &&
           left->file_index_low == right->file_index_low;
#else
    return left->device == right->device && left->inode == right->inode;
#endif
}

static bool stat_has_expected_size(const file_block_session_t *session,
                                   const file_block_stat_t *status) {
    if (status->st_size < 0)
        return false;
    return (uint64_t)status->st_size == session->expected_size;
}

static session_validation_t session_revalidate(file_block_session_t *session) {
    file_block_identity_t identity;
    file_block_stat_t status;
    bool has_links;
    int saved_error;

    if (!session || !session->active || session->descriptor < 0)
        return SESSION_VALIDATION_ERROR;

    errno = 0;
    if (host_fstat(session->descriptor, &status) != 0) {
        saved_error = errno;
        if (saved_error == EINTR)
            return SESSION_VALIDATION_RETRY;
        session_remember_error(session, saved_error);
        return SESSION_VALIDATION_ERROR;
    }
    if (!host_is_regular(&status) || !stat_has_expected_size(session, &status)) {
        session_remember_error(session, EIO);
        return SESSION_VALIDATION_ERROR;
    }
    if (!host_capture_identity(session->descriptor, &status, &identity,
                               &has_links, &saved_error)) {
        session_remember_error(session, saved_error);
        return SESSION_VALIDATION_ERROR;
    }
    if (!has_links ||
        !identities_equal(&identity, &session->initial_identity)) {
        session_remember_error(session, EIO);
        return SESSION_VALIDATION_ERROR;
    }
    return SESSION_VALIDATION_OK;
}

static bool callback_request_valid(const file_block_session_t *session,
                                   uint64_t offset,
                                   size_t requested) {
    uint64_t length;

    if (!session || requested > (size_t)VM_BLOCK_MAX_CALLBACK_BYTES)
        return false;
#ifndef _WIN32
    if (requested > (size_t)SSIZE_MAX)
        return false;
#endif
    length = (uint64_t)requested;
    if (offset > session->expected_size ||
        length > session->expected_size - offset)
        return false;
    return host_offset_supported(offset);
}

#ifdef _WIN32
static vm_block_io_status_t windows_seek(file_block_session_t *session,
                                         uint64_t offset) {
    __int64 position;
    int saved_error;

    errno = 0;
    /* The CRT documents zero as the absolute-file-position origin. */
    position = _lseeki64(session->descriptor, (__int64)offset, 0);
    if (position >= 0)
        return VM_BLOCK_IO_OK;
    saved_error = errno;
    if (saved_error == EINTR)
        return VM_BLOCK_IO_RETRY;
    session_remember_error(session, saved_error);
    return VM_BLOCK_IO_ERROR;
}
#endif

static void counter_add_u64(uint64_t *counter, uint64_t amount) {
    if (UINT64_MAX - *counter < amount)
        *counter = UINT64_MAX;
    else
        *counter += amount;
}

static void read_cache_touch(file_block_session_t *session,
                             file_block_read_cache_entry_t *entry) {
    if (session->read_cache_clock != UINT64_MAX)
        session->read_cache_clock++;
    entry->last_use = session->read_cache_clock;
}

static uint8_t *read_cache_entry_data(file_block_session_t *session,
                                      size_t entry_index) {
    return session->read_cache_data +
           entry_index * (size_t)FILE_BLOCK_READ_CACHE_LINE_BYTES;
}

static void read_cache_invalidate_range(file_block_session_t *session,
                                        uint64_t offset,
                                        size_t requested) {
    uint64_t end = offset + (uint64_t)requested;

    if (!session->read_cache_data || requested == 0u)
        return;
    for (size_t i = 0u; i < FILE_BLOCK_READ_CACHE_ENTRY_COUNT; i++) {
        file_block_read_cache_entry_t *entry =
            &session->read_cache_entries[i];
        uint64_t entry_end;

        if (!entry->valid)
            continue;
        entry_end = entry->offset + (uint64_t)entry->valid_bytes;
        if (offset < entry_end && entry->offset < end) {
            entry->valid = false;
            counter_add_u64(&session->read_cache_stats.invalidated_lines, 1u);
        }
    }
}

static vm_block_io_status_t host_read_once(file_block_session_t *session,
                                            uint64_t offset,
                                            void *destination,
                                            size_t requested,
                                            size_t *actual) {
    int saved_error;

    *actual = 0u;
#ifdef _WIN32
    {
        vm_block_io_status_t seek_status = windows_seek(session, offset);
        int transferred;

        if (seek_status != VM_BLOCK_IO_OK)
            return seek_status;
        errno = 0;
        counter_add_u64(&session->read_cache_stats.host_reads, 1u);
        transferred = _read(session->descriptor, destination,
                            (unsigned int)requested);
        if (transferred > 0) {
            *actual = (size_t)transferred;
            counter_add_u64(&session->read_cache_stats.host_read_bytes,
                            (uint64_t)*actual);
            return VM_BLOCK_IO_OK;
        }
        if (transferred == 0) {
            session_remember_error(session, EIO);
            return VM_BLOCK_IO_ERROR;
        }
    }
#else
    {
        ssize_t transferred;
        off_t position = (off_t)offset;

        errno = 0;
        counter_add_u64(&session->read_cache_stats.host_reads, 1u);
        transferred = pread(session->descriptor, destination, requested,
                            position);
        if (transferred > 0) {
            *actual = (size_t)transferred;
            counter_add_u64(&session->read_cache_stats.host_read_bytes,
                            (uint64_t)*actual);
            return VM_BLOCK_IO_OK;
        }
        if (transferred == 0) {
            session_remember_error(session, EIO);
            return VM_BLOCK_IO_ERROR;
        }
    }
#endif

    saved_error = errno;
    if (saved_error == EINTR)
        return VM_BLOCK_IO_RETRY;
    session_remember_error(session, saved_error);
    return VM_BLOCK_IO_ERROR;
}

static vm_block_io_status_t read_cache_read(file_block_session_t *session,
                                             uint64_t offset,
                                             void *destination,
                                             size_t requested,
                                             size_t *actual) {
    const uint64_t line_bytes = FILE_BLOCK_READ_CACHE_LINE_BYTES;
    uint64_t line_offset = offset & ~(line_bytes - 1u);
    size_t within = (size_t)(offset - line_offset);
    uint64_t remaining = session->expected_size - line_offset;
    size_t line_length = remaining < line_bytes
        ? (size_t)remaining : (size_t)line_bytes;
    uint64_t line_number = line_offset / line_bytes;
    size_t set = (size_t)(line_number % FILE_BLOCK_READ_CACHE_SET_COUNT);
    size_t first = set * FILE_BLOCK_READ_CACHE_WAYS;
    size_t victim = first;
    bool found_invalid = false;

    if (requested > line_length - within) {
        counter_add_u64(&session->read_cache_stats.cache_bypasses, 1u);
        return host_read_once(session, offset, destination, requested, actual);
    }

    for (size_t way = 0u; way < FILE_BLOCK_READ_CACHE_WAYS; way++) {
        size_t index = first + way;
        file_block_read_cache_entry_t *entry =
            &session->read_cache_entries[index];

        if (entry->valid && entry->offset == line_offset &&
            within < entry->valid_bytes) {
            size_t available = entry->valid_bytes - within;
            size_t transferred = requested < available ? requested : available;

            memcpy(destination,
                   read_cache_entry_data(session, index) + within,
                   transferred);
            *actual = transferred;
            read_cache_touch(session, entry);
            counter_add_u64(&session->read_cache_stats.cache_hits, 1u);
            counter_add_u64(&session->read_cache_stats.cache_hit_bytes,
                            (uint64_t)transferred);
            return VM_BLOCK_IO_OK;
        }
        if (!entry->valid && !found_invalid) {
            victim = index;
            found_invalid = true;
        } else if (!found_invalid &&
                   entry->last_use <
                       session->read_cache_entries[victim].last_use) {
            victim = index;
        }
    }

    counter_add_u64(&session->read_cache_stats.cache_misses, 1u);
    file_block_read_cache_entry_t *entry =
        &session->read_cache_entries[victim];
    size_t filled = 0u;
    vm_block_io_status_t status = host_read_once(
        session, line_offset, read_cache_entry_data(session, victim),
        line_length, &filled);
    if (status != VM_BLOCK_IO_OK)
        return status;

    entry->offset = line_offset;
    entry->valid_bytes = filled;
    entry->valid = filled != 0u;
    read_cache_touch(session, entry);
    if (filled <= within) {
        entry->valid = false;
        counter_add_u64(&session->read_cache_stats.cache_bypasses, 1u);
        return host_read_once(session, offset, destination, requested, actual);
    }

    size_t available = filled - within;
    size_t transferred = requested < available ? requested : available;
    memcpy(destination, read_cache_entry_data(session, victim) + within,
           transferred);
    *actual = transferred;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t file_read_at(void *context,
                                         uint64_t offset,
                                         void *destination,
                                         size_t requested,
                                         size_t *actual) {
    file_block_session_t *session = (file_block_session_t *)context;
    session_validation_t validation;
    if (!actual)
        return VM_BLOCK_IO_ERROR;
    *actual = 0;
    if (!session || !session->active || session->descriptor < 0 ||
        !callback_request_valid(session, offset, requested))
        return VM_BLOCK_IO_ERROR;
    if (requested == 0)
        return session->sticky_error ? VM_BLOCK_IO_ERROR : VM_BLOCK_IO_OK;
    if (!destination)
        return VM_BLOCK_IO_ERROR;
    if (session->sticky_error)
        return VM_BLOCK_IO_ERROR;
    validation = session_revalidate(session);
    if (validation == SESSION_VALIDATION_RETRY)
        return VM_BLOCK_IO_RETRY;
    if (validation != SESSION_VALIDATION_OK)
        return VM_BLOCK_IO_ERROR;
    counter_add_u64(&session->read_cache_stats.read_callbacks, 1u);
    counter_add_u64(&session->read_cache_stats.requested_bytes,
                    (uint64_t)requested);
    if (session->read_cache_enabled && session->read_cache_data &&
        requested <= (size_t)FILE_BLOCK_READ_CACHE_LINE_BYTES)
        return read_cache_read(session, offset, destination, requested, actual);

    counter_add_u64(&session->read_cache_stats.cache_bypasses, 1u);
    return host_read_once(session, offset, destination, requested, actual);
}

static vm_block_io_status_t file_write_at(void *context,
                                          uint64_t offset,
                                          const void *source,
                                          size_t requested,
                                          size_t *actual) {
    file_block_session_t *session = (file_block_session_t *)context;
    session_validation_t validation;
    int saved_error;

    if (!actual)
        return VM_BLOCK_IO_ERROR;
    *actual = 0;
    if (!session || !session->active || session->descriptor < 0 ||
        !callback_request_valid(session, offset, requested))
        return VM_BLOCK_IO_ERROR;
    if (requested == 0)
        return session->sticky_error ? VM_BLOCK_IO_ERROR : VM_BLOCK_IO_OK;
    if (!source)
        return VM_BLOCK_IO_ERROR;
    if (session->sticky_error)
        return VM_BLOCK_IO_ERROR;
    validation = session_revalidate(session);
    if (validation == SESSION_VALIDATION_RETRY)
        return VM_BLOCK_IO_RETRY;
    if (validation != SESSION_VALIDATION_OK)
        return VM_BLOCK_IO_ERROR;
    /* Invalidate before the first possible backing-store mutation.  A short
     * or failed host write can only make this conservative; it can never let
     * a later read observe bytes cached before a committed partial write. */
    read_cache_invalidate_range(session, offset, requested);
#ifdef _WIN32
    {
        vm_block_io_status_t seek_status = windows_seek(session, offset);
        int transferred;

        if (seek_status != VM_BLOCK_IO_OK)
            return seek_status;
        errno = 0;
        transferred = _write(session->descriptor, source,
                             (unsigned int)requested);
        if (transferred > 0) {
            *actual = (size_t)transferred;
            return VM_BLOCK_IO_OK;
        }
        if (transferred == 0) {
            session_remember_error(session, EIO);
            return VM_BLOCK_IO_ERROR;
        }
    }
#else
    {
        ssize_t transferred;
        off_t position = (off_t)offset;

        errno = 0;
        transferred = pwrite(session->descriptor, source, requested, position);
        if (transferred > 0) {
            *actual = (size_t)transferred;
            return VM_BLOCK_IO_OK;
        }
        if (transferred == 0) {
            session_remember_error(session, EIO);
            return VM_BLOCK_IO_ERROR;
        }
    }
#endif

    saved_error = errno;
    if (saved_error == EINTR)
        return VM_BLOCK_IO_RETRY;
    session_remember_error(session, saved_error);
    return VM_BLOCK_IO_ERROR;
}

static vm_block_io_status_t file_flush_callback(void *context) {
    file_block_session_t *session = (file_block_session_t *)context;
    session_validation_t validation;
    int saved_error;

    if (!session || !session->active || session->descriptor < 0)
        return VM_BLOCK_IO_ERROR;
    if (session->sticky_error)
        return VM_BLOCK_IO_ERROR;
    validation = session_revalidate(session);
    if (validation == SESSION_VALIDATION_RETRY)
        return VM_BLOCK_IO_RETRY;
    if (validation != SESSION_VALIDATION_OK)
        return VM_BLOCK_IO_ERROR;
    errno = 0;
    if (host_sync(session->descriptor) == 0)
        return VM_BLOCK_IO_OK;
    saved_error = errno;
    if (saved_error == EINTR)
        return VM_BLOCK_IO_RETRY;
    session_remember_error(session, saved_error);
    return VM_BLOCK_IO_ERROR;
}

static void initialize_session(file_block_session_t *session,
                               file_block_t *owner,
                               int descriptor,
                               uint64_t expected_size,
                               const file_block_identity_t *identity) {
    memset(session, 0, sizeof *session);
    session->owner = owner;
    session->next = NULL;
    session->expected_size = expected_size;
    session->descriptor = descriptor;
    session->last_system_error = 0;
    session->active = true;
    session->sticky_error = false;
    session->initial_identity = *identity;
    session->read_cache_enabled = owner->read_cache_enabled;
    session->read_cache_stats.enabled = owner->read_cache_enabled;
    if (owner->read_cache_enabled) {
        session->read_cache_data =
            (uint8_t *)malloc((size_t)FILE_BLOCK_READ_CACHE_BYTES);
        session->read_cache_stats.allocated =
            session->read_cache_data != NULL;
    }
    session->block.context = session;
    session->block.size = expected_size;
    session->block.identity = 0;
    session->block.generation = 0;
    session->block.read_at = file_read_at;
    session->block.write_at = file_write_at;
    session->block.flush = file_flush_callback;
}

file_block_t *file_block_create(void) {
    file_block_t *adapter = (file_block_t *)malloc(sizeof *adapter);

    if (!adapter)
        return NULL;
    adapter->current = NULL;
    adapter->retired = NULL;
    adapter->last_system_error = 0;
    adapter->read_cache_enabled = true;
    return adapter;
}

file_block_status_t file_block_set_read_cache_enabled(file_block_t *adapter,
                                                      bool enabled) {
    if (!adapter)
        return FILE_BLOCK_STATUS_INVALID_ARGUMENT;
    if (adapter->current)
        return FILE_BLOCK_STATUS_ALREADY_OPEN;
    adapter->read_cache_enabled = enabled;
    return FILE_BLOCK_STATUS_OK;
}

file_block_status_t file_block_open(file_block_t *adapter,
                                    const char *path,
                                    uint64_t expected_size) {
    file_block_identity_t before_identity;
    file_block_identity_t identity;
    file_block_stat_t before_lock;
    file_block_stat_t status;
    file_block_session_t *session;
    bool before_has_links;
    bool has_links;
    int descriptor;
    int saved_error;

    if (!adapter || !path || !*path)
        return FILE_BLOCK_STATUS_INVALID_ARGUMENT;
    if (adapter->current)
        return FILE_BLOCK_STATUS_ALREADY_OPEN;
    adapter->last_system_error = 0;
    if (!host_offset_supported(expected_size)) {
        adapter_remember_error(adapter, FILE_BLOCK_OVERFLOW_ERROR);
        return FILE_BLOCK_STATUS_OFFSET_UNSUPPORTED;
    }

    descriptor = host_open_exclusive(path, &saved_error);
    if (descriptor < 0) {
        adapter_remember_error(adapter, saved_error);
        return FILE_BLOCK_STATUS_OPEN_FAILED;
    }

    if (host_fstat_bounded(descriptor, &before_lock, &saved_error) != 0) {
        (void)host_close(descriptor);
        adapter_remember_error(adapter, saved_error);
        return FILE_BLOCK_STATUS_IO;
    }
    if (!host_is_regular(&before_lock)) {
        (void)host_close(descriptor);
        return FILE_BLOCK_STATUS_NOT_REGULAR;
    }
    if (!host_capture_identity(descriptor, &before_lock, &before_identity,
                               &before_has_links, &saved_error)) {
        (void)host_close(descriptor);
        adapter_remember_error(adapter, saved_error);
        return FILE_BLOCK_STATUS_IO;
    }
    if (!before_has_links) {
        (void)host_close(descriptor);
        adapter_remember_error(adapter, EIO);
        return FILE_BLOCK_STATUS_IO;
    }

#ifndef _WIN32
    errno = 0;
    if (host_lock_whole_file(descriptor) != 0) {
        saved_error = errno;
        (void)host_close(descriptor);
        adapter_remember_error(adapter, saved_error);
        return FILE_BLOCK_STATUS_LOCK_FAILED;
    }
#endif

    if (host_fstat_bounded(descriptor, &status, &saved_error) != 0) {
        (void)host_close(descriptor);
        adapter_remember_error(adapter, saved_error);
        return FILE_BLOCK_STATUS_IO;
    }
    if (!host_is_regular(&status)) {
        (void)host_close(descriptor);
        return FILE_BLOCK_STATUS_NOT_REGULAR;
    }
    if (status.st_size < 0 || !host_offset_supported((uint64_t)status.st_size)) {
        (void)host_close(descriptor);
        adapter_remember_error(adapter, FILE_BLOCK_OVERFLOW_ERROR);
        return FILE_BLOCK_STATUS_OFFSET_UNSUPPORTED;
    }
    if ((uint64_t)status.st_size != expected_size) {
        (void)host_close(descriptor);
        return FILE_BLOCK_STATUS_SIZE_MISMATCH;
    }
    if (!host_capture_identity(descriptor, &status, &identity, &has_links,
                               &saved_error)) {
        (void)host_close(descriptor);
        adapter_remember_error(adapter, saved_error);
        return FILE_BLOCK_STATUS_IO;
    }
    if (!has_links || !identities_equal(&before_identity, &identity)) {
        (void)host_close(descriptor);
        adapter_remember_error(adapter, EIO);
        return FILE_BLOCK_STATUS_IO;
    }

    session = (file_block_session_t *)malloc(sizeof *session);
    if (!session) {
        (void)host_close(descriptor);
        adapter_remember_error(adapter, ENOMEM);
        return FILE_BLOCK_STATUS_NO_MEMORY;
    }
    initialize_session(session, adapter, descriptor, expected_size, &identity);
    adapter->current = session;
    return FILE_BLOCK_STATUS_OK;
}

const vm_block_t *file_block_get(const file_block_t *adapter) {
    if (!adapter || !adapter->current || !adapter->current->active)
        return NULL;
    return &adapter->current->block;
}

bool file_block_get_read_cache_stats(
    const file_block_t *adapter,
    file_block_read_cache_stats_t *out_stats) {
    if (!adapter || !out_stats || !adapter->current ||
        !adapter->current->active)
        return false;
    *out_stats = adapter->current->read_cache_stats;
    return true;
}

file_block_status_t file_block_flush(file_block_t *adapter) {
    vm_block_io_status_t status;
    unsigned retry_count;

    if (!adapter)
        return FILE_BLOCK_STATUS_INVALID_ARGUMENT;
    if (!adapter->current || !adapter->current->active)
        return FILE_BLOCK_STATUS_NOT_OPEN;

    retry_count = 0;
    do {
        status = file_flush_callback(adapter->current);
        if (status == VM_BLOCK_IO_OK) {
            adapter->last_system_error = 0;
            return FILE_BLOCK_STATUS_OK;
        }
        if (status != VM_BLOCK_IO_RETRY) {
            if (adapter->last_system_error == 0)
                adapter_remember_error(adapter, EIO);
            return FILE_BLOCK_STATUS_IO;
        }
        retry_count++;
    } while (retry_count < VM_BLOCK_NO_PROGRESS_LIMIT);

    adapter_remember_error(adapter, EINTR);
    return FILE_BLOCK_STATUS_IO;
}

file_block_status_t file_block_close(file_block_t *adapter) {
    file_block_session_t *session;
    session_validation_t validation;
    unsigned retry_count;
    bool sync_succeeded;
    bool failed;
    int saved_error;
    int sync_error;

    if (!adapter)
        return FILE_BLOCK_STATUS_INVALID_ARGUMENT;
    session = adapter->current;
    if (!session || !session->active || session->descriptor < 0)
        return FILE_BLOCK_STATUS_NOT_OPEN;

    failed = session->sticky_error;
    saved_error = session->last_system_error;

    /* Bound transient metadata retries, then still attempt durability/close. */
    retry_count = 0;
    do {
        validation = session_revalidate(session);
        if (validation != SESSION_VALIDATION_RETRY)
            break;
        retry_count++;
    } while (retry_count < VM_BLOCK_NO_PROGRESS_LIMIT);
    if (validation == SESSION_VALIDATION_RETRY) {
        session_remember_error(session, EINTR);
        validation = SESSION_VALIDATION_ERROR;
    }
    if (validation != SESSION_VALIDATION_OK) {
        failed = true;
        if (saved_error == 0)
            saved_error = session->last_system_error;
    }

    sync_succeeded = false;
    sync_error = 0;
    retry_count = 0;
    do {
        errno = 0;
        if (host_sync(session->descriptor) == 0) {
            sync_succeeded = true;
            break;
        }
        sync_error = errno;
        if (sync_error != EINTR)
            break;
        retry_count++;
    } while (retry_count < VM_BLOCK_NO_PROGRESS_LIMIT);
    if (!sync_succeeded) {
        failed = true;
        session_remember_error(session, sync_error);
        if (saved_error == 0)
            saved_error = synthesized_error(sync_error);
    }

    errno = 0;
    if (host_close(session->descriptor) != 0) {
        int close_error = errno;
        failed = true;
        session_remember_error(session, close_error);
        if (saved_error == 0)
            saved_error = synthesized_error(close_error);
    }

    /* Never retry close: POSIX leaves descriptor state unspecified on EINTR. */
    free(session->read_cache_data);
    session->read_cache_data = NULL;
    session->descriptor = -1;
    session->active = false;
    session->sticky_error = failed;
    adapter->current = NULL;
    session->next = adapter->retired;
    adapter->retired = session;
    adapter->last_system_error = failed ? synthesized_error(saved_error) : 0;
    return failed ? FILE_BLOCK_STATUS_IO : FILE_BLOCK_STATUS_OK;
}

file_block_status_t file_block_destroy(file_block_t **adapter_address) {
    file_block_t *adapter;
    file_block_status_t result = FILE_BLOCK_STATUS_OK;
    file_block_session_t *session;

    if (!adapter_address)
        return FILE_BLOCK_STATUS_INVALID_ARGUMENT;
    adapter = *adapter_address;
    if (!adapter)
        return FILE_BLOCK_STATUS_OK;

    if (adapter->current)
        result = file_block_close(adapter);

    session = adapter->retired;
    while (session) {
        file_block_session_t *next = session->next;
        free(session->read_cache_data);
        free(session);
        session = next;
    }
    free(adapter);
    *adapter_address = NULL;
    return result;
}

bool file_block_is_open(const file_block_t *adapter) {
    return adapter && adapter->current && adapter->current->active &&
           adapter->current->descriptor >= 0;
}

int file_block_last_system_error(const file_block_t *adapter) {
    return adapter ? adapter->last_system_error : 0;
}

const char *file_block_strerror(file_block_status_t status) {
    switch (status) {
    case FILE_BLOCK_STATUS_OK:                 return "success";
    case FILE_BLOCK_STATUS_INVALID_ARGUMENT:   return "invalid argument";
    case FILE_BLOCK_STATUS_ALREADY_OPEN:       return "file block is already open";
    case FILE_BLOCK_STATUS_NOT_OPEN:           return "file block is not open";
    case FILE_BLOCK_STATUS_OPEN_FAILED:        return "could not open file block";
    case FILE_BLOCK_STATUS_LOCK_FAILED:        return "could not lock file block";
    case FILE_BLOCK_STATUS_NOT_REGULAR:        return "file block is not a regular file";
    case FILE_BLOCK_STATUS_SIZE_MISMATCH:      return "file block size mismatch";
    case FILE_BLOCK_STATUS_OFFSET_UNSUPPORTED: return "file block offset is unsupported";
    case FILE_BLOCK_STATUS_NO_MEMORY:          return "could not allocate file block session";
    case FILE_BLOCK_STATUS_IO:                 return "file block I/O error";
    default:                                   return "unknown file block status";
    }
}
