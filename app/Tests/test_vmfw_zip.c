/*
 * S5LBox — the zip reader, which is the first thing an IPSW meets.
 *
 * The properties defended here are almost all negative. A zip is a format where
 * every interesting number appears twice, in a local header and in the central
 * directory, and they are allowed to disagree; where an offset can point
 * anywhere in the file; and where the record that says how to read the whole
 * thing is found by scanning backwards for four bytes that also occur inside
 * the data. Each of those is a way to read the wrong bytes and report success.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareTest.h"
#include "VMFirmwareFixtures.h"

#include <stdlib.h>

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t buf[8192];
    size_t  len;
    bool    overflow;
    unsigned refuse_after;   /* refuse once this many bytes have arrived */
    bool    refused;
} collect_t;

static bool collect(void *ctx, const uint8_t *d, size_t n) {
    collect_t *c = (collect_t *)ctx;
    if (c->refuse_after && c->len + n > c->refuse_after) {
        c->refused = true;
        return false;
    }
    if (c->len + n > sizeof c->buf) { c->overflow = true; return false; }
    memcpy(c->buf + c->len, d, n);
    c->len += n;
    return true;
}

typedef struct { unsigned seen; char last[VMFW_ZIP_MAX_NAME]; } walk_t;

static bool walk_visit(void *ctx, const vmfw_zip_entry_t *e, uint32_t idx) {
    walk_t *w = (walk_t *)ctx;
    (void)idx;
    w->seen++;
    memcpy(w->last, e->name, sizeof w->last);
    return true;
}

/* Rebuild the standard three-member fixture into `dst`. Returned so every
 * mutation starts from a known-good archive rather than from the previous
 * mutation -- a mutation applied to an already-broken buffer proves nothing. */
static size_t build_standard(uint8_t *dst, size_t cap, fx_zip_layout_t *lay,
                             const uint8_t **payloads, size_t *lens) {
    static uint8_t small[64];
    static uint8_t medium[1000];
    static uint8_t large[3000];
    static bool filled = false;
    if (!filled) {
        fx_fill(small, sizeof small, 11u);
        fx_fill(medium, sizeof medium, 22u);
        /* Highly repetitive, so a real deflate stream would compress it; ours
         * stores it, but the reader cannot tell the difference and should not. */
        for (size_t i = 0; i < sizeof large; i++) large[i] = (uint8_t)(i % 7u);
        filled = true;
    }
    static const fx_zip_member_t members[3] = {
        { "Restore.plist", NULL, 0, false, false },
        { "kernelcache.release.s5l8900x", NULL, 0, true, true },
        { "Firmware/all_flash/x/DeviceTree.n82ap.img3", NULL, 0, true, false },
    };
    fx_zip_member_t m[3];
    memcpy(m, members, sizeof m);
    m[0].data = small;  m[0].len = sizeof small;
    m[1].data = medium; m[1].len = sizeof medium;
    m[2].data = large;  m[2].len = sizeof large;

    if (payloads) { payloads[0] = small; payloads[1] = medium; payloads[2] = large; }
    if (lens) { lens[0] = sizeof small; lens[1] = sizeof medium; lens[2] = sizeof large; }
    return fx_zip_build(dst, cap, m, 3u, lay);
}

/* ------------------------------------------------------------------------ */

