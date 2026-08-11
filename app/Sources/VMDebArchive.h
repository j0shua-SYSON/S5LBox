/*
 * S5LBox -- strict reader for the Debian packages used by guest provisioning.
 *
 * This owns no package catalogue and downloads nothing. It validates one
 * caller-owned .deb byte buffer, exposes its control/data spans, and expands a
 * gzip member through the already-tested portable DEFLATE decoder. Exact
 * package size and SHA-256 checks belong to the manifest layer above it.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMDEBARCHIVE_H
#define S5LBOX_APP_VMDEBARCHIVE_H

#include "VMFirmwareFormats.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VM_DEB_COMPRESSION_GZIP = 0,
    VM_DEB_COMPRESSION_BZIP2,
    VM_DEB_COMPRESSION_LZMA,
    VM_DEB_COMPRESSION_XZ
} vm_deb_compression_t;

typedef struct {
    const uint8_t *bytes;       /* points into the caller-owned .deb buffer */
    size_t size;
    vm_deb_compression_t compression;
} vm_deb_member_t;

typedef struct {
    vm_deb_member_t control;
    vm_deb_member_t data;
} vm_deb_archive_t;

typedef enum {
    VM_DEB_OK = 0,
    VM_DEB_ERR_INVALID_ARGUMENT,
    VM_DEB_ERR_NOT_ARCHIVE,
    VM_DEB_ERR_TRUNCATED,
    VM_DEB_ERR_HEADER,
    VM_DEB_ERR_SIZE_FIELD,
    VM_DEB_ERR_PADDING,
    VM_DEB_ERR_MEMBER_ORDER,
    VM_DEB_ERR_VERSION,
    VM_DEB_ERR_COMPRESSION,
    VM_DEB_ERR_TRAILING_DATA,
    VM_DEB_ERR_GZIP_HEADER,
    VM_DEB_ERR_GZIP_TRUNCATED,
    VM_DEB_ERR_GZIP_SIZE_LIMIT,
    VM_DEB_ERR_GZIP_INFLATE,
    VM_DEB_ERR_GZIP_TRAILING_DATA,
    VM_DEB_ERR_GZIP_CRC,
    VM_DEB_ERR_SINK
} vm_deb_status_t;

/* Accepts the era-correct three-member form only:
 *   debian-binary (exactly "2.0\n"), control.tar.*, data.tar.*
 * Known gzip/bzip2/lzma/xz member names are identified, but this layer only
 * expands gzip. The guest's own dpkg handles the later compressed packages. */
vm_deb_status_t vm_deb_archive_open(vm_deb_archive_t *out,
                                    const uint8_t *bytes, size_t size);

/* Parse a complete single-member gzip wrapper and return its 32-bit ISIZE.
 * This is a sizing result, not integrity proof; inflate performs CRC-32,
 * exact-size and no-trailing-compressed-data checks. */
vm_deb_status_t vm_deb_gzip_size(const vm_deb_member_t *member,
                                 uint32_t *out_size);

/* Expand one gzip member. `max_output` is a caller policy cap checked before
 * the first sink call. `inflate_status` receives the underlying RFC 1951
 * refusal when this returns VM_DEB_ERR_GZIP_INFLATE. */
vm_deb_status_t
vm_deb_gzip_inflate(const vm_deb_member_t *member,
                    vmfw_sink_fn sink, void *sink_ctx,
                    uint64_t max_output, uint64_t *out_produced,
                    vmfw_inflate_status_t *inflate_status);

const char *vm_deb_status_text(vm_deb_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMDEBARCHIVE_H */
