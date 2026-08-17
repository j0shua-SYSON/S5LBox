/*
 * S5LBox -- bounded host-side rootfs work-image provisioning.
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
#ifndef S5LBOX_ROOTFS_WORK_H
#define S5LBOX_ROOTFS_WORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROOTFS_WORK_MAX_IO_BUFFER (1024u * 1024u)
#define ROOTFS_WORK_DETAIL_CAPACITY 256u

/*
 * The record written over the guest's own. The last field is the fsck PASS
 * NUMBER, and it is 0 -- "never check at boot" -- rather than the 1 a real
 * iPhone carries. That is a deliberate difference from the hardware, so here
 * is the whole reason.
 *
 * THE VOLUME IS NOT JOURNALED. The HFS+ volume header at byte 1024 of the
 * source image reads attributes 0x00000100: kHFSVolumeUnmountedBit set,
 * kHFSVolumeJournaledBit CLEAR. On real hardware a dirty mount replays a
 * journal in moments; with no journal there is nothing to replay, so fsck_hfs
 * falls back to a full structural scan of all 445 MB.
 *
 * AND THE VOLUME IS ALWAYS DIRTY AFTER THE FIRST RUN. Closing the app does not
 * unmount the guest's root filesystem -- there is no guest shutdown path yet --
 * so every launch after the first inherits an uncleanly-mounted volume and
 * pays that full scan at emulator speed before anything can happen. A first
 * launch is fast because it starts from a pristine copy; the second is not,
 * which is exactly the shape of the problem as reported.
 *
 * WHAT IS GIVEN UP. A volume that really is damaged will no longer be repaired
 * automatically at boot. That is a genuine loss and is accepted knowingly: the
 * cost being avoided is minutes on EVERY launch, against a repair pass that
 * cannot fix a torn write in a 445 MB image any better than recreating it can.
 * VMFirmwareBoot.c already deletes and remakes an INCOMPLETE work image, which
 * is the failure this project actually produces.
 *
 * THE BETTER FIXES, in the order they should replace this one: unmount cleanly
 * on exit, or resume from a snapshot so the volume is never left dirty at all.
 * Both need work that does not exist yet; this is one character and available
 * now.
 *
 * SIZE-NEUTRAL, which is load-bearing. rootfs_work rewrites the fstab record
 * in place and cannot change its length, so 1 -> 0 keeps the record byte count
 * identical. Any longer replacement would be refused.
 *
 * bootkernel keeps its own literal and is deliberately NOT changed: its runs
 * each create a fresh work image, so their volumes are clean and their fsck is
 * already quick, and every instruction index recorded in docs/BOOTLOG.md was
 * taken with pass 1.
 */
#define ROOTFS_WORK_DEFAULT_FSTAB "/dev/md0 / hfs rw,update 0 0"

/*
 * Bounds for the catalog provisioner below.  Every one of them is a policy cap
 * enforced with a named refusal rather than a truncation, and the state each
 * one bounds is heap-allocated, so raising a constant costs only memory.
 *
 * AN EARLIER VERSION OF THIS COMMENT WAS WRONG and the correction is worth
 * keeping.  It claimed raising ROOTFS_WORK_MAX_ENTRIES from 64 was "the whole
 * change needed to admit a larger payload".  It was not.  A batch of entries is
 * not one ascending run of keys even when the caller's paths are sorted:
 * creating file f under folder P writes a name key (P, f) AND a thread key
 * (cnid(f), ""), and every cnid handed out is larger than P, so the SECOND file
 * under P has to be inserted between the first file's name record and the first
 * file's thread record.  Once that stretch of the tree fills one leaf, the next
 * name record is an interior insert into a full leaf -- which the writer used
 * to refuse by name (PROVISION_NODE_FULL) because the only split it had was the
 * rightmost append.
 *
 * MEASURED, not reasoned: with the cap raised and nothing else changed, the
 * 678-entry scale request in core/tests/test_rootfs_provision.c refused after
 * THREE entries -- "leaf 8 has 186 free bytes, the record needs 274" -- and the
 * record it could not place was the second file in the payload's first
 * directory.  catalog_node_split() in rootfs_work.c is the general split added
 * to fix it, and that test is what now holds the fix to 678 entries, 665 leaf
 * splits and 58 index splits deep.
 *
 * WHAT IS STILL A STRUCTURAL LIMIT, and it is not one of these constants:
 * splitting the B-tree's ROOT is not implemented, so a request that would have
 * to grow a new root and increment treeDepth is a PROVISION_SPLIT_UNSUPPORTED
 * refusal.  On the shipping 7E18 catalog it should be far out of reach -- that
 * volume's root index node was measured at 3280 free bytes against 15 children
 * (see catalog_level_extend(), a prior measurement not re-checked here), and a
 * 644-entry payload is roughly 1300 catalog records, which at that volume's
 * 4096-byte nodes is on the order of a hundred new leaves and so a handful of
 * new level-2 index records in the root.  The scale test starts a depth-4 tree
 * with a deliberately roomy root for the same reason and finishes without
 * needing to grow it.  UNVERIFIED against the real firmware image: no test in
 * this suite opens firmware/rootfs.img.
 */

