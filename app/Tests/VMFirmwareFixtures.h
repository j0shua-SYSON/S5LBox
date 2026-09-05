/*
 * S5LBox — builders for the firmware-import suite's fixtures.
 *
 * Every container the importer reads is BUILT here rather than embedded as a
 * captured blob, for two reasons that matter more than convenience:
 *
 *   - a fixture that is a hex dump can share a mistake with the code that reads
 *     it, and this project has already lost a day to exactly that (a
 *     byte-swapped IMG3 magic survived a green suite because the fixtures were
 *     wrong in the same direction). A builder that lays out fields by name and
 *     offset, from the format description, cannot agree with a reader that has
 *     the layout wrong.
 *   - a mutation test needs to change one field of a valid container. That is a
 *     line of code against a builder and a hex-editing exercise against a blob.
 *
 * Compression is sidestepped rather than implemented: DEFLATE's stored block
 * and LZSS's all-literals encoding are both valid outputs of a trivial
 * "compressor", so a real decompressor can be driven with real streams without
 * a real compressor in the test. Streams that need actual Huffman coding are
 * the inflate suite's job and use fixtures from a known-good implementation.
 *
 * Nothing here contains Apple data.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_VM_FIRMWARE_FIXTURES_H
#define S5LBOX_VM_FIRMWARE_FIXTURES_H

#include "VMFirmwareFormats.h"

#include "aes.h"
#include "img3.h"
#include "lzss.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* Scalar writers                                                            */
/* ------------------------------------------------------------------------ */

static inline void fx_w16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static inline void fx_w32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static inline void fx_w64le(uint8_t *p, uint64_t v) {
    fx_w32le(p, (uint32_t)(v & 0xffffffffu));
    fx_w32le(p + 4, (uint32_t)(v >> 32));
}

static inline void fx_w32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static inline void fx_w64be(uint8_t *p, uint64_t v) {
    fx_w32be(p, (uint32_t)(v >> 32));
    fx_w32be(p + 4, (uint32_t)(v & 0xffffffffu));
}

/* A cheap deterministic filler, so a fixture's contents are reproducible and
 * are not all-zero (which would hide an off-by-one that copies the wrong
 * region). */
static inline void fx_fill(uint8_t *p, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        p[i] = (uint8_t)((s >> 16) & 0xffu);
    }
}

/* ------------------------------------------------------------------------ */
/* DEFLATE and zlib, using only stored blocks                                */
/* ------------------------------------------------------------------------ */
/*
 * RFC 1951 section 3.2.4: a stored block is BFINAL/BTYPE in the first three
 * bits, padding to a byte boundary, then LEN and ~LEN as 16-bit little-endian
 * values and LEN literal bytes. It is a legal DEFLATE stream, so a decoder that
 * handles it is really decoding, and no compressor is needed to make one.
 */
static inline size_t fx_deflate_stored(uint8_t *dst, size_t cap,
                                       const uint8_t *src, size_t len) {
    size_t out = 0, done = 0;
    do {
        size_t chunk = len - done;
        if (chunk > 0xffffu) chunk = 0xffffu;
        bool final = (done + chunk == len);
        if (out + 5 + chunk > cap) return 0;
        dst[out++] = final ? 0x01u : 0x00u;      /* BFINAL, BTYPE=00 */
        fx_w16le(dst + out, (uint16_t)chunk);            out += 2;
        fx_w16le(dst + out, (uint16_t)(~chunk & 0xffffu)); out += 2;
        if (chunk) memcpy(dst + out, src + done, chunk);
        out += chunk;
        done += chunk;
    } while (done < len);
    return out;
}

static inline size_t fx_zlib_stored(uint8_t *dst, size_t cap,
                                    const uint8_t *src, size_t len) {
    if (cap < 6) return 0;
    /* CMF=0x78 (deflate, 32 KB window), FLG=0x01: no dictionary, and
     * (0x78 * 256 + 0x01) % 31 == 0, which is the header's own check. */
    dst[0] = 0x78u;
    dst[1] = 0x01u;
    size_t n = fx_deflate_stored(dst + 2, cap - 6, src, len);
    if (n == 0 && len != 0) return 0;
    fx_w32be(dst + 2 + n, lzss_adler32(src, len));
    return 2 + n + 4;
}

