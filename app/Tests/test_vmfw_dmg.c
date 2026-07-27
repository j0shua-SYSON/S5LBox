/*
 * S5LBox — the two layers between an IPSW member and rootfs.img.
 *
 * The root filesystem is the artefact with the most ways to come out subtly
 * wrong, and almost none of them announce themselves:
 *
 *   - `encrcdsa` has no authentication tag. A wrong key decrypts successfully
 *     and produces noise. The earliest honest signal is the UDIF trailer
 *     failing to appear, and this suite requires that signal to be reported as
 *     a key problem rather than as a corrupt image.
 *   - A UDIF is a table of chunks addressed by sector. A table with a gap, an
 *     overlap, or a chunk that decompresses to the wrong length still produces
 *     a file of plausible size. Every one of those is refused here by name.
 *   - The decrypted image is a whole disk. Taking all of it rather than the
 *     Apple_HFSX partition gives a file 43,008 bytes too long that still looks
 *     like a filesystem.
 *
 * The fixture is a complete two-partition disk image built field by field, so
 * a reader with the layout wrong cannot agree with it.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareTest.h"
#include "VMFirmwareFixtures.h"

#include <stdio.h>
#include <stdlib.h>

#define SECTORS_FREE   4u        /* a leading Apple_Free partition          */
#define SECTORS_ROOT  16u        /* the Apple_HFSX partition we want        */
#define ROOT_BYTES    (SECTORS_ROOT * 512u)

/* An arbitrary, non-secret test key. Not firmware key material of any kind. */
static const uint8_t k_test_key[VMFW_DMG_KEY_BLOB_SIZE] = {
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
    0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
    0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,
    0x90,0xa0,0xb0,0xc0,0xd0,0xe0,0xf0,0x01,
    0x02,0x03,0x04,0x05
};

/* ------------------------------------------------------------------------ */
/* A UDIF built from a chunk plan                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t type;
    uint32_t sector_count;    /* 0 for comment/END                          */
    /*
     * Two ways to build a table that is internally consistent about totals and
     * still wrong. Both had to be added because the first version of this
     * fixture computed every chunk's sector number cumulatively, which made a
     * gap impossible to express -- and a check that no fixture can violate is
     * a check no test is exercising.
     */
    int32_t  sector_bias;     /* shift this chunk's declared start sector    */
    bool     bad_extent;      /* declare compressed data past the image      */
} plan_t;

typedef struct {
    uint8_t  image[262144];   /* the finished UDIF (plaintext)              */
    size_t   image_len;
    uint8_t  expect[ROOT_BYTES];   /* what the root partition must expand to */
    size_t   xml_offset;
    size_t   xml_len;
    size_t   koly_offset;
    /* where the root partition's chunk descriptors ended up, so a mutation can
     * change one field of one chunk. */
    size_t   root_blkx_b64_offset;
} udif_t;