/*
 * 1024.  The acquired Cydia payload is 555 regular files plus 89 symlinks --
 * 644 objects -- and every directory in its tree is an entry too, so the real
 * request is somewhere above that; the exact directory count has not been
 * measured here.  1024 admits 644 with half again in headroom, plus the two
 * activation entries.  The cost is one catalog_content_t (24 bytes) per entry
 * in the plan, so the cap itself is 24 KiB.  The scale test runs 678.
 */
#define ROOTFS_WORK_MAX_ENTRIES 1024u
#define ROOTFS_WORK_MAX_FILE_REPAIRS 16u
#define ROOTFS_WORK_MAX_ENTRY_BYTES (16u * 1024u * 1024u)
/*
 * 4096 catalog nodes one request may TOUCH -- not a bound on the tree, which
 * may be any size.  Every node this writer touches is a node of the tree, so
 * the tree's own node count is the ceiling, and the worst case for that count
 * is the SMALLEST nodeSize the writer accepts (512 bytes), where the same
 * records need the most nodes to hold them.
 *
 * Measured at that worst case: the 678-entry scale test publishes a tree of 669
 * leaves and 731 nodes in all.  Scaled linearly to this file's 1024-entry cap
 * that is about 1100, and the shipping volume's 4096-byte nodes would need
 * roughly an eighth of it.  4096 clears the measured worst case by 5.6x and the
 * extrapolated one by 3.7x.  The cap itself costs only its 16-byte cache slot
 * (64 KiB in all); node buffers are allocated per node actually touched.
 */
#define ROOTFS_WORK_MAX_CATALOG_NODES 4096u
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
    /* The PPP launchd job, which reuses the CA plist's mechanism exactly. */
    ROOTFS_WORK_PPP_PLIST_NOT_UNIQUE,
    ROOTFS_WORK_PPP_PLIST_INVALID,
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
    ROOTFS_WORK_PROVISION_NODE_FULL,     /* the node is full and NO split of */
                                         /* its records plus the new one     */
                                         /* leaves two halves that both fit  */
    ROOTFS_WORK_PROVISION_LEAF_HEAD,     /* insert would become a node's     */
                                         /* first key, so an index key would */
                                         /* have to be rewritten             */
    ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED, /* a split was the right answer */
                                         /* but its shape is outside what is */
                                         /* implemented: the node is the     */
                                         /* ROOT, so a new root would have   */
                                         /* to be grown and treeDepth        */
                                         /* incremented; or the record does  */
                                         /* not fit even an empty node       */
    ROOTFS_WORK_PROVISION_BTREE_FULL,    /* the catalog B-tree's own free-   */
                                         /* node map is exhausted; growing   */
                                         /* the catalog fork is a different  */
                                         /* capability and is not implemented*/
    ROOTFS_WORK_PROVISION_NO_SPACE,      /* free blocks or CNIDs exhausted   */
    ROOTFS_WORK_PROVISION_LIMIT,         /* a ROOTFS_WORK_MAX_* cap          */
    /* An existing regular file did not match either the exact legacy or the
     * exact desired BSD metadata tuple, or its bytes did not match the
     * caller-pinned size and SHA-256. Decided before the first catalog write. */
    ROOTFS_WORK_FILE_REPAIR_MISMATCH,
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
    ROOTFS_WORK_STAGE_PPP_PLIST_SCAN,
    ROOTFS_WORK_STAGE_PPP_PLIST_WRITE,
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
    ROOTFS_WORK_ENTRY_FILE = 1,
    /*
     * A BSD symbolic link.  `content`/`content_size` carry the link TARGET as
     * a path string -- not NUL-terminated on disk, because logicalSize is
     * exactly strlen(target) -- and the record is a catalog FILE record whose
     * fileMode says S_IFLNK and whose Finder userInfo says 'slnk'/'rhap'.  See
     * catalog_build_symlink() for the byte-level derivation, which was read
     * out of the stock image's own symlinks rather than recalled.
     */
    ROOTFS_WORK_ENTRY_SYMLINK = 2
} rootfs_work_entry_kind_t;

/*
 * What an entry may do when its final catalog key already exists.
 *
 * REFUSE remains zero so every existing caller is create-only.  Reusing a
 * directory is deliberately narrower than a generic "overlay" switch: the
 * provisioner verifies that the existing record really is a directory and
 * leaves its metadata and children untouched.  Files and symlinks still fail
 * closed until their bytes/targets can be compared or replaced transactionally.
 */
typedef enum rootfs_work_existing_policy {
    ROOTFS_WORK_EXISTING_REFUSE = 0,
    ROOTFS_WORK_EXISTING_REUSE_DIRECTORY = 1,
    ROOTFS_WORK_EXISTING_REUSE_IDENTICAL_SYMLINK = 2
} rootfs_work_existing_policy_t;

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
    rootfs_work_existing_policy_t existing_policy;
} rootfs_work_entry_t;

/*
 * One exact, in-place BSD metadata migration for an existing regular file.
 *
 * This is deliberately not a generic chmod primitive. The catalog record is
 * eligible only when the data fork has exactly expected_size bytes and the
 * exact expected_sha256 digest. Its current owner/group/permission tuple must
 * then be either the named legacy tuple (NEEDED) or the desired tuple
 * (SATISFIED). Any third state fails closed before a catalog byte is written.
 * The file contents, CNID, extents, dates, Finder metadata and resource fork
 * are never changed.
 */
typedef struct rootfs_work_file_repair {
    const char *path;
    uint64_t expected_size;
    uint8_t expected_sha256[IOS3_SHA256_DIGEST_SIZE];
    uint32_t expected_owner_id;
    uint32_t expected_group_id;
    uint16_t expected_permissions;
    uint32_t desired_owner_id;
    uint32_t desired_group_id;
    uint16_t desired_permissions;
} rootfs_work_file_repair_t;

typedef enum rootfs_work_file_repair_state {
    /* The final object or one of its parent directories does not exist. */
    ROOTFS_WORK_FILE_REPAIR_MISSING = 0,
    /* Exact file identity and exact legacy metadata; mutation is permitted. */
    ROOTFS_WORK_FILE_REPAIR_NEEDED,
    /* Exact file identity and exact desired metadata; no mutation is needed. */
    ROOTFS_WORK_FILE_REPAIR_SATISFIED
} rootfs_work_file_repair_state_t;