/* ------------------------------------------------------------------------ */
/* ZIP                                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char    *name;
    const uint8_t *data;        /* uncompressed contents                     */
    size_t         len;
    bool           deflate;     /* store when false                          */
    bool           streaming;   /* set flag bit 3, as every IPSW member does */
} fx_zip_member_t;

/* Offsets a test can reach back into, so a mutation names a field rather than
 * a magic number. */
typedef struct {
    size_t local_offset[16];
    size_t central_offset[16];
    size_t eocd_offset;
    size_t cd_offset;
    size_t cd_size;
    unsigned count;
    size_t total;
} fx_zip_layout_t;

static inline size_t fx_zip_build(uint8_t *dst, size_t cap,
                                  const fx_zip_member_t *members,
                                  unsigned count,
                                  fx_zip_layout_t *layout) {
    if (count > 16u) return 0;

    fx_zip_layout_t lay;
    memset(&lay, 0, sizeof lay);
    lay.count = count;

    size_t out = 0;
    uint32_t crcs[16];
    size_t comp_sizes[16];

    for (unsigned i = 0; i < count; i++) {
        const fx_zip_member_t *m = &members[i];
        size_t name_len = strlen(m->name);
        lay.local_offset[i] = out;
        crcs[i] = vmfw_crc32(0u, m->data, m->len);

        if (out + 30 + name_len > cap) return 0;
        fx_w32le(dst + out, 0x04034b50u);
        fx_w16le(dst + out + 4, 20u);
        fx_w16le(dst + out + 6, m->streaming ? 0x0008u : 0x0000u);
        fx_w16le(dst + out + 8, m->deflate ? 8u : 0u);
        fx_w16le(dst + out + 10, 0u);
        fx_w16le(dst + out + 12, 0u);
        /* A streaming member leaves crc and both sizes zero here; the truth is
         * only in the central directory. That is exactly the shape of a real
         * IPSW and the reason this reader never trusts a local header. */
        fx_w32le(dst + out + 14, m->streaming ? 0u : crcs[i]);
        fx_w16le(dst + out + 26, (uint16_t)name_len);
        fx_w16le(dst + out + 28, 0u);
        memcpy(dst + out + 30, m->name, name_len);
        size_t payload_at = out + 30 + name_len;

        size_t comp;
        if (m->deflate) {
            comp = fx_deflate_stored(dst + payload_at, cap - payload_at,
                                     m->data, m->len);
            if (comp == 0 && m->len != 0) return 0;
        } else {
            if (payload_at + m->len > cap) return 0;
            memcpy(dst + payload_at, m->data, m->len);
            comp = m->len;
        }
        comp_sizes[i] = comp;
        if (!m->streaming) {
            fx_w32le(dst + out + 18, (uint32_t)comp);
            fx_w32le(dst + out + 22, (uint32_t)m->len);
        }
        out = payload_at + comp;
    }

    lay.cd_offset = out;
    for (unsigned i = 0; i < count; i++) {
        const fx_zip_member_t *m = &members[i];
        size_t name_len = strlen(m->name);
        lay.central_offset[i] = out;
        if (out + 46 + name_len > cap) return 0;
        fx_w32le(dst + out, 0x02014b50u);
        fx_w16le(dst + out + 4, 20u);
        fx_w16le(dst + out + 6, 20u);
        fx_w16le(dst + out + 8, m->streaming ? 0x0008u : 0x0000u);
        fx_w16le(dst + out + 10, m->deflate ? 8u : 0u);
        fx_w32le(dst + out + 16, crcs[i]);
        fx_w32le(dst + out + 20, (uint32_t)comp_sizes[i]);
        fx_w32le(dst + out + 24, (uint32_t)m->len);
        fx_w16le(dst + out + 28, (uint16_t)name_len);
        fx_w16le(dst + out + 30, 0u);
        fx_w16le(dst + out + 32, 0u);
        fx_w16le(dst + out + 34, 0u);
        fx_w16le(dst + out + 36, 0u);
        fx_w32le(dst + out + 38, 0u);
        fx_w32le(dst + out + 42, (uint32_t)lay.local_offset[i]);
        memcpy(dst + out + 46, m->name, name_len);
        out += 46 + name_len;
    }
    lay.cd_size = out - lay.cd_offset;

    lay.eocd_offset = out;
    if (out + 22 > cap) return 0;
    fx_w32le(dst + out, 0x06054b50u);
    fx_w16le(dst + out + 4, 0u);
    fx_w16le(dst + out + 6, 0u);
    fx_w16le(dst + out + 8, (uint16_t)count);
    fx_w16le(dst + out + 10, (uint16_t)count);
    fx_w32le(dst + out + 12, (uint32_t)lay.cd_size);
    fx_w32le(dst + out + 16, (uint32_t)lay.cd_offset);
    fx_w16le(dst + out + 20, 0u);
    out += 22;

    lay.total = out;
    if (layout) *layout = lay;
    return out;
}