/* Build one mish blob for a partition. Returns its length. */
static size_t build_mish(uint8_t *dst, size_t cap, uint64_t first_sector,
                         uint64_t sector_count, const plan_t *plan,
                         unsigned nplan, const uint8_t *source,
                         uint8_t *data_fork, size_t *data_fork_len,
                         size_t data_fork_cap, uint8_t *expect) {
    size_t need = 0xCCu + (size_t)nplan * 40u;
    if (need > cap) return 0;
    memset(dst, 0, need);
    memcpy(dst, "mish", 4);
    fx_w32be(dst + 4, 1u);
    fx_w64be(dst + 8, first_sector);
    fx_w64be(dst + 0x10, sector_count);
    fx_w64be(dst + 0x18, 0u);              /* per-blkx data offset */
    fx_w32be(dst + 0xC8, nplan);

    uint64_t sector = 0;
    for (unsigned i = 0; i < nplan; i++) {
        uint8_t *c = dst + 0xCC + (size_t)i * 40u;
        uint32_t type = plan[i].type;
        uint64_t count = plan[i].sector_count;
        uint64_t declared = (uint64_t)((int64_t)sector + plan[i].sector_bias);
        fx_w32be(c, type);
        fx_w32be(c + 4, 0u);
        fx_w64be(c + 8, declared);
        fx_w64be(c + 0x10, count);

        if (type == 0x7FFFFFFEu || type == 0xFFFFFFFFu) {
            fx_w64be(c + 0x18, (uint64_t)*data_fork_len);
            fx_w64be(c + 0x20, 0u);
            continue;
        }

        size_t bytes = (size_t)count * 512u;
        const uint8_t *src = source ? source + sector * 512u : NULL;

        if (type == 0x00000000u || type == 0x00000002u) {   /* ZERO, IGNORE */
            if (expect) memset(expect + sector * 512u, 0, bytes);
            fx_w64be(c + 0x18, (uint64_t)*data_fork_len);
            fx_w64be(c + 0x20, 0u);
        } else if (type == 0x00000001u) {                   /* RAW */
            if (*data_fork_len + bytes > data_fork_cap) return 0;
            memcpy(data_fork + *data_fork_len, src, bytes);
            if (expect) memcpy(expect + sector * 512u, src, bytes);
            fx_w64be(c + 0x18, (uint64_t)*data_fork_len);
            fx_w64be(c + 0x20, (uint64_t)bytes);
            *data_fork_len += bytes;
        } else if (type == 0x80000005u) {                   /* zlib */
            size_t n = fx_zlib_stored(data_fork + *data_fork_len,
                                      data_fork_cap - *data_fork_len,
                                      src, bytes);
            if (n == 0) return 0;
            if (expect) memcpy(expect + sector * 512u, src, bytes);
            if (plan[i].bad_extent) {
                /* An offset inside the image with a length that runs off the
                 * end of it. Unbounded, this is a read past the mapping. */
                fx_w64be(c + 0x18, (uint64_t)*data_fork_len);
                fx_w64be(c + 0x20, (uint64_t)0xffffffffu);
            } else {
                fx_w64be(c + 0x18, (uint64_t)*data_fork_len);
                fx_w64be(c + 0x20, (uint64_t)n);
            }
            *data_fork_len += n;
        } else {
            /* An unimplemented compression, used on purpose by one test. */
            fx_w64be(c + 0x18, (uint64_t)*data_fork_len);
            fx_w64be(c + 0x20, 16u);
        }
        sector += count;
    }
    return need;
}

static bool build_udif(udif_t *u, const plan_t *root_plan, unsigned root_n) {
    memset(u, 0, sizeof *u);

    uint8_t source[ROOT_BYTES];
    fx_fill(source, sizeof source, 4242u);
    memcpy(u->expect, source, sizeof source);

    static uint8_t data_fork[131072];
    size_t data_len = 0;

    /* A leading Apple_Free partition, so the root partition does not start at
     * sector zero -- an expander that ignored first_sector would still pass if
     * it did. */
    static uint8_t mish_free[512];
    static const plan_t free_plan[2] = {
        { 0x00000002u, SECTORS_FREE, 0, false }, { 0xFFFFFFFFu, 0u, 0, false }
    };
    size_t mfree = build_mish(mish_free, sizeof mish_free, 0u, SECTORS_FREE,
                              free_plan, 2u, NULL, data_fork, &data_len,
                              sizeof data_fork, NULL);
    if (!mfree) return false;

    static uint8_t mish_root[8192];
    size_t mroot = build_mish(mish_root, sizeof mish_root, SECTORS_FREE,
                              SECTORS_ROOT, root_plan, root_n, source,
                              data_fork, &data_len, sizeof data_fork,
                              u->expect);
    if (!mroot) return false;

    static char b64_free[4096];
    static char b64_root[32768];
    if (!fx_base64(b64_free, sizeof b64_free, mish_free, mfree, 64u)) return false;
    if (!fx_base64(b64_root, sizeof b64_root, mish_root, mroot, 64u)) return false;

    static char xml[65536];
    int n = snprintf(xml, sizeof xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>resource-fork</key>\n"
        "\t<dict>\n"
        "\t\t<key>blkx</key>\n"
        "\t\t<array>\n"
        "\t\t\t<dict>\n"
        "\t\t\t\t<key>Attributes</key>\n"
        "\t\t\t\t<string>0x0050</string>\n"
        "\t\t\t\t<key>Data</key>\n"
        "\t\t\t\t<data>\n\t\t\t\t%s\n\t\t\t\t</data>\n"
        "\t\t\t\t<key>ID</key>\n"
        "\t\t\t\t<string>0</string>\n"
        "\t\t\t\t<key>Name</key>\n"
        "\t\t\t\t<string> (Apple_Free : 1)</string>\n"
        "\t\t\t</dict>\n"
        "\t\t\t<dict>\n"
        "\t\t\t\t<key>Attributes</key>\n"
        "\t\t\t\t<string>0x0050</string>\n"
        "\t\t\t\t<key>Data</key>\n"
        "\t\t\t\t<data>\n\t\t\t\t%s\n\t\t\t\t</data>\n"
        "\t\t\t\t<key>ID</key>\n"
        "\t\t\t\t<string>1</string>\n"
        "\t\t\t\t<key>Name</key>\n"
        "\t\t\t\t<string>Mac_OS_X (Apple_HFSX : 2)</string>\n"
        "\t\t\t</dict>\n"
        "\t\t</array>\n"
        "\t</dict>\n"
        "</dict>\n"
        "</plist>\n", b64_free, b64_root);
    if (n <= 0 || (size_t)n >= sizeof xml) return false;

    /* data fork, then the resource fork, then the trailer. */
    if (data_len + (size_t)n + 512u > sizeof u->image) return false;
    memcpy(u->image, data_fork, data_len);
    u->xml_offset = data_len;
    u->xml_len = (size_t)n;
    memcpy(u->image + u->xml_offset, xml, u->xml_len);
    u->koly_offset = u->xml_offset + u->xml_len;

    uint8_t *k = u->image + u->koly_offset;
    memset(k, 0, 512);
    memcpy(k, "koly", 4);
    fx_w32be(k + 4, 4u);
    fx_w32be(k + 8, 512u);
    fx_w64be(k + 0x18, 0u);                       /* dataForkOffset */
    fx_w64be(k + 0x20, (uint64_t)data_len);       /* dataForkLength */
    fx_w64be(k + 0xD8, (uint64_t)u->xml_offset);
    fx_w64be(k + 0xE0, (uint64_t)u->xml_len);
    fx_w64be(k + 0x1EC, (uint64_t)(SECTORS_FREE + SECTORS_ROOT));

    u->image_len = u->koly_offset + 512u;

    /* Find the root partition's base64 span inside the XML, so mutations can
     * reach a chunk descriptor. */
    const char *found = strstr(xml, b64_root);
    u->root_blkx_b64_offset = found ? u->xml_offset + (size_t)(found - xml) : 0u;
    return true;
}

