/*
 * S5LBox -- pinned, download-only package manifest for guest provisioning.
 *
 * The app contains metadata, never package payload bytes. Every package is
 * fetched from the original archive over HTTPS and must match both the exact
 * byte count and SHA-256 below before any archive parser sees it.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMGUESTPACKAGEMANIFEST_H
#define S5LBOX_APP_VMGUESTPACKAGEMANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VM_GUEST_PACKAGE_SHA256_SIZE 32u
#define VM_GUEST_PACKAGE_SHA256_HEX_SIZE 65u

typedef enum {
    /* Copy the original package into the guest cache and ask guest dpkg to
     * unpack/configure it on first boot. */
    VM_GUEST_PACKAGE_INSTALL = 1u << 0,

    /* Its gzip data member also seeds the staged disk on the host. These are
     * the tools required for the old guest dpkg to run in the first place. */
    VM_GUEST_PACKAGE_FOUNDATION = 1u << 1
} vm_guest_package_role_t;

typedef struct {
    const char *package;
    const char *version;
    const char *filename;
    const char *source_url;
    uint64_t size;
    const char *sha256_hex;
    uint32_t roles;
} vm_guest_package_t;

size_t vm_guest_package_count(void);
const vm_guest_package_t *vm_guest_package_at(size_t index);
const vm_guest_package_t *vm_guest_package_find(const char *package);

/* Validate one caller-owned record, or the complete shipping manifest. */
bool vm_guest_package_validate_entry(const vm_guest_package_t *package,
                                     char *why, size_t why_capacity);
bool vm_guest_package_manifest_validate(char *why, size_t why_capacity);

/* Decode the pinned lowercase hexadecimal digest and compare completed
 * download evidence without opening the package. */
bool vm_guest_package_expected_sha256(
    const vm_guest_package_t *package,
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE]);
bool vm_guest_package_download_matches(
    const vm_guest_package_t *package, uint64_t size,
    const uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE]);

/* Stable identity of the ordered metadata, including source URL and roles.
 * The higher-level install manifest will combine this with injected-script
 * and image-construction identities before publishing the transaction. */
bool vm_guest_package_manifest_sha256(
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE]);

uint64_t vm_guest_package_total_download_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMGUESTPACKAGEMANIFEST_H */