typedef struct rootfs_work_options {
    /* NULL selects ROOTFS_WORK_DEFAULT_FSTAB unless preserve_fstab is true. */
    const char *fstab_line;

    /*
     * Opt-in, OFF by default: do not scan or rewrite fstab.  This is for a
     * transaction whose immutable source is an already-provisioned machine
     * image rather than Apple's pristine rootfs.  Supplying fstab_line at the
     * same time is invalid; callers must say either "preserve" or "replace".
     */
    bool preserve_fstab;

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
     * Opt-in, OFF by default: replace the stock com.apple.chud.pilotfish
     * LaunchDaemon plist, in place and at exactly its own 530 bytes, with a
     * job that runs the guest's own /usr/sbin/pppd against the emulated uart4.
     *
     * WHY THAT FILE. The rootfs ships no PPP launchd job and /private/etc/ppp
     * is empty, and this provisioner cannot split a catalog B-tree node, so a
     * new file at a path of our choosing is not available. What IS available
     * is that several shipped plists name binaries that do not exist on this
     * image, which makes rewriting one of them size-neutral with zero
     * collateral damage. pilotfish points at /Developer/usr/libexec/pilotfish,
     * /Developer does not exist, and at 530 bytes it is the largest of the
     * four inert candidates -- enough for a fully-argumented job.
     *
     * It is a HIJACK and it is TEMPORARY. docs/networking.md is explicit that
     * PPP over an emulated UART is a stopgap until real drivers and
     * controllers exist; this option is the guest half of that stopgap, and
     * the day a provisioner can create a file it should be replaced by one at
     * a path that says what it is.
     *
     * Same mechanism as ca_software_render above: an exactly-once byte
     * pattern, overwritten by exactly as many bytes, so logicalSize,
     * totalBlocks and the file's single extent are all untouched.
     */
    bool ppp_launchd_job;

    /*
     * Same incremental arithmetic as bootkernel's historical --grow
     * implementation: floor(growth_bytes / allocationBlockSize), less the
     * old reserved-tail block, is added to totalBlocks. Zero disables this
     * incremental request.
     *
     * Growth may extend the allocation special file when its existing bitmap
     * is too short. That remains deliberately bounded: the new bitmap must fit
     * ROOTFS_WORK_MAX_BITMAP_BYTES and one of the fork's eight inline extent
     * slots. The writer does not create an extents-overflow record.
     *
     * For compatibility with the already-proven bootkernel transformation,
     * this narrow image builder leaves HFS lastMountedVersion and writeCount
     * unchanged. It is not a general-purpose HFS writer or fsck replacement.
     */
    uint64_t growth_bytes;

    /*
     * Absolute lower bound for the published HFS volume, rounded up to an
     * allocation block. This composes with growth_bytes by selecting the
     * larger result. Zero disables the lower bound; a nonzero value already
     * met by the source is a no-op rather than an error.
     */
    uint64_t minimum_volume_bytes;

    /*
     * OFF by default, and intentionally not a repair switch. An unjournaled
     * HFS volume normally has to carry kHFSVolumeUnmountedBit before this
     * transformer will even copy it. A caller may set this only when it owns
     * separate, durable proof that the guest completed full power-off and the
     * backing file was flushed afterwards. All geometry, alternate-header,
     * allocation-bitmap and catalog checks still run, and the dirty/inconsistent
     * attributes are preserved in the output so the next guest boot can fsck;
     * this option never lies by stamping a volume clean.
     */
    bool allow_unclean_source;

    /*
     * Opt-in, OFF by default, and valid only with allow_unclean_source.  An
     * unjournaled guest disk can be stopped between the two writes that splice
     * a B-tree node into a doubly-linked level: the authoritative forward
     * chain and every index child can be complete while the following node's
     * redundant bLink still names the old predecessor.
     *
     * This does not make catalog corruption generally repairable.  Before one
     * bLink is changed, the unpublished clone must still pass every node,
     * forward-chain, global key-order, leaf-record-count, lastLeafNode,
     * index-child-sequence and first-key descent check.  Only the bLink values
     * that disagree with those independently established forward chains are
     * rewritten, and the strict audit is repeated after commit.  The immutable
     * source is never changed.  Callers must gate this with the same durable
     * powered-off witness required by allow_unclean_source.
     */
    bool repair_catalog_backlinks;

    /*
     * Opt-in, OFF by default, and valid only with allow_unclean_source.  This
     * is the broader powered-off recovery used when the guest persisted all
     * allocated catalog records but stopped between the writes that publish
     * their B-tree topology.
     *
     * The unpublished clone is repairable only when every allocated leaf is
     * independently readable, their complete record set has the header's
     * exact count and one strict global key order, and every allocated node at
     * each index level is named exactly once.  Child references to nodes the
     * catalog map calls free are discarded; links and index keys are then
     * derived from that independently proven allocated-node order.  No node
     * is allocated or freed, no leaf record or header identity is changed,
     * and an ambiguous or incomplete topology is refused.  A strict raw
     * audit is repeated after commit.  Callers must gate this with durable
     * proof of full guest power-off, exactly like allow_unclean_source.
     *
     * This is deliberately separate from repair_catalog_backlinks.  The two
     * repair modes are mutually exclusive so a caller cannot accidentally
     * weaken the narrow backlink-only contract.
     */
    bool repair_catalog_topology;

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
     * Existing regular files whose BSD metadata may be migrated in the same
     * unpublished catalog transaction. Repairs run after new-entry planning;
     * every repair is fully identity-checked before the first catalog write.
     * A zero count is the default and does not open the catalog by itself.
     */
    const rootfs_work_file_repair_t *file_repairs;
    size_t file_repair_count;

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

    /*
     * OPTIONAL PROGRESS OBSERVER, and it exists because this call takes long
     * enough that a user is entitled to be told.  Creating a work image copies
     * ~433 MB and then grows the volume; on a phone that is tens of seconds
     * during which the app could only say "Preparing iPhone OS" and hope.
     *
     * Called during the copy with the bytes done and the total, and once more
     * at completion so a bar can reach its end rather than stopping at 99%.
     * `total` is the source size and never changes within one call.
     *
     * RATE-LIMITED BY THE CALLER'S BUFFER, not by a timer: it fires once per
     * I/O chunk, which at the default buffer is a few hundred calls over the
     * whole copy -- frequent enough to look smooth, rare enough that a
     * callback doing UI work cannot dominate the copy it is reporting on.
     *
     * MUST NOT touch the image, must not block for long, and must tolerate
     * being called from whatever thread the caller used.  It cannot cancel:
     * returning nothing keeps this a report rather than a control, so a
     * progress bar cannot leave a half-built image behind.
     */
    void (*progress)(void *ctx, uint64_t done, uint64_t total);
    void  *progress_ctx;
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
    /* UINT64_MAX unless the PPP launchd-job rewrite actually ran. */
    uint64_t ppp_plist_offset;
    /* Catalog provisioning, all zero unless entries were requested. */
    uint32_t provision_entries;
    /* Existing directories or symlinks accepted by an explicit reuse policy. */
    uint32_t provision_reused_entries;
    uint32_t provision_first_cnid;
    uint32_t provision_last_cnid;
    uint32_t provision_blocks;
    uint32_t file_repairs_applied;
    uint32_t file_repairs_satisfied;
    /*
     * B-tree nodes the request had to add, reported rather than absorbed: a
     * split changes the catalog's shape, and a caller comparing two work
     * images is entitled to know it happened.  Both stay zero when every
     * record fitted where it belonged.
     */
    uint32_t provision_leaf_splits;
    uint32_t provision_index_splits;
    /* Populated only after the complete tolerant audit succeeds. */
    uint32_t catalog_backlinks_repairable;
    /* Populated only after those repairs are committed and strictly re-read. */
    uint32_t catalog_backlinks_repaired;
    /* Allocated nodes whose derived topology differs in the recovery plan. */
    uint32_t catalog_topology_nodes_repairable;
    /* Populated only after the topology commit passes a strict raw re-audit. */
    uint32_t catalog_topology_nodes_repaired;
    /* Index records naming map-free nodes, removed by the proven plan. */
    uint32_t catalog_topology_stale_refs;
    /* Referenced volume blocks the allocation bitmap incorrectly called free. */
    uint32_t allocation_missing_blocks;
    /* Bitmap-used blocks not named by any completely audited fork. */
    uint32_t allocation_orphan_blocks;
    /* Blocks named by more than one non-sharing HFS+ extent. */
    uint32_t allocation_extent_collisions;
    /* Catalog fork records whose unique extent/logical-size pair was fixed. */
    uint32_t catalog_extent_records_repairable;
    uint32_t catalog_extent_records_repaired;
    /* Allocation-bitmap bits changed by the complete reference reconstruction. */
    uint32_t allocation_bits_repairable;
    uint32_t allocation_bits_repaired;
    uint8_t source_sha256[IOS3_SHA256_DIGEST_SIZE];
    size_t io_buffer_bytes;
    bool source_sha256_valid;
    bool source_identity_verified;
    bool source_unclean_accepted;
    bool published;
    bool temporary_left;
    char detail[ROOTFS_WORK_DETAIL_CAPACITY];
} rootfs_work_result_t;

