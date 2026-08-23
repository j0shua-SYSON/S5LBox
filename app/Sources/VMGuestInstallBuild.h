/*
 * S5LBox -- build and atomically publish one verified guest installation.
 *
 * Downloads happen above this portable layer. The caller supplies one
 * directory containing the exact pinned package filenames and guarantees the
 * machine is stopped. This layer refuses historical snapshots, constructs an
 * unpublished HFS image, and hands it to VMGuestInstall's recovery journal.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMGUESTINSTALLBUILD_H
#define S5LBOX_APP_VMGUESTINSTALLBUILD_H

#include "VMGuestInstall.h"
#include "VMGuestRootfsPlan.h"
#include "rootfs_work.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY 256u
#define VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES \
    (UINT64_C(2) * 1024u * 1024u * 1024u)

typedef enum {
    VM_GUEST_INSTALL_BUILD_OK = 0,
    VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT,
    VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION,
    VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS,
    VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN,
    VM_GUEST_INSTALL_BUILD_ERR_PACKAGES,
    VM_GUEST_INSTALL_BUILD_ERR_MANIFEST,
    VM_GUEST_INSTALL_BUILD_ERR_PATH,
    VM_GUEST_INSTALL_BUILD_ERR_ROOTFS,
    VM_GUEST_INSTALL_BUILD_ERR_PUBLISH
} vm_guest_install_build_status_t;

typedef enum {
    VM_GUEST_INSTALL_BUILD_RECOVERING = 0,
    VM_GUEST_INSTALL_BUILD_PLANNING,
    VM_GUEST_INSTALL_BUILD_STAGING,
    VM_GUEST_INSTALL_BUILD_COPYING,
    VM_GUEST_INSTALL_BUILD_PUBLISHING,
    VM_GUEST_INSTALL_BUILD_COMPLETE
} vm_guest_install_build_phase_t;

typedef void (*vm_guest_install_build_progress_t)(
    void *context, vm_guest_install_build_phase_t phase,
    uint64_t completed, uint64_t total);

typedef struct {
    bool already_installed;
    bool storage_upgraded;
    bool cydia_privileges_repaired;
    bool cydia_privileges_verified;
    bool cydia_sources_added;
    bool cydia_sources_verified;
    bool apt_trust_installed;
    bool apt_trust_verified;
    bool apt_verifier_staged;
    bool apt_verifier_verified;
    /* The exact legacy APT tools and retryable boot job are on disk. This is
     * deliberately not named "verified": guest cache completion is runtime
     * evidence and cannot be inferred from the host disk transaction. */
    bool cydia_cache_staged;
    bool filesystem_repaired;
    bool powered_off_checkpoint_witnessed;
    size_t historical_snapshots;
    vm_guest_rootfs_stats_t plan;
    rootfs_work_result_t rootfs;
    rootfs_work_result_t filesystem_recovery;
    vm_guest_install_result_t transaction;
    vm_guest_install_result_t filesystem_recovery_transaction;
    vm_guest_install_result_t storage_transaction;
    vm_guest_install_result_t privilege_transaction;
    /* Historical v1 source transaction, retained for safe upgrade recovery. */
    vm_guest_install_result_t sources_transaction;
    /* Current repository-cache compatibility transaction. */
    vm_guest_install_result_t sources_v2_transaction;
    /* Legacy APT's exact trusted.gpg compatibility transaction. */
    vm_guest_install_result_t apt_trust_transaction;
    /* Exact legacy gnupg package and one-shot guest-dpkg transaction. */
    vm_guest_install_result_t apt_verifier_transaction;
    /* Out-of-process cache-builder deployment; not guest completion proof. */
    vm_guest_install_result_t cydia_cache_transaction;
    uint8_t manifest_sha256[VM_GUEST_INSTALL_SHA256_SIZE];
} vm_guest_install_build_result_t;

