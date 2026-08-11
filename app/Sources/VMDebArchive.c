/* See VMDebArchive.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMDebArchive.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VM_DEB_AR_HEADER_SIZE 60u
#define VM_DEB_AR_NAME_SIZE   16u
#define VM_DEB_AR_SIZE_OFFSET 48u
#define VM_DEB_AR_SIZE_SIZE   10u
#define VM_DEB_NAME_CAPACITY  32u

static const uint8_t VM_DEB_AR_MAGIC[8] = {
    '!', '<', 'a', 'r', 'c', 'h', '>', '\n'
};

typedef struct {
    const uint8_t *bytes;
    size_t size;
    size_t compressed_offset;
    size_t compressed_size;
    uint32_t crc32;
    uint32_t output_size;
} vm_deb_gzip_t;

static bool deb_decimal(const uint8_t *field, size_t size, size_t *out) {
    if (!field || !size || !out) return false;
    size_t first = 0u;
    while (first < size && field[first] == ' ') first++;
    size_t last = size;
    while (last > first && field[last - 1u] == ' ') last--;
    if (first == last) return false;

    size_t value = 0u;
    for (size_t i = first; i < last; i++) {
        if (field[i] < '0' || field[i] > '9') return false;
        unsigned digit = (unsigned)(field[i] - '0');
        if (value > (SIZE_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static bool deb_member_name(const uint8_t field[VM_DEB_AR_NAME_SIZE],
                            char out[VM_DEB_NAME_CAPACITY]) {
    size_t size = VM_DEB_AR_NAME_SIZE;
    while (size > 0u && field[size - 1u] == ' ') size--;
    if (size > 0u && field[size - 1u] == '/') size--;
    if (size == 0u || size >= VM_DEB_NAME_CAPACITY) return false;
    for (size_t i = 0u; i < size; i++) {
        if (field[i] < 0x21u || field[i] > 0x7eu ||
            field[i] == '/' || field[i] == '\\')
            return false;
        out[i] = (char)field[i];
    }
    out[size] = '\0';
    return true;
}

static bool deb_compression(const char *name, const char *stem,
                            vm_deb_compression_t *out) {
    if (!name || !stem || !out) return false;
    size_t n = strlen(stem);
    if (strlen(name) <= n || strncmp(name, stem, n) != 0) return false;
    const char *suffix = name + n;
    if (strcmp(suffix, ".gz") == 0)
        *out = VM_DEB_COMPRESSION_GZIP;
    else if (strcmp(suffix, ".bz2") == 0)
        *out = VM_DEB_COMPRESSION_BZIP2;
    else if (strcmp(suffix, ".lzma") == 0)
        *out = VM_DEB_COMPRESSION_LZMA;
    else if (strcmp(suffix, ".xz") == 0)
        *out = VM_DEB_COMPRESSION_XZ;
    else
        return false;
    return true;
}

vm_deb_status_t vm_deb_archive_open(vm_deb_archive_t *out,
                                    const uint8_t *bytes, size_t size) {
    if (out) memset(out, 0, sizeof *out);
    if (!out || !bytes) return VM_DEB_ERR_INVALID_ARGUMENT;
    if (size < sizeof VM_DEB_AR_MAGIC ||
        memcmp(bytes, VM_DEB_AR_MAGIC, sizeof VM_DEB_AR_MAGIC) != 0)
        return VM_DEB_ERR_NOT_ARCHIVE;

    size_t offset = sizeof VM_DEB_AR_MAGIC;
    for (unsigned index = 0u; index < 3u; index++) {
        if (size - offset < VM_DEB_AR_HEADER_SIZE)
            return VM_DEB_ERR_TRUNCATED;
        const uint8_t *header = bytes + offset;
        if (header[58] != '`' || header[59] != '\n')
            return VM_DEB_ERR_HEADER;

        char name[VM_DEB_NAME_CAPACITY];
        if (!deb_member_name(header, name)) return VM_DEB_ERR_HEADER;
        size_t member_size = 0u;
        if (!deb_decimal(header + VM_DEB_AR_SIZE_OFFSET,
                         VM_DEB_AR_SIZE_SIZE, &member_size))
            return VM_DEB_ERR_SIZE_FIELD;
        offset += VM_DEB_AR_HEADER_SIZE;
        if (member_size > size - offset) return VM_DEB_ERR_TRUNCATED;
        const uint8_t *member = bytes + offset;

        if (index == 0u) {
            static const uint8_t VERSION[4] = {'2', '.', '0', '\n'};
            if (strcmp(name, "debian-binary") != 0)
                return VM_DEB_ERR_MEMBER_ORDER;
            if (member_size != sizeof VERSION ||
                memcmp(member, VERSION, sizeof VERSION) != 0)
                return VM_DEB_ERR_VERSION;
        } else {
            vm_deb_member_t *destination =
                index == 1u ? &out->control : &out->data;
            const char *stem = index == 1u ? "control.tar" : "data.tar";
            if (strncmp(name, stem, strlen(stem)) != 0)
                return VM_DEB_ERR_MEMBER_ORDER;
            if (!deb_compression(name, stem, &destination->compression))
                return VM_DEB_ERR_COMPRESSION;
            destination->bytes = member;
            destination->size = member_size;
            if (member_size == 0u) return VM_DEB_ERR_TRUNCATED;
        }

        offset += member_size;
        if ((member_size & 1u) != 0u) {
            if (offset >= size) return VM_DEB_ERR_TRUNCATED;
            if (bytes[offset] != '\n') return VM_DEB_ERR_PADDING;
            offset++;
        }
    }

    if (offset != size) return VM_DEB_ERR_TRAILING_DATA;
    return VM_DEB_OK;
}

static uint16_t deb_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u));
}

static uint32_t deb_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static bool deb_gzip_terminated(const uint8_t *bytes, size_t end,
                                size_t *offset) {
    while (*offset < end && bytes[*offset] != 0u) (*offset)++;
    if (*offset >= end) return false;
    (*offset)++;
    return true;
}

static vm_deb_status_t deb_gzip_parse(const vm_deb_member_t *member,
                                      vm_deb_gzip_t *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!member || !out || !member->bytes ||
        member->compression != VM_DEB_COMPRESSION_GZIP)
        return VM_DEB_ERR_INVALID_ARGUMENT;
    if (member->size < 18u) return VM_DEB_ERR_GZIP_TRUNCATED;

    const uint8_t *bytes = member->bytes;
    if (bytes[0] != 0x1fu || bytes[1] != 0x8bu || bytes[2] != 8u ||
        (bytes[3] & 0xe0u) != 0u)
        return VM_DEB_ERR_GZIP_HEADER;
    uint8_t flags = bytes[3];
    size_t trailer = member->size - 8u;
    size_t offset = 10u;

    if ((flags & 0x04u) != 0u) {
        if (trailer - offset < 2u) return VM_DEB_ERR_GZIP_TRUNCATED;
        size_t extra = deb_le16(bytes + offset);
        offset += 2u;
        if (extra > trailer - offset) return VM_DEB_ERR_GZIP_TRUNCATED;
        offset += extra;
    }
    if ((flags & 0x08u) != 0u &&
        !deb_gzip_terminated(bytes, trailer, &offset))
        return VM_DEB_ERR_GZIP_TRUNCATED;
    if ((flags & 0x10u) != 0u &&
        !deb_gzip_terminated(bytes, trailer, &offset))
        return VM_DEB_ERR_GZIP_TRUNCATED;
    if ((flags & 0x02u) != 0u) {
        if (trailer - offset < 2u) return VM_DEB_ERR_GZIP_TRUNCATED;
        uint16_t wanted = deb_le16(bytes + offset);
        uint16_t actual = (uint16_t)(vmfw_crc32(0u, bytes, offset) & 0xffffu);
        if (wanted != actual) return VM_DEB_ERR_GZIP_HEADER;
        offset += 2u;
    }
    if (offset >= trailer) return VM_DEB_ERR_GZIP_TRUNCATED;

    out->bytes = bytes;
    out->size = member->size;
    out->compressed_offset = offset;
    out->compressed_size = trailer - offset;
    out->crc32 = deb_le32(bytes + trailer);
    out->output_size = deb_le32(bytes + trailer + 4u);
    return VM_DEB_OK;
}

vm_deb_status_t vm_deb_gzip_size(const vm_deb_member_t *member,
                                 uint32_t *out_size) {
    if (out_size) *out_size = 0u;
    if (!out_size) return VM_DEB_ERR_INVALID_ARGUMENT;
    vm_deb_gzip_t gzip;
    vm_deb_status_t status = deb_gzip_parse(member, &gzip);
    if (status == VM_DEB_OK) *out_size = gzip.output_size;
    return status;
}

typedef struct {
    vmfw_mem_source_t memory;
} deb_byte_source_t;

static size_t deb_byte_source_read(void *ctx, uint8_t *buffer, size_t capacity) {
    deb_byte_source_t *source = (deb_byte_source_t *)ctx;
    if (capacity > 1u) capacity = 1u;
    return vmfw_mem_source_read(&source->memory, buffer, capacity);
}

typedef struct {
    vmfw_sink_fn sink;
    void *sink_ctx;
    uint32_t crc32;
    uint64_t produced;
    bool refused;
} deb_crc_sink_t;

static bool deb_crc_sink_write(void *ctx, const uint8_t *bytes, size_t size) {
    deb_crc_sink_t *sink = (deb_crc_sink_t *)ctx;
    sink->crc32 = vmfw_crc32(sink->crc32, bytes, size);
    sink->produced += (uint64_t)size;
    if (!sink->sink(sink->sink_ctx, bytes, size)) {
        sink->refused = true;
        return false;
    }
    return true;
}

vm_deb_status_t
vm_deb_gzip_inflate(const vm_deb_member_t *member,
                    vmfw_sink_fn sink, void *sink_ctx,
                    uint64_t max_output, uint64_t *out_produced,
                    vmfw_inflate_status_t *inflate_status) {
    if (out_produced) *out_produced = 0u;
    if (inflate_status) *inflate_status = VMFW_INFLATE_OK;
    if (!sink) return VM_DEB_ERR_INVALID_ARGUMENT;

    vm_deb_gzip_t gzip;
    vm_deb_status_t status = deb_gzip_parse(member, &gzip);
    if (status != VM_DEB_OK) return status;
    if ((uint64_t)gzip.output_size > max_output)
        return VM_DEB_ERR_GZIP_SIZE_LIMIT;

    deb_byte_source_t source;
    vmfw_mem_source_init(&source.memory,
                         gzip.bytes + gzip.compressed_offset,
                         gzip.compressed_size);
    deb_crc_sink_t output;
    memset(&output, 0, sizeof output);
    output.sink = sink;
    output.sink_ctx = sink_ctx;

    vmfw_inflate_status_t inflated =
        vmfw_inflate(deb_byte_source_read, &source,
                     deb_crc_sink_write, &output,
                     gzip.output_size, NULL);
    if (out_produced) *out_produced = output.produced;
    if (inflate_status) *inflate_status = inflated;
    if (output.refused) return VM_DEB_ERR_SINK;
    if (inflated != VMFW_INFLATE_OK) return VM_DEB_ERR_GZIP_INFLATE;
    if (source.memory.pos != gzip.compressed_size)
        return VM_DEB_ERR_GZIP_TRAILING_DATA;
    if (output.crc32 != gzip.crc32) return VM_DEB_ERR_GZIP_CRC;
    return VM_DEB_OK;
}

const char *vm_deb_status_text(vm_deb_status_t status) {
    switch (status) {
        case VM_DEB_OK:                     return "ok";
        case VM_DEB_ERR_INVALID_ARGUMENT:   return "invalid argument";
        case VM_DEB_ERR_NOT_ARCHIVE:        return "not a Debian ar archive";
        case VM_DEB_ERR_TRUNCATED:          return "truncated archive";
        case VM_DEB_ERR_HEADER:             return "malformed ar header";
        case VM_DEB_ERR_SIZE_FIELD:         return "invalid ar size field";
        case VM_DEB_ERR_PADDING:            return "invalid ar padding";
        case VM_DEB_ERR_MEMBER_ORDER:       return "unexpected Debian member";
        case VM_DEB_ERR_VERSION:            return "unsupported Debian format version";
        case VM_DEB_ERR_COMPRESSION:        return "unsupported member compression";
        case VM_DEB_ERR_TRAILING_DATA:      return "data after the third Debian member";
        case VM_DEB_ERR_GZIP_HEADER:        return "invalid gzip header";
        case VM_DEB_ERR_GZIP_TRUNCATED:     return "truncated gzip member";
        case VM_DEB_ERR_GZIP_SIZE_LIMIT:    return "gzip output exceeds the policy limit";
        case VM_DEB_ERR_GZIP_INFLATE:       return "invalid DEFLATE payload";
        case VM_DEB_ERR_GZIP_TRAILING_DATA: return "data after the DEFLATE stream";
        case VM_DEB_ERR_GZIP_CRC:           return "gzip CRC-32 mismatch";
        case VM_DEB_ERR_SINK:               return "output sink refused gzip bytes";
        default:                            return "unknown status";
    }
}