/*
 * Read-only validation of one prospective source image. This applies the
 * exact HFS format, geometry, allocation-bitmap, clean-unmount and source-file
 * identity checks that rootfs_work_create() performs before it creates a
 * temporary destination. It never hashes, transforms or publishes the source,
 * and leaves final_size/published zero on success.
 *
 * Callers that need to create their own transaction directory can therefore
 * reject a dirty or malformed source before making that durable transaction
 * visible. rootfs_work_create() still repeats every check: this preflight is a
 * user-facing early refusal, not a substitute for the copy-time race gate.
 */
rootfs_work_status_t rootfs_work_validate_source(
    const char *source_path, rootfs_work_result_t *result);

/*
 * Same read-only validation, with one narrowly scoped exception for a caller
 * that has already proved a full guest power-off by an independent durable
 * witness. `allow_unclean_source` bypasses only the missing unmounted bit; it
 * does not bypass any structural check or modify the source.
 */
rootfs_work_status_t rootfs_work_validate_source_ex(
    const char *source_path, bool allow_unclean_source,
    rootfs_work_result_t *result);

/*
 * Read-only preflight for one exact file repair. The same HFS validation,
 * catalog audit, path resolution, data-fork identity and BSD metadata checks
 * are repeated by rootfs_work_create(). No destination or temporary file is
 * created. A missing path is an OK result with state MISSING; an unexpected
 * object is a named mismatch/unsupported/corruption refusal.
 */