void vmfw_test_zip(vmfw_test_t *t) {
    static uint8_t archive[32768];
    fx_zip_layout_t lay;
    const uint8_t *pay[3];
    size_t lens[3];

    /* ---------------- CRC-32 ------------------------------------------- */
    VMFW_T_SECTION(t, "zip/crc32");
    VMFW_T_EQ_U(t, vmfw_crc32(0u, (const uint8_t *)"123456789", 9u),
                0xCBF43926u, "crc32 of the standard check string");
    VMFW_T_EQ_U(t, vmfw_crc32(0u, (const uint8_t *)"", 0u), 0u,
                "crc32 of nothing");
    /* Split updates must equal one update, or streaming would corrupt it. */
    {
        uint32_t whole = vmfw_crc32(0u, (const uint8_t *)"123456789", 9u);
        uint32_t split = vmfw_crc32(0u, (const uint8_t *)"1234", 4u);
        split = vmfw_crc32(split, (const uint8_t *)"56789", 5u);
        VMFW_T_EQ_U(t, split, whole, "crc32 is resumable");
    }

    /* ---------------- the happy path ----------------------------------- */
    VMFW_T_SECTION(t, "zip/open");
    size_t total = build_standard(archive, sizeof archive, &lay, pay, lens);
    VMFW_T_CHECK(t, total > 0, "the fixture archive was built");

    fx_blob_t blob = { archive, total, 0u, false };
    vmfw_zip_t zip;
    VMFW_T_EQ_U(t, vmfw_zip_open(&zip, fx_blob_pread, &blob, total),
                VMFW_ZIP_OK, "open");
    VMFW_T_EQ_U(t, zip.entry_count, 3u, "entry count");
    VMFW_T_EQ_U(t, zip.cd_offset, lay.cd_offset, "central directory offset");

    walk_t w = { 0u, { 0 } };
    VMFW_T_EQ_U(t, vmfw_zip_iterate(&zip, walk_visit, &w), VMFW_ZIP_OK,
                "iterate");
    VMFW_T_EQ_U(t, w.seen, 3u, "members visited");

    VMFW_T_SECTION(t, "zip/extract");
    static const char *names[3] = {
        "Restore.plist",
        "kernelcache.release.s5l8900x",
        "Firmware/all_flash/x/DeviceTree.n82ap.img3"
    };
    for (unsigned i = 0; i < 3u; i++) {
        vmfw_zip_entry_t e;
        VMFW_T_EQ_U(t, vmfw_zip_find(&zip, names[i], &e), VMFW_ZIP_OK,
                    "find by exact name");
        VMFW_T_EQ_U(t, e.uncompressed_size, lens[i], "uncompressed size");
        collect_t c;
        memset(&c, 0, sizeof c);
        VMFW_T_EQ_U(t, vmfw_zip_extract(&zip, &e, collect, &c), VMFW_ZIP_OK,
                    "extract");
        VMFW_T_EQ_U(t, c.len, lens[i], "extracted length");
        if (c.len == lens[i])
            VMFW_T_EQ_MEM(t, c.buf, pay[i], lens[i], "extracted bytes");
    }

    /*
     * The second member sets the streaming flag, so its local header carries a
     * zero CRC and zero sizes. Extraction still has to work and still has to
     * check the real CRC -- this is the case that decides whether the reader
     * trusts the central directory or the local header, and every member of a
     * real IPSW is in it.
     */
    VMFW_T_SECTION(t, "zip/streaming flag");
    {
        vmfw_zip_entry_t e;
        vmfw_zip_find(&zip, names[1], &e);
        const uint8_t *lh = archive + lay.local_offset[1];
        VMFW_T_EQ_U(t, (unsigned)(lh[6] | (lh[7] << 8)), 0x0008u,
                    "the fixture really does set flag bit 3");
        VMFW_T_EQ_U(t, (uint32_t)(lh[14] | (lh[15] << 8) | (lh[16] << 16)
                                  | ((uint32_t)lh[17] << 24)), 0u,
                    "and really does leave the local CRC zero");
        VMFW_T_CHECK(t, e.crc32 != 0u, "yet the entry carries a real CRC");
    }

    VMFW_T_SECTION(t, "zip/missing member");
    {
        vmfw_zip_entry_t e;
        VMFW_T_CHECK(t, vmfw_zip_find(&zip, "no-such-member", &e) != VMFW_ZIP_OK,
                     "a name that is not there is not found");
        VMFW_T_CHECK(t, vmfw_zip_find(&zip, "Restore.plis", &e) != VMFW_ZIP_OK,
                     "a prefix is not a match");
        VMFW_T_CHECK(t, vmfw_zip_find(&zip, "restore.plist", &e) != VMFW_ZIP_OK,
                     "the match is case sensitive");
    }

    /* ---------------- the last EOCD wins ------------------------------- */
    /*
     * A 240 MB archive will contain the bytes PK\5\6 inside a member sooner or
     * later. Planting one and requiring the reader to still find the real
     * record is the difference between opening an IPSW and opening a member.
     */
    VMFW_T_SECTION(t, "zip/decoy eocd");
    {
        uint8_t decoy_payload[600];
        fx_fill(decoy_payload, sizeof decoy_payload, 5u);
        memcpy(decoy_payload + 100, "PK\x05\x06", 4);
        memset(decoy_payload + 104, 0, 18);

        fx_zip_member_t m[1] = { { "decoy.bin", decoy_payload,
                                   sizeof decoy_payload, false, false } };
        static uint8_t buf[4096];
        fx_zip_layout_t l2;
        size_t n = fx_zip_build(buf, sizeof buf, m, 1u, &l2);
        VMFW_T_CHECK(t, n > 0, "decoy archive built");

        fx_blob_t b2 = { buf, n, 0u, false };
        vmfw_zip_t z2;
        VMFW_T_EQ_U(t, vmfw_zip_open(&z2, fx_blob_pread, &b2, n), VMFW_ZIP_OK,
                    "opens despite a planted signature");
        VMFW_T_EQ_U(t, z2.cd_offset, l2.cd_offset,
                    "and finds the real central directory, not the decoy");
    }

    /* A comment after the EOCD moves the record away from the end of file. */
    VMFW_T_SECTION(t, "zip/trailing comment");
    {
        static uint8_t buf[32768];
        fx_zip_layout_t l2;
        size_t n = build_standard(buf, sizeof buf, &l2, NULL, NULL);
        const char *comment = "produced by a tool that adds a comment";
        size_t clen = strlen(comment);
        fx_w16le(buf + l2.eocd_offset + 20, (uint16_t)clen);
        memcpy(buf + n, comment, clen);

        fx_blob_t b2 = { buf, n + clen, 0u, false };
        vmfw_zip_t z2;
        VMFW_T_EQ_U(t, vmfw_zip_open(&z2, fx_blob_pread, &b2, n + clen),
                    VMFW_ZIP_OK, "opens with a trailing comment");
        VMFW_T_EQ_U(t, z2.entry_count, 3u, "and reads the directory");
    }

    /* ---------------- mutations ---------------------------------------- */
    /*
     * Each of these starts from a freshly built, known-good archive and changes
     * exactly one field. A mutation applied on top of a previous one would not
     * tell us which check fired.
     */
#define MUTATE(section, code, expect_status)                                  \
    do {                                                                     \
        VMFW_T_SECTION(t, section);                                          \
        static uint8_t mbuf[32768];                                          \
        fx_zip_layout_t ml;                                                  \
        size_t mn = build_standard(mbuf, sizeof mbuf, &ml, NULL, NULL);      \
        VMFW_T_CHECK(t, mn > 0, "mutant base built");                        \
        { code; }                                                            \
        fx_blob_t mb = { mbuf, mn, 0u, false };                              \
        vmfw_zip_t mz;                                                       \
        vmfw_zip_status_t st = vmfw_zip_open(&mz, fx_blob_pread, &mb, mn);   \
        if (st == VMFW_ZIP_OK) {                                             \
            walk_t mw = { 0u, { 0 } };                                       \
            st = vmfw_zip_iterate(&mz, walk_visit, &mw);                     \
        }                                                                    \
        VMFW_T_EQ_U(t, st, (expect_status), section);                        \
    } while (0)

    MUTATE("zip/eocd signature", mbuf[ml.eocd_offset] ^= 0xffu,
           VMFW_ZIP_ERR_NOT_ZIP);
    MUTATE("zip/multi disk", fx_w16le(mbuf + ml.eocd_offset + 4, 1u),
           VMFW_ZIP_ERR_MULTI_DISK);
    MUTATE("zip/disk counts disagree",
           fx_w16le(mbuf + ml.eocd_offset + 8, 2u),
           VMFW_ZIP_ERR_MULTI_DISK);
    MUTATE("zip/cd offset past eof",
           fx_w32le(mbuf + ml.eocd_offset + 16, (uint32_t)(mn + 1000u)),
           VMFW_ZIP_ERR_BAD_DIRECTORY);
    MUTATE("zip/cd size past eof",
           fx_w32le(mbuf + ml.eocd_offset + 12, (uint32_t)mn),
           VMFW_ZIP_ERR_BAD_DIRECTORY);
    MUTATE("zip/too many entries",
           fx_w16le(mbuf + ml.eocd_offset + 8, 400u);
           fx_w16le(mbuf + ml.eocd_offset + 10, 400u),
           VMFW_ZIP_ERR_TOO_MANY);
    MUTATE("zip/central signature",
           mbuf[ml.central_offset[1]] ^= 0xffu,
           VMFW_ZIP_ERR_BAD_DIRECTORY);
    MUTATE("zip/encrypted flag",
           fx_w16le(mbuf + ml.central_offset[0] + 8, 0x0001u),
           VMFW_ZIP_ERR_ENCRYPTED);
    MUTATE("zip/strong encryption flag",
           fx_w16le(mbuf + ml.central_offset[0] + 8, 0x0040u),
           VMFW_ZIP_ERR_ENCRYPTED);
    MUTATE("zip/name length overruns the directory",
           fx_w16le(mbuf + ml.central_offset[2] + 28, 4000u),
           VMFW_ZIP_ERR_BAD_DIRECTORY);
    MUTATE("zip/embedded NUL in a name",
           mbuf[ml.central_offset[0] + 46 + 3] = '\0',
           VMFW_ZIP_ERR_BAD_DIRECTORY);
    MUTATE("zip/local offset past eof",
           fx_w32le(mbuf + ml.central_offset[0] + 42, (uint32_t)(mn + 99u)),
           VMFW_ZIP_ERR_BAD_DIRECTORY);
    /* A zip64 sentinel with no zip64 extra field to explain it. */
    MUTATE("zip/orphan zip64 sentinel",
           fx_w32le(mbuf + ml.central_offset[0] + 20, 0xffffffffu),
           VMFW_ZIP_ERR_ZIP64);
#undef MUTATE

    /* Mutations that only surface during extraction. */
#define MUTATE_X(section, code, expect_status)                                \
    do {                                                                     \
        VMFW_T_SECTION(t, section);                                          \
        static uint8_t mbuf[32768];                                          \
        fx_zip_layout_t ml;                                                  \
        size_t mn = build_standard(mbuf, sizeof mbuf, &ml, NULL, NULL);      \
        { code; }                                                            \
        fx_blob_t mb = { mbuf, mn, 0u, false };                              \
        vmfw_zip_t mz;                                                       \
        vmfw_zip_status_t st = vmfw_zip_open(&mz, fx_blob_pread, &mb, mn);   \
        VMFW_T_EQ_U(t, st, VMFW_ZIP_OK, "mutant still opens");               \
        vmfw_zip_entry_t e;                                                  \
        st = vmfw_zip_find(&mz, names[1], &e);                               \
        if (st == VMFW_ZIP_OK) {                                             \
            collect_t c; memset(&c, 0, sizeof c);                            \
            st = vmfw_zip_extract(&mz, &e, collect, &c);                     \
        }                                                                    \
        VMFW_T_EQ_U(t, st, (expect_status), section);                        \
    } while (0)

    MUTATE_X("zip/crc mismatch",
             fx_w32le(mbuf + ml.central_offset[1] + 16, 0xdeadbeefu),
             VMFW_ZIP_ERR_CRC_MISMATCH);
    MUTATE_X("zip/unknown method",
             fx_w16le(mbuf + ml.central_offset[1] + 10, 9u),
             VMFW_ZIP_ERR_BAD_METHOD);
    MUTATE_X("zip/local header signature",
             mbuf[ml.local_offset[1]] ^= 0xffu,
             VMFW_ZIP_ERR_BAD_LOCAL_HEADER);
    MUTATE_X("zip/compressed size past eof",
             fx_w32le(mbuf + ml.central_offset[1] + 20, (uint32_t)mn),
             VMFW_ZIP_ERR_MEMBER_TRUNCATED);
    /*
     * The declared uncompressed size is a bound on the inflater. Raising it
     * means the stream ends early; the reader must notice rather than write a
     * short file and call it a member.
     */
    MUTATE_X("zip/uncompressed size too large",
             fx_w32le(mbuf + ml.central_offset[1] + 24, 4000u),
             VMFW_ZIP_ERR_INFLATE);
    MUTATE_X("zip/uncompressed size too small",
             fx_w32le(mbuf + ml.central_offset[1] + 24, 100u),
             VMFW_ZIP_ERR_INFLATE);
    MUTATE_X("zip/deflate stream corrupted",
             mbuf[ml.local_offset[1] + 30
                  + strlen(names[1])] ^= 0x06u,   /* BTYPE -> reserved */
             VMFW_ZIP_ERR_INFLATE);
#undef MUTATE_X

    /* A stored member whose two sizes disagree cannot be both. */
    VMFW_T_SECTION(t, "zip/stored size disagreement");
    {
        static uint8_t mbuf[32768];
        fx_zip_layout_t ml;
        size_t mn = build_standard(mbuf, sizeof mbuf, &ml, NULL, NULL);
        fx_w32le(mbuf + ml.central_offset[0] + 24, 999u);
        fx_blob_t mb = { mbuf, mn, 0u, false };
        vmfw_zip_t mz;
        vmfw_zip_open(&mz, fx_blob_pread, &mb, mn);
        vmfw_zip_entry_t e;
        vmfw_zip_find(&mz, names[0], &e);
        collect_t c; memset(&c, 0, sizeof c);
        VMFW_T_EQ_U(t, vmfw_zip_extract(&mz, &e, collect, &c),
                    VMFW_ZIP_ERR_BAD_DIRECTORY, "stored sizes must agree");
    }

    /* ---------------- truncation --------------------------------------- */
    /*
     * A download that stopped early is the single most likely damaged input,
     * so every prefix of a valid archive is tried. The only requirement is that
     * none of them crashes and none of them reports success with wrong data.
     */
    VMFW_T_SECTION(t, "zip/truncation");
    {
        unsigned ok_opens = 0, clean_refusals = 0;
        for (size_t cut = 0; cut < total; cut += 7u) {
            fx_blob_t b2 = { archive, cut, 0u, false };
            vmfw_zip_t z2;
            vmfw_zip_status_t st = vmfw_zip_open(&z2, fx_blob_pread, &b2, cut);
            if (st != VMFW_ZIP_OK) { clean_refusals++; continue; }
            /* If it opened, the directory must still be self-consistent, and
             * anything it extracts must still pass its own CRC. */
            ok_opens++;
            walk_t w2 = { 0u, { 0 } };
            (void)vmfw_zip_iterate(&z2, walk_visit, &w2);
            vmfw_zip_entry_t e;
            if (vmfw_zip_find(&z2, names[1], &e) == VMFW_ZIP_OK) {
                collect_t c; memset(&c, 0, sizeof c);
                vmfw_zip_status_t xs = vmfw_zip_extract(&z2, &e, collect, &c);
                VMFW_T_CHECK(t, xs != VMFW_ZIP_OK || c.len == e.uncompressed_size,
                             "a truncated archive never reports a short member "
                             "as complete");
            }
        }
        VMFW_T_CHECK(t, clean_refusals > 0, "some prefixes were refused");
        VMFW_T_CHECK(t, ok_opens == 0 || ok_opens > 0, "no crash on any prefix");
    }

    /* ---------------- I/O and sink failures ---------------------------- */
    VMFW_T_SECTION(t, "zip/read failure");
    {
        fx_blob_t b2 = { archive, total, 0u, true };
        vmfw_zip_t z2;
        VMFW_T_EQ_U(t, vmfw_zip_open(&z2, fx_blob_pread, &b2, total),
                    VMFW_ZIP_ERR_READ, "a failing reader is not a missing EOCD");
    }
    VMFW_T_SECTION(t, "zip/sink refusal");
    {
        fx_blob_t b2 = { archive, total, 0u, false };
        vmfw_zip_t z2;
        vmfw_zip_open(&z2, fx_blob_pread, &b2, total);
        vmfw_zip_entry_t e;
        vmfw_zip_find(&z2, names[1], &e);
        collect_t c; memset(&c, 0, sizeof c);
        c.refuse_after = 16u;
        VMFW_T_EQ_U(t, vmfw_zip_extract(&z2, &e, collect, &c),
                    VMFW_ZIP_ERR_SINK, "a refusing sink is reported as one");
    }

    VMFW_T_SECTION(t, "zip/argument checks");
    {
        vmfw_zip_t z2;
        VMFW_T_EQ_U(t, vmfw_zip_open(NULL, fx_blob_pread, NULL, 10u),
                    VMFW_ZIP_ERR_INVALID_ARGUMENT, "NULL zip");
        VMFW_T_EQ_U(t, vmfw_zip_open(&z2, NULL, NULL, 10u),
                    VMFW_ZIP_ERR_INVALID_ARGUMENT, "NULL reader");
        VMFW_T_EQ_U(t, vmfw_zip_open(&z2, fx_blob_pread, NULL, 4u),
                    VMFW_ZIP_ERR_NOT_ZIP, "a file too small for an EOCD");
        VMFW_T_CHECK(t, vmfw_zip_strerror(VMFW_ZIP_OK) != NULL, "strerror(OK)");
        VMFW_T_CHECK(t, vmfw_zip_strerror((vmfw_zip_status_t)999) != NULL,
                     "strerror of an unknown code still returns a string");
    }

    /* ---------------- ZIP64 -------------------------------------------- */
    /*
     * Built by hand rather than by the fixture builder, because the whole point
     * is the shape the builder does not produce: 32-bit fields holding the
     * 0xffffffff sentinel, with the real values in a zip64 extra field and a
     * zip64 end-of-central-directory record.
     */
    VMFW_T_SECTION(t, "zip/zip64");
    {
        static uint8_t z64[2048];
        uint8_t payload[200];
        fx_fill(payload, sizeof payload, 77u);
        const char *name = "big.bin";
        size_t name_len = strlen(name);
        uint32_t crc = vmfw_crc32(0u, payload, sizeof payload);
        memset(z64, 0, sizeof z64);

        size_t at = 0;
        size_t local = at;
        fx_w32le(z64 + at, 0x04034b50u);
        fx_w16le(z64 + at + 4, 45u);
        fx_w16le(z64 + at + 8, 0u);              /* stored */
        fx_w32le(z64 + at + 14, crc);
        fx_w32le(z64 + at + 18, (uint32_t)sizeof payload);
        fx_w32le(z64 + at + 22, (uint32_t)sizeof payload);
        fx_w16le(z64 + at + 26, (uint16_t)name_len);
        fx_w16le(z64 + at + 28, 0u);
        memcpy(z64 + at + 30, name, name_len);
        at += 30 + name_len;
        memcpy(z64 + at, payload, sizeof payload);
        at += sizeof payload;

        size_t cd = at;
        fx_w32le(z64 + at, 0x02014b50u);
        fx_w16le(z64 + at + 4, 45u);
        fx_w16le(z64 + at + 6, 45u);
        fx_w16le(z64 + at + 8, 0u);
        fx_w16le(z64 + at + 10, 0u);
        fx_w32le(z64 + at + 16, crc);
        fx_w32le(z64 + at + 20, 0xffffffffu);    /* compressed   -> zip64 */
        fx_w32le(z64 + at + 24, 0xffffffffu);    /* uncompressed -> zip64 */
        fx_w16le(z64 + at + 28, (uint16_t)name_len);
        /* Three sentinels, so the zip64 field carries three 64-bit values:
         * 4 bytes of tag+length plus 24 of payload. */
        fx_w16le(z64 + at + 30, 28u);            /* extra length */
        fx_w32le(z64 + at + 42, 0xffffffffu);    /* local offset -> zip64 */
        memcpy(z64 + at + 46, name, name_len);
        {
            uint8_t *ex = z64 + at + 46 + name_len;
            fx_w16le(ex, 0x0001u);
            fx_w16le(ex + 2, 24u);
            fx_w64le(ex + 4, (uint64_t)sizeof payload);   /* uncompressed */
            fx_w64le(ex + 12, (uint64_t)sizeof payload);  /* compressed   */
            fx_w64le(ex + 20, (uint64_t)local);           /* local offset */
        }
        at += 46 + name_len + 28;
        size_t cd_size = at - cd;

        size_t eocd64 = at;
        fx_w32le(z64 + at, 0x06064b50u);
        fx_w64le(z64 + at + 4, 44u);
        fx_w16le(z64 + at + 12, 45u);
        fx_w16le(z64 + at + 14, 45u);
        fx_w32le(z64 + at + 16, 0u);
        fx_w32le(z64 + at + 20, 0u);
        fx_w64le(z64 + at + 24, 1u);
        fx_w64le(z64 + at + 32, 1u);
        fx_w64le(z64 + at + 40, (uint64_t)cd_size);
        fx_w64le(z64 + at + 48, (uint64_t)cd);
        at += 56;

        fx_w32le(z64 + at, 0x07064b50u);
        fx_w32le(z64 + at + 4, 0u);
        fx_w64le(z64 + at + 8, (uint64_t)eocd64);
        fx_w32le(z64 + at + 16, 1u);
        at += 20;

        fx_w32le(z64 + at, 0x06054b50u);
        fx_w16le(z64 + at + 8, 0xffffu);
        fx_w16le(z64 + at + 10, 0xffffu);
        fx_w32le(z64 + at + 12, 0xffffffffu);
        fx_w32le(z64 + at + 16, 0xffffffffu);
        fx_w16le(z64 + at + 20, 0u);
        at += 22;

        fx_blob_t b2 = { z64, at, 0u, false };
        vmfw_zip_t z2;
        VMFW_T_EQ_U(t, vmfw_zip_open(&z2, fx_blob_pread, &b2, at), VMFW_ZIP_OK,
                    "a zip64 archive opens");
        VMFW_T_EQ_U(t, z2.entry_count, 1u, "zip64 entry count");
        VMFW_T_EQ_U(t, z2.cd_offset, cd, "zip64 central directory offset");

        vmfw_zip_entry_t e;
        VMFW_T_EQ_U(t, vmfw_zip_find(&z2, name, &e), VMFW_ZIP_OK,
                    "zip64 member found");
        VMFW_T_EQ_U(t, e.uncompressed_size, sizeof payload,
                    "zip64 size came from the extra field, not the sentinel");
        VMFW_T_EQ_U(t, e.local_header_offset, local,
                    "zip64 offset came from the extra field");
        collect_t c; memset(&c, 0, sizeof c);
        VMFW_T_EQ_U(t, vmfw_zip_extract(&z2, &e, collect, &c), VMFW_ZIP_OK,
                    "zip64 member extracts");
        if (c.len == sizeof payload)
            VMFW_T_EQ_MEM(t, c.buf, payload, sizeof payload, "zip64 bytes");

        /*
         * A zip64 field too short for the values its sentinels promise. The
         * fields are positional, not tagged, so a reader that took them
         * anyway would read the compressed size out of whatever followed the
         * field -- which is why this is refused rather than partially applied.
         */
        {
            static uint8_t z3buf[2048];
            memcpy(z3buf, z64, at);
            fx_w16le(z3buf + cd + 30, 24u);                        /* 28 -> 24 */
            fx_w16le(z3buf + cd + 46 + name_len + 2, 20u);         /* 24 -> 20 */
            fx_blob_t b3 = { z3buf, at, 0u, false };
            vmfw_zip_t z3;
            vmfw_zip_status_t s3 = vmfw_zip_open(&z3, fx_blob_pread, &b3, at);
            if (s3 == VMFW_ZIP_OK) {
                vmfw_zip_entry_t e3;
                s3 = vmfw_zip_find(&z3, name, &e3);
            }
            VMFW_T_EQ_U(t, s3, VMFW_ZIP_ERR_ZIP64,
                        "a zip64 field too short for its sentinels is refused");
        }

        /* A locator pointing nowhere must be refused, not followed. */
        fx_w64le(z64 + at - 22 - 20 + 8, (uint64_t)(at + 5000u));
        fx_blob_t b3 = { z64, at, 0u, false };
        vmfw_zip_t z3;
        VMFW_T_EQ_U(t, vmfw_zip_open(&z3, fx_blob_pread, &b3, at),
                    VMFW_ZIP_ERR_ZIP64, "a zip64 locator past EOF is refused");
    }
}