/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t  buf[ROOT_BYTES * 2u];
    size_t   len;
    bool     overflow;
    unsigned refuse_after;
} sink_t;

static bool sink_write(void *ctx, const uint8_t *d, size_t n) {
    sink_t *s = (sink_t *)ctx;
    if (s->refuse_after && s->len + n > s->refuse_after) return false;
    if (s->len + n > sizeof s->buf) { s->overflow = true; return false; }
    memcpy(s->buf + s->len, d, n);
    s->len += n;
    return true;
}

static const plan_t k_good_plan[] = {
    { 0x7FFFFFFEu, 0u, 0, false },   /* comment: carries no sectors      */
    { 0x80000005u, 4u, 0, false },   /* zlib                             */
    { 0x00000001u, 2u, 0, false },   /* raw                              */
    { 0x00000000u, 3u, 0, false },   /* zero                             */
    { 0x80000005u, 5u, 0, false },   /* zlib again, after a hole         */
    { 0x00000002u, 2u, 0, false },   /* ignore                           */
    { 0xFFFFFFFFu, 0u, 0, false },   /* end                              */
};
#define GOOD_PLAN_N (sizeof k_good_plan / sizeof k_good_plan[0])

void vmfw_test_dmg(vmfw_test_t *t) {
    static udif_t u;
    static uint8_t wrapped[262144 + 0x1e000u];
    static uint8_t xml_scratch[65536];
    static uint8_t table[8192];

    VMFW_T_SECTION(t, "dmg/fixture");
    VMFW_T_CHECK(t, build_udif(&u, k_good_plan, (unsigned)GOOD_PLAN_N),
                 "the fixture disk image was built");

    /* ---------------- plain, unwrapped --------------------------------- */
    VMFW_T_SECTION(t, "dmg/plain udif");
    {
        fx_blob_t b = { u.image, u.image_len, 0u, false };
        vmfw_dmg_reader_t r;
        VMFW_T_EQ_U(t, vmfw_dmg_reader_open(&r, fx_blob_pread, &b,
                                            u.image_len, NULL, 0u),
                    VMFW_DMG_OK, "an unencrypted image opens with no key");
        VMFW_T_CHECK(t, !r.encrypted, "and is not marked encrypted");

        vmfw_dmg_info_t info;
        size_t needed = 0;
        (void)vmfw_dmg_probe(&r, NULL, 0, &needed, &info);
        VMFW_T_EQ_U(t, needed, u.xml_len, "probe reports the resource fork size");

        VMFW_T_EQ_U(t, vmfw_dmg_probe(&r, xml_scratch, sizeof xml_scratch,
                                      NULL, &info),
                    VMFW_DMG_OK, "probe");
        VMFW_T_EQ_U(t, info.partition_count, 2u, "two partitions");
        VMFW_T_EQ_U(t, info.sector_count, SECTORS_FREE + SECTORS_ROOT,
                    "whole-disk sector count");

        uint32_t idx = 99u;
        VMFW_T_EQ_U(t, vmfw_dmg_find_partition(&info, "Apple_HFSX", &idx),
                    VMFW_DMG_OK, "the root partition is found by name");
        VMFW_T_EQ_U(t, idx, 1u, "and it is the second one");
        VMFW_T_EQ_U(t, info.partitions[idx].first_sector, SECTORS_FREE,
                    "its first sector is not zero");
        VMFW_T_EQ_U(t, info.partitions[idx].sector_count, SECTORS_ROOT,
                    "its sector count");

        static sink_t s;
        memset(&s, 0, sizeof s);
        uint64_t got = 0;
        VMFW_T_EQ_U(t, vmfw_dmg_extract_partition(&r, &info, xml_scratch,
                                                  u.xml_len, idx, table,
                                                  sizeof table, sink_write, &s,
                                                  &got),
                    VMFW_DMG_OK, "extract");
        VMFW_T_EQ_U(t, got, ROOT_BYTES, "produced the partition's byte count");
        VMFW_T_EQ_U(t, s.len, ROOT_BYTES, "sink received that many");
        if (s.len == ROOT_BYTES)
            VMFW_T_EQ_MEM(t, s.buf, u.expect, ROOT_BYTES,
                          "expanded bytes match, including the zeroed holes");

        uint32_t missing = 0;
        VMFW_T_EQ_U(t, vmfw_dmg_find_partition(&info, "Apple_APFS", &missing),
                    VMFW_DMG_ERR_NO_PARTITION, "a partition that is not there");
    }

    /* ---------------- wrapped in encrcdsa ------------------------------ */
    VMFW_T_SECTION(t, "dmg/encrcdsa");
    size_t wlen = fx_encrcdsa_wrap(wrapped, sizeof wrapped, u.image,
                                   u.image_len, k_test_key, 4096u);
    VMFW_T_CHECK(t, wlen > 0, "the fixture was wrapped");
    {
        fx_blob_t b = { wrapped, wlen, 0u, false };
        vmfw_dmg_reader_t r;

        VMFW_T_EQ_U(t, vmfw_dmg_reader_open(&r, fx_blob_pread, &b, wlen, NULL, 0u),
                    VMFW_DMG_ERR_KEY_REQUIRED,
                    "an encrypted image with no key says so specifically");

        uint8_t shortkey[8] = { 0 };
        VMFW_T_EQ_U(t, vmfw_dmg_reader_open(&r, fx_blob_pread, &b, wlen,
                                            shortkey, sizeof shortkey),
                    VMFW_DMG_ERR_BAD_KEY_LENGTH, "a key of the wrong length");

        VMFW_T_EQ_U(t, vmfw_dmg_reader_open(&r, fx_blob_pread, &b, wlen,
                                            k_test_key, sizeof k_test_key),
                    VMFW_DMG_OK, "opens with the right key");
        VMFW_T_CHECK(t, r.encrypted, "and is marked encrypted");
        VMFW_T_EQ_U(t, r.plain_size, u.image_len, "logical size");

        /* The transparent reader must reproduce the plaintext exactly, at
         * every alignment -- a block-boundary bug would show up only for reads
         * that straddle one. */
        for (size_t off = 0; off + 300u < u.image_len; off += 997u) {
            uint8_t got[300];
            if (vmfw_dmg_reader_pread(&r, off, got, sizeof got) != VMFW_DMG_OK) {
                VMFW_T_CHECK(t, false, "pread failed at offset %zu", off);
                break;
            }
            if (memcmp(got, u.image + off, sizeof got) != 0) {
                VMFW_T_CHECK(t, false, "decrypted bytes differ at %zu", off);
                break;
            }
        }
        VMFW_T_CHECK(t, true, "transparent decryption matches at every offset");

        /* Reading past the logical end must be refused, not served out of the
         * ciphertext padding. */
        uint8_t tail[64];
        VMFW_T_EQ_U(t, vmfw_dmg_reader_pread(&r, u.image_len - 10u, tail, 64u),
                    VMFW_DMG_ERR_BAD_GEOMETRY,
                    "a read past the logical end is refused");

        vmfw_dmg_info_t info;
        VMFW_T_EQ_U(t, vmfw_dmg_probe(&r, xml_scratch, sizeof xml_scratch,
                                      NULL, &info),
                    VMFW_DMG_OK, "probe through the wrapper");
        uint32_t idx = 0;
        vmfw_dmg_find_partition(&info, "Apple_HFSX", &idx);

        static sink_t s;
        memset(&s, 0, sizeof s);
        uint64_t got = 0;
        VMFW_T_EQ_U(t, vmfw_dmg_extract_partition(&r, &info, xml_scratch,
                                                  u.xml_len, idx, table,
                                                  sizeof table, sink_write, &s,
                                                  &got),
                    VMFW_DMG_OK, "extract through the wrapper");
        if (s.len == ROOT_BYTES)
            VMFW_T_EQ_MEM(t, s.buf, u.expect, ROOT_BYTES,
                          "decrypt-and-expand fused in one pass matches");
    }

    /* A key that is not this image's. There is no tag to catch it, so the
     * trailer is the first thing that disagrees, and that must be reported as
     * a key problem rather than as a damaged image. */
    VMFW_T_SECTION(t, "dmg/wrong key");
    {
        uint8_t other[VMFW_DMG_KEY_BLOB_SIZE];
        memcpy(other, k_test_key, sizeof other);
        other[0] ^= 0x01u;

        fx_blob_t b = { wrapped, wlen, 0u, false };
        vmfw_dmg_reader_t r;
        VMFW_T_EQ_U(t, vmfw_dmg_reader_open(&r, fx_blob_pread, &b, wlen,
                                            other, sizeof other),
                    VMFW_DMG_OK, "a wrong key still opens: nothing checks it");
        vmfw_dmg_info_t info;
        size_t needed = 0;
        VMFW_T_EQ_U(t, vmfw_dmg_probe(&r, NULL, 0, &needed, &info),
                    VMFW_DMG_ERR_WRONG_KEY,
                    "and the missing trailer is reported as a key problem");

        /* The same failure on an UNencrypted image is not a key problem. */
        uint8_t junk[2048];
        fx_fill(junk, sizeof junk, 9u);
        fx_blob_t b2 = { junk, sizeof junk, 0u, false };
        vmfw_dmg_reader_t r2;
        VMFW_T_EQ_U(t, vmfw_dmg_reader_open(&r2, fx_blob_pread, &b2,
                                            sizeof junk, NULL, 0u),
                    VMFW_DMG_OK, "random bytes open as a plain image");
        VMFW_T_EQ_U(t, vmfw_dmg_probe(&r2, NULL, 0, &needed, &info),
                    VMFW_DMG_ERR_NO_KOLY,
                    "and their missing trailer is NOT blamed on a key");
    }

    /* ---------------- chunk-table mutations ---------------------------- */
    /*
     * Each rebuilds the fixture with one plan changed, so the mutation is a
     * property of the image rather than of a hand-edited buffer.
     */
#define PLAN_CASE(section, mutate, expect_status)                             \
    do {                                                                     \
        VMFW_T_SECTION(t, section);                                          \
        plan_t p[GOOD_PLAN_N + 2u];                                          \
        unsigned pn = (unsigned)GOOD_PLAN_N;                                 \
        memcpy(p, k_good_plan, sizeof k_good_plan);                          \
        { mutate; }                                                          \
        static udif_t mu;                                                    \
        if (!build_udif(&mu, p, pn)) {                                       \
            VMFW_T_CHECK(t, false, "%s: fixture build failed", section);     \
            break;                                                           \
        }                                                                    \
        fx_blob_t mb = { mu.image, mu.image_len, 0u, false };                \
        vmfw_dmg_reader_t mr;                                                \
        vmfw_dmg_reader_open(&mr, fx_blob_pread, &mb, mu.image_len, NULL, 0u);\
        vmfw_dmg_info_t mi;                                                  \
        vmfw_dmg_status_t ms = vmfw_dmg_probe(&mr, xml_scratch,              \
                                              sizeof xml_scratch, NULL, &mi);\
        if (ms == VMFW_DMG_OK) {                                             \
            uint32_t mx = 0;                                                 \
            ms = vmfw_dmg_find_partition(&mi, "Apple_HFSX", &mx);            \
            if (ms == VMFW_DMG_OK) {                                         \
                static sink_t ss; memset(&ss, 0, sizeof ss);                 \
                uint64_t g = 0;                                              \
                ms = vmfw_dmg_extract_partition(&mr, &mi, xml_scratch,       \
                                                mu.xml_len, mx, table,       \
                                                sizeof table, sink_write,    \
                                                &ss, &g);                    \
            }                                                                \
        }                                                                    \
        VMFW_T_EQ_U(t, ms, (expect_status), section);                        \
    } while (0)

    /* A chunk table that stops before the partition ends. Without the total
     * check this yields a short file that still mounts. */
    PLAN_CASE("dmg/table stops early", p[4].sector_count = 4u,
              VMFW_DMG_ERR_NOT_CONTIGUOUS);
    /* And one that claims more than the partition holds. */
    PLAN_CASE("dmg/table overruns", p[4].sector_count = 6u,
              VMFW_DMG_ERR_BAD_CHUNK);
    /* A compression we do not implement is named, not silently skipped. */
    PLAN_CASE("dmg/bzip2 chunk", p[2].type = 0x80000006u,
              VMFW_DMG_ERR_CHUNK_TYPE);
    PLAN_CASE("dmg/lzfse chunk", p[2].type = 0x80000007u,
              VMFW_DMG_ERR_CHUNK_TYPE);
    PLAN_CASE("dmg/adc chunk", p[2].type = 0x80000004u,
              VMFW_DMG_ERR_CHUNK_TYPE);
    /* An END chunk that claims sectors would shift everything after it. */
    PLAN_CASE("dmg/end chunk with sectors",
              p[GOOD_PLAN_N - 1u].sector_count = 3u,
              VMFW_DMG_ERR_BAD_CHUNK);
    PLAN_CASE("dmg/comment chunk with sectors", p[0].sector_count = 2u,
              VMFW_DMG_ERR_BAD_CHUNK);
    /*
     * A gap: the totals still add up, every chunk is individually plausible,
     * and the sectors simply do not tile. Streaming output would write each
     * chunk at the wrong place and produce a full-length filesystem with its
     * contents shifted -- the failure the per-chunk check exists for, and one
     * the partition-total check cannot see.
     */
    PLAN_CASE("dmg/chunk starts after a gap", p[4].sector_bias = 2,
              VMFW_DMG_ERR_NOT_CONTIGUOUS);
    PLAN_CASE("dmg/chunk overlaps the one before it", p[4].sector_bias = -2,
              VMFW_DMG_ERR_NOT_CONTIGUOUS);
    /* A compressed extent that runs off the end of the image. */
    PLAN_CASE("dmg/chunk extent past the image", p[1].bad_extent = true,
              VMFW_DMG_ERR_BAD_CHUNK);
#undef PLAN_CASE

    /* A chunk whose sectors are out of order: streaming output would silently
     * write the wrong region. */
    VMFW_T_SECTION(t, "dmg/chunks out of order");
    {
        static udif_t mu;
        build_udif(&mu, k_good_plan, (unsigned)GOOD_PLAN_N);
        /* Reach into the base64 and flip a chunk's sector number. Doing it
         * through the encoded form would need a re-encode, so instead the
         * partition's declared sector count is raised, which makes the last
         * chunk stop short of the end. */
        fx_blob_t mb = { mu.image, mu.image_len, 0u, false };
        vmfw_dmg_reader_t mr;
        vmfw_dmg_reader_open(&mr, fx_blob_pread, &mb, mu.image_len, NULL, 0u);
        vmfw_dmg_info_t mi;
        vmfw_dmg_probe(&mr, xml_scratch, sizeof xml_scratch, NULL, &mi);
        uint32_t mx = 0;
        vmfw_dmg_find_partition(&mi, "Apple_HFSX", &mx);

        vmfw_dmg_info_t bumped = mi;
        bumped.partitions[mx].sector_count += 4u;
        static sink_t ss; memset(&ss, 0, sizeof ss);
        uint64_t g = 0;
        VMFW_T_EQ_U(t, vmfw_dmg_extract_partition(&mr, &bumped, xml_scratch,
                                                  mu.xml_len, mx, table,
                                                  sizeof table, sink_write,
                                                  &ss, &g),
                    VMFW_DMG_ERR_NOT_CONTIGUOUS,
                    "a partition longer than its chunks is refused");
    }

    /* ---------------- header and trailer mutations --------------------- */
    VMFW_T_SECTION(t, "dmg/header mutations");
    {
        static uint8_t mw[sizeof wrapped];

#define WRAP_CASE(desc, mutate, expect_open, expect_probe)                    \
        do {                                                                 \
            memcpy(mw, wrapped, wlen);                                        \
            { mutate; }                                                       \
            fx_blob_t b = { mw, wlen, 0u, false };                            \
            vmfw_dmg_reader_t r;                                              \
            vmfw_dmg_status_t s = vmfw_dmg_reader_open(&r, fx_blob_pread, &b, \
                                                       wlen, k_test_key,      \
                                                       sizeof k_test_key);    \
            VMFW_T_EQ_U(t, s, (expect_open), desc " (open)");                 \
            if (s == VMFW_DMG_OK) {                                           \
                vmfw_dmg_info_t info; size_t need = 0;                        \
                VMFW_T_EQ_U(t, vmfw_dmg_probe(&r, NULL, 0, &need, &info),     \
                            (expect_probe), desc " (probe)");                 \
            }                                                                 \
        } while (0)

        WRAP_CASE("version 3", fx_w32be(mw + 8, 3u),
                  VMFW_DMG_ERR_ENCRCDSA_VERSION, VMFW_DMG_OK);
        WRAP_CASE("block size zero", fx_w32be(mw + 0x34, 0u),
                  VMFW_DMG_ERR_BAD_BLOCK_SIZE, VMFW_DMG_OK);
        WRAP_CASE("block size not an AES multiple", fx_w32be(mw + 0x34, 4095u),
                  VMFW_DMG_ERR_BAD_BLOCK_SIZE, VMFW_DMG_OK);
        WRAP_CASE("block size larger than the reader's buffer",
                  fx_w32be(mw + 0x34, 8192u),
                  VMFW_DMG_OK, VMFW_DMG_ERR_BAD_BLOCK_SIZE);
        WRAP_CASE("data offset past eof",
                  fx_w64be(mw + 0x40, (uint64_t)wlen + 1000u),
                  VMFW_DMG_ERR_BAD_GEOMETRY, VMFW_DMG_OK);
        WRAP_CASE("data size larger than the file",
                  fx_w64be(mw + 0x38, (uint64_t)wlen * 2u),
                  VMFW_DMG_ERR_BAD_GEOMETRY, VMFW_DMG_OK);
        WRAP_CASE("data size zero", fx_w64be(mw + 0x38, 0u),
                  VMFW_DMG_OK, VMFW_DMG_ERR_NO_KOLY);
#undef WRAP_CASE
    }

    /* Trailer fields, mutated on the plaintext image. */
    VMFW_T_SECTION(t, "dmg/trailer mutations");
    {
        static uint8_t mi_buf[sizeof u.image];

#define KOLY_CASE(desc, mutate, expect_probe)                                 \
        do {                                                                 \
            memcpy(mi_buf, u.image, u.image_len);                             \
            uint8_t *k = mi_buf + u.koly_offset;                              \
            (void)k;                                                          \
            { mutate; }                                                       \
            fx_blob_t b = { mi_buf, u.image_len, 0u, false };                 \
            vmfw_dmg_reader_t r;                                              \
            vmfw_dmg_reader_open(&r, fx_blob_pread, &b, u.image_len, NULL, 0u);\
            vmfw_dmg_info_t info;                                             \
            vmfw_dmg_status_t s = vmfw_dmg_probe(&r, xml_scratch,             \
                                                 sizeof xml_scratch, NULL,    \
                                                 &info);                      \
            VMFW_T_EQ_U(t, s, (expect_probe), desc);                          \
        } while (0)

        KOLY_CASE("koly magic", k[0] ^= 0xffu, VMFW_DMG_ERR_NO_KOLY);
        KOLY_CASE("xml length zero", fx_w64be(k + 0xE0, 0u),
                  VMFW_DMG_ERR_BAD_KOLY);
        KOLY_CASE("xml offset past the image",
                  fx_w64be(k + 0xD8, (uint64_t)u.image_len + 10u),
                  VMFW_DMG_ERR_BAD_KOLY);
        KOLY_CASE("xml length past the image",
                  fx_w64be(k + 0xE0, (uint64_t)u.image_len),
                  VMFW_DMG_ERR_BAD_KOLY);
        KOLY_CASE("data fork past the image",
                  fx_w64be(k + 0x18, (uint64_t)u.image_len + 1u),
                  VMFW_DMG_ERR_BAD_KOLY);
        /* The whole resource fork, not just its first line: the scanner locates
         * the <plist> root wherever it appears, so damaging only the XML
         * declaration leaves a perfectly readable document behind it. */
        KOLY_CASE("resource fork is not a plist",
                  memset(mi_buf + u.xml_offset, 'x', u.xml_len),
                  VMFW_DMG_ERR_XML);
        KOLY_CASE("resource fork has no blkx array",
                  memcpy(mi_buf + u.xml_offset
                         + (size_t)(strstr((const char *)mi_buf + u.xml_offset,
                                           "blkx")
                                    - (const char *)(mi_buf + u.xml_offset)),
                         "blky", 4u),
                  VMFW_DMG_ERR_XML);
#undef KOLY_CASE
    }

    /* ---------------- buffers, sinks and arguments --------------------- */
    VMFW_T_SECTION(t, "dmg/scratch and sink");
    {
        fx_blob_t b = { u.image, u.image_len, 0u, false };
        vmfw_dmg_reader_t r;
        vmfw_dmg_reader_open(&r, fx_blob_pread, &b, u.image_len, NULL, 0u);
        vmfw_dmg_info_t info;

        size_t needed = 0;
        VMFW_T_EQ_U(t, vmfw_dmg_probe(&r, xml_scratch, 4u, &needed, &info),
                    VMFW_DMG_ERR_INVALID_ARGUMENT,
                    "too small a scratch buffer is refused, not truncated");
        VMFW_T_EQ_U(t, needed, u.xml_len, "and the required size is reported");

        vmfw_dmg_probe(&r, xml_scratch, sizeof xml_scratch, NULL, &info);
        uint32_t idx = 0;
        vmfw_dmg_find_partition(&info, "Apple_HFSX", &idx);

        static sink_t s;
        memset(&s, 0, sizeof s);
        uint64_t got = 0;
        VMFW_T_EQ_U(t, vmfw_dmg_extract_partition(&r, &info, xml_scratch,
                                                  u.xml_len, idx, table, 8u,
                                                  sink_write, &s, &got),
                    VMFW_DMG_ERR_SCRATCH_TOO_SMALL,
                    "a chunk table that does not fit is refused by name");

        memset(&s, 0, sizeof s);
        s.refuse_after = 600u;
        VMFW_T_EQ_U(t, vmfw_dmg_extract_partition(&r, &info, xml_scratch,
                                                  u.xml_len, idx, table,
                                                  sizeof table, sink_write, &s,
                                                  &got),
                    VMFW_DMG_ERR_SINK, "a refusing destination is reported");

        memset(&s, 0, sizeof s);
        VMFW_T_EQ_U(t, vmfw_dmg_extract_partition(&r, &info, xml_scratch,
                                                  u.xml_len, 99u, table,
                                                  sizeof table, sink_write, &s,
                                                  &got),
                    VMFW_DMG_ERR_NO_PARTITION, "an out-of-range partition");

        VMFW_T_EQ_U(t, vmfw_dmg_reader_open(NULL, fx_blob_pread, NULL, 0u,
                                            NULL, 0u),
                    VMFW_DMG_ERR_INVALID_ARGUMENT, "NULL reader");
        VMFW_T_EQ_U(t, vmfw_dmg_reader_pread(&r, 0u, NULL, 16u),
                    VMFW_DMG_ERR_INVALID_ARGUMENT, "NULL destination");
        VMFW_T_EQ_U(t, vmfw_dmg_reader_pread(&r, 0u, NULL, 0u), VMFW_DMG_OK,
                    "a zero-length read is fine");
        VMFW_T_CHECK(t, vmfw_dmg_strerror(VMFW_DMG_OK) != NULL, "strerror(OK)");
        VMFW_T_CHECK(t, vmfw_dmg_strerror((vmfw_dmg_status_t)999) != NULL,
                     "strerror of an unknown code");
    }

    /* ---------------- truncation --------------------------------------- */
    VMFW_T_SECTION(t, "dmg/truncation");
    {
        unsigned refused = 0;
        for (size_t cut = 16u; cut < u.image_len; cut += 331u) {
            fx_blob_t b = { u.image, cut, 0u, false };
            vmfw_dmg_reader_t r;
            if (vmfw_dmg_reader_open(&r, fx_blob_pread, &b, cut, NULL, 0u)
                    != VMFW_DMG_OK) { refused++; continue; }
            vmfw_dmg_info_t info;
            if (vmfw_dmg_probe(&r, xml_scratch, sizeof xml_scratch, NULL, &info)
                    != VMFW_DMG_OK) { refused++; continue; }
            uint32_t idx = 0;
            if (vmfw_dmg_find_partition(&info, "Apple_HFSX", &idx)
                    != VMFW_DMG_OK) { refused++; continue; }
            static sink_t s;
            memset(&s, 0, sizeof s);
            uint64_t got = 0;
            vmfw_dmg_status_t st =
                vmfw_dmg_extract_partition(&r, &info, xml_scratch, u.xml_len,
                                           idx, table, sizeof table,
                                           sink_write, &s, &got);
            /* Whatever happens, a truncated image must never yield a
             * complete-looking partition of the wrong bytes. */
            if (st == VMFW_DMG_OK)
                VMFW_T_EQ_MEM(t, s.buf, u.expect, ROOT_BYTES,
                              "a truncated image that still succeeded was "
                              "genuinely complete");
        }
        VMFW_T_CHECK(t, refused > 0, "truncated images are refused");
    }
}