rootfs_work_status_t rootfs_work_probe_file_repair(
    const char *source_path, const rootfs_work_file_repair_t *repair,
    rootfs_work_file_repair_state_t *state,
    rootfs_work_result_t *result);

/* Exact-file probe counterpart to rootfs_work_validate_source_ex(). */
rootfs_work_status_t rootfs_work_probe_file_repair_ex(
    const char *source_path, const rootfs_work_file_repair_t *repair,
    bool allow_unclean_source, rootfs_work_file_repair_state_t *state,
    rootfs_work_result_t *result);

/*
 * Powered-off recovery preflight.  This is still read-only: when
 * allow_catalog_backlink_recovery is true it accepts only the deterministic
 * stale-bLink shape described by repair_catalog_backlinks and reports the
 * count in catalog_backlinks_repairable.  It never changes the source.  The
 * recovery flag is invalid unless allow_unclean_source is also true.
 */
rootfs_work_status_t rootfs_work_probe_file_repair_policy(
    const char *source_path, const rootfs_work_file_repair_t *repair,
    bool allow_unclean_source, bool allow_catalog_backlink_recovery,
    rootfs_work_file_repair_state_t *state,
    rootfs_work_result_t *result);

/*
 * Repair one CALLER-OWNED, UNPUBLISHED clone in place after the caller has
 * durably proved a full guest power-off.  This avoids copying a multi-GiB work
 * image twice: the app can create a filesystem clone in its transaction
 * directory, call this function, strictly validate the repaired clone, and
 * atomically publish it only on success.
 *
 * The function obtains exclusive read/write access, rejects links and path
 * indirection, performs the same bounded topology proof documented by
 * repair_catalog_topology, plans every byte before the first write, fsyncs the
 * clone, and never renames or publishes it.  Once the call reaches
 * PROVISION_WRITE the clone may have been modified even if a later write,
 * validation, or sync fails; the caller must discard that unpublished clone.
 * Passing a live or otherwise published disk is a contract violation.
 */
