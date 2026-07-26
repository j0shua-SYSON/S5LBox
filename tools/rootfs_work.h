/*
 * iOS3-VM -- bounded host-side rootfs work-image provisioning.
 *
 * This interface deliberately owns no guest or emulator state.  It copies an
 * immutable bare HFS+/HFSX source into a new file beside the requested
 * destination name, applies the narrowly-defined fstab, opt-in SpringBoard
 * launchd-plist, and volume-growth transformations to that unpublished
 * temporary file, validates the result, flushes it, and publishes it without
 * replacing an existing path.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_ROOTFS_WORK_H
#define IOS3VM_ROOTFS_WORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROOTFS_WORK_MAX_IO_BUFFER (1024u * 1024u)
#define ROOTFS_WORK_DETAIL_CAPACITY 256u

#define ROOTFS_WORK_DEFAULT_FSTAB "/dev/md0 / hfs rw,update 0 1"

/*
 * Bounds for the catalog provisioner below.  Every one of them is a policy
 * cap enforced with a named refusal, not a structural limit: the plan state is
 * heap-allocated from the caller's entry count, so raising a constant is the
 * whole change needed to admit a larger payload.
 */
#define ROOTFS_WORK_MAX_ENTRIES 64u
#define ROOTFS_WORK_MAX_ENTRY_BYTES (16u * 1024u * 1024u)
#define ROOTFS_WORK_MAX_CATALOG_NODES 1024u
#define ROOTFS_WORK_MAX_BITMAP_BYTES (1024u * 1024u)
#define ROOTFS_WORK_MAX_PATH 1024u
#define ROOTFS_WORK_MAX_PATH_DEPTH 32u

/*
 * HFS+ epoch seconds (1904-01-01) stamped on provisioned records when the
 * caller does not choose a time.  This is the newest contentModDate carried by
 * the stock iPhone OS 3.1.3 rootfs (2009-12-21T17:46:32Z), so a provisioned
 * work image is bit-for-bit reproducible and its new records do not look
 * newer than the volume that contains them.
 */
#define ROOTFS_WORK_DEFAULT_MAC_TIME 3344262392u

typedef enum rootfs_work_status {
    ROOTFS_WORK_OK = 0,
    ROOTFS_WORK_INVALID_ARGUMENT,
    ROOTFS_WORK_NO_MEMORY,
    ROOTFS_WORK_PATH_UNSAFE,
    ROOTFS_WORK_SOURCE_OPEN_FAILED,
    ROOTFS_WORK_SOURCE_NOT_REGULAR,
    ROOTFS_WORK_SOURCE_ALIAS,
    ROOTFS_WORK_SOURCE_BUSY,
    ROOTFS_WORK_DESTINATION_EXISTS,
    ROOTFS_WORK_DESTINATION_OPEN_FAILED,
    ROOTFS_WORK_TEMP_CREATE_FAILED,
    ROOTFS_WORK_READ_FAILED,
    ROOTFS_WORK_WRITE_FAILED,
    ROOTFS_WORK_SYNC_FAILED,
    ROOTFS_WORK_SOURCE_CHANGED,
    ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH,
    ROOTFS_WORK_HFS_INVALID,
    ROOTFS_WORK_FSTAB_NOT_UNIQUE,
    ROOTFS_WORK_FSTAB_LINE_INVALID,
    ROOTFS_WORK_CA_PLIST_NOT_UNIQUE,
    ROOTFS_WORK_CA_PLIST_INVALID,
    ROOTFS_WORK_GROW_INVALID,
    /*
     * Catalog provisioning refusals.  Every one of them is decided during the
     * read-only plan phase, before the provisioner's first write, so a work
     * image that gets any of these back is byte-identical to the image the
     * provisioner was handed.  See rootfs_work_options_t::entries.
     */
    ROOTFS_WORK_PROVISION_INVALID,       /* the request itself is malformed  */
    ROOTFS_WORK_PROVISION_UNSUPPORTED,   /* valid HFS+ this writer will not  */
                                         /* touch (key ordering, node        */
                                         /* geometry, catalog spill)         */
    ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, /* the catalog disagrees with     */
                                         /* itself -- NEVER reported as      */
                                         /* "not found"                      */
    ROOTFS_WORK_PROVISION_PARENT_MISSING,
    ROOTFS_WORK_PROVISION_EXISTS,
    ROOTFS_WORK_PROVISION_NODE_FULL,     /* the leaf would have to split     */
    ROOTFS_WORK_PROVISION_LEAF_HEAD,     /* insert would become a leaf's     */
                                         /* first key, so an index key would */
                                         /* have to be rewritten             */
    ROOTFS_WORK_PROVISION_NO_SPACE,      /* free blocks or CNIDs exhausted   */
    ROOTFS_WORK_PROVISION_LIMIT,         /* a ROOTFS_WORK_MAX_* cap          */
    ROOTFS_WORK_RANGE_ERROR,
    ROOTFS_WORK_PUBLISH_FAILED,
    ROOTFS_WORK_PUBLISH_DURABILITY_FAILED
} rootfs_work_status_t;