/* A pread over a fixture buffer, which is what every reader here consumes. */
typedef struct {
    const uint8_t *data;
    uint64_t       len;
    unsigned       reads;
    bool           fail_next;
} fx_blob_t;

static inline size_t fx_blob_pread(void *ctx, uint64_t off, uint8_t *buf,
                                   size_t len) {
    fx_blob_t *b = (fx_blob_t *)ctx;
    b->reads++;
    if (b->fail_next) return 0;
    if (off > b->len) return 0;
    uint64_t avail = b->len - off;
    size_t take = (uint64_t)len < avail ? len : (size_t)avail;
    memcpy(buf, b->data + off, take);
    return take;
}

/* ------------------------------------------------------------------------ */
/* IMG3                                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t       ident;        /* 'krnl', 'dtre'                           */
    const uint8_t *payload;
    size_t         payload_len;
    bool           encrypt;      /* wrap payload in AES-CBC and add a KBAG   */
    const uint8_t *key;          /* 16 bytes when encrypting                 */
    const uint8_t *iv;           /* 16 bytes when encrypting                 */
    /* Emit a KBAG that img3.c can find but cannot parse -- a real shape,
     * because an image whose key bag is damaged is still an encrypted image
     * and treating it as plaintext would copy ciphertext onward. */
    bool           malformed_kbag;
} fx_img3_spec_t;

static inline size_t fx_img3_tag(uint8_t *dst, size_t cap, uint32_t magic,
                                 const uint8_t *data, size_t data_len,
                                 size_t pad) {
    size_t total = 12 + data_len + pad;
    if (total > cap) return 0;
    /* Tag magics are stored the same way the header's is: the four characters
     * in reverse, so a little-endian read gives the constant. */
    fx_w32le(dst, magic);
    fx_w32le(dst + 4, (uint32_t)total);
    fx_w32le(dst + 8, (uint32_t)data_len);
    if (data_len) memcpy(dst + 12, data, data_len);
    if (pad) memset(dst + 12 + data_len, 0, pad);
    return total;
}

static inline size_t fx_img3_build(uint8_t *dst, size_t cap,
                                   const fx_img3_spec_t *spec) {
    size_t out = 20;   /* header filled in last */
    if (cap < 20) return 0;

    uint8_t type_body[4];
    fx_w32le(type_body, spec->ident);
    size_t n = fx_img3_tag(dst + out, cap - out, IMG3_TAG_TYPE, type_body, 4, 16);
    if (!n) return 0;
    out += n;

    /* DATA */
    size_t storage = spec->payload_len;
    if (spec->encrypt) {
        if (storage > SIZE_MAX - (AES_BLOCK_SIZE - 1u)) return 0;
        storage = (storage + AES_BLOCK_SIZE - 1u) & ~(size_t)(AES_BLOCK_SIZE - 1u);
    }
    if (storage > cap - out || cap - out - storage < 12u) return 0;
    fx_w32le(dst + out, IMG3_TAG_DATA);
    fx_w32le(dst + out + 4, (uint32_t)(12 + storage));
    fx_w32le(dst + out + 8, (uint32_t)spec->payload_len);
    memcpy(dst + out + 12, spec->payload, spec->payload_len);
    if (storage > spec->payload_len)
        memset(dst + out + 12 + spec->payload_len, 0, storage - spec->payload_len);
    if (spec->encrypt) {
        aes_ctx_t ctx;
        if (!aes_init(&ctx, spec->key, 128u)) return 0;
        if (!aes_cbc_encrypt(&ctx, spec->iv, dst + out + 12,
                             dst + out + 12, storage))
            return 0;
    }
    out += 12 + storage;

    if (spec->malformed_kbag) {
        /* Present, and too short for the fields it promises: keyBits says 256
         * but only 8 bytes follow. img3.c marks this `malformed` rather than
         * `present`, and the distinction is the whole point of the flag. */
        uint8_t kbag[32];
        memset(kbag, 0, sizeof kbag);
        fx_w32le(kbag, 1u);
        fx_w32le(kbag + 4, 256u);
        n = fx_img3_tag(dst + out, cap - out, IMG3_TAG_KBAG, kbag, sizeof kbag, 0);
        if (!n) return 0;
        out += n;
    } else if (spec->encrypt) {
        /* cryptState(4) keyBits(4) IV(16) key(16). The key and IV stored here
         * are deliberately NOT the ones that decrypt the payload -- on real
         * firmware they are wrapped, and a reader that used them would produce
         * garbage. The fixture reproduces that trap. */
        uint8_t kbag[40];
        memset(kbag, 0, sizeof kbag);
        fx_w32le(kbag, 1u);
        fx_w32le(kbag + 4, 128u);
        memset(kbag + 8, 0xA5, 16);
        memset(kbag + 24, 0x5A, 16);
        n = fx_img3_tag(dst + out, cap - out, IMG3_TAG_KBAG, kbag, sizeof kbag, 4);
        if (!n) return 0;
        out += n;
    }

    uint8_t shsh[128];
    memset(shsh, 0x11, sizeof shsh);
    n = fx_img3_tag(dst + out, cap - out, IMG3_TAG_SHSH, shsh, sizeof shsh, 0);
    if (!n) return 0;
    out += n;

    fx_w32le(dst, IMG3_MAGIC);
    fx_w32le(dst + 4, (uint32_t)out);          /* fullSize   */
    fx_w32le(dst + 8, (uint32_t)(out - 20));   /* dataSize   */
    fx_w32le(dst + 12, (uint32_t)(out - 20));  /* signedSize */
    fx_w32le(dst + 16, spec->ident);
    return out;
}

/* ------------------------------------------------------------------------ */
/* complzss, all literals                                                    */
/* ------------------------------------------------------------------------ */
/*
 * Okumura's LZSS emits a flag byte for every eight items, with a set bit
 * meaning "the next byte is a literal". A stream of 0xFF flag bytes followed by
 * eight literals each is therefore a valid, if useless, encoding of any input,
 * and it drives the real decompressor over its real literal path.
 */
static inline size_t fx_lzss_literal(uint8_t *dst, size_t cap,
                                     const uint8_t *src, size_t len) {
    if (cap < LZSS_HEADER_SIZE) return 0;
    memset(dst, 0, LZSS_HEADER_SIZE);
    memcpy(dst, "complzss", 8);

    size_t out = LZSS_HEADER_SIZE;
    size_t i = 0;
    while (i < len) {
        size_t group = len - i;
        if (group > 8) group = 8;
        if (out + 1 + group > cap) return 0;
        dst[out++] = (uint8_t)((1u << group) - 1u);   /* all literals */
        memcpy(dst + out, src + i, group);
        out += group;
        i += group;
    }

    fx_w32be(dst + 8, lzss_adler32(src, len));
    fx_w32be(dst + 12, (uint32_t)len);
    fx_w32be(dst + 16, (uint32_t)(out - LZSS_HEADER_SIZE));
    return out;
}