rootfs_work_status_t rootfs_work_repair_powered_off_catalog_clone(
    const char *clone_path, rootfs_work_result_t *result);

/*
 * Perform the broader powered-off repair used by the boot transaction.  It
 * includes the catalog-topology proof above, then independently reconstructs
 * the complete volume-block reference set from the reserved areas, all five
 * special files, every allocated catalog leaf, and the attributes tree.
 * Extents-overflow records, attribute fork records, unknown attribute record
 * types, ambiguous collisions, and unrecognised content are refused.
 *
 * A duplicated one-block catalog extent may be redirected to one orphaned
 * allocated block only when an exact binary-property-list parser finds one and
 * only one complete assignment compatible with both catalog logical sizes.
 * Referenced-but-free bits and redundant freeBlocks accounting are then
 * derived from the proven final reference set.  The same unpublished-clone,
 * exclusive-open, write-plan, fsync, and no-publication contract applies.
 */
rootfs_work_status_t rootfs_work_repair_powered_off_clone(
    const char *clone_path, rootfs_work_result_t *result);

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
 * phone.  docs/derivations.md 23.3 has the derivation.
 *
 * Returns the number of entries required (2).  Fills `entries` only when
 * `capacity` is at least that, so a caller can size an array from the return
 * value.  The filled entries point at static storage and stay valid forever.
 */
size_t rootfs_work_activation_entries(rootfs_work_entry_t *entries,
                                      size_t capacity);

/*
 * The guest's own pppd, given somewhere to send.
 *
 * run129 measured a link that comes all the way up -- LCP Opened, IPCP Opened,
 * guest 10.0.2.15 -- and then carried zero IP datagrams for the following 1.2
 * billion instructions.  The NAT's ip_in was 0 with every refusal counter also
 * 0 and the egress open, so nothing was dropped and nothing arrived.
 *
 * The cause is not in the emulator.  The launchd job runs pppd with `local
 * nocrtscts nodetach` and no `defaultroute`, so ppp0 comes up holding an
 * address that nothing in the guest's routing table points at.
 *
 * That job is written in place at the stock file's exact 530 bytes and has
 * four bytes of slack, which is nowhere near the thirty an extra argument
 * costs.  pppd reads /etc/ppp/options before argv, so the option goes THERE
 * instead, where there is no size constraint at all -- the same catalog writer
 * that provisions data_ark.plist creates it.  A missing options file is not
 * fatal to pppd, which is why the file does not exist to begin with and why
 * creating it needs the catalog rather than an overwrite.
 *
 * /private/etc/ppp already exists on the stock volume, so only the options
 * file is created; asking for the parent too made the provisioner refuse the
 * whole plan with "an object already exists under CNID 1410".
 *
 * The options file carries the fixed pppd service ID, resolv.conf remains a
 * libc fallback, and the SystemConfiguration directory plus preferences.plist
 * give configd the matching persistent PPPSerial service. These are one
 * transaction: a short buffer is left untouched rather than producing a
 * half-described network. The stock image has /private/var/preferences but not
 * its SystemConfiguration child, so the directory entry is required.
 *
 * Returns the number of entries required (4). Fills `entries` only when
 * `capacity` is at least that. The filled entries point at static storage.
 */
size_t rootfs_work_ppp_entries(rootfs_work_entry_t *entries, size_t capacity);

/*
 * The shared image-time plan used by both the desktop harness and the iOS app.
 * Keeping the merge here prevents one frontend from enabling the PPP launchd
 * rewrite while forgetting the files that make the resulting link usable.
 * Returns the required count and fills nothing unless the whole requested
 * activation/PPP plan fits.
 */
size_t rootfs_work_standard_entries(bool activate, bool ppp,
                                    rootfs_work_entry_t *entries,
                                    size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_ROOTFS_WORK_H */