typedef enum rootfs_work_stage {
    ROOTFS_WORK_STAGE_NONE = 0,
    ROOTFS_WORK_STAGE_ARGUMENTS,
    ROOTFS_WORK_STAGE_SOURCE_PATH,
    ROOTFS_WORK_STAGE_DESTINATION_PATH,
    ROOTFS_WORK_STAGE_SOURCE_OPEN,
    ROOTFS_WORK_STAGE_SOURCE_VALIDATE,
    ROOTFS_WORK_STAGE_SOURCE_IDENTITY,
    ROOTFS_WORK_STAGE_TEMP_CREATE,
    ROOTFS_WORK_STAGE_COPY,
    ROOTFS_WORK_STAGE_COPY_VERIFY,
    ROOTFS_WORK_STAGE_FSTAB_SCAN,
    ROOTFS_WORK_STAGE_FSTAB_WRITE,
    ROOTFS_WORK_STAGE_CA_PLIST_SCAN,
    ROOTFS_WORK_STAGE_CA_PLIST_WRITE,
    ROOTFS_WORK_STAGE_GROW_PLAN,
    ROOTFS_WORK_STAGE_GROW_WRITE,
    ROOTFS_WORK_STAGE_PROVISION_PLAN,
    ROOTFS_WORK_STAGE_PROVISION_WRITE,
    ROOTFS_WORK_STAGE_FINAL_VALIDATE,
    ROOTFS_WORK_STAGE_FLUSH,
    ROOTFS_WORK_STAGE_PUBLISH,
    ROOTFS_WORK_STAGE_DIRECTORY_SYNC,
    ROOTFS_WORK_STAGE_CLEANUP
} rootfs_work_stage_t;

typedef struct rootfs_work_source_identity {
    bool required;
    uint64_t expected_size;
    uint8_t expected_sha256[IOS3_SHA256_DIGEST_SIZE];
} rootfs_work_source_identity_t;

typedef enum rootfs_work_entry_kind {
    ROOTFS_WORK_ENTRY_DIRECTORY = 0,
    ROOTFS_WORK_ENTRY_FILE = 1
} rootfs_work_entry_kind_t;

/*
 * One catalog object to create in the work image.
 *
 * `path` is absolute, '/'-separated and printable-ASCII: every component is
 * created as an HFS+ Unicode name of the same code points.  Parent components
 * must already exist, either in the stock image or earlier in the same entries
 * array; the provisioner resolves each path against the state it has already
 * planned, so {"/a/b" directory, "/a/b/c" file} is a legal two-entry request.
 *
 * `permissions` is the low 12 bits of a BSD mode word -- rwx plus setuid,
 * setgid and sticky -- and the provisioner ORs in S_IFDIR/S_IFREG itself.
 * Zero selects 0755 for a directory and 0644 for a file.  The field is
 * deliberately wide enough for the Cydia payload's 06755 MobileCydia, 04555
 * bin/su and 02775 var/local without an interface change.
 */
typedef struct rootfs_work_entry {
    rootfs_work_entry_kind_t kind;
    const char *path;
    /* FILE only.  NULL is permitted when content_size is zero. */
    const uint8_t *content;
    size_t content_size;
    uint16_t permissions;
    uint32_t owner_id;
    uint32_t group_id;
} rootfs_work_entry_t;

typedef struct rootfs_work_options {
    /* NULL selects ROOTFS_WORK_DEFAULT_FSTAB. */
    const char *fstab_line;

    /*
     * Opt-in, OFF by default: rewrite the stock SpringBoard LaunchDaemon plist
     * in place so launchd exports CA_ENABLE_MBX2D=0 to SpringBoard, which is
     * how Apple's own QuartzCore selects its software renderer instead of the
     * MBX2D path this machine has no GPU for.  Same-length overwrite of an
     * exactly-once byte pattern, so no HFS catalog change is involved; see
     * ca_plist_rewrite() for the whole argument.  When false the transformation
     * is not attempted at all and the stock record is left untouched.
     */
    bool ca_software_render;

    /*
     * Same arithmetic as bootkernel's historical --grow implementation:
     * floor(growth_bytes / allocationBlockSize), less the old reserved-tail
     * block, is added to totalBlocks and then clamped to the existing
     * allocation bitmap's bit capacity.  Zero disables growth.
     *
     * For compatibility with the already-proven bootkernel transformation,
     * this narrow image builder leaves HFS lastMountedVersion and writeCount
     * unchanged. It is not a general-purpose HFS writer or fsck replacement.
     */
    uint64_t growth_bytes;

    /* Zero selects ROOTFS_WORK_MAX_IO_BUFFER; otherwise 1..that limit. */
    size_t io_buffer_bytes;

    /*
     * Catalog objects to create, applied in array order.  entry_count == 0
     * (the zero-initialised default) means the catalog B-tree is not opened
     * at all and this build behaves exactly as it did before provisioning
     * existed.
     *
     * ORDERING IS ENFORCED, NOT ASSUMED.  Provisioning runs after grow_volume
     * and re-reads the volume header from the image it is about to modify, so
     * it can only ever observe post-growth free space.  The stock rootfs ships
     * freeBlocks = 0; asking for a file without also asking for growth is a
     * ROOTFS_WORK_PROVISION_NO_SPACE refusal that names the ordering, never a
     * silent write into a block someone else owns.
     */
    const rootfs_work_entry_t *entries;
    size_t entry_count;

    /*
     * HFS+ epoch seconds stamped on created records and on the modification
     * times of the folders that gain a child.  Zero selects
     * ROOTFS_WORK_DEFAULT_MAC_TIME, which keeps output reproducible.
     */
    uint32_t entry_mac_time;

    /*
     * Optional exact source gate.  When required is true, expected_size is
     * checked before a temporary file is created and expected_sha256 is
     * checked after the immutable source has been copied and re-stamped, but
     * before any HFS transformation or publication.  The fields are ignored
     * when required is false; the observed digest is still reported.
     */
    rootfs_work_source_identity_t source_identity;
} rootfs_work_options_t;

typedef struct rootfs_work_result {
    rootfs_work_status_t status;
    rootfs_work_stage_t stage;
    int system_error;
    int cleanup_system_error;
    uint64_t source_size;
    uint64_t final_size;
    uint64_t bytes_copied;
    uint64_t fstab_offset;
    /* UINT64_MAX unless the CA software-render rewrite actually ran. */
    uint64_t ca_plist_offset;
    /* Catalog provisioning, all zero unless entries were requested. */
    uint32_t provision_entries;
    uint32_t provision_first_cnid;
    uint32_t provision_last_cnid;
    uint32_t provision_blocks;
    uint8_t source_sha256[IOS3_SHA256_DIGEST_SIZE];
    size_t io_buffer_bytes;
    bool source_sha256_valid;
    bool source_identity_verified;
    bool published;
    bool temporary_left;
    char detail[ROOTFS_WORK_DETAIL_CAPACITY];
} rootfs_work_result_t;

/*
 * Create destination_path.  The destination must not exist.  On success the
 * exact published size is in result->final_size.  Once result->published is
 * true, the complete destination is intentionally preserved even if a later
 * durability or temporary-link cleanup step returns non-OK; callers must
 * inspect published on every failure.
 *
 * This remains a generic format transformer, not a signature verifier.  Its
 * optional source_identity policy provides an exact caller-selected size and
 * SHA-256 gate without a second source read.  Both backends reject link/reparse
 * ambiguity and publish without replacement, but portable POSIX linkat and
 * Win32 MoveFileEx still name the temporary file by path.  Atomic destination-
 * entry creation is guaranteed; object identity is not a security boundary
 * against a hostile same-user namespace racer.
 */
rootfs_work_status_t rootfs_work_create(const char *source_path,
                                        const char *destination_path,
                                        const rootfs_work_options_t *options,
                                        rootfs_work_result_t *result);

const char *rootfs_work_status_name(rootfs_work_status_t status);
const char *rootfs_work_stage_name(rootfs_work_stage_t stage);

/*
 * The two catalog objects activation needs, in dependency order: the
 * /private/var/root/Library/Lockdown directory the stock image does not have
 * (the guest says so itself -- "Can't stat /var/root//Library/Lockdown"), and
 * the data_ark.plist inside it.
 *
 * The plist bytes live in exactly one place, here, because they are derived
 * from lockdownd's disassembly rather than guessed: the leading '-' on each
 * key is the global-domain composed form lockdownd reads, FactoryActivated is
 * the one activation state that survives lockdownd's boot recompute, and
 * -BrickState keeps the fix in force on a device that does not report as a
 * phone.  docs/AGENT_HANDOFF.md 23.3 has the derivation.
 *
 * Returns the number of entries required (2).  Fills `entries` only when
 * `capacity` is at least that, so a caller can size an array from the return
 * value.  The filled entries point at static storage and stay valid forever.
 */
size_t rootfs_work_activation_entries(rootfs_work_entry_t *entries,
                                      size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* IOS3VM_ROOTFS_WORK_H */
