/*
 * S5LBox -- authenticate one downloaded guest package file.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMGUESTPACKAGEFILE_H
#define S5LBOX_APP_VMGUESTPACKAGEFILE_H

#include "VMGuestPackageManifest.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VM_GUEST_PACKAGE_FILE_OK = 0,
    VM_GUEST_PACKAGE_FILE_ERR_ARGUMENT,
    VM_GUEST_PACKAGE_FILE_ERR_OPEN,
    VM_GUEST_PACKAGE_FILE_ERR_READ,
    VM_GUEST_PACKAGE_FILE_ERR_SIZE,
    VM_GUEST_PACKAGE_FILE_ERR_DIGEST
} vm_guest_package_file_status_t;

/*
 * Stream one file through the project's SHA-256 implementation and accept it
 * only when both byte count and digest match the pinned package record. The
 * file is never mapped or retained in memory, which keeps this suitable for a
 * download callback on the oldest supported phones.
 */
vm_guest_package_file_status_t
vm_guest_package_file_verify(const vm_guest_package_t *package,
                             const char *path,
                             uint64_t *size_out,
                             char *detail, size_t detail_capacity);

const char *vm_guest_package_file_status_text(
    vm_guest_package_file_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMGUESTPACKAGEFILE_H */
