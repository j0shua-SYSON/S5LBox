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
    bool powered_off_checkpoint_witnessed;
    size_t historical_snapshots;
    vm_guest_rootfs_stats_t plan;
    rootfs_work_result_t rootfs;
    vm_guest_install_result_t transaction;
    vm_guest_install_result_t storage_transaction;
    vm_guest_install_result_t privilege_transaction;
    vm_guest_install_result_t sources_transaction;
    uint8_t manifest_sha256[VM_GUEST_INSTALL_SHA256_SIZE];
} vm_guest_install_build_result_t;

/*
 * Build from <work>/rootfs-work.img to the transaction's private stage, then
 * publish it. The machine must be completely stopped before this call and must
 * remain stopped through completion. The source-change checks in rootfs_work
 * are a final refusal, not a substitute for that lifecycle precondition.
 *
 * A valid existing v1 marker needs no package cache. If that installation's
 * live HFS image predates the 2 GiB minimum, a separate crash-safe storage
 * transaction clones and grows it while the v1 boot-policy marker remains
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
 */
vm_guest_install_build_status_t
vm_guest_install_build_from_directory(
    const char *work_directory, const char *package_directory,
    vm_guest_install_build_progress_t progress, void *progress_context,
    vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity);

const char *vm_guest_install_build_status_text(
    vm_guest_install_build_status_t status);
const char *vm_guest_install_build_phase_text(
    vm_guest_install_build_phase_t phase);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMGUESTINSTALLBUILD_H */
