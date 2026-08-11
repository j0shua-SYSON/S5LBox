/* See VMGuestPackageFile.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMGuestPackageFile.h"

#include "sha256.h"

#include <stdio.h>
#include <string.h>

static void file_detail(char *detail, size_t capacity, const char *text) {
    if (!detail || capacity == 0u) return;
    (void)snprintf(detail, capacity, "%s", text ? text : "");
    detail[capacity - 1u] = '\0';
}

vm_guest_package_file_status_t
vm_guest_package_file_verify(const vm_guest_package_t *package,
                             const char *path,
                             uint64_t *size_out,
                             char *detail, size_t detail_capacity) {
    if (size_out) *size_out = 0u;
    file_detail(detail, detail_capacity, "");
    if (!package || !path || !*path || package->size == 0u) {
        file_detail(detail, detail_capacity,
                    "The package record or file path is missing.");
        return VM_GUEST_PACKAGE_FILE_ERR_ARGUMENT;
    }

    uint8_t expected[VM_GUEST_PACKAGE_SHA256_SIZE];
    if (!vm_guest_package_expected_sha256(package, expected)) {
        file_detail(detail, detail_capacity,
                    "The package record has an invalid SHA-256 identity.");
        return VM_GUEST_PACKAGE_FILE_ERR_ARGUMENT;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        file_detail(detail, detail_capacity,
                    "The downloaded package cannot be opened.");
        return VM_GUEST_PACKAGE_FILE_ERR_OPEN;
    }

    ios3_sha256_context_t context;
    uint8_t buffer[64u * 1024u];
    uint64_t total = 0u;
    bool hash_ok = ios3_sha256_init(&context);
    bool read_ok = true;
    while (hash_ok) {
        size_t amount = fread(buffer, 1u, sizeof buffer, file);
        if (amount > 0u) {
            if (total > UINT64_MAX - (uint64_t)amount) {
                hash_ok = false;
                break;
            }
            total += (uint64_t)amount;
            if (total > package->size) break;
            hash_ok = ios3_sha256_update(&context, buffer, amount);
        }
        if (amount < sizeof buffer) {
            if (ferror(file)) read_ok = false;
            break;
        }
    }
    if (fclose(file) != 0) read_ok = false;
    if (size_out) *size_out = total;
    if (!read_ok || !hash_ok) {
        file_detail(detail, detail_capacity,
                    "The downloaded package could not be read completely.");
        return VM_GUEST_PACKAGE_FILE_ERR_READ;
    }
    if (total != package->size) {
        file_detail(detail, detail_capacity,
                    "The downloaded package has the wrong byte count.");
        return VM_GUEST_PACKAGE_FILE_ERR_SIZE;
    }

    uint8_t actual[VM_GUEST_PACKAGE_SHA256_SIZE];
    if (!ios3_sha256_final(&context, actual)) {
        file_detail(detail, detail_capacity,
                    "The downloaded package hash could not be finalized.");
        return VM_GUEST_PACKAGE_FILE_ERR_READ;
    }
    if (memcmp(actual, expected, sizeof actual) != 0) {
        file_detail(detail, detail_capacity,
                    "The downloaded package failed its SHA-256 gate.");
        return VM_GUEST_PACKAGE_FILE_ERR_DIGEST;
    }

    return VM_GUEST_PACKAGE_FILE_OK;
}

const char *vm_guest_package_file_status_text(
    vm_guest_package_file_status_t status) {
    switch (status) {
        case VM_GUEST_PACKAGE_FILE_OK:           return "ok";
        case VM_GUEST_PACKAGE_FILE_ERR_ARGUMENT: return "invalid argument";
        case VM_GUEST_PACKAGE_FILE_ERR_OPEN:     return "open failure";
        case VM_GUEST_PACKAGE_FILE_ERR_READ:     return "read failure";
        case VM_GUEST_PACKAGE_FILE_ERR_SIZE:     return "size mismatch";
        case VM_GUEST_PACKAGE_FILE_ERR_DIGEST:   return "digest mismatch";
        default:                                  return "unknown status";
    }
}
