/*
 * S5LBox — copy-on-write overlays, so an older snapshot can be restored.
 *
 * WHY THIS EXISTS. file_block.h states the problem exactly: "an in-place
 * mutable file cannot preserve the historical contents required to restore an
 * older checkpoint." A machine keeps one work image for its whole life, which
 * makes a SINGLE suspend/resume slot coherent by construction. A LIST of
 * snapshots is not: take A, keep running, take B, then open A, and RAM is at A
 * while the disk is at B -- the quiet guest-filesystem corruption that
 * VMFirmwareBoot.h warns about and that a resume never runs fsck to catch.
 *
 * Copying the 445 MiB image per snapshot would fix it and cost more than a
 * phone can spare, so the history is kept as a difference instead.
 *
 * HOW IT WORKS. This is a WRAPPING adapter, not a change to the block layer:
 * it takes the work image's vm_block_t and returns another one to hand to the
 * machine. Reads pass straight through, because the live image is always
 * current. A write first saves the affected blocks' PREVIOUS contents into the
 * newest snapshot's overlay -- once per block, the first time it is written
 * after that snapshot was taken -- and then delegates.
 *
 * An overlay therefore holds exactly the blocks that have changed since its
 * snapshot, needs no base copy and no full-image scan, and costs one extra
 * read and append per block on first touch and nothing at all thereafter.
 *
 * THE REPLAY ORDER, WHICH IS EASY TO GET BACKWARDS. Overlay S holds each
 * block's value AS OF S. Restoring S needs S's overlay AND EVERY NEWER ONE: a
 * block left untouched between S and T but written after T appears only in T's
 * overlay, and its recorded T-value is still its S-value because nothing wrote
 * it in between. So replay runs NEWEST FIRST with older overlays overwriting
 * newer ones. vm_snapshot_list() returns newest-first for this reason, so the
 * caller never re-sorts and cannot get the direction wrong.
 *
 * WHAT IS DELIBERATELY NOT HERE. No compression: a block that changed is
 * written as it is, because a snapshot that cannot be replayed after a crash
 * mid-write is worth less than one that is merely larger. No sparse encoding
 * of runs: the record is fixed-width so a truncated overlay is detectable by
 * arithmetic rather than by parsing.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef VM_SNAPSHOT_COW_H
#define VM_SNAPSHOT_COW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vm_block.h"
#include "VMSnapshotStore.h"

/*
 * 4096 to match the guest's page size and the HFS+ allocation block, so a
 * guest write that dirties one page dirties one record rather than straddling
 * two. Changing this changes the on-disk format; the header records it so a
 * mismatched overlay is refused rather than misread.
 */
#define VM_COW_BLOCK_BYTES   4096u

/* "S5LBoxCOW\0" then version, block size, and the image size the overlay was
 * built against. The image size is part of the identity: replaying an overlay
 * onto an image of a different length would place blocks at the wrong offsets
 * for any block index past the shorter length. */
#define VM_COW_MAGIC         "S5LBoxCOW"
#define VM_COW_VERSION       1u

typedef enum {
    VM_COW_OK = 0,
    VM_COW_BAD_ARGUMENT,
    VM_COW_IO,
    VM_COW_BAD_FORMAT,        /* magic, version or block size disagree      */
    VM_COW_SIZE_MISMATCH,     /* overlay was built against a different image */
    VM_COW_TRUNCATED,         /* trailing partial record: crash mid-append   */
    VM_COW_OUT_OF_MEMORY,
    VM_COW_STATUS_COUNT
} vm_cow_status_t;

const char *vm_cow_status_text(vm_cow_status_t status);

typedef struct vm_cow vm_cow_t;

/*
 * Begin recording. `under` is the live work image; `overlay_path` is the
 * newest snapshot's disk.cow, created if absent and APPENDED to if present so
 * that a session resumed after a restart keeps recording into the same
 * snapshot rather than losing the blocks it already saved.
 *
 * The returned descriptor from vm_cow_block() is what the machine must use.
 * Writing through `under` directly after this point bypasses recording and
 * silently makes every existing snapshot unrestorable.
 */
vm_cow_status_t vm_cow_open(vm_cow_t **out,
                            const vm_block_t *under,
                            const char *overlay_path,
                            char *detail, size_t detail_capacity);

/* The descriptor to hand to the machine. Valid until vm_cow_close(). */
const vm_block_t *vm_cow_block(const vm_cow_t *cow);

/* Bytes appended so far, which is what the UI shows as a snapshot's size. */
uint64_t vm_cow_overlay_bytes(const vm_cow_t *cow);

/* Flush the overlay and the underlying image, in that order: an overlay that
 * is durable while the image is not describes a past that never happened. */
vm_cow_status_t vm_cow_flush(vm_cow_t *cow);

vm_cow_status_t vm_cow_close(vm_cow_t **cow);

/*
 * Replay one overlay onto an image, oldest-wins. Call once per overlay from
 * NEWEST to OLDEST -- see the header comment for why newer ones are needed at
 * all -- so that each older overlay overwrites what a newer one just wrote.
 *
 * `progress` is called with bytes replayed and the total, for the restore
 * progress bar; it may be NULL. A truncated trailing record is reported rather
 * than skipped: it means a crash during append, and a caller that treats it as
 * "end of file" silently restores a half-recorded past.
 */
vm_cow_status_t vm_cow_replay(const char *overlay_path,
                              const vm_block_t *onto,
                              void (*progress)(void *ctx, uint64_t done,
                                               uint64_t total),
                              void *progress_ctx,
                              char *detail, size_t detail_capacity);

#endif /* VM_SNAPSHOT_COW_H */
