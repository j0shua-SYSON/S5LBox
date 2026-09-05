/*
 * S5LBox — the whole import, on a synthetic IPSW.
 *
 * The suite below builds an archive with the same shape as a real one --
 * Restore.plist naming a platform and a system restore image, an IMG3
 * kernelcache holding an encrypted complzss payload, an IMG3 device tree for a
 * named board, and an encrcdsa-wrapped UDIF -- and then requires the pipeline
 * to behave the same way at each of the four points where a real user's run can
 * end:
 *
 *   no keys        every encrypted artefact reports NEEDS A KEY, by name, and
 *                  the RUN still succeeds. Not having a key is a state of the
 *                  world, not a malfunction, and a UI that showed it as an
 *                  error would be lying to the majority of users.
 *   right keys     the artefacts come out and are hashed.
 *   wrong bytes    a build we hold a reference hash for that does not match is
 *                  MISMATCH, never VERIFIED.
 *   unknown build  no reference hash, so EXTRACTED, and the report says why
 *                  rather than claiming a verification it did not perform.
 *
 * Nothing here is Apple data. The manifest is a synthetic one and every payload
 * is generated.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareTest.h"
#include "VMFirmwareFixtures.h"
#include "VMFirmwareImport.h"

#include <stdio.h>
#include <stdlib.h>

#define KERNEL_PLAIN_LEN 2045u    /* not a multiple of 16, on purpose */
#define DTREE_PLAIN_LEN  1024u
#define ROOT_SECTORS       12u
#define ROOT_PLAIN_LEN   (ROOT_SECTORS * 512u)

static const uint8_t k_kernel_key[16] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10
};
static const uint8_t k_kernel_iv[16] = {
    0xf0,0xe1,0xd2,0xc3,0xb4,0xa5,0x96,0x87,
    0x78,0x69,0x5a,0x4b,0x3c,0x2d,0x1e,0x0f
};
static const uint8_t k_dtree_key[16] = {
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
    0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};
static const uint8_t k_dtree_iv[16] = {
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
    0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
};
static const uint8_t k_root_key[VMFW_DMG_KEY_BLOB_SIZE] = {
    0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,0x30,
    0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,
    0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,
    0x41,0x42,0x43,0x44
};

static const char *k_kernel_key_hex = "0102030405060708090a0b0c0d0e0f10";
static const char *k_kernel_iv_hex  = "f0e1d2c3b4a5968778695a4b3c2d1e0f";
static const char *k_dtree_key_hex  = "1112131415161718191a1b1c1d1e1f20";
static const char *k_dtree_iv_hex   = "00112233445566778899aabbccddeeff";
static const char *k_root_key_hex   =
    "2122232425262728292a2b2c2d2e2f30"
    "3132333435363738393a3b3c3d3e3f40"
    "41424344";

/* ------------------------------------------------------------------------ */
/* An in-memory implementation of the file interface                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    char     name[64];
    uint8_t  data[262144];
    size_t   len;
    bool     in_use;
    bool     closed;
    bool     kept;
    unsigned refuse_write_after;
} memfile_t;

typedef struct {
    memfile_t f[8];
    unsigned  count;
    bool      refuse_part;      /* refuse to open the .part intermediate    */
    bool      refuse_outputs;   /* refuse to open any real artefact         */
    unsigned  refuse_write_after;
} memfs_t;

static void *mem_open(void *ctx, const char *name) {
    memfs_t *fs = (memfs_t *)ctx;
    size_t n = strlen(name);
    bool is_part = (n > 5 && strcmp(name + n - 5, ".part") == 0);
    if (is_part && fs->refuse_part) return NULL;
    if (!is_part && fs->refuse_outputs) return NULL;
    if (fs->count >= 8u) return NULL;

    memfile_t *f = &fs->f[fs->count++];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->in_use = true;
    f->refuse_write_after = fs->refuse_write_after;
    return f;
}

static bool mem_write(void *ctx, void *h, const uint8_t *d, size_t n) {
    (void)ctx;
    memfile_t *f = (memfile_t *)h;
    if (f->refuse_write_after && f->len + n > f->refuse_write_after)
        return false;
    if (f->len + n > sizeof f->data) return false;
    memcpy(f->data + f->len, d, n);
    f->len += n;
    return true;
}

static size_t mem_pread(void *ctx, void *h, uint64_t off, uint8_t *b, size_t n) {
    (void)ctx;
    memfile_t *f = (memfile_t *)h;
    if (off > f->len) return 0;
    size_t avail = f->len - (size_t)off;
    size_t take = n < avail ? n : avail;
    memcpy(b, f->data + off, take);
    return take;
}

static void mem_close(void *ctx, void *h, bool keep) {
    (void)ctx;
    memfile_t *f = (memfile_t *)h;
    f->in_use = false;
    f->closed = true;
    f->kept = keep;
}

static const memfile_t *mem_find(const memfs_t *fs, const char *name) {
    for (unsigned i = 0; i < fs->count; i++)
        if (strcmp(fs->f[i].name, name) == 0 && fs->f[i].kept) return &fs->f[i];
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* The synthetic IPSW                                                        */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t archive[1u << 20];
    size_t  len;
    uint8_t kernel_plain[KERNEL_PLAIN_LEN];
    uint8_t dtree_plain[DTREE_PLAIN_LEN];
    uint8_t root_plain[ROOT_PLAIN_LEN];
} ipsw_t;

/* A minimal single-partition UDIF: one RAW chunk and an END. Enough to drive
 * the whole disk-image path without repeating the chunk-type matrix, which the
 * dmg suite already covers. */
static size_t build_small_udif(uint8_t *dst, size_t cap, const uint8_t *payload,
                               size_t payload_len) {
    if (payload_len % 512u) return 0;
    uint64_t sectors = payload_len / 512u;

    uint8_t mish[0xCC + 80];
    memset(mish, 0, sizeof mish);
    memcpy(mish, "mish", 4);
    fx_w32be(mish + 4, 1u);
    fx_w64be(mish + 8, 0u);
    fx_w64be(mish + 0x10, sectors);
    fx_w64be(mish + 0x18, 0u);
    fx_w32be(mish + 0xC8, 2u);
    fx_w32be(mish + 0xCC, 0x00000001u);              /* RAW */
    fx_w64be(mish + 0xCC + 8, 0u);
    fx_w64be(mish + 0xCC + 0x10, sectors);
    fx_w64be(mish + 0xCC + 0x18, 0u);
    fx_w64be(mish + 0xCC + 0x20, (uint64_t)payload_len);
    fx_w32be(mish + 0xCC + 40, 0xFFFFFFFFu);         /* END */
    fx_w64be(mish + 0xCC + 40 + 8, sectors);
    size_t mish_len = 0xCCu + 80u;

    static char b64[8192];
    if (!fx_base64(b64, sizeof b64, mish, mish_len, 64u)) return 0;

    static char xml[16384];
    int n = snprintf(xml, sizeof xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "\t<key>resource-fork</key>\n\t<dict>\n\t\t<key>blkx</key>\n"
        "\t\t<array>\n\t\t\t<dict>\n"
        "\t\t\t\t<key>Data</key>\n\t\t\t\t<data>\n\t\t\t\t%s\n\t\t\t\t</data>\n"
        "\t\t\t\t<key>Name</key>\n"
        "\t\t\t\t<string>Mac_OS_X (Apple_HFSX : 1)</string>\n"
        "\t\t\t</dict>\n\t\t</array>\n\t</dict>\n</dict>\n</plist>\n", b64);
    if (n <= 0 || (size_t)n >= sizeof xml) return 0;

    if (payload_len + (size_t)n + 512u > cap) return 0;
    memcpy(dst, payload, payload_len);
    memcpy(dst + payload_len, xml, (size_t)n);

    uint8_t *k = dst + payload_len + (size_t)n;
    memset(k, 0, 512);
    memcpy(k, "koly", 4);
    fx_w32be(k + 4, 4u);
    fx_w64be(k + 0x18, 0u);
    fx_w64be(k + 0x20, (uint64_t)payload_len);
    fx_w64be(k + 0xD8, (uint64_t)payload_len);
    fx_w64be(k + 0xE0, (uint64_t)n);
    fx_w64be(k + 0x1EC, sectors);
    return payload_len + (size_t)n + 512u;
}

/*
 * Ways the kernelcache member can be wrong. Each exists because a mutation of
 * the corresponding check survived a suite that only ever fed the importer
 * well-formed containers -- which is the failure mode of every fixture that is
 * built by the same understanding as the code.
 */
typedef enum {
    KERNEL_GOOD = 0,
    KERNEL_NOT_IMG3,        /* a valid container with its magic flipped     */
    KERNEL_LZSS_OVERLONG,   /* complzss declaring more input than it has    */
    KERNEL_LZSS_OVERPRODUCE,/* a stream encoding more than it declares      */
    KERNEL_LZSS_SHORT,      /* complete input with too little output        */
    KERNEL_LZSS_BAD_ADLER,  /* exact output length but corrupted checksum   */
    KERNEL_MALFORMED_KBAG   /* a key bag that is present but unparseable    */
} kernel_mode_t;

typedef struct {
    const char   *product_type;
    const char   *build;
    const char   *platform;
    const char   *board;
    bool          omit_manifest;
    bool          omit_kernel;
    bool          omit_root_name;
    bool          encrypt;
    kernel_mode_t kernel_mode;
} ipsw_spec_t;

static bool build_ipsw(ipsw_t *ip, const ipsw_spec_t *spec) {
    memset(ip, 0, sizeof *ip);
    fx_fill(ip->kernel_plain, sizeof ip->kernel_plain, 101u);
    fx_fill(ip->dtree_plain, sizeof ip->dtree_plain, 202u);
    fx_fill(ip->root_plain, sizeof ip->root_plain, 303u);

    static char manifest[2048];
    int mn = snprintf(manifest, sizeof manifest,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "\t<key>DeviceClass</key>\n\t<string>iPhone</string>\n"
        "\t<key>DeviceMap</key>\n\t<array>\n\t\t<dict>\n"
        "\t\t\t<key>BDID</key>\n\t\t\t<integer>4</integer>\n"
        "\t\t\t<key>BoardConfig</key>\n\t\t\t<string>%s</string>\n"
        "\t\t\t<key>Platform</key>\n\t\t\t<string>%s</string>\n"
        "\t\t</dict>\n\t</array>\n"
        "\t<key>KernelCachesByPlatform</key>\n\t<dict>\n\t\t<key>%s</key>\n"
        "\t\t<dict>\n\t\t\t<key>Release</key>\n"
        "\t\t\t<string>kernelcache.release.%s</string>\n\t\t</dict>\n\t</dict>\n"
        "\t<key>ProductBuildVersion</key>\n\t<string>%s</string>\n"
        "\t<key>ProductType</key>\n\t<string>%s</string>\n"
        "\t<key>ProductVersion</key>\n\t<string>3.1.3</string>\n"
        "\t<key>SystemRestoreImages</key>\n\t<dict>\n\t\t<key>%s</key>\n"
        "\t\t<string>018-1234-001.dmg</string>\n\t</dict>\n"
        "</dict>\n</plist>\n",
        spec->board, spec->platform, spec->platform, spec->platform,
        spec->build, spec->product_type,
        spec->omit_root_name ? "Restore" : "User");
    if (mn <= 0 || (size_t)mn >= sizeof manifest) return false;

    /* kernelcache: complzss, then IMG3, optionally encrypted. */
    static uint8_t comp[8192];
    size_t comp_len = fx_lzss_literal(comp, sizeof comp, ip->kernel_plain,
                                      sizeof ip->kernel_plain);
    if (!comp_len) return false;

    if (spec->kernel_mode == KERNEL_LZSS_OVERLONG) {
        /* Claim far more compressed input than the payload actually carries.
         * Unchecked, the decompressor reads past the end of the member. */
        fx_w32be(comp + 16, (uint32_t)(comp_len * 4u));
    } else if (spec->kernel_mode == KERNEL_LZSS_OVERPRODUCE) {
        /* Claim a much smaller output than the stream encodes, so a
         * decompressor that trusted the stream rather than its own capacity
         * would run off the end of the buffer it was given. */
        fx_w32be(comp + 12, 64u);
    } else if (spec->kernel_mode == KERNEL_LZSS_SHORT) {
        fx_w32be(comp + 12, (uint32_t)sizeof ip->kernel_plain + 42u);
    } else if (spec->kernel_mode == KERNEL_LZSS_BAD_ADLER) {
        comp[8] ^= 1u;
    }

    static uint8_t kc_img3[16384];
    fx_img3_spec_t ks;
    memset(&ks, 0, sizeof ks);
    ks.ident = 0x6b726e6cu;
    ks.payload = comp;
    ks.payload_len = comp_len;
    ks.encrypt = spec->encrypt;
    ks.key = k_kernel_key;
    ks.iv = k_kernel_iv;
    ks.malformed_kbag = (spec->kernel_mode == KERNEL_MALFORMED_KBAG);
    size_t kc_len = fx_img3_build(kc_img3, sizeof kc_img3, &ks);
    if (!kc_len) return false;

    if (spec->kernel_mode == KERNEL_NOT_IMG3) {
        /* Everything about this member is a well-formed IMG3 except the four
         * bytes that say so. A reader that skipped the magic check would parse
         * it happily all the way to a decrypted kernel. */
        kc_img3[0] ^= 0xffu;
    }

    static uint8_t dt_img3[8192];
    fx_img3_spec_t ds;
    memset(&ds, 0, sizeof ds);
    ds.ident = 0x64747265u;
    ds.payload = ip->dtree_plain;
    ds.payload_len = sizeof ip->dtree_plain;
    ds.encrypt = spec->encrypt;
    ds.key = k_dtree_key;
    ds.iv = k_dtree_iv;
    size_t dt_len = fx_img3_build(dt_img3, sizeof dt_img3, &ds);
    if (!dt_len) return false;

    static uint8_t udif[131072];
    size_t udif_len = build_small_udif(udif, sizeof udif, ip->root_plain,
                                       sizeof ip->root_plain);
    if (!udif_len) return false;

    static uint8_t dmg[262144 + 0x1e000u];
    size_t dmg_len;
    if (spec->encrypt) {
        dmg_len = fx_encrcdsa_wrap(dmg, sizeof dmg, udif, udif_len,
                                   k_root_key, 4096u);
        if (!dmg_len) return false;
    } else {
        memcpy(dmg, udif, udif_len);
        dmg_len = udif_len;
    }

    static char kc_name[64];
    snprintf(kc_name, sizeof kc_name, "kernelcache.release.%s", spec->platform);
    static char dt_name[128];
    snprintf(dt_name, sizeof dt_name,
             "Firmware/all_flash/all_flash.%s.production/DeviceTree.%s.img3",
             spec->board, spec->board);

    fx_zip_member_t members[4];
    unsigned n = 0;
    if (!spec->omit_manifest) {
        fx_zip_member_t m = { "Restore.plist", (const uint8_t *)manifest,
                              (size_t)mn, true, true };
        members[n++] = m;
    }
    if (!spec->omit_kernel) {
        fx_zip_member_t m = { kc_name, kc_img3, kc_len, true, true };
        members[n++] = m;
    }
    {
        fx_zip_member_t m = { dt_name, dt_img3, dt_len, true, true };
        members[n++] = m;
    }
    {
        fx_zip_member_t m = { "018-1234-001.dmg", dmg, dmg_len, true, true };
        members[n++] = m;
    }

    ip->len = fx_zip_build(ip->archive, sizeof ip->archive, members, n, NULL);
    return ip->len > 0;
}

