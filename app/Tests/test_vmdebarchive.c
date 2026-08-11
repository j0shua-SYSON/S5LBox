/*
 * S5LBox -- Debian ar and gzip validation tests.
 *
 * The ordinary suite builds its own tiny archives, so it is fast and carries
 * no third-party package. S5LBOX_DEB_FIXTURE optionally points at one local
 * package for a structural/inflate check; absence is an announced skip.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMDebArchive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition, ...) do {                                         \
    checks++;                                                              \
    if (!(condition)) {                                                    \
        failures++;                                                        \
        printf("  FAIL %s:%d: ", __func__, __LINE__);                    \
        printf(__VA_ARGS__);                                               \
        printf("\n");                                                     \
    }                                                                      \
} while (0)

#define FIXTURE_CAPACITY 8192u

typedef struct {
    uint8_t bytes[FIXTURE_CAPACITY];
    size_t size;
    size_t version_header;
    size_t version_content;
    size_t control_header;
    size_t control_content;
    size_t control_pad;
    size_t data_header;
    size_t data_content;
    size_t data_pad;
    size_t gzip_size;
} deb_fixture_t;

static void little32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8u);
    out[2] = (uint8_t)(value >> 16u);
    out[3] = (uint8_t)(value >> 24u);
}

static bool append_bytes(uint8_t *out, size_t capacity, size_t *offset,
                         const void *bytes, size_t size) {
    if (!out || !offset || (!bytes && size != 0u) || *offset > capacity ||
        size > capacity - *offset)
        return false;
    if (size) memcpy(out + *offset, bytes, size);
    *offset += size;
    return true;
}

/* One stored final DEFLATE block inside the simplest valid gzip wrapper. */
static size_t make_gzip(uint8_t *out, size_t capacity,
                        const uint8_t *payload, size_t payload_size) {
    if (!out || !payload || payload_size > 65535u ||
        capacity < payload_size + 23u)
        return 0u;
    size_t offset = 0u;
    static const uint8_t HEADER[10] = {
        0x1fu, 0x8bu, 8u, 0u, 0u, 0u, 0u, 0u, 2u, 3u
    };
    if (!append_bytes(out, capacity, &offset, HEADER, sizeof HEADER)) return 0u;
    out[offset++] = 0x01u;                    /* final stored block */
    uint16_t length = (uint16_t)payload_size;
    uint16_t inverse = (uint16_t)~length;
    out[offset++] = (uint8_t)length;
    out[offset++] = (uint8_t)(length >> 8u);
    out[offset++] = (uint8_t)inverse;
    out[offset++] = (uint8_t)(inverse >> 8u);
    if (!append_bytes(out, capacity, &offset, payload, payload_size)) return 0u;
    uint32_t crc = vmfw_crc32(0u, payload, payload_size);
    little32(out + offset, crc);
    little32(out + offset + 4u, (uint32_t)payload_size);
    return offset + 8u;
}

static bool ar_field(uint8_t *header, size_t offset, size_t capacity,
                     const char *value) {
    size_t length = strlen(value);
    if (length > capacity) return false;
    memcpy(header + offset, value, length);
    return true;
}

static bool ar_member(deb_fixture_t *fixture, const char *name,
                      const uint8_t *content, size_t content_size,
                      size_t *header_offset, size_t *content_offset,
                      size_t *pad_offset) {
    if (!fixture || !name || !content || strlen(name) > 16u) return false;
    if (fixture->size > FIXTURE_CAPACITY - 60u) return false;
    uint8_t header[60];
    memset(header, ' ', sizeof header);
    char size_text[16];
    int written = snprintf(size_text, sizeof size_text, "%zu", content_size);
    if (written < 0 || (size_t)written > 10u) return false;
    if (!ar_field(header, 0u, 16u, name) ||
        !ar_field(header, 16u, 12u, "0") ||
        !ar_field(header, 28u, 6u, "0") ||
        !ar_field(header, 34u, 6u, "0") ||
        !ar_field(header, 40u, 8u, "100644") ||
        !ar_field(header, 48u, 10u, size_text))
        return false;
    header[58] = '`';
    header[59] = '\n';
    if (header_offset) *header_offset = fixture->size;
    if (!append_bytes(fixture->bytes, sizeof fixture->bytes, &fixture->size,
                      header, sizeof header))
        return false;
    if (content_offset) *content_offset = fixture->size;
    if (!append_bytes(fixture->bytes, sizeof fixture->bytes, &fixture->size,
                      content, content_size))
        return false;
    if (pad_offset) *pad_offset = SIZE_MAX;
    if ((content_size & 1u) != 0u) {
        static const uint8_t PAD = '\n';
        if (pad_offset) *pad_offset = fixture->size;
        if (!append_bytes(fixture->bytes, sizeof fixture->bytes,
                          &fixture->size, &PAD, 1u))
            return false;
    }
    return true;
}

