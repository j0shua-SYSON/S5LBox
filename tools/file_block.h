/*
 * S5LBox -- bounded descriptor-backed adapter for a writable vm_block_t.
 *
 * This host-tool layer owns one already-existing regular work file.  It never
 * allocates an image-sized buffer and never creates, grows, or copies media.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_FILE_BLOCK_H
#define S5LBOX_FILE_BLOCK_H

#include "vm_block.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FILE_BLOCK_STATUS_OK = 0,
    FILE_BLOCK_STATUS_INVALID_ARGUMENT,
    FILE_BLOCK_STATUS_ALREADY_OPEN,
    FILE_BLOCK_STATUS_NOT_OPEN,
    FILE_BLOCK_STATUS_OPEN_FAILED,
    FILE_BLOCK_STATUS_LOCK_FAILED,
    FILE_BLOCK_STATUS_NOT_REGULAR,
    FILE_BLOCK_STATUS_SIZE_MISMATCH,
    FILE_BLOCK_STATUS_OFFSET_UNSUPPORTED,
    FILE_BLOCK_STATUS_NO_MEMORY,
    FILE_BLOCK_STATUS_IO
} file_block_status_t;

/*
 * Opaque owner for the current descriptor and all retired per-open sessions.
 * Heap ownership prevents accidental bitwise copies from duplicating close
 * authority.  Creation performs only a tiny fixed-size allocation.
 */
typedef struct file_block file_block_t;

/*
 * Four MiB of 16 KiB lines is deliberately small beside the roughly 450 MiB
 * work image.  The cache removes repeated 4 KiB descriptor reads and folds
 * adjacent guest pages into one host read without making startup copy the
 * media into RAM.  Every nonempty callback still performs the adapter's
 * descriptor size/identity revalidation before consulting a cached line.
 */
#define FILE_BLOCK_READ_CACHE_LINE_BYTES UINT32_C(16384)
#define FILE_BLOCK_READ_CACHE_BYTES      UINT32_C(4194304)

typedef struct {
    bool enabled;
    bool allocated;
    uint64_t read_callbacks;
    uint64_t requested_bytes;
    uint64_t cache_hits;
    uint64_t cache_hit_bytes;
    uint64_t cache_misses;
    uint64_t cache_bypasses;
    uint64_t host_reads;
    uint64_t host_read_bytes;
    uint64_t invalidated_lines;
} file_block_read_cache_stats_t;

file_block_t *file_block_create(void);

/*
 * The bounded read cache is enabled by default.  A frontend may disable it
 * before opening a session to obtain a same-binary diagnostic control.  An
 * active session cannot be reconfigured because its vm_block_t may already be
 * borrowed by the machine.
 */
file_block_status_t file_block_set_read_cache_enabled(file_block_t *adapter,
                                                      bool enabled);

/*
 * Open an existing regular file for exact-size random reads and writes.  The
 * caller must pass a dedicated working image, never the immutable source image
 * from which an outer provisioner created it.  Detecting source/work aliases is
 * intentionally the provisioner's responsibility because this adapter sees
 * only one path.
 *
 * The adapter opens with non-inheritable descriptor semantics and exclusive
 * sharing on Windows.  POSIX uses a cooperative advisory whole-file write lock.
 * POSIX record locks are process-wide and closing any same-inode descriptor in
 * that process may release them, so the lock is not the correctness boundary.
 * The opened object's size, nonzero link count, and identity are captured from
 * its descriptor and revalidated before every nonempty read, write, and durable
 * flush.  Windows identity uses the underlying handle's volume serial and file
 * index, not the CRT stat fields (which are not meaningful NTFS identities).
 * That per-operation check is the fail-closed drift guard.  Calls and any
 * cooperating external access must still be serialized by the caller.  While
 * a session is open, content mutations must use the returned vm_block_t write
 * callback so its read cache can be invalidated; an external writer must wait
 * until file_block_close().
 * Interrupted open/setup operations are retried with the same bound.  During
 * I/O, interrupted metadata and sync calls remain retryable; the vm_block
 * exact-I/O helpers and this adapter's public flush/close operations cap them
 * at VM_BLOCK_NO_PROGRESS_LIMIT instead of hanging under a signal storm.
 *
 * This writable adapter is always snapshot-unbindable: the returned vm_block_t
 * exposes identity=0 and generation=0.  A future immutable checkpoint/COW
 * backend must provide snapshot binding; an in-place mutable file cannot
 * preserve the historical contents required to restore an older checkpoint.
 */
file_block_status_t file_block_open(file_block_t *adapter,
                                    const char *path,
                                    uint64_t expected_size);

/*
 * Return the descriptor for the current per-open session, or NULL when closed.
 * The descriptor may be copied while its adapter remains alive.  Closing makes
 * that session permanently fail closed; reopening creates a distinct session,
 * so a stale copy can never target the new file.  All descriptor borrows must
 * end before file_block_destroy().
 */
const vm_block_t *file_block_get(const file_block_t *adapter);

/* Snapshot the active session's host-only cache telemetry. */
bool file_block_get_read_cache_stats(
    const file_block_t *adapter,
    file_block_read_cache_stats_t *out_stats);

/* Revalidate size/identity, then request bounded descriptor-level durability. */
file_block_status_t file_block_flush(file_block_t *adapter);

/*
 * Boundedly revalidate and sync, then close the current descriptor and
 * permanently retire its session, even when validation, sync, or close reports
 * an error.  The adapter remains reusable for a fresh open.
 */
file_block_status_t file_block_close(file_block_t *adapter);

/*
 * Close the current session if necessary, free all retired sessions and the
 * owner, and set *adapter_address to NULL.  A NULL *adapter_address is already
 * destroyed and succeeds.  The pointer-to-pointer itself must be non-NULL.
 * The caller must have ended every vm_block_t borrow before this call.
 */
file_block_status_t file_block_destroy(file_block_t **adapter_address);

bool file_block_is_open(const file_block_t *adapter);
/* errno-style host errors, or a native Win32 error from handle identity I/O. */
int file_block_last_system_error(const file_block_t *adapter);
const char *file_block_strerror(file_block_status_t status);

#endif /* S5LBOX_FILE_BLOCK_H */