/*
 * Build from <work>/rootfs-work.img to the transaction's private stage, then
 * publish it. The machine must be completely stopped before this call and must
 * remain stopped through completion. The source-change checks in rootfs_work
 * are a final refusal, not a substitute for that lifecycle precondition.
 *
 * A valid existing v1 marker normally needs no package cache. A guest missing
 * the signature verifier requests only the exact pinned archive needed for
 * its repair. If the live HFS image predates the 2 GiB minimum, a separate
 * crash-safe storage transaction clones and grows it while the v1 boot-policy
 * marker remains
 * continuously authoritative. Because a running checkpoint leaves this
 * unjournaled HFS volume legitimately dirty. Physical iPhone OS 3.1.3 testing
 * proved that even RB_HALT/PMU-standby can leave the primary header's clean bit
 * unset, so the builder accepts that one case only when the exact automatic
 * checkpoint independently verifies GO_STANDBY against the same disk size.
 * Every structural HFS audit still runs and the output remains marked dirty
 * for the guest's next fsck. A running, absent, corrupt or size-mismatched
 * checkpoint still refuses before a transaction is staged. A machine already
 * at the minimum is idempotent success with no disk rewrite.
 *
 * A committed older install may also carry Cydia_'s historical root:root 0755
 * metadata. The builder probes the exact 320704-byte pinned executable by
 * SHA-256 and accepts only that legacy tuple or root:root 06755. A needed
 * repair is applied to an unpublished clone under its own versioned recovery
 * journal (or folded into the same clone when storage also needs growth).
 * Different bytes or a third metadata tuple are refused, never normalized.
 *
 * The same maintenance pass also ensures the installer-owned BigBoss source
 * file exists as exact root:root 0644 data. A missing file is created in the
 * unpublished clone; an unexpected existing file is refused rather than
 * overwritten. Its independent marker makes retries idempotent for guests
 * installed by older rootfs plans.
 *
 * Legacy APT 0.7 reads one /etc/apt/trusted.gpg and does not scan the modern
 * trusted.gpg.d convention. Fresh plans seed the exact verified BigBoss public
 * key; older installations receive it through another crash-safe transaction.
 * The exact known historical multi-key ring is preserved, while any unknown
 * existing trust store is refused instead of overwritten. Signature checking
 * is never disabled.
 *
 * That APT build also carries only /usr/lib/apt/methods/gpgv: the executable
 * verifier it invokes is supplied by the period-compatible gnupg package.
 * Fresh plans install that exact archive. Older guests atomically receive the
 * archive and a one-shot guest-dpkg job, preserving package ownership and the
 * dpkg database instead of copying an untracked binary into /usr/bin.
 *
 * A strict source preflight also distinguishes an exact HFS freeBlocks/bitmap
 * disagreement from every other validation failure without parsing diagnostic
 * prose. For a cleanly unmounted disk, or one independently authorized by the
 * exact powered-off checkpoint, that condition is repaired only on an
 * unpublished recovery clone. The complete powered-off scanner determines
 * whether the header, bitmap, or a uniquely identifiable catalog extent is
 * stale; it may also canonicalize derivable catalog topology. Ambiguous
 * content remains a refusal. Geometry must stay identical, every planned
 * mutation must be reported as applied, the raw clone must pass strict
 * revalidation, historical snapshots still block the operation, and only the
 * crash-safe recovery journal can publish it.
 */
vm_guest_install_build_status_t
vm_guest_install_build_from_directory(
    const char *work_directory, const char *package_directory,
    vm_guest_install_build_progress_t progress, void *progress_context,
    vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity);

#if defined(S5LBOX_GUEST_INSTALL_TESTING)
/* Pure policy seam: production reaches the same predicate only after the
 * unpublished clone's complete repair and strict raw re-audit. */
bool vm_guest_install_build_test_allocation_repair_proven(
    const rootfs_work_result_t *preflight,
    const rootfs_work_result_t *repair, uint64_t live_size);

/* Returns the required entry count and fills nothing when capacity is too
 * small. This keeps the legacy-stashing compatibility policy directly
 * testable without publishing an image. */
size_t vm_guest_install_build_test_bigboss_source_entries(
    rootfs_work_entry_t *entries, size_t capacity, bool create_source);
size_t vm_guest_install_build_test_apt_trust_entries(
    rootfs_work_entry_t *entries, size_t capacity, bool create_keyring);
size_t vm_guest_install_build_test_apt_verifier_entries(
    rootfs_work_entry_t *entries, size_t capacity,
    const uint8_t *package, size_t package_size);
size_t vm_guest_install_build_test_cydia_cache_entries(
    rootfs_work_entry_t *entries, size_t capacity,
    const uint8_t *package, size_t package_size);
#endif

const char *vm_guest_install_build_status_text(
    vm_guest_install_build_status_t status);
const char *vm_guest_install_build_phase_text(
    vm_guest_install_build_phase_t phase);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMGUESTINSTALLBUILD_H */
