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
#define VM_GUEST_INSTALL_ADDITIONAL_GROWTH_BYTES \
    (UINT64_C(32) * 1024u * 1024u)

typedef enum {
    VM_GUEST_INSTALL_BUILD_OK = 0,
    VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT,
    VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION,
    VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS,
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
    size_t historical_snapshots;
    vm_guest_rootfs_stats_t plan;
    rootfs_work_result_t rootfs;
    vm_guest_install_result_t transaction;
    uint8_t manifest_sha256[VM_GUEST_INSTALL_SHA256_SIZE];
} vm_guest_install_build_result_t;

/*
 * Build from <work>/rootfs-work.img to the transaction's private stage, then
 * publish it. The machine must be completely stopped before this call and must
 * remain stopped through completion. The source-change checks in rootfs_work
 * are a final refusal, not a substitute for that lifecycle precondition.
 *
 * A valid existing v1 marker is idempotent success and needs no package cache.
 * Upgrades use a new transaction version rather than mutating an installed v1.
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
