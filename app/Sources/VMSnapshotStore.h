/*
 * S5LBox — the set of snapshots belonging to one machine.
 *
 * WHY A DIRECTORY PER SNAPSHOT AND NOT A FILE. A snapshot is three things that
 * must agree with each other: the CPU and RAM state, the host-side bridge
 * state, and enough of the guest DISK to put it back the way it was. Three
 * files that can be half-written independently is three ways to resume a
 * machine whose RAM disagrees with its disk, which corrupts the guest
 * filesystem quietly -- the exact failure fsck exists to catch and which a
 * resume never runs fsck to catch. A directory can be renamed atomically, so a
 * snapshot becomes visible in one step or not at all.
 *
 *   <machine>/snapshots/<id>.partial/     being written; never listed
 *   <machine>/snapshots/<id>/             complete
 *       meta                              text, one key per line
 *       state.snap                        CPU + RAM, from snapshot_save()
 *       state.snap.mdstate                host bridge counters and overlay
 *       disk.cow                          blocks saved before being overwritten
 *
 * WHY THE DISK PART IS COPY-ON-WRITE. The app keeps one work image for a
 * machine's whole life. That makes a SINGLE suspend/resume slot coherent by
 * construction -- you restore the state you saved with nothing in between --
 * and it is why VMFirmwareBoot.h correctly says no image sidecar is needed.
 * A LIST of snapshots breaks that argument: take A, keep running, take B, then
 * open A, and RAM is at A while the disk is at B.
 *
 * Copying the whole 445 MiB image per snapshot would fix it and cost more than
 * a phone can spare. So the block layer saves a block's PREVIOUS contents into
 * the newest snapshot's overlay the first time that block is written after the
 * snapshot was taken. An overlay therefore holds only what has actually
 * changed since, needs no base copy and no full-image scan, and restoring a
 * snapshot means replaying overlays newest-first onto the live image.
 *
 * WHY IDS ARE TIMESTAMPS AND SORT LEXICALLY. The restore path replays overlays
 * in order, so "newer than" has to be decidable without opening anything. A
 * fixed-width UTC id compares as a string, needs no clock at read time, and is
 * still what the UI wants to show. Two snapshots inside one second get a
 * disambiguating suffix rather than one silently replacing the other.
 *
 * WHY PLAIN C11. An id becomes a PATH COMPONENT, and the same validation has
 * to hold on the host tests and on the device. This file is compiled into both
 * and shares no code with the Objective-C layer above it.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef VM_SNAPSHOT_STORE_H
#define VM_SNAPSHOT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "VMFirmwareBoot.h"      /* VM_FW_BOOT_PATH_CAPACITY */

/* Fixed width so ids sort lexically: "20260731T142200Z" plus an optional
 * "-NN" for a collision inside the same second, plus the terminator. */
#define VM_SNAPSHOT_ID_CAPACITY   24u

/* A cap, not a policy. The UI may refuse earlier for space; this only bounds
 * the listing buffer so enumeration needs no allocation. */
#define VM_SNAPSHOT_MAX           64u

typedef enum {
    VM_SNAPSHOT_OK = 0,
    VM_SNAPSHOT_BAD_ARGUMENT,
    VM_SNAPSHOT_PATH_TOO_LONG,
    VM_SNAPSHOT_BAD_ID,           /* not the fixed shape, or not path-safe   */
    VM_SNAPSHOT_IO,               /* the detail string names the operation   */
    VM_SNAPSHOT_NOT_FOUND,
    VM_SNAPSHOT_INCOMPLETE,       /* directory exists but meta does not      */
    VM_SNAPSHOT_TOO_MANY,
    VM_SNAPSHOT_STATUS_COUNT
} vm_snapshot_status_t;

const char *vm_snapshot_status_text(vm_snapshot_status_t status);

typedef struct {
    char     id[VM_SNAPSHOT_ID_CAPACITY];
    uint64_t created_unix;    /* seconds since the epoch, UTC               */
    uint64_t retired;         /* guest instructions retired at capture      */
    uint64_t bytes;           /* total size of the snapshot directory       */
    bool     complete;        /* meta parsed and every required file present */
} vm_snapshot_info_t;

/*
 * Whether `id` is one this store would have written. Enforced before an id is
 * ever joined to a path: an id arrives from a directory listing, which is
 * to say from the filesystem, which is to say from outside. Rejects anything
 * that is not the fixed shape, and therefore rejects "..", separators and
 * every other traversal by construction rather than by blacklist.
 */
bool vm_snapshot_id_valid(const char *id);

/*
 * Build an id from a UTC timestamp. `sequence` disambiguates collisions inside
 * one second; 0 produces the bare form. The caller supplies the time rather
 * than this reading a clock, so tests are deterministic and the id can be made
 * from the same instant that is written into meta.
 */
vm_snapshot_status_t vm_snapshot_id_make(uint64_t created_unix,
                                         unsigned sequence,
                                         char *out, size_t capacity);

/*
 * The snapshots directory for a machine: <machine>/snapshots. Does not create
 * it; C has no portable mkdir and the Objective-C layer already owns directory
 * creation for the machine itself.
 */
vm_snapshot_status_t vm_snapshot_dir(const char *machine_dir,
                                     char *out, size_t capacity);

/* <snapshots>/<id>, and <snapshots>/<id>.partial for the in-progress name. */
vm_snapshot_status_t vm_snapshot_path(const char *snapshots_dir,
                                      const char *id,
                                      bool partial,
                                      char *out, size_t capacity);

/* <snapshots>/<id>/<leaf>, for the four members named in the header comment. */
vm_snapshot_status_t vm_snapshot_member_path(const char *snapshots_dir,
                                             const char *id,
                                             bool partial,
                                             const char *leaf,
                                             char *out, size_t capacity);

#define VM_SNAPSHOT_META_FILE      "meta"
#define VM_SNAPSHOT_STATE_FILE     "state.snap"
#define VM_SNAPSHOT_MD_FILE        "state.snap.mdstate"
#define VM_SNAPSHOT_COW_FILE       "disk.cow"

/*
 * Write and read the one-key-per-line meta file. Unknown keys are ignored on
 * read so a newer build's file stays loadable, but a MISSING required key is a
 * refusal: a snapshot whose instruction count is unknown cannot be ordered
 * against the others, and guessing would put the restore chain in the wrong
 * order.
 */
vm_snapshot_status_t vm_snapshot_meta_write(const char *path,
                                            const vm_snapshot_info_t *info,
                                            char *detail, size_t detail_capacity);
vm_snapshot_status_t vm_snapshot_meta_read(const char *path,
                                           vm_snapshot_info_t *out,
                                           char *detail, size_t detail_capacity);

/*
 * Enumerate complete snapshots, NEWEST FIRST -- which is also overlay-replay
 * order for a restore, so the caller never re-sorts and cannot get it wrong.
 *
 * `.partial` directories are skipped rather than reported: one is either being
 * written right now or was left by a crash, and neither is something a user
 * can open. `*count` is the number written; the return distinguishes "none"
 * from "could not look".
 */
vm_snapshot_status_t vm_snapshot_list(const char *snapshots_dir,
                                      vm_snapshot_info_t *out,
                                      size_t capacity,
                                      size_t *count,
                                      char *detail, size_t detail_capacity);

#endif /* VM_SNAPSHOT_STORE_H */
