/* Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * Offline HFSX access. The caller owns an exclusive machine-storage lease and
 * proves a clean shutdown before opening. Changes remain private until commit
 * to an UNPUBLISHED, exact clone of the source. Never pass the live image as
 * the commit destination. Publication and checkpoint invalidation belong to
 * the caller's crash-safe disk transaction.
 */
#ifndef S5LBOX_HFS_OFFLINE_H
#define S5LBOX_HFS_OFFLINE_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct hfs_offline hfs_offline;
typedef struct {
    uint32_t id, parent, mode, uid, gid;
    uint64_t size;
    int64_t modified;
    uint16_t name[255]; /* host-endian UTF-16, as stored in the HFSX catalog */
    size_t name_length;
} hfs_offline_info;
/* Functions return a POSIX errno value; no process-global error state. */
int hfs_offline_open(const char *image, hfs_offline **out);
/* Installation batches may checkpoint individual files in ONE unpublished
 * clone to bound memory. This constructor must NEVER receive the live image.
 * The caller publishes the clone only after the entire batch succeeds. */
int hfs_offline_open_private(const char *unpublished_clone, hfs_offline **out);
int hfs_offline_checkpoint_private(hfs_offline *);
void hfs_offline_close(hfs_offline *volume);
bool hfs_offline_dirty(const hfs_offline *volume);
int hfs_offline_stat(hfs_offline *, uint32_t id, hfs_offline_info *);
int hfs_offline_lookup(hfs_offline *, uint32_t parent, const uint16_t *name,
                       size_t length, hfs_offline_info *);
/* A stable cursor for an unchanged directory; zero starts enumeration. */
int hfs_offline_next(hfs_offline *, uint32_t parent, size_t *cursor,
                     hfs_offline_info *); /* ENOENT at end */
int hfs_offline_read(hfs_offline *, uint32_t id, uint64_t offset,
                     void *bytes, size_t length, size_t *read_count);
int hfs_offline_write(hfs_offline *, uint32_t id, uint64_t offset,
                      const void *bytes, size_t length);
int hfs_offline_truncate(hfs_offline *, uint32_t id, uint64_t length);
int hfs_offline_create(hfs_offline *, uint32_t parent, const uint16_t *name,
                       size_t length, uint32_t mode, uint32_t *id);
int hfs_offline_remove(hfs_offline *, uint32_t id);
/* Installer-only unlink when the caller has no open handles to this inode. */
int hfs_offline_remove_closed(hfs_offline *, uint32_t id);
/* Replacement has POSIX rename semantics. Open, unlinked files retain data
 * until the volume is closed; CNIDs are never reused during a session. */
int hfs_offline_rename(hfs_offline *, uint32_t id, uint32_t parent,
                       const uint16_t *name, size_t length);
/* The clone is flushed before success. On ANY failure discard it and close
 * this volume. A successful commit rebinds reads to the clone's inode; renaming
 * that clone into place does not invalidate open file IDs. */
int hfs_offline_commit(hfs_offline *, const char *unpublished_clone);
#endif