static const ipsw_spec_t k_reference_spec = {
    "iPhone1,2", "7E18", "s5l8900x", "n82ap", false, false, false, true,
    KERNEL_GOOD
};

/* ------------------------------------------------------------------------ */

typedef struct { unsigned calls; unsigned cancel_at; } cancel_ctx_t;

static bool cancel_after(void *ctx) {
    cancel_ctx_t *c = (cancel_ctx_t *)ctx;
    return ++c->calls > c->cancel_at;
}

static unsigned g_progress_calls;
static void count_progress(void *ctx, vm_fw_artefact_t which,
                           vm_fw_stage_t stage, uint64_t done, uint64_t total) {
    (void)ctx; (void)which; (void)stage; (void)done; (void)total;
    g_progress_calls++;
}

void vmfw_test_import(vmfw_test_t *t) {
    static ipsw_t ip;
    static memfs_t fs;

    /* ---------------- hex parsing -------------------------------------- */
    VMFW_T_SECTION(t, "import/hex");
    {
        uint8_t out[16];
        VMFW_T_EQ_U(t, vm_fw_parse_hex("000102030405060708090a0b0c0d0e0f",
                                       out, 16u), VM_FW_OK, "plain hex");
        VMFW_T_EQ_U(t, out[15], 0x0fu, "last byte");
        VMFW_T_EQ_U(t, vm_fw_parse_hex("00 01 02 03 04 05 06 07\n"
                                       "08 09 0A 0B 0C 0D 0E 0F", out, 16u),
                    VM_FW_OK, "whitespace anywhere is ignored");
        VMFW_T_EQ_U(t, out[10], 0x0au, "and case does not matter");
        /*
         * A stray character must be refused, not skipped. A key pasted from a
         * web page with a zero-width space in it would otherwise still be the
         * right length and would decrypt into garbage blamed on the IPSW.
         */
        VMFW_T_EQ_U(t, vm_fw_parse_hex("000102030405060708090a0b0c0d0e0g",
                                       out, 16u),
                    VM_FW_ERR_KEY_NOT_HEX, "a non-hex character is refused");
        VMFW_T_EQ_U(t, vm_fw_parse_hex("0x00102030405060708090a0b0c0d0e0f",
                                       out, 16u),
                    VM_FW_ERR_KEY_NOT_HEX, "an 0x prefix is not accepted");
        VMFW_T_EQ_U(t, vm_fw_parse_hex("000102030405060708090a0b0c0d0e",
                                       out, 16u),
                    VM_FW_ERR_KEY_WRONG_LENGTH, "too short");
        VMFW_T_EQ_U(t, vm_fw_parse_hex("000102030405060708090a0b0c0d0e0f00",
                                       out, 16u),
                    VM_FW_ERR_KEY_WRONG_LENGTH, "too long");
        VMFW_T_EQ_U(t, vm_fw_parse_hex("000102030405060708090a0b0c0d0e0f0",
                                       out, 16u),
                    VM_FW_ERR_KEY_WRONG_LENGTH, "an odd digit count");
        VMFW_T_EQ_U(t, vm_fw_parse_hex("", out, 16u),
                    VM_FW_ERR_KEY_WRONG_LENGTH, "empty");
        VMFW_T_EQ_U(t, vm_fw_parse_hex(NULL, out, 16u),
                    VM_FW_ERR_INVALID_ARGUMENT, "NULL text");

        vm_fw_keys_t keys;
        vm_fw_keys_clear(&keys);
        VMFW_T_EQ_U(t, vm_fw_keys_set_img3(&keys, VM_FW_KERNEL,
                                           k_kernel_key_hex, k_kernel_iv_hex),
                    VM_FW_OK, "a 128-bit key and IV");
        VMFW_T_EQ_U(t, keys.kernel.key_bits, 128u, "key size inferred");
        VMFW_T_CHECK(t, keys.kernel.present, "and recorded as present");

        vm_fw_keys_clear(&keys);
        VMFW_T_EQ_U(t, vm_fw_keys_set_img3(&keys, VM_FW_KERNEL,
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
            k_kernel_iv_hex), VM_FW_OK, "a 256-bit key");
        VMFW_T_EQ_U(t, keys.kernel.key_bits, 256u, "256 inferred");

        /* A bad IV must not leave a half-set key behind for a later run. */
        vm_fw_keys_clear(&keys);
        VMFW_T_EQ_U(t, vm_fw_keys_set_img3(&keys, VM_FW_KERNEL,
                                           k_kernel_key_hex, "abcd"),
                    VM_FW_ERR_KEY_WRONG_LENGTH, "a short IV is refused");
        VMFW_T_CHECK(t, !keys.kernel.present,
                     "and the key slot is left unset, not half-set");

        VMFW_T_EQ_U(t, vm_fw_keys_set_img3(&keys, VM_FW_ROOT_FILESYSTEM,
                                           k_kernel_key_hex, k_kernel_iv_hex),
                    VM_FW_ERR_INVALID_ARGUMENT,
                    "the root filesystem does not take an IMG3 key");
        VMFW_T_EQ_U(t, vm_fw_keys_set_root(&keys, k_root_key_hex), VM_FW_OK,
                    "a 36-byte root key");
        VMFW_T_CHECK(t, keys.root_present, "recorded");
        VMFW_T_EQ_U(t, vm_fw_keys_set_root(&keys, k_kernel_key_hex),
                    VM_FW_ERR_KEY_WRONG_LENGTH,
                    "a 16-byte value is not a root filesystem key");
    }

    /* ---------------- the fixture -------------------------------------- */
    VMFW_T_SECTION(t, "import/fixture");
    VMFW_T_CHECK(t, build_ipsw(&ip, &k_reference_spec),
                 "the synthetic IPSW was built");

    /* ---------------- no keys ------------------------------------------ */
    /*
     * The single most common run: a user picks their IPSW and has pasted
     * nothing. Everything that can be done without a key must be done, and the
     * three that cannot must say so specifically.
     */
    VMFW_T_SECTION(t, "import/no keys");
    {
        memset(&fs, 0, sizeof fs);
        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        static fx_blob_t blob;
        blob.data = ip.archive; blob.len = ip.len; blob.reads = 0;
        blob.fail_next = false;
        imp.pread_ctx = &blob;
        imp.size = ip.len;
        imp.files = &files;
        imp.keys = NULL;

        vm_fw_report_t rep;
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, &rep), VM_FW_OK,
                    "a run with no keys is not a failed run");
        VMFW_T_CHECK(t, rep.manifest_read, "the manifest was read");
        VMFW_T_EQ_STR(t, rep.product_type, "iPhone1,2", "product type");
        VMFW_T_EQ_STR(t, rep.build, "7E18", "build");
        VMFW_T_EQ_STR(t, rep.board, "n82ap", "board");
        VMFW_T_EQ_STR(t, rep.platform, "s5l8900x", "platform");
        VMFW_T_EQ_STR(t, rep.product_version, "3.1.3", "version");
        VMFW_T_CHECK(t, rep.reference_build,
                     "this build is one we hold reference hashes for");

        for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
            VMFW_T_EQ_U(t, rep.artefacts[i].state, VM_FW_STATE_NEEDS_KEY,
                        "every artefact needs a key");
            VMFW_T_CHECK(t, rep.artefacts[i].awaiting_key,
                         "and says so through awaiting_key, which is what the "
                         "UI keys its paste affordance off");
            VMFW_T_EQ_U(t, rep.artefacts[i].reason, VM_FW_ERR_KEY_REQUIRED,
                        "with a named reason");
            VMFW_T_CHECK(t, rep.artefacts[i].member[0] != '\0',
                         "and still names the member it found");
            VMFW_T_CHECK(t, rep.artefacts[i].detail[0] != '\0',
                         "and still explains itself");
            VMFW_T_CHECK(t, rep.artefacts[i].encrypted, "marked encrypted");
        }
        VMFW_T_CHECK(t, rep.artefacts[VM_FW_KERNEL].is_img3, "kernel is IMG3");
        VMFW_T_EQ_STR(t, rep.artefacts[VM_FW_KERNEL].ident, "krnl",
                      "kernel ident");
        VMFW_T_EQ_STR(t, rep.artefacts[VM_FW_DEVICE_TREE].ident, "dtre",
                      "device tree ident");
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].key_bits, 128u,
                    "kernel key size reported from the KBAG");

        /* Nothing may have been produced, and in particular the 208 MB-class
         * intermediate must not have been written to discover a key was
         * needed. */
        VMFW_T_EQ_U(t, fs.count, 0u,
                    "no file was opened, including the .part intermediate");
    }

    /* ---------------- with keys ---------------------------------------- */
    VMFW_T_SECTION(t, "import/with keys");
    {
        memset(&fs, 0, sizeof fs);
        vm_fw_keys_t keys;
        vm_fw_keys_clear(&keys);
        vm_fw_keys_set_img3(&keys, VM_FW_KERNEL, k_kernel_key_hex,
                            k_kernel_iv_hex);
        vm_fw_keys_set_img3(&keys, VM_FW_DEVICE_TREE, k_dtree_key_hex,
                            k_dtree_iv_hex);
        vm_fw_keys_set_root(&keys, k_root_key_hex);

        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = ip.archive; blob.len = ip.len; blob.fail_next = false;

        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = ip.len;
        imp.files = &files;
        imp.keys = &keys;
        g_progress_calls = 0;
        imp.progress = count_progress;

        vm_fw_report_t rep;
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, &rep), VM_FW_OK, "run");
        VMFW_T_CHECK(t, g_progress_calls > 0, "progress was reported");

        /*
         * The synthetic payloads are not the real 7E18 artefacts, and the
         * manifest claims to BE 7E18 -- so the reference hashes must reject
         * them. A suite where this came out VERIFIED would be a suite whose
         * verification does nothing.
         */
        for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
            VMFW_T_EQ_U(t, rep.artefacts[i].state, VM_FW_STATE_MISMATCH,
                        "produced, and correctly not the known-good bytes");
            VMFW_T_CHECK(t, rep.artefacts[i].sha256_valid, "hashed");
            VMFW_T_CHECK(t, rep.artefacts[i].reference_known,
                         "a reference existed to compare against");
            VMFW_T_CHECK(t, !rep.artefacts[i].matches_reference, "and it did not match");
            VMFW_T_CHECK(t, !rep.artefacts[i].awaiting_key,
                         "and it is not waiting on a key");
        }

        /* The bytes themselves must be exactly the plaintexts we encrypted. */
        const memfile_t *k = mem_find(&fs, "kernel.macho");
        VMFW_T_CHECK(t, k != NULL, "kernel.macho was produced and kept");
        if (k) {
            VMFW_T_EQ_U(t, k->len, KERNEL_PLAIN_LEN, "kernel length");
            if (k->len == KERNEL_PLAIN_LEN)
                VMFW_T_EQ_MEM(t, k->data, ip.kernel_plain, KERNEL_PLAIN_LEN,
                              "decrypt + decompress round-tripped exactly");
        }
        const memfile_t *d = mem_find(&fs, "devicetree.bin");
        VMFW_T_CHECK(t, d != NULL, "devicetree.bin was produced");
        if (d) {
            VMFW_T_EQ_U(t, d->len, DTREE_PLAIN_LEN, "device tree length");
            if (d->len == DTREE_PLAIN_LEN)
                VMFW_T_EQ_MEM(t, d->data, ip.dtree_plain, DTREE_PLAIN_LEN,
                              "device tree decrypted exactly");
        }
        const memfile_t *rf = mem_find(&fs, "rootfs.img");
        VMFW_T_CHECK(t, rf != NULL, "rootfs.img was produced");
        if (rf) {
            VMFW_T_EQ_U(t, rf->len, ROOT_PLAIN_LEN, "rootfs length");
            if (rf->len == ROOT_PLAIN_LEN)
                VMFW_T_EQ_MEM(t, rf->data, ip.root_plain, ROOT_PLAIN_LEN,
                              "decrypt + expand round-tripped exactly");
        }

        /* The intermediate must be gone. */
        bool part_kept = false;
        for (unsigned i = 0; i < fs.count; i++) {
            size_t n = strlen(fs.f[i].name);
            if (n > 5 && strcmp(fs.f[i].name + n - 5, ".part") == 0) {
                VMFW_T_CHECK(t, fs.f[i].closed, "the intermediate was closed");
                if (fs.f[i].kept) part_kept = true;
            }
        }
        VMFW_T_CHECK(t, !part_kept,
                     "and it was closed with keep=false so the caller deletes it");
    }

    /* ---------------- wrong keys --------------------------------------- */
    VMFW_T_SECTION(t, "import/wrong keys");
    {
        memset(&fs, 0, sizeof fs);
        vm_fw_keys_t keys;
        vm_fw_keys_clear(&keys);
        /* Right shape, wrong values -- which is what pasting the iPhone1,1
         * page's keys for an iPhone1,2 IPSW actually looks like. */
        vm_fw_keys_set_img3(&keys, VM_FW_KERNEL,
                            "ffeeddccbbaa99887766554433221100",
                            k_kernel_iv_hex);
        vm_fw_keys_set_img3(&keys, VM_FW_DEVICE_TREE,
                            "ffeeddccbbaa99887766554433221100",
                            k_dtree_iv_hex);
        vm_fw_keys_set_root(&keys,
            "ffeeddccbbaa998877665544332211000011223344556677"
            "8899aabbccddeeff01020304");

        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = ip.archive; blob.len = ip.len; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = ip.len;
        imp.files = &files;
        imp.keys = &keys;

        vm_fw_report_t rep;
        vm_fw_import_run(&imp, &rep);

        /* The kernel's wrong key shows up as "not a complzss image", which is
         * the earliest point at which anything disagrees. */
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].state, VM_FW_STATE_FAILED,
                    "a wrong kernel key fails");
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].reason,
                    VM_FW_ERR_NOT_COMPRESSED,
                    "and says the decrypted payload is not a compressed kernel");
        /* The device tree has no inner format to disagree with, so the only
         * thing that can catch it is the hash -- and it must. */
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_DEVICE_TREE].state,
                    VM_FW_STATE_MISMATCH,
                    "a wrong device tree key is caught by the hash");
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_ROOT_FILESYSTEM].state,
                    VM_FW_STATE_FAILED, "a wrong root key fails");
        VMFW_T_CHECK(t, rep.artefacts[VM_FW_ROOT_FILESYSTEM].detail[0] != '\0',
                     "and explains that the key may be another device's");
        VMFW_T_CHECK(t, mem_find(&fs, "rootfs.img") == NULL,
                     "and no rootfs.img was left behind");
    }

    /* ---------------- containers that are wrong ------------------------ */
    /*
     * Every case below was added because a mutation of the corresponding check
     * SURVIVED a suite that only fed the importer well-formed containers. That
     * is the failure mode of a fixture built by the same understanding as the
     * code it tests, and the only cure is to build things that are wrong.
     */
    VMFW_T_SECTION(t, "import/malformed containers");
    {
        struct {
            kernel_mode_t mode;
            vm_fw_state_t state;
            vm_fw_status_t reason;
            const char *what;
        } cases[] = {
            { KERNEL_NOT_IMG3, VM_FW_STATE_FAILED, VM_FW_ERR_NOT_IMG3,
              "a container whose magic is wrong is not opened anyway" },
            { KERNEL_LZSS_OVERLONG, VM_FW_STATE_FAILED,
              VM_FW_ERR_NOT_COMPRESSED,
              "a complzss header claiming more input than the member holds" },
            { KERNEL_LZSS_OVERPRODUCE, VM_FW_STATE_FAILED,
              VM_FW_ERR_DECOMPRESS_FAILED,
              "a bounded prefix with the wrong checksum is refused" },
            { KERNEL_LZSS_SHORT, VM_FW_STATE_FAILED,
              VM_FW_ERR_DECOMPRESS_FAILED,
              "a short stream is refused without zero-filling" },
            { KERNEL_LZSS_BAD_ADLER, VM_FW_STATE_FAILED,
              VM_FW_ERR_DECOMPRESS_FAILED,
              "the checksum is checked even when output length matches" },
        };

        for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
            static ipsw_t bad;
            ipsw_spec_t spec = k_reference_spec;
            spec.kernel_mode = cases[ci].mode;
            if (!build_ipsw(&bad, &spec)) {
                VMFW_T_CHECK(t, false, "fixture build failed");
                continue;
            }

            memset(&fs, 0, sizeof fs);
            vm_fw_keys_t keys;
            vm_fw_keys_clear(&keys);
            vm_fw_keys_set_img3(&keys, VM_FW_KERNEL, k_kernel_key_hex,
                                k_kernel_iv_hex);
            vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close,
                                    &fs };
            static fx_blob_t blob;
            blob.data = bad.archive; blob.len = bad.len; blob.fail_next = false;
            vm_fw_import_t imp;
            memset(&imp, 0, sizeof imp);
            imp.pread = fx_blob_pread;
            imp.pread_ctx = &blob;
            imp.size = bad.len;
            imp.files = &files;
            imp.keys = &keys;

            vm_fw_report_t rep;
            vm_fw_import_run(&imp, &rep);
            VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].state,
                        cases[ci].state, cases[ci].what);
            if (cases[ci].reason != VM_FW_OK)
                VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].reason,
                            cases[ci].reason, "with the named reason");
            /*
             * Whatever went wrong, the bytes must never be accepted as the
             * known-good kernel.
             */
            VMFW_T_CHECK(t, !rep.artefacts[VM_FW_KERNEL].matches_reference,
                         "and never claimed to be the known-good kernel");
            for (unsigned fi = 0; fi < fs.count; fi++)
                VMFW_T_CHECK(t, strcmp(fs.f[fi].name, "kernel.macho") != 0,
                             "invalid kernel never opens an output file");
        }
    }

    /*
     * A key bag that is present but cannot be parsed. img3.c reports that
     * separately from "absent" on purpose: guessing "not encrypted" would copy
     * ciphertext into the output and report success, and the file would look
     * like a kernel right up until the emulator refused it.
     */
    VMFW_T_SECTION(t, "import/malformed key bag");
    {
        static ipsw_t bad;
        ipsw_spec_t spec = k_reference_spec;
        spec.kernel_mode = KERNEL_MALFORMED_KBAG;
        VMFW_T_CHECK(t, build_ipsw(&bad, &spec), "built");

        memset(&fs, 0, sizeof fs);
        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = bad.archive; blob.len = bad.len; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = bad.len;
        imp.files = &files;
        imp.keys = NULL;

        vm_fw_report_t rep;
        vm_fw_import_run(&imp, &rep);
        VMFW_T_CHECK(t, rep.artefacts[VM_FW_KERNEL].encrypted,
                     "an unparseable key bag still means encrypted");
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].state,
                    VM_FW_STATE_NEEDS_KEY,
                    "so the artefact waits for a key rather than being written");
        VMFW_T_CHECK(t, mem_find(&fs, "kernel.macho") == NULL,
                     "and no ciphertext was written out as a kernel");
    }

    /* A key of the wrong size for the image that wants it. */
    VMFW_T_SECTION(t, "import/key size mismatch");
    {
        memset(&fs, 0, sizeof fs);
        vm_fw_keys_t keys;
        vm_fw_keys_clear(&keys);
        vm_fw_keys_set_img3(&keys, VM_FW_KERNEL,
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
            k_kernel_iv_hex);
        VMFW_T_EQ_U(t, keys.kernel.key_bits, 256u, "a 256-bit key was accepted");

        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = ip.archive; blob.len = ip.len; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = ip.len;
        imp.files = &files;
        imp.keys = &keys;

        vm_fw_report_t rep;
        vm_fw_import_run(&imp, &rep);
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].state, VM_FW_STATE_FAILED,
                    "but the image wants AES-128, so it is refused");
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].reason,
                    VM_FW_ERR_KEY_WRONG_LENGTH, "by name");
        VMFW_T_CHECK(t, mem_find(&fs, "kernel.macho") == NULL,
                     "and nothing was produced from it");
    }

    /* A manifest too large to be one. */
    VMFW_T_SECTION(t, "import/oversized manifest");
    {
        static uint8_t big[2u << 20];
        static uint8_t arch[3u << 20];
        memset(big, ' ', sizeof big);
        memcpy(big, "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>", 48);
        memcpy(big + sizeof big - 16, "</dict></plist>", 15);

        fx_zip_member_t m[1] = {
            { "Restore.plist", big, sizeof big, false, true }
        };
        size_t n = fx_zip_build(arch, sizeof arch, m, 1u, NULL);
        VMFW_T_CHECK(t, n > 0, "oversized-manifest archive built");

        static fx_blob_t blob;
        blob.data = arch; blob.len = n; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = n;

        vm_fw_report_t rep;
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, &rep),
                    VM_FW_ERR_MANIFEST_TOO_BIG,
                    "a manifest larger than any real one is refused before it "
                    "is read into memory");
    }

    /* ---------------- the reference comparison itself ------------------ */
    /*
     * Driven directly, because an import can only ever supply digests that
     * differ everywhere. A comparison shortened to sixteen bytes, or one that
     * stopped checking the length, passes every end-to-end test in this file
     * and would ship.
     */
    VMFW_T_SECTION(t, "import/reference comparison");
    {
        uint8_t a[VM_FW_SHA256_LEN], b[VM_FW_SHA256_LEN];
        for (size_t i = 0; i < VM_FW_SHA256_LEN; i++) a[i] = (uint8_t)(i + 1u);
        memcpy(b, a, sizeof b);

        VMFW_T_CHECK(t, vm_fw_reference_matches(100u, a, 100u, b),
                     "identical size and digest match");

        /* Every single byte of the digest must matter, including the last. */
        for (size_t i = 0; i < VM_FW_SHA256_LEN; i++) {
            memcpy(b, a, sizeof b);
            b[i] ^= 0x01u;
            if (vm_fw_reference_matches(100u, a, 100u, b)) {
                VMFW_T_CHECK(t, false,
                             "byte %zu of the digest is not compared", i);
                break;
            }
        }
        VMFW_T_CHECK(t, true, "all 32 digest bytes are compared");

        memcpy(b, a, sizeof b);
        VMFW_T_CHECK(t, !vm_fw_reference_matches(99u, a, 100u, b),
                     "a size that differs is not a match even when the digest "
                     "is identical");
        VMFW_T_CHECK(t, !vm_fw_reference_matches(100u, a, 100u, NULL),
                     "a missing reference is not a match");
        VMFW_T_CHECK(t, !vm_fw_reference_matches(100u, NULL, 100u, b),
                     "a missing digest is not a match");
    }

    VMFW_T_SECTION(t, "import/complete kernel reference");
    {
        static const char *hashes[] = {
            "f36a88d611d3b906ae858f377e21853b40b214b2bea99cb2f988e380698e6ce9",
            "0d8cdb339d37cf37a1db2638fff79272ecd63a17764bf7666efa1618725df70c"
        };
        for (size_t h = 0; h < sizeof hashes / sizeof hashes[0]; h++) {
            uint8_t digest[VM_FW_SHA256_LEN];
            VMFW_T_EQ_U(t, vm_fw_parse_hex(hashes[h], digest, sizeof digest),
                        VM_FW_OK, "reference digest");
            VMFW_T_CHECK(t, vm_fw_build_reference_matches("iPhone1,2", "7E18",
                              VM_FW_KERNEL, 7942144u, digest),
                         "complete and historical kernel references accepted");
            for (size_t i = 0; i < sizeof digest; i++) {
                digest[i] ^= 1u;
                VMFW_T_CHECK(t, !vm_fw_build_reference_matches("iPhone1,2", "7E18",
                                  VM_FW_KERNEL, 7942144u, digest),
                             "every digest byte is checked");
                digest[i] ^= 1u;
            }
            VMFW_T_CHECK(t, !vm_fw_build_reference_matches("iPhone2,1", "7E18",
                              VM_FW_KERNEL, 7942144u, digest) &&
                           !vm_fw_build_reference_matches("iPhone1,2", "10B500",
                              VM_FW_KERNEL, 7942144u, digest) &&
                           !vm_fw_build_reference_matches("iPhone1,2", "7E18",
                              VM_FW_DEVICE_TREE, 7942144u, digest) &&
                           !vm_fw_build_reference_matches("iPhone1,2", "7E18",
                              VM_FW_KERNEL, 7942143u, digest),
                         "reference remains device/build/artefact/size specific");
            VMFW_T_CHECK(t, !vm_fw_build_reference_matches(NULL, "7E18",
                              VM_FW_KERNEL, 7942144u, digest) &&
                           !vm_fw_build_reference_matches("iPhone1,2", NULL,
                              VM_FW_KERNEL, 7942144u, digest) &&
                           !vm_fw_build_reference_matches("iPhone1,2", "7E18",
                              (vm_fw_artefact_t)-1, 7942144u, digest) &&
                           !vm_fw_build_reference_matches("iPhone1,2", "7E18",
                              VM_FW_KERNEL, 7942144u, NULL),
                         "invalid reference requests refused");
        }
    }

    /* ---------------- an unknown build --------------------------------- */
    VMFW_T_SECTION(t, "import/unknown build");
    {
        static ipsw_t other;
        ipsw_spec_t spec = k_reference_spec;
        spec.build = "9Z99";
        VMFW_T_CHECK(t, build_ipsw(&other, &spec), "built");

        memset(&fs, 0, sizeof fs);
        vm_fw_keys_t keys;
        vm_fw_keys_clear(&keys);
        vm_fw_keys_set_img3(&keys, VM_FW_KERNEL, k_kernel_key_hex, k_kernel_iv_hex);
        vm_fw_keys_set_img3(&keys, VM_FW_DEVICE_TREE, k_dtree_key_hex, k_dtree_iv_hex);
        vm_fw_keys_set_root(&keys, k_root_key_hex);

        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = other.archive; blob.len = other.len; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = other.len;
        imp.files = &files;
        imp.keys = &keys;

        vm_fw_report_t rep;
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, &rep), VM_FW_OK, "run");
        VMFW_T_CHECK(t, !rep.reference_build, "no reference for this build");
        for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
            VMFW_T_EQ_U(t, rep.artefacts[i].state, VM_FW_STATE_EXTRACTED,
                        "extracted but not claimed to be verified");
            VMFW_T_CHECK(t, !rep.artefacts[i].reference_known,
                         "and honest about having nothing to check against");
        }
    }

    /* ---------------- a device we do not emulate ----------------------- */
    VMFW_T_SECTION(t, "import/unsupported device");
    {
        static ipsw_t other;
        ipsw_spec_t spec = k_reference_spec;
        spec.product_type = "iPhone2,1";
        spec.platform = "s5l8920x";
        spec.board = "n88ap";
        spec.build = "7E18";
        VMFW_T_CHECK(t, build_ipsw(&other, &spec), "built");

        memset(&fs, 0, sizeof fs);
        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = other.archive; blob.len = other.len; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = other.len;
        imp.files = &files;

        vm_fw_report_t rep;
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, &rep), VM_FW_ERR_UNSUPPORTED_DEVICE,
                    "an IPSW for another SoC is refused");
        /* Refused, but still identified -- "this is an iPhone 3GS image" is the
         * useful answer, and "could not import" is not. */
        VMFW_T_EQ_STR(t, rep.product_type, "iPhone2,1", "still identified");
        VMFW_T_EQ_STR(t, rep.platform, "s5l8920x", "and its platform named");
        VMFW_T_CHECK(t, rep.detail[0] != '\0', "with an explanation");
        VMFW_T_EQ_U(t, fs.count, 0u, "and nothing was written");
    }

    /* ---------------- archives that are not IPSWs ---------------------- */
    VMFW_T_SECTION(t, "import/not an ipsw");
    {
        uint8_t junk[500];
        fx_fill(junk, sizeof junk, 7u);
        static fx_blob_t blob;
        blob.data = junk; blob.len = sizeof junk; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = sizeof junk;

        vm_fw_report_t rep;
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, &rep), VM_FW_ERR_NOT_AN_ARCHIVE,
                    "random bytes are not an archive");
        VMFW_T_CHECK(t, rep.detail[0] != '\0', "and it says so");

        static ipsw_t noman;
        ipsw_spec_t spec = k_reference_spec;
        spec.omit_manifest = true;
        build_ipsw(&noman, &spec);
        static fx_blob_t b2;
        b2.data = noman.archive; b2.len = noman.len; b2.fail_next = false;
        imp.pread_ctx = &b2;
        imp.size = noman.len;
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, &rep), VM_FW_ERR_NO_MANIFEST,
                    "a zip with no Restore.plist is not an IPSW");

        VMFW_T_EQ_U(t, vm_fw_import_run(NULL, &rep), VM_FW_ERR_INVALID_ARGUMENT,
                    "a NULL configuration");
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, NULL), VM_FW_ERR_INVALID_ARGUMENT,
                    "a NULL report");
    }

    /* ---------------- missing members ---------------------------------- */
    VMFW_T_SECTION(t, "import/missing members");
    {
        static ipsw_t partial;
        ipsw_spec_t spec = k_reference_spec;
        spec.omit_kernel = true;
        spec.omit_root_name = true;
        build_ipsw(&partial, &spec);

        memset(&fs, 0, sizeof fs);
        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = partial.archive; blob.len = partial.len; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = partial.len;
        imp.files = &files;

        vm_fw_report_t rep;
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, &rep), VM_FW_OK,
                    "a partial archive still produces a report");
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].state,
                    VM_FW_STATE_NOT_IN_ARCHIVE, "no kernelcache");
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_ROOT_FILESYSTEM].state,
                    VM_FW_STATE_NOT_IN_ARCHIVE,
                    "the manifest never said which member is the root");
        /* The device tree is still there and still found by board tag. */
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_DEVICE_TREE].state,
                    VM_FW_STATE_NEEDS_KEY, "the device tree is still located");
    }

    /* ---------------- identification only ------------------------------ */
    VMFW_T_SECTION(t, "import/identify only");
    {
        static ipsw_t plainipsw;
        ipsw_spec_t spec = k_reference_spec;
        spec.encrypt = false;
        build_ipsw(&plainipsw, &spec);

        static fx_blob_t blob;
        blob.data = plainipsw.archive; blob.len = plainipsw.len;
        blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = plainipsw.len;
        imp.files = NULL;      /* no destination: identify, produce nothing */

        vm_fw_report_t rep;
        VMFW_T_EQ_U(t, vm_fw_import_run(&imp, &rep), VM_FW_OK, "run");
        for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
            VMFW_T_EQ_U(t, rep.artefacts[i].state, VM_FW_STATE_FOUND,
                        "found, and nothing produced");
            VMFW_T_CHECK(t, !rep.artefacts[i].encrypted,
                         "an unencrypted fixture is reported as unencrypted");
        }
        VMFW_T_EQ_U(t, rep.member_count, 4u, "member count");
    }

    /* ---------------- destinations that refuse ------------------------- */
    VMFW_T_SECTION(t, "import/destination failures");
    {
        vm_fw_keys_t keys;
        vm_fw_keys_clear(&keys);
        vm_fw_keys_set_img3(&keys, VM_FW_KERNEL, k_kernel_key_hex, k_kernel_iv_hex);
        vm_fw_keys_set_img3(&keys, VM_FW_DEVICE_TREE, k_dtree_key_hex, k_dtree_iv_hex);
        vm_fw_keys_set_root(&keys, k_root_key_hex);

        memset(&fs, 0, sizeof fs);
        fs.refuse_part = true;
        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = ip.archive; blob.len = ip.len; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = ip.len;
        imp.files = &files;
        imp.keys = &keys;

        vm_fw_report_t rep;
        vm_fw_import_run(&imp, &rep);
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_ROOT_FILESYSTEM].reason,
                    VM_FW_ERR_SCRATCH_REFUSED,
                    "nowhere to put the intermediate is its own named reason");

        memset(&fs, 0, sizeof fs);
        fs.refuse_write_after = 64u;
        vm_fw_import_run(&imp, &rep);
        VMFW_T_EQ_U(t, rep.artefacts[VM_FW_KERNEL].state, VM_FW_STATE_FAILED,
                    "a destination that stops accepting bytes fails the artefact");
    }

    /* ---------------- cancellation ------------------------------------- */
    VMFW_T_SECTION(t, "import/cancel");
    {
        vm_fw_keys_t keys;
        vm_fw_keys_clear(&keys);
        vm_fw_keys_set_img3(&keys, VM_FW_KERNEL, k_kernel_key_hex, k_kernel_iv_hex);
        vm_fw_keys_set_img3(&keys, VM_FW_DEVICE_TREE, k_dtree_key_hex, k_dtree_iv_hex);
        vm_fw_keys_set_root(&keys, k_root_key_hex);

        memset(&fs, 0, sizeof fs);
        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = ip.archive; blob.len = ip.len; blob.fail_next = false;
        cancel_ctx_t cc = { 0u, 1u };

        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = ip.len;
        imp.files = &files;
        imp.keys = &keys;
        imp.cancel = cancel_after;
        imp.cancel_ctx = &cc;

        vm_fw_report_t rep;
        vm_fw_status_t st = vm_fw_import_run(&imp, &rep);
        VMFW_T_CHECK(t, st == VM_FW_ERR_CANCELLED
                        || rep.artefacts[VM_FW_KERNEL].reason == VM_FW_ERR_CANCELLED,
                     "cancellation is honoured and named");
        for (unsigned i = 0; i < fs.count; i++)
            VMFW_T_CHECK(t, !fs.f[i].kept,
                         "nothing half-written is kept after a cancel");
    }

    /* ---------------- rendering ---------------------------------------- */
    VMFW_T_SECTION(t, "import/render");
    {
        memset(&fs, 0, sizeof fs);
        vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
        static fx_blob_t blob;
        blob.data = ip.archive; blob.len = ip.len; blob.fail_next = false;
        vm_fw_import_t imp;
        memset(&imp, 0, sizeof imp);
        imp.pread = fx_blob_pread;
        imp.pread_ctx = &blob;
        imp.size = ip.len;
        imp.files = &files;

        vm_fw_report_t rep;
        vm_fw_import_run(&imp, &rep);

        size_t need = vm_fw_report_render(&rep, NULL, 0);
        VMFW_T_CHECK(t, need > 0, "sizing pass returns a length");

        char *buf = (char *)malloc(need + 1u);
        VMFW_T_CHECK(t, buf != NULL, "allocated");
        if (buf) {
            size_t again = vm_fw_report_render(&rep, buf, need + 1u);
            VMFW_T_EQ_U(t, again, need, "the two passes agree");
            VMFW_T_EQ_U(t, strlen(buf), need, "and the result is that long");
            VMFW_T_CHECK(t, strstr(buf, "iPhone1,2") != NULL, "names the device");
            VMFW_T_CHECK(t, strstr(buf, "kernel.macho") != NULL,
                         "names the file it would produce");
            free(buf);
        }

        /* Truncation must be safe and must still terminate the string. */
        char small[40];
        memset(small, 'X', sizeof small);
        size_t r = vm_fw_report_render(&rep, small, sizeof small);
        VMFW_T_EQ_U(t, r, need, "a short buffer still reports the full length");
        VMFW_T_CHECK(t, small[sizeof small - 1] == '\0',
                     "and never leaves the buffer unterminated");
        VMFW_T_EQ_U(t, vm_fw_report_render(NULL, small, sizeof small), 0u,
                    "a NULL report renders nothing");
    }

    /* ---------------- strings ------------------------------------------ */
    VMFW_T_SECTION(t, "import/strings");
    {
        VMFW_T_EQ_STR(t, vm_fw_artefact_filename(VM_FW_KERNEL), "kernel.macho",
                      "the emulator's kernel file name");
        VMFW_T_EQ_STR(t, vm_fw_artefact_filename(VM_FW_DEVICE_TREE),
                      "devicetree.bin", "device tree file name");
        VMFW_T_EQ_STR(t, vm_fw_artefact_filename(VM_FW_ROOT_FILESYSTEM),
                      "rootfs.img", "root filesystem file name");
        VMFW_T_CHECK(t, vm_fw_artefact_filename((vm_fw_artefact_t)99)[0] == '\0',
                     "an unknown artefact has no file name");
        for (int i = 0; i <= VM_FW_STAGE_DONE; i++)
            VMFW_T_CHECK(t, vm_fw_stage_name((vm_fw_stage_t)i)[0] != '\0',
                         "every stage has a name");
        VMFW_T_CHECK(t, vm_fw_strerror(VM_FW_OK) != NULL, "strerror(OK)");
        VMFW_T_CHECK(t, vm_fw_strerror((vm_fw_status_t)999) != NULL,
                     "strerror of an unknown code");
        VMFW_T_CHECK(t, vm_fw_import_peak_memory() > 0u,
                     "a peak-memory bound is published");
    }

    /* ---------------- truncated archives ------------------------------- */
    /*
     * The whole pipeline over every prefix of a valid IPSW. The requirement is
     * not that any particular prefix behaves a particular way -- it is that
     * none of them crashes and none of them ever reports VERIFIED.
     */
    VMFW_T_SECTION(t, "import/truncation");
    {
        unsigned runs = 0;
        for (size_t cut = 64u; cut < ip.len; cut += 4099u) {
            memset(&fs, 0, sizeof fs);
            vm_fw_files_t files = { mem_open, mem_write, mem_pread, mem_close, &fs };
            static fx_blob_t blob;
            blob.data = ip.archive; blob.len = cut; blob.fail_next = false;
            vm_fw_import_t imp;
            memset(&imp, 0, sizeof imp);
            imp.pread = fx_blob_pread;
            imp.pread_ctx = &blob;
            imp.size = cut;
            imp.files = &files;

            vm_fw_report_t rep;
            (void)vm_fw_import_run(&imp, &rep);
            runs++;
            for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++)
                if (rep.artefacts[i].state == VM_FW_STATE_VERIFIED) {
                    VMFW_T_CHECK(t, false,
                                 "a truncated archive reported a verified "
                                 "artefact at cut %zu", cut);
                    break;
                }
        }
        VMFW_T_CHECK(t, runs > 0, "every prefix of the archive was run");
    }
}
