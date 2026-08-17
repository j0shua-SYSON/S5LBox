/*
 * S5LBox -- verified package bytes turned into one bounded rootfs overlay.
 *
 * This layer downloads nothing and publishes no disk. It authenticates each
 * caller-owned package, expands only the gzip foundation needed to start the
 * guest's own dpkg, remaps iPhone OS root aliases, and owns the resulting
 * rootfs_work_entry_t array until close.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMGUESTROOTFSPLAN_H
#define S5LBOX_APP_VMGUESTROOTFSPLAN_H

#include "VMGuestPackageManifest.h"
#include "rootfs_work.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VM_GUEST_ROOTFS_DETAIL_CAPACITY 256u
#define VM_GUEST_ROOTFS_MAX_PACKAGES 64u
#define VM_GUEST_ROOTFS_MAX_DOWNLOAD_BYTES (64u * 1024u * 1024u)

#define VM_GUEST_ROOTFS_STATE_DIRECTORY "/private/var/lib/s5lbox"
#define VM_GUEST_ROOTFS_PACKAGE_DIRECTORY \
    "/private/var/lib/s5lbox/packages"
#define VM_GUEST_ROOTFS_INSTALL_SCRIPT \
    "/usr/libexec/s5lbox-guest-install"
#define VM_GUEST_ROOTFS_LAUNCHD_PLIST \
    "/System/Library/LaunchDaemons/com.j0shua.s5lbox.guest-install.plist"

/* Period-compatible APT sources owned by the guest installer. Keep each
 * source in its own file so a later migration can add a missing repository
 * without replacing user-managed APT configuration. */
#define VM_GUEST_ROOTFS_SAURIK_SOURCE_PATH \
    "/private/etc/apt/sources.list.d/saurik.list"
#define VM_GUEST_ROOTFS_SAURIK_SOURCE_LINE \
    "deb http://apt.saurik.com/cydia/ ./\n"
#define VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH \
    "/private/etc/apt/sources.list.d/s5lbox-bigboss.list"
#define VM_GUEST_ROOTFS_BIGBOSS_SOURCE_LINE \
    "deb http://apt.thebigboss.org/repofiles/cydia/ stable main\n"
#define VM_GUEST_ROOTFS_TRUSTED_KEYRING_PATH \
    "/private/etc/apt/trusted.gpg"

/* Exact legacy BigBoss public signing key. The returned storage is static and
 * remains valid for the process lifetime. */
const uint8_t *vm_guest_rootfs_bigboss_keyring(size_t *out_size);

typedef enum {
    VM_GUEST_ROOTFS_OK = 0,
    VM_GUEST_ROOTFS_ERR_ARGUMENT,
    VM_GUEST_ROOTFS_ERR_MANIFEST,
    VM_GUEST_ROOTFS_ERR_PACKAGE_IO,
    VM_GUEST_ROOTFS_ERR_PACKAGE_IDENTITY,
    VM_GUEST_ROOTFS_ERR_ARCHIVE,
    VM_GUEST_ROOTFS_ERR_COMPRESSION,
    VM_GUEST_ROOTFS_ERR_TAR,
    VM_GUEST_ROOTFS_ERR_ENTRY,
    VM_GUEST_ROOTFS_ERR_LIMIT,
    VM_GUEST_ROOTFS_ERR_MEMORY,
    VM_GUEST_ROOTFS_ERR_SCRIPT
} vm_guest_rootfs_status_t;

typedef struct {
    const vm_guest_package_t *package;
    const uint8_t *bytes;
    size_t size;
} vm_guest_package_input_t;

typedef struct {
    size_t packages;
    size_t foundation_packages;
    size_t entries;
    size_t deduplicated_entries;
    size_t files;
    size_t directories;
    size_t symlinks;
    size_t hardlinks_materialised;
    uint64_t download_bytes;
    uint64_t foundation_tar_bytes;
    uint64_t provision_content_bytes;
} vm_guest_rootfs_stats_t;

typedef struct vm_guest_rootfs_plan vm_guest_rootfs_plan_t;

/* Copies all package and expanded tar bytes it needs. Inputs may be released
 * immediately after return. Metadata records need not outlive the call. */
vm_guest_rootfs_plan_t *
vm_guest_rootfs_plan_open(const vm_guest_package_input_t *inputs,
                          size_t input_count,
                          vm_guest_rootfs_status_t *out_status,
                          char *detail, size_t detail_capacity);

/* Production convenience: load every record in the shipping manifest from a
 * directory containing the exact manifest filenames. */
vm_guest_rootfs_plan_t *
vm_guest_rootfs_plan_open_directory(const char *directory,
                                    vm_guest_rootfs_status_t *out_status,
                                    char *detail, size_t detail_capacity);

void vm_guest_rootfs_plan_close(vm_guest_rootfs_plan_t **slot);

const rootfs_work_entry_t *
vm_guest_rootfs_plan_entries(const vm_guest_rootfs_plan_t *plan);
size_t vm_guest_rootfs_plan_entry_count(const vm_guest_rootfs_plan_t *plan);
void vm_guest_rootfs_plan_get_stats(const vm_guest_rootfs_plan_t *plan,
                                    vm_guest_rootfs_stats_t *out);
bool vm_guest_rootfs_plan_manifest_sha256(
    const vm_guest_rootfs_plan_t *plan,
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE]);

const char *vm_guest_rootfs_status_text(vm_guest_rootfs_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMGUESTROOTFSPLAN_H */
