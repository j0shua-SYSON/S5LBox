/*
 * S5LBox — the root filesystem, which is two containers deep.
 *
 * The `018-6482-014.dmg` member of a 7E18 IPSW is an AES-128 `encrcdsa` wrapper
 * around a UDIF disk image, and the UDIF is a table of compressed chunks
 * covering a whole disk: driver map, partition map, an ATAPI driver, free
 * space, and the one Apple_HFSX partition the emulator actually wants.
 *
 * Two things here are worth stating because they are the difference between
 * this running on a phone and not:
 *
 * 1. The wrapper is decrypted LAZILY, not in a pass of its own. Every
 *    4096-byte block derives its own IV from its index, so the plaintext is
 *    randomly addressable — and the chunk table needs random access anyway.
 *    Fusing them means the 208 MB intermediate that the desktop procedure in
 *    docs/BOOT_CHAIN.md writes is never written at all.
 *
 * 2. Chunks stream. A 433 MB partition costs the same memory as a 4 KB one,
 *    because the only thing held is the chunk table.
 *
 * What this does NOT do is verify anything. `encrcdsa` has no authentication
 * tag we check, and a wrong key produces plausible-looking garbage. The only
 * defence against that is the SHA-256 the import core compares afterwards,
 * which is why a wrong key surfaces as "this did not produce the root
 * filesystem" rather than as a decryption error.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareFormats.h"

#include "aes.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* Big-endian scalar reads (Apple's disk-image structures are all BE)        */
/* ------------------------------------------------------------------------ */

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

#define ENCRCDSA_HEADER 0x48u        /* the fields we read fit in this many  */
#define KOLY_SIZE      512u
#define MISH_HEADER   0xCCu
#define MISH_CHUNK      40u

/* Chunk kinds. Only four of these are implemented; the rest exist so an image
 * that uses one is refused by name instead of producing nothing. */
#define CHUNK_ZERO    0x00000000u
#define CHUNK_RAW     0x00000001u
#define CHUNK_IGNORE  0x00000002u
#define CHUNK_ADC     0x80000004u
#define CHUNK_ZLIB    0x80000005u
#define CHUNK_BZIP2   0x80000006u
#define CHUNK_LZFSE   0x80000007u
#define CHUNK_LZMA    0x80000008u
#define CHUNK_COMMENT 0x7FFFFFFEu
#define CHUNK_END     0xFFFFFFFFu

/* A 7E18 root filesystem has 1,659. The cap exists so a corrupt count cannot
 * demand an unbounded scratch buffer before anything else notices. */
#define MAX_CHUNKS 1048576u

const char *vmfw_dmg_strerror(vmfw_dmg_status_t st) {
    switch (st) {
        case VMFW_DMG_OK:                    return "ok";
        case VMFW_DMG_ERR_INVALID_ARGUMENT:  return "invalid disk image argument";
        case VMFW_DMG_ERR_READ:              return "the disk image could not be read";
        case VMFW_DMG_ERR_NOT_ENCRCDSA:      return "not an encrypted disk image";
        case VMFW_DMG_ERR_ENCRCDSA_VERSION:  return "unsupported encrypted disk image version";
        case VMFW_DMG_ERR_BAD_BLOCK_SIZE:    return "the image declares an impossible block size";
        case VMFW_DMG_ERR_BAD_GEOMETRY:      return "the image's declared extent does not fit the file";
        case VMFW_DMG_ERR_KEY_REQUIRED:      return "this disk image is encrypted and needs a key you supply";
        case VMFW_DMG_ERR_BAD_KEY_LENGTH:    return "a disk image key is 36 bytes (72 hex characters)";
        case VMFW_DMG_ERR_WRONG_KEY:         return "decryption did not produce a disk image; the key is probably not this one's";
        case VMFW_DMG_ERR_NO_KOLY:           return "no UDIF trailer at the end of the image";
        case VMFW_DMG_ERR_BAD_KOLY:          return "the UDIF trailer is malformed";
        case VMFW_DMG_ERR_XML:               return "the image's partition table will not parse";
        case VMFW_DMG_ERR_NO_PARTITION:      return "the image contains no such partition";
        case VMFW_DMG_ERR_BAD_BLKX:          return "a partition's chunk table is malformed";
        case VMFW_DMG_ERR_BAD_CHUNK:         return "a chunk's extent is impossible";
        case VMFW_DMG_ERR_CHUNK_TYPE:        return "the image uses a compression this reader does not implement";
        case VMFW_DMG_ERR_CHUNK_SIZE:        return "a chunk produced the wrong number of bytes";
        case VMFW_DMG_ERR_INFLATE:           return "a chunk's compressed data is corrupt";
        case VMFW_DMG_ERR_SINK:              return "the destination refused the expanded bytes";
        case VMFW_DMG_ERR_TOO_MANY_CHUNKS:   return "a partition declares more chunks than this reader accepts";
        case VMFW_DMG_ERR_SCRATCH_TOO_SMALL: return "the chunk table does not fit the buffer provided";
        case VMFW_DMG_ERR_NOT_CONTIGUOUS:    return "the chunks do not tile the partition without gaps";
        default:                             return "unknown disk image error";
    }
}

/* ------------------------------------------------------------------------ */
/* The encrcdsa wrapper                                                      */
/* ------------------------------------------------------------------------ */

vmfw_dmg_status_t vmfw_dmg_reader_open(vmfw_dmg_reader_t *r,
                                       vmfw_pread_fn pread, void *ctx,
                                       uint64_t file_size,
                                       const uint8_t *key, size_t key_len) {
    if (!r || !pread) return VMFW_DMG_ERR_INVALID_ARGUMENT;
    memset(r, 0, sizeof *r);
    r->pread = pread;
    r->ctx = ctx;
    r->file_size = file_size;

    uint8_t h[ENCRCDSA_HEADER];
    if (file_size < ENCRCDSA_HEADER) {
        /* Too small to be wrapped, so it can only be a plain image. Let the
         * trailer parse decide whether it is one. */
        r->encrypted = false;
        r->plain_size = file_size;
        return VMFW_DMG_OK;
    }
    if (pread(ctx, 0, h, sizeof h) != sizeof h) return VMFW_DMG_ERR_READ;

    if (memcmp(h, "encrcdsa", 8) != 0) {
        /* A plain UDIF. Nothing to decrypt and no key needed -- and a key
         * supplied for one is quietly ignored rather than treated as an
         * error, because "I pasted a key and it said the key was wrong" is a
         * worse message than "that image was not encrypted". */
        r->encrypted = false;
        r->plain_size = file_size;
        return VMFW_DMG_OK;
    }

    uint32_t version = be32(h + 8);
    if (version != 2u) return VMFW_DMG_ERR_ENCRCDSA_VERSION;

    r->block_size = be32(h + 0x34);
    r->plain_size = be64(h + 0x38);
    r->data_offset = be64(h + 0x40);
    r->encrypted = true;

    /*
     * The block size drives an AES-CBC pass and an IV derivation per block, so
     * a nonsensical one is refused before it can be used as a divisor or a
     * buffer size. It must also be a whole number of AES blocks, or CBC over it
     * is not defined.
     */
    if (r->block_size == 0 || r->block_size > 65536u ||
        (r->block_size % AES_BLOCK_SIZE) != 0)
        return VMFW_DMG_ERR_BAD_BLOCK_SIZE;

    if (r->data_offset > file_size) return VMFW_DMG_ERR_BAD_GEOMETRY;

    /* The ciphertext is padded up to a block boundary, so the plaintext must
     * fit inside what is actually there. A dataSize larger than the file is the
     * shape a truncated download takes. */
    uint64_t span = file_size - r->data_offset;
    if (r->plain_size > span) return VMFW_DMG_ERR_BAD_GEOMETRY;

    if (!key) return VMFW_DMG_ERR_KEY_REQUIRED;
    if (key_len != VMFW_DMG_KEY_BLOB_SIZE) return VMFW_DMG_ERR_BAD_KEY_LENGTH;

    memcpy(r->aes_key, key, 16);
    memcpy(r->hmac_key, key + 16, 20);
    return VMFW_DMG_OK;
}

vmfw_dmg_status_t vmfw_dmg_reader_pread(const vmfw_dmg_reader_t *r,
                                        uint64_t offset, uint8_t *buf,
                                        size_t len) {
    if (!r || !r->pread || (!buf && len)) return VMFW_DMG_ERR_INVALID_ARGUMENT;
    if (len == 0) return VMFW_DMG_OK;

    /* Bound against the LOGICAL size, not the file size: the ciphertext is
     * longer than the plaintext and reading into the padding would hand back
     * bytes that are not part of the image. */
    if (offset > r->plain_size || (uint64_t)len > r->plain_size - offset)
        return VMFW_DMG_ERR_BAD_GEOMETRY;

    if (!r->encrypted) {
        if (r->pread(r->ctx, offset, buf, len) != len) return VMFW_DMG_ERR_READ;
        return VMFW_DMG_OK;
    }

    aes_ctx_t aes;
    if (!aes_init(&aes, r->aes_key, 128u)) return VMFW_DMG_ERR_INVALID_ARGUMENT;

    /*
     * open() already bounded block_size at 64 KB, but the working buffer here
     * is a fixed 4096 -- what every encrcdsa image of this era declares -- so
     * that a phone thread does not carry a 64 KB frame through a 208 MB read.
     * A larger declared block is refused by name rather than truncated, which
     * would decrypt each block against the wrong ciphertext tail and produce
     * plausible garbage.
     */
    enum { DMG_BLOCK_BUFFER = 4096u };
    uint8_t block[DMG_BLOCK_BUFFER];
    if (r->block_size > DMG_BLOCK_BUFFER) return VMFW_DMG_ERR_BAD_BLOCK_SIZE;

    uint64_t pos = offset;
    size_t done = 0;
    while (done < len) {
        uint64_t index = pos / r->block_size;
        uint32_t within = (uint32_t)(pos - index * r->block_size);
        if (index > 0xffffffffu) return VMFW_DMG_ERR_BAD_GEOMETRY;

        uint64_t phys = r->data_offset + index * (uint64_t)r->block_size;
        if (phys > r->file_size ||
            (uint64_t)r->block_size > r->file_size - phys)
            return VMFW_DMG_ERR_BAD_GEOMETRY;

        if (r->pread(r->ctx, phys, block, r->block_size) != r->block_size)
            return VMFW_DMG_ERR_READ;

        /*
         * IV = HMAC-SHA1(hmac_key, big-endian block index), truncated to 16.
         * The index is the plaintext block number, so it must be derived from
         * the logical offset -- deriving it from the physical one would work
         * only while data_offset happened to be a multiple of block_size.
         */
        uint8_t be_index[4];
        be_index[0] = (uint8_t)((index >> 24) & 0xffu);
        be_index[1] = (uint8_t)((index >> 16) & 0xffu);
        be_index[2] = (uint8_t)((index >> 8) & 0xffu);
        be_index[3] = (uint8_t)(index & 0xffu);

        uint8_t mac[VMFW_SHA1_DIGEST_SIZE];
        if (!vmfw_hmac_sha1(r->hmac_key, sizeof r->hmac_key,
                            be_index, sizeof be_index, mac))
            return VMFW_DMG_ERR_INVALID_ARGUMENT;

        if (!aes_cbc_decrypt(&aes, mac, block, block, r->block_size))
            return VMFW_DMG_ERR_INVALID_ARGUMENT;

        size_t take = (size_t)(r->block_size - within);
        if (take > len - done) take = len - done;
        memcpy(buf + done, block + within, take);
        done += take;
        pos  += take;
    }
    return VMFW_DMG_OK;
}

/* ------------------------------------------------------------------------ */
/* The UDIF trailer and its resource fork                                    */
/* ------------------------------------------------------------------------ */

/*
 * Decode just the first `want` bytes of a base64 span.
 *
 * The chunk tables are up to 66 KB decoded and there are seven of them; probing
 * only needs each one's 204-byte header. Taking a prefix is safe because base64
 * groups are independent: the first ceil(want/3) groups decode to the first
 * 3*that bytes regardless of what follows.
 */
static vmfw_plist_status_t decode_b64_prefix(const uint8_t *b64, size_t b64_len,
                                             uint8_t *out, size_t out_cap,
                                             size_t want) {
    size_t groups = (want + 2u) / 3u;
    size_t need_chars = groups * 4u;

    /* Walk to the end of the Nth alphabet character, skipping the newlines and
     * tabs Apple's plist writer inserts every 64 characters. */
    size_t seen = 0, i = 0;
    for (; i < b64_len && seen < need_chars; i++) {
        uint8_t c = b64[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        seen++;
    }
    if (seen < need_chars) return VMFW_PLIST_ERR_BAD_BASE64;

    size_t got = 0;
    vmfw_plist_status_t st = vmfw_base64_decode(b64, i, out, out_cap, &got);
    if (st != VMFW_PLIST_OK) return st;
    return got >= want ? VMFW_PLIST_OK : VMFW_PLIST_ERR_BAD_BASE64;
}

vmfw_dmg_status_t vmfw_dmg_probe(const vmfw_dmg_reader_t *r,
                                 uint8_t *scratch, size_t scratch_cap,
                                 size_t *needed, vmfw_dmg_info_t *out) {
    if (!r || !r->pread || !out) return VMFW_DMG_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (needed) *needed = 0;

    if (r->plain_size < KOLY_SIZE) return VMFW_DMG_ERR_NO_KOLY;

    uint8_t koly[KOLY_SIZE];
    vmfw_dmg_status_t st =
        vmfw_dmg_reader_pread(r, r->plain_size - KOLY_SIZE, koly, KOLY_SIZE);
    if (st != VMFW_DMG_OK) return st;

    /*
     * This is where a wrong key first shows itself. `encrcdsa` carries no tag
     * we verify, so decryption with the wrong key succeeds and produces noise;
     * the trailer failing to appear is the earliest honest signal, and it is
     * reported as a key problem rather than as a corrupt image because that is
     * overwhelmingly what it means.
     */
    if (memcmp(koly, "koly", 4) != 0)
        return r->encrypted ? VMFW_DMG_ERR_WRONG_KEY : VMFW_DMG_ERR_NO_KOLY;

    out->data_fork_offset = be64(koly + 0x18);
    out->data_fork_length = be64(koly + 0x20);
    out->xml_offset       = be64(koly + 0xD8);
    out->xml_length       = be64(koly + 0xE0);
    out->sector_count     = be64(koly + 0x1EC);

    if (out->xml_length == 0) return VMFW_DMG_ERR_BAD_KOLY;
    if (out->xml_offset > r->plain_size ||
        out->xml_length > r->plain_size - out->xml_offset)
        return VMFW_DMG_ERR_BAD_KOLY;
    if (out->data_fork_offset > r->plain_size ||
        out->data_fork_length > r->plain_size - out->data_fork_offset)
        return VMFW_DMG_ERR_BAD_KOLY;

    if (out->xml_length > (uint64_t)SIZE_MAX) return VMFW_DMG_ERR_BAD_KOLY;
    size_t xml_len = (size_t)out->xml_length;

    if (needed) *needed = xml_len;
    if (!scratch || scratch_cap < xml_len) return VMFW_DMG_ERR_INVALID_ARGUMENT;

    st = vmfw_dmg_reader_pread(r, out->xml_offset, scratch, xml_len);
    if (st != VMFW_DMG_OK) return st;

    vmfw_plist_t pl;
    if (vmfw_plist_init(&pl, scratch, xml_len) != VMFW_PLIST_OK)
        return VMFW_DMG_ERR_XML;

    size_t count = 0;
    if (vmfw_plist_array_count(&pl, "resource-fork/blkx", &count)
            != VMFW_PLIST_OK)
        return VMFW_DMG_ERR_XML;
    if (count == 0) return VMFW_DMG_ERR_XML;
    /*
     * Clamped rather than refused, and this is the one place in this file where
     * that is the right answer. A 7E18 root filesystem has seven blkx entries
     * and the cap is 32; an image with more is not malformed, just unusual, and
     * refusing it outright would reject something readable. The consequence of
     * the clamp is bounded and safe: a partition past the 32nd is reported as
     * absent by vmfw_dmg_find_partition, which is a refusal with a clear
     * message rather than a wrong extraction.
     */
    if (count > VMFW_DMG_MAX_PARTITIONS) count = VMFW_DMG_MAX_PARTITIONS;

    for (size_t i = 0; i < count; i++) {
        /* "resource-fork/blkx/<i>/Name" -- built by hand because there is no
         * allocation here and snprintf's return value would need checking
         * anyway. Two digits is enough for VMFW_DMG_MAX_PARTITIONS. */
        char path[64];
        static const char kPrefix[] = "resource-fork/blkx/";
        size_t p = 0;
        memcpy(path, kPrefix, sizeof kPrefix - 1); p = sizeof kPrefix - 1;
        if (i >= 10) path[p++] = (char)('0' + (i / 10));
        path[p++] = (char)('0' + (i % 10));
        path[p++] = '/';
        size_t stem = p;

        vmfw_dmg_partition_t *part = &out->partitions[i];

        memcpy(path + stem, "Name", 5);
        if (vmfw_plist_get_string(&pl, path, part->name, sizeof part->name)
                != VMFW_PLIST_OK)
            return VMFW_DMG_ERR_XML;

        memcpy(path + stem, "Data", 5);
        const uint8_t *b64 = NULL;
        size_t b64_len = 0;
        if (vmfw_plist_get_data_span(&pl, path, &b64, &b64_len)
                != VMFW_PLIST_OK)
            return VMFW_DMG_ERR_XML;

        uint8_t mish[MISH_HEADER];
        if (decode_b64_prefix(b64, b64_len, mish, sizeof mish, MISH_HEADER)
                != VMFW_PLIST_OK)
            return VMFW_DMG_ERR_BAD_BLKX;

        if (memcmp(mish, "mish", 4) != 0) return VMFW_DMG_ERR_BAD_BLKX;

        part->first_sector     = be64(mish + 0x08);
        part->sector_count     = be64(mish + 0x10);
        part->blkx_data_offset = be64(mish + 0x18);
        part->chunk_count      = be32(mish + 0xC8);
        part->b64_offset       = (size_t)(b64 - scratch);
        part->b64_length       = b64_len;

        if (part->chunk_count > MAX_CHUNKS) return VMFW_DMG_ERR_TOO_MANY_CHUNKS;
    }

    out->partition_count = (uint32_t)count;
    return VMFW_DMG_OK;
}

vmfw_dmg_status_t vmfw_dmg_find_partition(const vmfw_dmg_info_t *info,
                                          const char *needle,
                                          uint32_t *out_index) {
    if (!info || !needle || !out_index) return VMFW_DMG_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < info->partition_count; i++) {
        if (strstr(info->partitions[i].name, needle) != NULL) {
            *out_index = i;
            return VMFW_DMG_OK;
        }
    }
    return VMFW_DMG_ERR_NO_PARTITION;
}

/* ------------------------------------------------------------------------ */
/* Expanding one partition                                                   */
/* ------------------------------------------------------------------------ */

/* Feeds a chunk's compressed bytes to the inflater straight out of the image,
 * decrypting on the way, so no chunk is ever held whole. */
typedef struct {
    const vmfw_dmg_reader_t *r;
    uint64_t off;
    uint64_t remaining;
    bool failed;
} chunk_source_t;

static size_t chunk_read(void *ctx, uint8_t *buf, size_t cap) {
    chunk_source_t *c = (chunk_source_t *)ctx;
    if (c->remaining == 0) return 0;
    size_t want = cap;
    if ((uint64_t)want > c->remaining) want = (size_t)c->remaining;
    if (vmfw_dmg_reader_pread(c->r, c->off, buf, want) != VMFW_DMG_OK) {
        c->failed = true;
        return VMFW_SOURCE_ERROR;
    }
    c->off += want;
    c->remaining -= want;
    return want;
}

/* Counts what actually reached the caller, so a chunk that decompresses to the
 * wrong length is caught at the chunk rather than at the end of a 433 MB file. */
typedef struct {
    vmfw_sink_fn inner;
    void *inner_ctx;
    uint64_t total;
    bool refused;
} counting_sink_t;

static bool counting_write(void *ctx, const uint8_t *data, size_t len) {
    counting_sink_t *s = (counting_sink_t *)ctx;
    if (s->inner && !s->inner(s->inner_ctx, data, len)) {
        s->refused = true;
        return false;
    }
    s->total += len;
    return true;
}