static bool make_deb(deb_fixture_t *fixture) {
    static const uint8_t MAGIC[8] = {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
    static const uint8_t VERSION[4] = {'2', '.', '0', '\n'};
    static const uint8_t TAR_BYTES[8] = {
        't', 'a', 'r', '-', 'd', 'a', 't', 'a'
    };
    if (!fixture) return false;
    memset(fixture, 0, sizeof *fixture);
    uint8_t gzip[128];
    fixture->gzip_size = make_gzip(gzip, sizeof gzip,
                                   TAR_BYTES, sizeof TAR_BYTES);
    return fixture->gzip_size != 0u &&
           append_bytes(fixture->bytes, sizeof fixture->bytes, &fixture->size,
                        MAGIC, sizeof MAGIC) &&
           ar_member(fixture, "debian-binary", VERSION, sizeof VERSION,
                     &fixture->version_header, &fixture->version_content,
                     NULL) &&
           ar_member(fixture, "control.tar.gz", gzip, fixture->gzip_size,
                     &fixture->control_header, &fixture->control_content,
                     &fixture->control_pad) &&
           ar_member(fixture, "data.tar.gz", gzip, fixture->gzip_size,
                     &fixture->data_header, &fixture->data_content,
                     &fixture->data_pad);
}

static void replace_name(uint8_t *header, const char *name) {
    memset(header, ' ', 16u);
    memcpy(header, name, strlen(name));
}

static void expect_open(deb_fixture_t *fixture, size_t size,
                        vm_deb_status_t wanted, const char *what) {
    vm_deb_archive_t archive;
    vm_deb_status_t got =
        vm_deb_archive_open(&archive, fixture->bytes, size);
    CHECK(got == wanted, "%s: got %s, wanted %s", what,
          vm_deb_status_text(got), vm_deb_status_text(wanted));
}

static void test_archive_shape(void) {
    deb_fixture_t fixture;
    CHECK(make_deb(&fixture), "could not build valid archive");
    vm_deb_archive_t archive;
    vm_deb_status_t status =
        vm_deb_archive_open(&archive, fixture.bytes, fixture.size);
    CHECK(status == VM_DEB_OK, "valid archive refused: %s",
          vm_deb_status_text(status));
    CHECK(archive.control.bytes == fixture.bytes + fixture.control_content &&
          archive.control.size == fixture.gzip_size &&
          archive.control.compression == VM_DEB_COMPRESSION_GZIP,
          "control member span is wrong");
    CHECK(archive.data.bytes == fixture.bytes + fixture.data_content &&
          archive.data.size == fixture.gzip_size &&
          archive.data.compression == VM_DEB_COMPRESSION_GZIP,
          "data member span is wrong");

    CHECK(make_deb(&fixture), "could not rebuild bad-magic archive");
    fixture.bytes[0] = '?';
    expect_open(&fixture, fixture.size, VM_DEB_ERR_NOT_ARCHIVE, "bad magic");

    CHECK(make_deb(&fixture), "could not rebuild truncation archive");
    expect_open(&fixture, fixture.size - 1u, VM_DEB_ERR_TRUNCATED,
                "missing final pad");

    CHECK(make_deb(&fixture), "could not rebuild header archive");
    fixture.bytes[fixture.data_header + 58u] = '!';
    expect_open(&fixture, fixture.size, VM_DEB_ERR_HEADER,
                "bad ar terminator");

    CHECK(make_deb(&fixture), "could not rebuild size archive");
    fixture.bytes[fixture.data_header + 48u] = '-';
    expect_open(&fixture, fixture.size, VM_DEB_ERR_SIZE_FIELD,
                "signed size");

    CHECK(make_deb(&fixture), "could not rebuild padding archive");
    CHECK(fixture.control_pad != SIZE_MAX, "control member was unexpectedly even");
    fixture.bytes[fixture.control_pad] = 0u;
    expect_open(&fixture, fixture.size, VM_DEB_ERR_PADDING, "bad odd padding");

    CHECK(make_deb(&fixture), "could not rebuild version-name archive");
    replace_name(fixture.bytes + fixture.version_header, "version");
    expect_open(&fixture, fixture.size, VM_DEB_ERR_MEMBER_ORDER,
                "wrong first member");

    CHECK(make_deb(&fixture), "could not rebuild version archive");
    fixture.bytes[fixture.version_content] = '3';
    expect_open(&fixture, fixture.size, VM_DEB_ERR_VERSION,
                "wrong Debian version");

    CHECK(make_deb(&fixture), "could not rebuild order archive");
    replace_name(fixture.bytes + fixture.control_header, "data.tar.gz");
    expect_open(&fixture, fixture.size, VM_DEB_ERR_MEMBER_ORDER,
                "data before control");

    CHECK(make_deb(&fixture), "could not rebuild compression archive");
    replace_name(fixture.bytes + fixture.data_header, "data.tar.zst");
    expect_open(&fixture, fixture.size, VM_DEB_ERR_COMPRESSION,
                "unknown compression");

    CHECK(make_deb(&fixture), "could not rebuild trailing archive");
    fixture.bytes[fixture.size++] = 0u;
    expect_open(&fixture, fixture.size, VM_DEB_ERR_TRAILING_DATA,
                "fourth/trailing member bytes");

    static const struct {
        const char *name;
        vm_deb_compression_t compression;
    } KNOWN[] = {
        {"data.tar.bz2", VM_DEB_COMPRESSION_BZIP2},
        {"data.tar.lzma", VM_DEB_COMPRESSION_LZMA},
        {"data.tar.xz", VM_DEB_COMPRESSION_XZ},
    };
    for (size_t i = 0u; i < sizeof KNOWN / sizeof KNOWN[0]; i++) {
        CHECK(make_deb(&fixture), "could not rebuild known-compression archive");
        replace_name(fixture.bytes + fixture.data_header, KNOWN[i].name);
        status = vm_deb_archive_open(&archive, fixture.bytes, fixture.size);
        CHECK(status == VM_DEB_OK &&
              archive.data.compression == KNOWN[i].compression,
              "%s was not identified: %s", KNOWN[i].name,
              vm_deb_status_text(status));
    }
}

typedef struct {
    size_t calls;
} refusing_sink_t;

static bool refuse_output(void *ctx, const uint8_t *bytes, size_t size) {
    refusing_sink_t *sink = (refusing_sink_t *)ctx;
    (void)bytes;
    (void)size;
    sink->calls++;
    return false;
}

static void test_gzip(void) {
    static const uint8_t TAR_BYTES[8] = {
        't', 'a', 'r', '-', 'd', 'a', 't', 'a'
    };
    deb_fixture_t fixture;
    vm_deb_archive_t archive;
    CHECK(make_deb(&fixture), "could not build gzip archive");
    CHECK(vm_deb_archive_open(&archive, fixture.bytes, fixture.size) ==
              VM_DEB_OK,
          "could not reopen gzip archive");

    uint32_t output_size = 0u;
    CHECK(vm_deb_gzip_size(&archive.data, &output_size) == VM_DEB_OK &&
          output_size == sizeof TAR_BYTES,
          "gzip ISIZE is %u, expected %zu", output_size, sizeof TAR_BYTES);
    uint8_t output[32];
    vmfw_mem_sink_t sink;
    vmfw_mem_sink_init(&sink, output, sizeof output);
    uint64_t produced = 0u;
    vmfw_inflate_status_t inflated = VMFW_INFLATE_ERR_INVALID_ARGUMENT;
    vm_deb_status_t status =
        vm_deb_gzip_inflate(&archive.data, vmfw_mem_sink_write, &sink,
                            sizeof output, &produced, &inflated);
    CHECK(status == VM_DEB_OK && inflated == VMFW_INFLATE_OK,
          "valid gzip refused: %s / %s", vm_deb_status_text(status),
          vmfw_inflate_strerror(inflated));
    CHECK(produced == sizeof TAR_BYTES && sink.len == sizeof TAR_BYTES &&
          memcmp(output, TAR_BYTES, sizeof TAR_BYTES) == 0,
          "gzip output changed");

    /* Exercise all optional RFC 1952 header fields together. Era packages do
     * not normally use them, but accepting the flags without validating their
     * lengths and FHCRC would make a corrupted download ambiguous. */
    uint8_t optional[256];
    memcpy(optional, archive.data.bytes, 10u);
    optional[3] = 0x1eu;                 /* FHCRC, FEXTRA, FNAME, FCOMMENT */
    size_t optional_size = 10u;
    optional[optional_size++] = 2u;      /* XLEN */
    optional[optional_size++] = 0u;
    optional[optional_size++] = 0xa5u;
    optional[optional_size++] = 0x5au;
    static const uint8_t NAME[] = {'p', 'k', 'g', 0u};
    static const uint8_t COMMENT[] = {'o', 'k', 0u};
    CHECK(append_bytes(optional, sizeof optional, &optional_size,
                       NAME, sizeof NAME),
          "could not append gzip filename");
    CHECK(append_bytes(optional, sizeof optional, &optional_size,
                       COMMENT, sizeof COMMENT),
          "could not append gzip comment");
    uint16_t header_crc = (uint16_t)(
        vmfw_crc32(0u, optional, optional_size) & 0xffffu);
    optional[optional_size++] = (uint8_t)header_crc;
    optional[optional_size++] = (uint8_t)(header_crc >> 8u);
    CHECK(append_bytes(optional, sizeof optional, &optional_size,
                       archive.data.bytes + 10u, archive.data.size - 10u),
          "could not append gzip body");
    vm_deb_member_t optional_member = archive.data;
    optional_member.bytes = optional;
    optional_member.size = optional_size;
    vmfw_mem_sink_init(&sink, output, sizeof output);
    status = vm_deb_gzip_inflate(&optional_member, vmfw_mem_sink_write, &sink,
                                 sizeof output, &produced, &inflated);
    CHECK(status == VM_DEB_OK && produced == sizeof TAR_BYTES &&
          memcmp(output, TAR_BYTES, sizeof TAR_BYTES) == 0,
          "valid optional gzip header refused: %s / %s",
          vm_deb_status_text(status), vmfw_inflate_strerror(inflated));
    optional[optional_size - (archive.data.size - 10u) - 2u] ^= 0x01u;
    CHECK(vm_deb_gzip_size(&optional_member, &output_size) ==
              VM_DEB_ERR_GZIP_HEADER,
          "bad gzip FHCRC was accepted");

    vmfw_mem_sink_init(&sink, output, sizeof output);
    status = vm_deb_gzip_inflate(&archive.data, vmfw_mem_sink_write, &sink,
                                 sizeof TAR_BYTES - 1u, &produced, &inflated);
    CHECK(status == VM_DEB_ERR_GZIP_SIZE_LIMIT && sink.len == 0u,
          "gzip limit was checked after writing: %s",
          vm_deb_status_text(status));

    vm_deb_member_t mutated = archive.data;
    uint8_t gzip[256];
    memcpy(gzip, archive.data.bytes, archive.data.size);
    mutated.bytes = gzip;

    gzip[3] = 0xe0u;
    CHECK(vm_deb_gzip_size(&mutated, &output_size) ==
              VM_DEB_ERR_GZIP_HEADER,
          "reserved gzip flags were accepted");

    memcpy(gzip, archive.data.bytes, archive.data.size);
    mutated.size = 17u;
    CHECK(vm_deb_gzip_size(&mutated, &output_size) ==
              VM_DEB_ERR_GZIP_TRUNCATED,
          "truncated gzip wrapper was accepted");

    memcpy(gzip, archive.data.bytes, archive.data.size);
    mutated.size = archive.data.size;
    gzip[10] = 0x07u;                    /* final, reserved DEFLATE type 3 */
    vmfw_null_sink_t null_sink = {0};
    status = vm_deb_gzip_inflate(&mutated, vmfw_null_sink_write, &null_sink,
                                 1024u, &produced, &inflated);
    CHECK(status == VM_DEB_ERR_GZIP_INFLATE &&
          inflated == VMFW_INFLATE_ERR_BAD_BLOCK,
          "bad DEFLATE did not retain its reason: %s / %s",
          vm_deb_status_text(status), vmfw_inflate_strerror(inflated));

    memcpy(gzip, archive.data.bytes, archive.data.size);
    gzip[archive.data.size - 8u] ^= 0x01u;
    null_sink.len = 0u;
    status = vm_deb_gzip_inflate(&mutated, vmfw_null_sink_write, &null_sink,
                                 1024u, &produced, &inflated);
    CHECK(status == VM_DEB_ERR_GZIP_CRC,
          "bad gzip CRC was accepted: %s", vm_deb_status_text(status));

    memcpy(gzip, archive.data.bytes, archive.data.size);
    size_t trailer = archive.data.size - 8u;
    memmove(gzip + trailer + 1u, gzip + trailer, 8u);
    gzip[trailer] = 0u;
    mutated.size = archive.data.size + 1u;
    null_sink.len = 0u;
    status = vm_deb_gzip_inflate(&mutated, vmfw_null_sink_write, &null_sink,
                                 1024u, &produced, &inflated);
    CHECK(status == VM_DEB_ERR_GZIP_TRAILING_DATA,
          "junk after DEFLATE was accepted: %s", vm_deb_status_text(status));

    mutated = archive.data;
    refusing_sink_t refusing = {0};
    status = vm_deb_gzip_inflate(&mutated, refuse_output, &refusing,
                                 1024u, &produced, &inflated);
    CHECK(status == VM_DEB_ERR_SINK && refusing.calls == 1u,
          "sink refusal was hidden: %s", vm_deb_status_text(status));

    mutated.compression = VM_DEB_COMPRESSION_LZMA;
    CHECK(vm_deb_gzip_size(&mutated, &output_size) ==
              VM_DEB_ERR_INVALID_ARGUMENT,
          "LZMA member was passed to the gzip decoder");
}

static uint8_t *read_optional(const char *path, size_t *out_size) {
    if (out_size) *out_size = 0u;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long end = ftell(file);
    if (end <= 0 || end > 32L * 1024L * 1024L ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *bytes = (uint8_t *)malloc((size_t)end);
    if (!bytes || fread(bytes, 1u, (size_t)end, file) != (size_t)end) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    if (fclose(file) != 0) { free(bytes); return NULL; }
    *out_size = (size_t)end;
    return bytes;
}

static void test_optional_real_package(void) {
    const char *path = getenv("S5LBOX_DEB_FIXTURE");
    if (!path || !*path) {
        printf("SKIP: S5LBOX_DEB_FIXTURE does not name a local package\n");
        return;
    }
    size_t size = 0u;
    uint8_t *bytes = read_optional(path, &size);
    CHECK(bytes != NULL, "could not read optional package %s", path);
    if (!bytes) return;
    vm_deb_archive_t archive;
    vm_deb_status_t status = vm_deb_archive_open(&archive, bytes, size);
    CHECK(status == VM_DEB_OK, "optional package refused: %s",
          vm_deb_status_text(status));
    if (status == VM_DEB_OK &&
        archive.data.compression == VM_DEB_COMPRESSION_GZIP) {
        vmfw_null_sink_t sink = {0};
        uint64_t produced = 0u;
        vmfw_inflate_status_t inflated = VMFW_INFLATE_OK;
        status = vm_deb_gzip_inflate(&archive.data, vmfw_null_sink_write, &sink,
                                     32u * 1024u * 1024u,
                                     &produced, &inflated);
        CHECK(status == VM_DEB_OK && produced == sink.len,
              "optional data.tar.gz refused: %s / %s",
              vm_deb_status_text(status), vmfw_inflate_strerror(inflated));
        printf("fixture: %s, %zu-byte deb, %llu-byte data tar\n",
               path, size, (unsigned long long)produced);
    } else if (status == VM_DEB_OK) {
        printf("fixture: %s, %zu-byte deb, data compression %d (parsed only)\n",
               path, size, (int)archive.data.compression);
    }
    free(bytes);
}

int main(void) {
    printf("== Debian archive ==\n");
    test_archive_shape();
    test_gzip();
    test_optional_real_package();
    printf("== Debian archive: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
