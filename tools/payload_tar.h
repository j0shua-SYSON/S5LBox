/*
 * S5LBox — a jailbreak payload, read from a tar into provisioner entries.
 *
 * WHY A TAR AND NOT A DIRECTORY. tools/rootfs_work.c can already create files,
 * directories and symlinks in the work image's HFS+ catalog, with the exact
 * permission and numeric ownership metadata the payload needs -- its own
 * header names "the Cydia
 * payload's 06755 MobileCydia, 04555 bin/su and 02775 var/local". What was
 * missing was anything that turns a payload into the rootfs_work_entry_t array
 * it wants; activation and PPP both use small hardcoded tables.
 *
 * A host DIRECTORY cannot supply that faithfully. Walking a tree needs
 * per-platform metadata calls, and on Windows -- where this project is
 * developed -- an extracted tree has already lost every symlink and every
 * setuid bit before the walker sees it. A tar carries type, mode, numeric
 * owner/group and link target *inside the archive*, so the same file parses
 * identically on every host and the payload's non-root ownership, 88 symlinks
 * and four setuid binaries survive.
 *
 * WHAT IS NOT SHIPPED. No payload. Cydia is Jay Freeman's software and is not
 * this project's to redistribute, exactly as Apple's firmware is not. A caller
 * may supply a tar it is entitled to use, or pass the verified data member of
 * an original-server package. Nothing here contains, embeds or downloads one,
 * and a missing file is an ordinary refusal that names the path.
 *
 * NO EXPLOIT IS INVOLVED. Inside an emulator that loads the kernel and owns the
 * disk image there is nothing to exploit past: this writes files into a
 * filesystem the host already has read-write access to, and the kernel half is
 * three boot-argument tokens. That is why it is a provisioner rather than a
 * payload delivery chain.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_PAYLOAD_TAR_H
#define S5LBOX_PAYLOAD_TAR_H

#include "rootfs_work.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAYLOAD_TAR_DETAIL_CAPACITY 256u
#define PAYLOAD_TAR_MAX_BYTES (64u * 1024u * 1024u)

typedef struct payload_tar payload_tar_t;

/*
 * Read `path` and build the entry plan.
 *
 * Returns NULL on any refusal, with `detail` naming the reason and, where one
 * is at fault, the member. `detail` may be NULL.
 *
 * REFUSES rather than sanitises, because a payload that was quietly altered on
 * the way in is a payload nobody can reason about afterwards:
 *
 *   - a member path containing ".." or starting "/", which could escape the
 *     tree the caller asked for;
 *   - a non-printable-ASCII path, which rootfs_work.c requires and which is
 *     also how a crafted archive would smuggle a different name past a reader;
 *   - a member larger than ROOTFS_WORK_MAX_ENTRY_BYTES;
 *   - more members than ROOTFS_WORK_MAX_ENTRIES;
 *   - a header whose checksum does not verify, which is what a truncated or
 *     mangled download looks like.
 *
 * The returned entries' `path` and `content` point INTO storage this object
 * owns, so they stay valid exactly until payload_tar_close(). Content is not
 * copied: the archive is held in memory once and every file's bytes are a
 * pointer into it, which is why a 15 MB payload costs 15 MB rather than twice
 * that.
 */
payload_tar_t *payload_tar_open(const char *path, const char *prefix,
                                char *detail, size_t detail_capacity);

/* The same strict parser over caller-owned memory. The bytes are copied into
 * the returned object, so the caller may release or overwrite its buffer as
 * soon as this returns. The fixed size cap is checked before allocation. */
payload_tar_t *payload_tar_open_memory(const uint8_t *bytes, size_t size,
                                       const char *prefix,
                                       char *detail, size_t detail_capacity);

/* Safe on NULL and on an already-closed slot. */
void payload_tar_close(payload_tar_t **slot);

/* The plan, in archive order -- which is the order rootfs_work.c needs, since
 * a tar lists a directory before its children and the provisioner resolves each
 * path against what it has already planned. */
const rootfs_work_entry_t *payload_tar_entries(const payload_tar_t *tar);
size_t payload_tar_entry_count(const payload_tar_t *tar);

/* Counts for the run header, so an operator can see what was planned without
 * reading 776 lines: a payload that provisioned zero symlinks is not the
 * payload anybody thought it was. */
typedef struct {
    size_t files;
    size_t directories;
    size_t symlinks;
    size_t hardlinks_materialised;
    uint64_t content_bytes;
} payload_tar_stats_t;

void payload_tar_get_stats(const payload_tar_t *tar, payload_tar_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_PAYLOAD_TAR_H */