/* ------------------------------------------------------------------------ */
/* base64, for the UDIF resource fork                                        */
/* ------------------------------------------------------------------------ */

static inline size_t fx_base64(char *dst, size_t cap, const uint8_t *src,
                               size_t len, unsigned wrap) {
    static const char A[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = 0;
    unsigned col = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16;
        size_t have = 1;
        if (i + 1 < len) { v |= (uint32_t)src[i + 1] << 8; have = 2; }
        if (i + 2 < len) { v |= (uint32_t)src[i + 2];      have = 3; }
        if (out + 4 >= cap) return 0;
        dst[out++] = A[(v >> 18) & 0x3fu];
        dst[out++] = A[(v >> 12) & 0x3fu];
        dst[out++] = have > 1 ? A[(v >> 6) & 0x3fu] : '=';
        dst[out++] = have > 2 ? A[v & 0x3fu]        : '=';
        col += 4;
        if (wrap && col >= wrap) {
            if (out + 2 >= cap) return 0;
            dst[out++] = '\n';
            dst[out++] = '\t';
            col = 0;
        }
    }
    if (out >= cap) return 0;
    dst[out] = '\0';
    return out;
}

/* ------------------------------------------------------------------------ */
/* encrcdsa                                                                  */
/* ------------------------------------------------------------------------ */
/*
 * Wrap a plaintext image the way Apple's tool does: a 0x1e000-byte header area
 * we only ever read six fields out of, then the payload in `block_size` blocks,
 * each CBC-encrypted under an IV of HMAC-SHA1(hmac_key, big-endian index).
 */
#define FX_ENCRCDSA_DATA_OFFSET 0x1e000u

static inline size_t fx_encrcdsa_wrap(uint8_t *dst, size_t cap,
                                      const uint8_t *plain, size_t plain_len,
                                      const uint8_t key[36],
                                      uint32_t block_size) {
    size_t blocks = (plain_len + block_size - 1u) / block_size;
    size_t total = FX_ENCRCDSA_DATA_OFFSET + blocks * block_size;
    if (total > cap) return 0;

    memset(dst, 0, FX_ENCRCDSA_DATA_OFFSET);
    memcpy(dst, "encrcdsa", 8);
    fx_w32be(dst + 8, 2u);                     /* version    */
    fx_w32be(dst + 0x0c, 16u);                 /* encIvSize  */
    fx_w32be(dst + 0x34, block_size);
    fx_w64be(dst + 0x38, (uint64_t)plain_len); /* dataSize   */
    fx_w64be(dst + 0x40, FX_ENCRCDSA_DATA_OFFSET);

    aes_ctx_t ctx;
    if (!aes_init(&ctx, key, 128u)) return 0;

    for (size_t i = 0; i < blocks; i++) {
        uint8_t staging[8192];
        if (block_size > sizeof staging) return 0;
        size_t off = i * block_size;
        size_t take = plain_len - off < block_size ? plain_len - off : block_size;
        memset(staging, 0, block_size);
        memcpy(staging, plain + off, take);

        uint8_t be[4];
        fx_w32be(be, (uint32_t)i);
        uint8_t mac[VMFW_SHA1_DIGEST_SIZE];
        if (!vmfw_hmac_sha1(key + 16, 20u, be, 4u, mac)) return 0;

        if (!aes_cbc_encrypt(&ctx, mac, staging,
                             dst + FX_ENCRCDSA_DATA_OFFSET + off, block_size))
            return 0;
    }
    return total;
}

#endif /* S5LBOX_VM_FIRMWARE_FIXTURES_H */