static bool emit_zeros(counting_sink_t *s, uint64_t count) {
    static const uint8_t zeros[4096] = { 0 };
    while (count) {
        size_t n = count > sizeof zeros ? sizeof zeros : (size_t)count;
        if (!counting_write(s, zeros, n)) return false;
        count -= n;
    }
    return true;
}

vmfw_dmg_status_t vmfw_dmg_extract_partition(const vmfw_dmg_reader_t *r,
                                             const vmfw_dmg_info_t *info,
                                             const uint8_t *xml,
                                             size_t xml_len,
                                             uint32_t partition_index,
                                             uint8_t *chunk_scratch,
                                             size_t chunk_scratch_cap,
                                             vmfw_sink_fn sink, void *sink_ctx,
                                             uint64_t *out_bytes) {
    if (!r || !info || !xml || !chunk_scratch || !sink)
        return VMFW_DMG_ERR_INVALID_ARGUMENT;
    if (partition_index >= info->partition_count)
        return VMFW_DMG_ERR_NO_PARTITION;
    if (out_bytes) *out_bytes = 0;

    const vmfw_dmg_partition_t *part = &info->partitions[partition_index];

    if (part->b64_offset > xml_len ||
        part->b64_length > xml_len - part->b64_offset)
        return VMFW_DMG_ERR_BAD_BLKX;

    uint64_t table_bytes = (uint64_t)MISH_HEADER
                         + (uint64_t)part->chunk_count * MISH_CHUNK;
    if (table_bytes > (uint64_t)chunk_scratch_cap)
        return VMFW_DMG_ERR_SCRATCH_TOO_SMALL;

    if (decode_b64_prefix(xml + part->b64_offset, part->b64_length,
                          chunk_scratch, chunk_scratch_cap,
                          (size_t)table_bytes) != VMFW_PLIST_OK)
        return VMFW_DMG_ERR_BAD_BLKX;

    if (memcmp(chunk_scratch, "mish", 4) != 0) return VMFW_DMG_ERR_BAD_BLKX;

    counting_sink_t out = { sink, sink_ctx, 0u, false };

    /* Where this partition's compressed data sits in the file. All three terms
     * are zero or small in a real IPSW, but the format defines the sum and a
     * reader that dropped one would work on exactly the images it was tested
     * against. */
    uint64_t data_base = info->data_fork_offset + part->blkx_data_offset;

    uint64_t expect_sector = 0;   /* next sector this partition must produce */

    for (uint32_t i = 0; i < part->chunk_count; i++) {
        const uint8_t *c = chunk_scratch + MISH_HEADER + (size_t)i * MISH_CHUNK;
        uint32_t type   = be32(c);
        uint64_t sector = be64(c + 0x08);
        uint64_t count  = be64(c + 0x10);
        uint64_t coff   = be64(c + 0x18);
        uint64_t clen   = be64(c + 0x20);

        if (type == CHUNK_COMMENT || type == CHUNK_END) {
            /* Both carry no sectors. One that claimed some would silently
             * shift everything after it. */
            if (count != 0) return VMFW_DMG_ERR_BAD_CHUNK;
            continue;
        }

        /* Streaming output is only correct while the chunks tile the partition
         * in order. Checked, not assumed: see the header note. */
        if (sector != expect_sector) return VMFW_DMG_ERR_NOT_CONTIGUOUS;
        if (count > part->sector_count - sector)
            return VMFW_DMG_ERR_BAD_CHUNK;
        expect_sector = sector + count;

        uint64_t want = count * VMFW_DMG_SECTOR_SIZE;
        if (count != 0 && want / count != VMFW_DMG_SECTOR_SIZE)
            return VMFW_DMG_ERR_BAD_CHUNK;

        if (type == CHUNK_ZERO || type == CHUNK_IGNORE) {
            if (!emit_zeros(&out, want)) return VMFW_DMG_ERR_SINK;
            continue;
        }

        /* Every remaining kind reads from the data fork, so bound it once. */
        uint64_t abs = data_base + coff;
        if (coff > r->plain_size || abs > r->plain_size ||
            clen > r->plain_size - abs)
            return VMFW_DMG_ERR_BAD_CHUNK;

        if (type == CHUNK_RAW) {
            if (clen != want) return VMFW_DMG_ERR_CHUNK_SIZE;
            uint8_t buf[8192];
            uint64_t left = clen, at = abs;
            while (left) {
                size_t n = left > sizeof buf ? sizeof buf : (size_t)left;
                vmfw_dmg_status_t st = vmfw_dmg_reader_pread(r, at, buf, n);
                if (st != VMFW_DMG_OK) return st;
                if (!counting_write(&out, buf, n)) return VMFW_DMG_ERR_SINK;
                at += n;
                left -= n;
            }
            continue;
        }

        if (type != CHUNK_ZLIB) {
            /* ADC, bzip2, LZFSE and LZMA are all real UDIF compressions; none
             * appears in a 7E18 root filesystem, and implementing a decoder
             * that no available image would exercise is how untested code ends
             * up on the untrusted-input path. Named refusal instead. */
            return VMFW_DMG_ERR_CHUNK_TYPE;
        }

        chunk_source_t src = { r, abs, clen, false };
        uint64_t before = out.total;
        uint64_t produced = 0;
        vmfw_inflate_status_t ist =
            vmfw_zlib_inflate(chunk_read, &src, counting_write, &out,
                              want, &produced);
        if (ist != VMFW_INFLATE_OK) {
            if (out.refused) return VMFW_DMG_ERR_SINK;
            if (src.failed)  return VMFW_DMG_ERR_READ;
            return VMFW_DMG_ERR_INFLATE;
        }
        /* Redundant, and mutation testing confirms deleting it fails nothing:
         * the inflater was handed `want` as a hard bound and refuses a stream
         * that produces any other number. Kept as the backstop that does not
         * depend on that being true. */
        if (out.total - before != want) return VMFW_DMG_ERR_CHUNK_SIZE;
    }

    /*
     * The chunk table must account for the whole partition. A table that stops
     * early would otherwise produce a short image that mounts and is missing
     * its tail, which is exactly the failure the SHA-256 gate downstream would
     * report as "wrong file" without saying why.
     */
    if (expect_sector != part->sector_count)
        return VMFW_DMG_ERR_NOT_CONTIGUOUS;

    if (out.total != part->sector_count * VMFW_DMG_SECTOR_SIZE)
        return VMFW_DMG_ERR_CHUNK_SIZE;

    if (out_bytes) *out_bytes = out.total;
    return VMFW_DMG_OK;
}
