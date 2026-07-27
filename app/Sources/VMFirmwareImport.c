/*
 * S5LBox — the import pipeline: IPSW in, three verified files out.
 *
 * The order of operations is the interesting part, and it is chosen so that the
 * user learns the most useful thing as early as possible:
 *
 *   1. Open the archive and read Restore.plist. This costs a few kilobytes and
 *      answers "which device and build is this?" -- the question that decides
 *      whether anything else is worth attempting.
 *   2. Locate every member from the manifest rather than by guessing names.
 *      `018-6482-014.dmg` is the root filesystem and `018-6494-014.dmg` is the
 *      restore ramdisk; nothing about either name says so, and docs/debugging.md
 *      once named the wrong one. The manifest is not ambiguous, so it is read.
 *   3. Unwrap the small artefacts, which are cheap, before the 208 MB one.
 *   4. For anything encrypted with no key supplied, stop at exactly the point
 *      where the key would be used, and say so. Do not spend four minutes
 *      expanding a disk image to discover it decrypted into noise.
 *
 * Allocation policy. The format decoders in VMFirmwareFormats.h allocate
 * nothing, so none of them can fail halfway through a parse. This file does
 * allocate -- it has to hold a 4 MB member and its 7.9 MB expansion -- and
 * every allocation is checked and reported as VM_FW_ERR_OUT_OF_MEMORY. The
 * 433 MB artefact is never held: it streams.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareImport.h"

#include "img3.h"
#include "lzss.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A member we are willing to hold in RAM. The kernelcache is 4 MB; the root
 * filesystem is 208 MB and goes to a file instead. The cap exists so a
 * corrupted central directory cannot ask for a gigabyte before anything else
 * has a chance to notice.
 *
 * NOT covered by the suite: exercising it needs a fixture with a member over
 * 64 MB, which would dominate the runtime of every CI job to test one
 * comparison. MANIFEST_CAP is the same check at a size a test can afford, and
 * that one is exercised -- removing it fails the suite.
 */
#define MEMBER_MEMORY_CAP (64u * 1024u * 1024u)
#define MANIFEST_CAP      (1u * 1024u * 1024u)

/* ------------------------------------------------------------------------ */
/* Names and strings                                                         */
/* ------------------------------------------------------------------------ */

const char *vm_fw_artefact_filename(vm_fw_artefact_t which) {
    switch (which) {
        case VM_FW_KERNEL:          return "kernel.macho";
        case VM_FW_DEVICE_TREE:     return "devicetree.bin";
        case VM_FW_ROOT_FILESYSTEM: return "rootfs.img";
        default:                    return "";
    }
}

const char *vm_fw_artefact_title(vm_fw_artefact_t which) {
    switch (which) {
        case VM_FW_KERNEL:          return "Kernel";
        case VM_FW_DEVICE_TREE:     return "Device tree";
        case VM_FW_ROOT_FILESYSTEM: return "Root filesystem";
        default:                    return "";
    }
}

const char *vm_fw_stage_name(vm_fw_stage_t stage) {
    switch (stage) {
        case VM_FW_STAGE_OPENING:          return "opening the archive";
        case VM_FW_STAGE_READING_MANIFEST: return "reading the manifest";
        case VM_FW_STAGE_LOCATING:         return "locating members";
        case VM_FW_STAGE_EXTRACTING:       return "extracting";
        case VM_FW_STAGE_DECRYPTING:       return "decrypting";
        case VM_FW_STAGE_DECOMPRESSING:    return "decompressing";
        case VM_FW_STAGE_EXPANDING:        return "expanding the disk image";
        case VM_FW_STAGE_VERIFYING:        return "verifying";
        case VM_FW_STAGE_DONE:             return "done";
        default:                           return "working";
    }
}

const char *vm_fw_strerror(vm_fw_status_t st) {
    switch (st) {
        case VM_FW_OK:                       return "ok";
        case VM_FW_ERR_INVALID_ARGUMENT:     return "invalid import argument";
        case VM_FW_ERR_OUT_OF_MEMORY:        return "not enough memory to unpack this firmware";
        case VM_FW_ERR_CANCELLED:            return "cancelled";
        case VM_FW_ERR_NOT_AN_ARCHIVE:       return "this file is not an IPSW: it is not even a zip archive";
        case VM_FW_ERR_ARCHIVE_UNREADABLE:   return "the file could not be read";
        case VM_FW_ERR_ARCHIVE_MALFORMED:    return "the archive's directory is damaged";
        case VM_FW_ERR_NO_MANIFEST:          return "this zip has no Restore.plist, so it is not an IPSW";
        case VM_FW_ERR_MANIFEST_TOO_BIG:     return "the manifest is implausibly large";
        case VM_FW_ERR_MANIFEST_MALFORMED:   return "the manifest will not parse";
        case VM_FW_ERR_MANIFEST_INCOMPLETE:  return "the manifest does not name the files this needs";
        case VM_FW_ERR_UNSUPPORTED_DEVICE:   return "this firmware is for a device S5LBox does not emulate";
        case VM_FW_ERR_MEMBER_MISSING:       return "the archive is missing a member its own manifest names";
        case VM_FW_ERR_MEMBER_UNREADABLE:    return "a member could not be read out of the archive";
        case VM_FW_ERR_MEMBER_CHECKSUM:      return "a member's contents do not match the archive's own checksum";
        case VM_FW_ERR_MEMBER_TOO_BIG:       return "a member is too large to unpack in memory";
        case VM_FW_ERR_NOT_IMG3:             return "this member is not an IMG3 container";
        case VM_FW_ERR_IMG3_MALFORMED:       return "the IMG3 container is damaged";
        case VM_FW_ERR_IMG3_NO_PAYLOAD:      return "the IMG3 container carries no payload";
        case VM_FW_ERR_KEY_REQUIRED:         return "this file is encrypted and needs a key you supply";
        case VM_FW_ERR_KEY_WRONG_LENGTH:     return "that key is not the right length";
        case VM_FW_ERR_KEY_NOT_HEX:          return "that key contains characters that are not hexadecimal";
        case VM_FW_ERR_DECRYPT_FAILED:       return "decryption could not be performed";
        case VM_FW_ERR_NOT_COMPRESSED:       return "the decrypted kernel is not a complzss image; the key is probably not this one's";
        case VM_FW_ERR_DECOMPRESS_FAILED:    return "the kernel's compressed stream is damaged";
        case VM_FW_ERR_DMG:                  return "the disk image could not be expanded";
        case VM_FW_ERR_NO_ROOT_PARTITION:    return "the disk image contains no Apple_HFSX partition";
        case VM_FW_ERR_OUTPUT_REFUSED:       return "the destination would not accept the result";
        case VM_FW_ERR_SCRATCH_REFUSED:      return "there is nowhere to put the temporary copy this needs";
        default:                             return "unknown import error";
    }
}

/* ------------------------------------------------------------------------ */
/* Keys                                                                      */
/* ------------------------------------------------------------------------ */

void vm_fw_keys_clear(vm_fw_keys_t *keys) {
    if (keys) memset(keys, 0, sizeof *keys);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v'
        || c == '\f';
}

vm_fw_status_t vm_fw_parse_hex(const char *text, uint8_t *out, size_t want) {
    if (!text || !out || want == 0) return VM_FW_ERR_INVALID_ARGUMENT;

    size_t produced = 0;
    int high = -1;

    for (const char *p = text; *p; p++) {
        if (is_space(*p)) continue;
        int v = hex_value(*p);
        /*
         * Refused, not skipped. A key pasted with a stray character silently
         * dropped would still be the right length and would decrypt into
         * garbage that gets blamed on the IPSW.
         */
        if (v < 0) return VM_FW_ERR_KEY_NOT_HEX;

        if (high < 0) { high = v; continue; }
        if (produced >= want) return VM_FW_ERR_KEY_WRONG_LENGTH;
        out[produced++] = (uint8_t)((high << 4) | v);
        high = -1;
    }

    /* An odd number of digits is not a short key, it is a typo. */
    if (high >= 0) return VM_FW_ERR_KEY_WRONG_LENGTH;
    if (produced != want) return VM_FW_ERR_KEY_WRONG_LENGTH;
    return VM_FW_OK;
}

vm_fw_status_t vm_fw_keys_set_img3(vm_fw_keys_t *keys, vm_fw_artefact_t which,
                                   const char *key_hex, const char *iv_hex) {
    if (!keys) return VM_FW_ERR_INVALID_ARGUMENT;

    vm_fw_img3_key_t *slot;
    if (which == VM_FW_KERNEL)           slot = &keys->kernel;
    else if (which == VM_FW_DEVICE_TREE) slot = &keys->device_tree;
    else return VM_FW_ERR_INVALID_ARGUMENT;

    if (!key_hex || !iv_hex) return VM_FW_ERR_INVALID_ARGUMENT;

    /* Count the hex digits to choose the key length, so a 256-bit key is
     * accepted without the caller having to say which it pasted. */
    size_t digits = 0;
    for (const char *p = key_hex; *p; p++) {
        if (is_space(*p)) continue;
        if (hex_value(*p) < 0) return VM_FW_ERR_KEY_NOT_HEX;
        digits++;
    }
    unsigned bits;
    switch (digits) {
        case 32: bits = 128u; break;
        case 48: bits = 192u; break;
        case 64: bits = 256u; break;
        default: return VM_FW_ERR_KEY_WRONG_LENGTH;
    }

    vm_fw_img3_key_t staged;
    memset(&staged, 0, sizeof staged);

    vm_fw_status_t st = vm_fw_parse_hex(key_hex, staged.key, bits / 8u);
    if (st != VM_FW_OK) return st;
    st = vm_fw_parse_hex(iv_hex, staged.iv, sizeof staged.iv);
    if (st != VM_FW_OK) return st;

    /* Staged first so a rejected IV does not leave a half-set key behind that a
     * later run would use. */
    staged.present  = true;
    staged.key_bits = bits;
    *slot = staged;
    return VM_FW_OK;
}

vm_fw_status_t vm_fw_keys_set_root(vm_fw_keys_t *keys, const char *key_hex) {
    if (!keys || !key_hex) return VM_FW_ERR_INVALID_ARGUMENT;
    uint8_t staged[VMFW_DMG_KEY_BLOB_SIZE];
    vm_fw_status_t st = vm_fw_parse_hex(key_hex, staged, sizeof staged);
    if (st != VM_FW_OK) return st;
    memcpy(keys->root, staged, sizeof staged);
    keys->root_present = true;
    return VM_FW_OK;
}

/* ------------------------------------------------------------------------ */
/* Known-good references                                                     */
/* ------------------------------------------------------------------------ */
/*
 * The only build this project has ever verified end to end. Hashes are of the
 * FINAL artefacts, not of any IPSW member, so they identify what the emulator
 * accepts rather than what Apple shipped -- and they contain no Apple data.
 *
 * A different build still imports; its artefacts come out EXTRACTED rather than
 * VERIFIED, and the report says why. Claiming a verification we cannot perform
 * would be worse than not performing it.
 */
typedef struct {
    const char *product_type;
    const char *build;
    uint64_t    size[VM_FW_ARTEFACT_COUNT];
    uint8_t     sha256[VM_FW_ARTEFACT_COUNT][VM_FW_SHA256_LEN];
} vm_fw_reference_t;

static const vm_fw_reference_t k_references[] = {
    {
        "iPhone1,2", "7E18",
        { 7942144u, 40544u, 433274880u },
        {
            /* kernel.macho */
            { 0x0d,0x8c,0xdb,0x33,0x9d,0x37,0xcf,0x37,0xa1,0xdb,0x26,0x38,
              0xff,0xf7,0x92,0x72,0xec,0xd6,0x3a,0x17,0x76,0x4b,0xf7,0x66,
              0x6e,0xfa,0x16,0x18,0x72,0x5d,0xf7,0x0c },
            /* devicetree.bin */
            { 0x48,0x67,0xc9,0x5f,0xed,0xf5,0x44,0xbd,0xa2,0xec,0xaa,0x26,
              0x26,0xae,0x14,0xc0,0x1a,0x60,0xd7,0x77,0x1d,0xc5,0x3f,0xfe,
              0x6f,0xd3,0xa6,0xaa,0xc8,0xb8,0xba,0x57 },
            /* rootfs.img */
            { 0xc3,0x25,0x1e,0x7f,0x09,0x2c,0x93,0x9d,0x58,0x18,0xe9,0x20,
              0x86,0xcb,0x47,0x68,0x09,0x81,0xcf,0xb0,0x37,0x31,0xde,0x7b,
              0x55,0xd2,0x38,0xc9,0x42,0xeb,0x5e,0x82 }
        }
    }
};

static const vm_fw_reference_t *find_reference(const char *product_type,
                                               const char *build) {
    const size_t n = sizeof k_references / sizeof k_references[0];
    for (size_t i = 0; i < n; i++) {
        if (strcmp(k_references[i].product_type, product_type) == 0 &&
            strcmp(k_references[i].build, build) == 0)
            return &k_references[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

static void set_detail(char *dst, size_t cap, const char *text) {
    if (!dst || cap == 0) return;
    size_t n = strlen(text);
    if (n >= cap) n = cap - 1;
    memcpy(dst, text, n);
    dst[n] = '\0';
}

/* A private copy of the run's state, so the many small steps below do not each
 * take eight arguments. */
typedef struct {
    const vm_fw_import_t *cfg;
    vm_fw_report_t       *rep;
    vmfw_zip_t            zip;
    const vm_fw_reference_t *ref;
} run_t;

static bool cancelled(const run_t *r) {
    return r->cfg->cancel && r->cfg->cancel(r->cfg->cancel_ctx);
}

static void report_progress(const run_t *r, vm_fw_artefact_t which,
                            vm_fw_stage_t stage, uint64_t done, uint64_t total) {
    if (r->cfg->progress)
        r->cfg->progress(r->cfg->progress_ctx, which, stage, done, total);
}

/* ------------------------------------------------------------------------ */
/* Sinks                                                                     */
/* ------------------------------------------------------------------------ */

/* Collects into a caller-owned buffer while hashing, so a member is read once. */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;
} grow_sink_t;

static bool grow_write(void *ctx, const uint8_t *data, size_t len) {
    grow_sink_t *g = (grow_sink_t *)ctx;
    if (len > g->cap - g->len) { g->overflow = true; return false; }
    memcpy(g->buf + g->len, data, len);
    g->len += len;
    return true;
}

/* Captures only a prefix and then refuses, which is how the root filesystem's
 * wrapper is identified without inflating 208 MB to find out. */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
} peek_sink_t;

static bool peek_write(void *ctx, const uint8_t *data, size_t len) {
    peek_sink_t *p = (peek_sink_t *)ctx;
    size_t room = p->cap - p->len;
    size_t take = len < room ? len : room;
    memcpy(p->buf + p->len, data, take);
    p->len += take;
    return p->len < p->cap;     /* stop as soon as we have enough */
}

/* The production sink: file, hash, count, progress and cancellation in one
 * pass over data that is up to 433 MB. */
typedef struct {
    const run_t *run;
    const vm_fw_files_t *files;
    void *handle;
    ios3_sha256_context_t sha;
    uint64_t total;
    uint64_t expect;
    vm_fw_artefact_t which;
    vm_fw_stage_t stage;
    bool write_failed;
    bool cancel_requested;
    uint64_t last_report;
} out_sink_t;

static bool out_write(void *ctx, const uint8_t *data, size_t len) {
    out_sink_t *o = (out_sink_t *)ctx;

    if (cancelled(o->run)) { o->cancel_requested = true; return false; }

    if (!ios3_sha256_update(&o->sha, data, len)) { o->write_failed = true; return false; }

    if (o->handle && o->files &&
        !o->files->write(o->files->ctx, o->handle, data, len)) {
        o->write_failed = true;
        return false;
    }
    o->total += len;

    /* Report about every 4 MB. A callback per 8 KB chunk would spend more time
     * hopping to the main thread than expanding the image. */
    if (o->total - o->last_report >= (4u << 20) || o->total == o->expect) {
        o->last_report = o->total;
        report_progress(o->run, o->which, o->stage, o->total, o->expect);
    }
    return true;
}

static void out_sink_init(out_sink_t *o, const run_t *run, void *handle,
                          vm_fw_artefact_t which, vm_fw_stage_t stage,
                          uint64_t expect) {
    memset(o, 0, sizeof *o);
    o->run = run;
    o->files = run->cfg->files;
    o->handle = handle;
    o->which = which;
    o->stage = stage;
    o->expect = expect;
    ios3_sha256_init(&o->sha);
}

/* Random access over a file the caller opened for us. */
typedef struct {
    const vm_fw_files_t *files;
    void *handle;
} file_pread_t;

static size_t file_pread(void *ctx, uint64_t off, uint8_t *buf, size_t len) {
    file_pread_t *f = (file_pread_t *)ctx;
    return f->files->pread(f->files->ctx, f->handle, off, buf, len);
}

/* ------------------------------------------------------------------------ */
/* Locating members                                                          */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *needle_a;
    const char *needle_b;       /* both must appear; either may be NULL      */
    const char *prefer;         /* tie-break substring                       */
    vmfw_zip_entry_t best;
    bool found;
    bool preferred;
} search_t;

static bool search_visit(void *ctx, const vmfw_zip_entry_t *e, uint32_t idx) {
    search_t *s = (search_t *)ctx;
    (void)idx;
    if (e->is_directory) return true;
    if (s->needle_a && !strstr(e->name, s->needle_a)) return true;
    if (s->needle_b && !strstr(e->name, s->needle_b)) return true;

    bool preferred = s->prefer && strstr(e->name, s->prefer) != NULL;
    /* Keep looking after a hit so a later, better-matching member can win;
     * stopping at the first would make the result depend on directory order. */
    if (!s->found || (preferred && !s->preferred)) {
        s->best = *e;
        s->found = true;
        s->preferred = preferred;
    }
    return true;
}

static bool find_member_containing(const run_t *r, const char *a, const char *b,
                                   const char *prefer, vmfw_zip_entry_t *out) {
    search_t s;
    memset(&s, 0, sizeof s);
    s.needle_a = a;
    s.needle_b = b;
    s.prefer = prefer;
    if (vmfw_zip_iterate(&r->zip, search_visit, &s) != VMFW_ZIP_OK) return false;
    if (!s.found) return false;
    *out = s.best;
    return true;
}

typedef struct { uint32_t count; } count_t;

static bool count_visit(void *ctx, const vmfw_zip_entry_t *e, uint32_t idx) {
    (void)e; (void)idx;
    ((count_t *)ctx)->count++;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Reading one member into memory                                            */
/* ------------------------------------------------------------------------ */

static vm_fw_status_t read_member(const run_t *r, const vmfw_zip_entry_t *entry,
                                  uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;

    if (entry->uncompressed_size > MEMBER_MEMORY_CAP)
        return VM_FW_ERR_MEMBER_TOO_BIG;
    if (entry->uncompressed_size == 0) return VM_FW_ERR_MEMBER_UNREADABLE;

    size_t n = (size_t)entry->uncompressed_size;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) return VM_FW_ERR_OUT_OF_MEMORY;

    grow_sink_t g = { buf, n, 0, false };
    vmfw_zip_status_t zst = vmfw_zip_extract(&r->zip, entry, grow_write, &g);
    if (zst != VMFW_ZIP_OK) {
        free(buf);
        if (zst == VMFW_ZIP_ERR_CRC_MISMATCH) return VM_FW_ERR_MEMBER_CHECKSUM;
        if (zst == VMFW_ZIP_ERR_READ)         return VM_FW_ERR_ARCHIVE_UNREADABLE;
        return VM_FW_ERR_MEMBER_UNREADABLE;
    }
    if (g.len != n) { free(buf); return VM_FW_ERR_MEMBER_UNREADABLE; }

    *out = buf;
    *out_len = n;
    return VM_FW_OK;
}

/* ------------------------------------------------------------------------ */
/* The manifest                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    char kernel_member[VMFW_ZIP_MAX_NAME];
    char root_member[VMFW_ZIP_MAX_NAME];
} manifest_t;

static vm_fw_status_t read_manifest(run_t *r, manifest_t *man) {
    memset(man, 0, sizeof *man);

    vmfw_zip_entry_t entry;
    if (vmfw_zip_find(&r->zip, "Restore.plist", &entry) != VMFW_ZIP_OK &&
        vmfw_zip_find(&r->zip, "restore.plist", &entry) != VMFW_ZIP_OK)
        return VM_FW_ERR_NO_MANIFEST;

    if (entry.uncompressed_size > MANIFEST_CAP) return VM_FW_ERR_MANIFEST_TOO_BIG;

    uint8_t *xml = NULL;
    size_t xml_len = 0;
    vm_fw_status_t st = read_member(r, &entry, &xml, &xml_len);
    if (st != VM_FW_OK) return st;

    vmfw_plist_t pl;
    if (vmfw_plist_init(&pl, xml, xml_len) != VMFW_PLIST_OK) {
        free(xml);
        return VM_FW_ERR_MANIFEST_MALFORMED;
    }

    vm_fw_report_t *rep = r->rep;

    /* Identification. Missing any of these means this is not a restore IPSW,
     * whatever it is. */
    if (vmfw_plist_get_string(&pl, "ProductType", rep->product_type,
                              sizeof rep->product_type) != VMFW_PLIST_OK ||
        vmfw_plist_get_string(&pl, "ProductBuildVersion", rep->build,
                              sizeof rep->build) != VMFW_PLIST_OK) {
        free(xml);
        return VM_FW_ERR_MANIFEST_INCOMPLETE;
    }
    /* Not fatal: some builds omit the marketing version. */
    (void)vmfw_plist_get_string(&pl, "ProductVersion", rep->product_version,
                                sizeof rep->product_version);

    (void)vmfw_plist_get_string(&pl, "DeviceMap/0/BoardConfig", rep->board,
                                sizeof rep->board);
    (void)vmfw_plist_get_string(&pl, "DeviceMap/0/Platform", rep->platform,
                                sizeof rep->platform);

    /*
     * The kernelcache is named per platform. Building the path from the
     * platform the manifest itself declares keeps this correct for any S5L8900
     * IPSW instead of only the one we tested.
     */
    if (rep->platform[0]) {
        char path[128];
        int written = snprintf(path, sizeof path,
                               "KernelCachesByPlatform/%s/Release",
                               rep->platform);
        if (written > 0 && (size_t)written < sizeof path)
            (void)vmfw_plist_get_string(&pl, path, man->kernel_member,
                                        sizeof man->kernel_member);
    }
    if (man->kernel_member[0] == '\0')
        (void)vmfw_plist_get_string(&pl, "RestoreKernelCaches/Release",
                                    man->kernel_member,
                                    sizeof man->kernel_member);

    /* The one unambiguous statement of which DMG is the root filesystem. */
    (void)vmfw_plist_get_string(&pl, "SystemRestoreImages/User",
                                man->root_member, sizeof man->root_member);

    free(xml);
    rep->manifest_read = true;
    return VM_FW_OK;
}

/* ------------------------------------------------------------------------ */
/* Verification                                                              */
/* ------------------------------------------------------------------------ */

bool vm_fw_reference_matches(uint64_t produced_size,
                             const uint8_t produced_sha256[VM_FW_SHA256_LEN],
                             uint64_t reference_size,
                             const uint8_t reference_sha256[VM_FW_SHA256_LEN]) {
    if (!produced_sha256 || !reference_sha256) return false;
    if (produced_size != reference_size) return false;
    return memcmp(produced_sha256, reference_sha256, VM_FW_SHA256_LEN) == 0;
}

static void finish_hash(const run_t *r, vm_fw_artefact_report_t *ar,
                        out_sink_t *o, vm_fw_artefact_t which) {
    if (!ios3_sha256_final(&o->sha, ar->sha256)) return;
    ar->sha256_valid = true;
    ar->produced = o->total;

    if (!r->ref) {
        ar->state = VM_FW_STATE_EXTRACTED;
        return;
    }
    ar->reference_known = true;
    ar->matches_reference =
        vm_fw_reference_matches(o->total, ar->sha256,
                                r->ref->size[which], r->ref->sha256[which]);
    ar->state = ar->matches_reference ? VM_FW_STATE_VERIFIED
                                      : VM_FW_STATE_MISMATCH;
}

/* ------------------------------------------------------------------------ */
/* The IMG3 artefacts: kernel and device tree                                */
/* ------------------------------------------------------------------------ */

static void import_img3_artefact(run_t *r, vm_fw_artefact_t which,
                                 const vmfw_zip_entry_t *entry,
                                 const vm_fw_img3_key_t *key) {
    vm_fw_artefact_report_t *ar = &r->rep->artefacts[which];

    ar->member_size = entry->uncompressed_size;
    set_detail(ar->member, sizeof ar->member, entry->name);

    report_progress(r, which, VM_FW_STAGE_EXTRACTING, 0, entry->uncompressed_size);

    uint8_t *raw = NULL;
    size_t raw_len = 0;
    vm_fw_status_t st = read_member(r, entry, &raw, &raw_len);
    if (st != VM_FW_OK) {
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = st;
        set_detail(ar->detail, sizeof ar->detail, vm_fw_strerror(st));
        return;
    }

    img3_t img;
    img3_status_t ist = img3_parse(raw, raw_len, &img);
    if (ist != IMG3_OK) {
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = (ist == IMG3_ERR_BAD_MAGIC) ? VM_FW_ERR_NOT_IMG3
                                                 : VM_FW_ERR_IMG3_MALFORMED;
        snprintf(ar->detail, sizeof ar->detail,
                 "This member reads as %s.", img3_strerror(ist));
        free(raw);
        return;
    }

    ar->is_img3 = true;
    img3_ident_str(img.ident, ar->ident);

    if (!img.data || img.data_len == 0) {
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = VM_FW_ERR_IMG3_NO_PAYLOAD;
        set_detail(ar->detail, sizeof ar->detail,
                   vm_fw_strerror(VM_FW_ERR_IMG3_NO_PAYLOAD));
        free(raw);
        return;
    }

    /*
     * A KBAG that is present but unparseable is treated as encrypted, not as
     * absent. img3.c distinguishes the two deliberately: guessing "not
     * encrypted" would copy ciphertext onward and report success.
     */
    ar->encrypted = img.kbag.present || img.kbag.malformed;
    ar->key_bits  = img.kbag.present ? img.kbag.key_bits : 0u;

    if (ar->encrypted && (!key || !key->present)) {
        ar->state = VM_FW_STATE_NEEDS_KEY;
        ar->reason = VM_FW_ERR_KEY_REQUIRED;
        ar->awaiting_key = true;
        /*
         * ar->ident is copied out first: passing one field of a struct while
         * writing into another is something the compiler cannot prove is
         * disjoint, and it is right not to -- snprintf may legally assume its
         * arguments do not alias its destination.
         */
        char ident[sizeof ar->ident];
        memcpy(ident, ar->ident, sizeof ident);
        snprintf(ar->detail, sizeof ar->detail,
                 "Located in the IPSW and its container read: '%s' payload, "
                 "AES-%u encrypted. The key is not in the IPSW and cannot be "
                 "computed from it. Paste the %s key and IV for this device "
                 "and build to finish this one.",
                 ident, ar->key_bits ? ar->key_bits : 128u,
                 vm_fw_artefact_title(which));
        free(raw);
        return;
    }

    /* Decrypt in place: aes_cbc_decrypt allows in == out, and holding a second
     * 4 MB copy on a phone for no reason is a cost the user pays. */
    if (ar->encrypted) {
        report_progress(r, which, VM_FW_STAGE_DECRYPTING, 0, img.data_len);
        ar->key_supplied = true;
        if (key->key_bits != img.kbag.key_bits) {
            ar->state = VM_FW_STATE_FAILED;
            ar->reason = VM_FW_ERR_KEY_WRONG_LENGTH;
            snprintf(ar->detail, sizeof ar->detail,
                     "This image wants an AES-%u key and the one supplied is "
                     "AES-%u.", img.kbag.key_bits, key->key_bits);
            free(raw);
            return;
        }
        uint32_t got = 0;
        if (!img3_decrypt_data_iv(&img, key->key, key->key_bits, key->iv,
                                  (uint8_t *)img.data, img.data_len, &got)) {
            ar->state = VM_FW_STATE_FAILED;
            ar->reason = VM_FW_ERR_DECRYPT_FAILED;
            set_detail(ar->detail, sizeof ar->detail,
                       vm_fw_strerror(VM_FW_ERR_DECRYPT_FAILED));
            free(raw);
            return;
        }
    }

    const uint8_t *payload = img.data;
    size_t payload_len = img.data_len;
    uint8_t *expanded = NULL;
    bool short_stream = false;
    uint32_t shortfall = 0;

    if (which == VM_FW_KERNEL) {
        lzss_header_t lh;
        if (!lzss_parse_header(payload, payload_len, &lh)) {
            ar->state = VM_FW_STATE_FAILED;
            ar->reason = VM_FW_ERR_NOT_COMPRESSED;
            set_detail(ar->detail, sizeof ar->detail,
                       vm_fw_strerror(VM_FW_ERR_NOT_COMPRESSED));
            free(raw);
            return;
        }
        report_progress(r, which, VM_FW_STAGE_DECOMPRESSING, 0,
                        lh.uncompressed_size);

        if (lh.uncompressed_size == 0 ||
            lh.uncompressed_size > MEMBER_MEMORY_CAP) {
            ar->state = VM_FW_STATE_FAILED;
            ar->reason = VM_FW_ERR_DECOMPRESS_FAILED;
            set_detail(ar->detail, sizeof ar->detail,
                       "The kernel declares an implausible uncompressed size.");
            free(raw);
            return;
        }

        expanded = (uint8_t *)malloc(lh.uncompressed_size);
        if (!expanded) {
            ar->state = VM_FW_STATE_FAILED;
            ar->reason = VM_FW_ERR_OUT_OF_MEMORY;
            set_detail(ar->detail, sizeof ar->detail,
                       vm_fw_strerror(VM_FW_ERR_OUT_OF_MEMORY));
            free(raw);
            return;
        }

        size_t got = lzss_decompress(expanded, lh.uncompressed_size,
                                     payload + LZSS_HEADER_SIZE,
                                     lh.compressed_size);
        /*
         * lzss_decompress is bounded and cannot exceed the capacity it is
         * given, so this can only fire if that guarantee is ever broken. It is
         * checked anyway because the alternative to noticing here is not
         * noticing at all: the overrun would land in a heap buffer, the file
         * written would still be exactly uncompressed_size bytes, and the only
         * symptom would be corruption somewhere else entirely.
         */
        if (got > lh.uncompressed_size) {
            ar->state = VM_FW_STATE_FAILED;
            ar->reason = VM_FW_ERR_DECOMPRESS_FAILED;
            set_detail(ar->detail, sizeof ar->detail,
                       "The kernel's compressed stream produced more bytes "
                       "than it declared.");
            free(expanded);
            free(raw);
            return;
        }
        if (got == 0) {
            ar->state = VM_FW_STATE_FAILED;
            ar->reason = VM_FW_ERR_DECOMPRESS_FAILED;
            set_detail(ar->detail, sizeof ar->detail,
                       vm_fw_strerror(VM_FW_ERR_DECOMPRESS_FAILED));
            free(expanded);
            free(raw);
            return;
        }

        /*
         * KNOWN AND DOCUMENTED, not papered over. On the 7E18 kernelcache the
         * stream encodes 42 bytes fewer than the header declares and stops with
         * one source byte left. tools/unlzss.c has carried this note since the
         * canonical kernel.macho was produced, an independent implementation
         * agrees byte for byte, and the shortfall lands in __PRELINK_INFO. The
         * zero fill is what makes every Mach-O segment addressable, and it is
         * baked into the hash this file checks against.
         */
        if (got < lh.uncompressed_size) {
            shortfall = lh.uncompressed_size - (uint32_t)got;
            short_stream = true;
            memset(expanded + got, 0, shortfall);
        }
        payload = expanded;
        payload_len = lh.uncompressed_size;
    }

    /*
     * Produce -- or, with no destination, stop here and say what would have
     * happened.
     *
     * This deliberately does NOT hash and compare when there is nowhere to
     * write. It could: the bytes are in hand and the reference is known, so a
     * dry run reporting VERIFIED is technically available for these two
     * artefacts. It would also be available for exactly two of the three, since
     * the root filesystem cannot be expanded without somewhere to put it, and a
     * screen where two rows say "verified" and the third says "found" for a
     * reason the user cannot see is worse than one that says "found" three
     * times. One contract, honestly reported.
     */
    void *handle = NULL;
    if (r->cfg->files)
        handle = r->cfg->files->open(r->cfg->files->ctx,
                                     vm_fw_artefact_filename(which));
    if (!handle) {
        ar->state = VM_FW_STATE_FOUND;
        snprintf(ar->detail, sizeof ar->detail,
                 "Ready: this member would produce %llu bytes. No destination "
                 "was opened for it, so nothing was written or checked.",
                 (unsigned long long)payload_len);
        free(expanded);
        free(raw);
        return;
    }

    out_sink_t o;
    out_sink_init(&o, r, handle, which, VM_FW_STAGE_VERIFYING, payload_len);
    bool ok = out_write(&o, payload, payload_len);

    if (r->cfg->files && handle)
        r->cfg->files->close(r->cfg->files->ctx, handle, ok);

    if (!ok) {
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = o.cancel_requested ? VM_FW_ERR_CANCELLED
                                        : VM_FW_ERR_OUTPUT_REFUSED;
        set_detail(ar->detail, sizeof ar->detail, vm_fw_strerror(ar->reason));
        free(expanded);
        free(raw);
        return;
    }

    finish_hash(r, ar, &o, which);

    if (ar->state == VM_FW_STATE_VERIFIED) {
        snprintf(ar->detail, sizeof ar->detail,
                 "Extracted and verified: %llu bytes, SHA-256 matches the "
                 "known-good %s for this build.%s",
                 (unsigned long long)ar->produced,
                 vm_fw_artefact_filename(which),
                 short_stream ? " The compressed stream ended 42 bytes short of"
                                " its declared size, which is the documented"
                                " known discrepancy and is part of that hash."
                              : "");
    } else if (ar->state == VM_FW_STATE_MISMATCH) {
        snprintf(ar->detail, sizeof ar->detail,
                 "Extracted %llu bytes, but they are not the known-good %s "
                 "for %s %s. The most likely cause is a key that belongs to a "
                 "different device or build.",
                 (unsigned long long)ar->produced,
                 vm_fw_artefact_filename(which), r->rep->product_type,
                 r->rep->build);
    } else {
        snprintf(ar->detail, sizeof ar->detail,
                 "Extracted %llu bytes. S5LBox has no reference hash for "
                 "%s %s, so this is unpacked but unverified.",
                 (unsigned long long)ar->produced,
                 r->rep->product_type, r->rep->build);
    }

    free(expanded);
    free(raw);
}

/* ------------------------------------------------------------------------ */
/* The root filesystem                                                       */
/* ------------------------------------------------------------------------ */

/*
 * This one cannot be done in a single pass. The member is deflated inside the
 * zip, and a deflate stream is not randomly addressable, but the UDIF chunk
 * table lives at the END of the image and every chunk is addressed by offset.
 * So the member is materialised once, and the two remaining layers -- the
 * encrcdsa decryption and the chunk expansion -- are then fused into one pass
 * over it. That costs 208 MB of temporary space instead of 208 + 208.
 */
static void import_root_filesystem(run_t *r, const vmfw_zip_entry_t *entry,
                                   const vm_fw_keys_t *keys) {
    vm_fw_artefact_report_t *ar = &r->rep->artefacts[VM_FW_ROOT_FILESYSTEM];
    const vm_fw_artefact_t which = VM_FW_ROOT_FILESYSTEM;

    ar->member_size = entry->uncompressed_size;
    set_detail(ar->member, sizeof ar->member, entry->name);

    /*
     * Identify the wrapper cheaply first. Inflating 208 MB to discover that a
     * key is needed would be four minutes spent to deliver bad news that the
     * first 72 bytes already contain.
     */
    uint8_t head[0x48];
    peek_sink_t peek = { head, sizeof head, 0 };
    vmfw_zip_status_t zst = vmfw_zip_extract(&r->zip, entry, peek_write, &peek);
    /* The sink stops the extraction on purpose, so a sink refusal here is
     * success. Anything else is a real archive problem. */
    if (peek.len < sizeof head && zst != VMFW_ZIP_OK) {
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = (zst == VMFW_ZIP_ERR_CRC_MISMATCH)
                   ? VM_FW_ERR_MEMBER_CHECKSUM : VM_FW_ERR_MEMBER_UNREADABLE;
        snprintf(ar->detail, sizeof ar->detail, "This member %s.",
                 vmfw_zip_strerror(zst));
        return;
    }

    bool wrapped = (peek.len >= 8 && memcmp(head, "encrcdsa", 8) == 0);
    ar->encrypted = wrapped;
    ar->key_bits = wrapped ? 128u : 0u;

    if (wrapped && (!keys || !keys->root_present)) {
        ar->state = VM_FW_STATE_NEEDS_KEY;
        ar->reason = VM_FW_ERR_KEY_REQUIRED;
        ar->awaiting_key = true;
        snprintf(ar->detail, sizeof ar->detail,
                 "Located in the IPSW: %llu bytes, an AES-128 encrypted "
                 "disk image. This is the one file whose key cannot be derived "
                 "from the IPSW at all -- it was recovered from hardware and "
                 "published per device and build. Paste the 72-character root "
                 "filesystem key to finish this one.",
                 (unsigned long long)entry->uncompressed_size);
        return;
    }

    if (!r->cfg->files) {
        ar->state = VM_FW_STATE_FOUND;
        snprintf(ar->detail, sizeof ar->detail,
                 "Located: %llu bytes, %s. No destination was opened for it.",
                 (unsigned long long)entry->uncompressed_size,
                 wrapped ? "an encrypted disk image" : "a disk image");
        return;
    }

    /* Materialise the member. */
    void *scratch = r->cfg->files->open(r->cfg->files->ctx, "rootfs.dmg.part");
    if (!scratch) {
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = VM_FW_ERR_SCRATCH_REFUSED;
        set_detail(ar->detail, sizeof ar->detail,
                   vm_fw_strerror(VM_FW_ERR_SCRATCH_REFUSED));
        return;
    }

    report_progress(r, which, VM_FW_STAGE_EXTRACTING, 0,
                    entry->uncompressed_size);

    out_sink_t staging;
    out_sink_init(&staging, r, scratch, which, VM_FW_STAGE_EXTRACTING,
                  entry->uncompressed_size);

    zst = vmfw_zip_extract(&r->zip, entry, out_write, &staging);
    if (zst != VMFW_ZIP_OK) {
        r->cfg->files->close(r->cfg->files->ctx, scratch, false);
        ar->state = VM_FW_STATE_FAILED;
        if (staging.cancel_requested)       ar->reason = VM_FW_ERR_CANCELLED;
        else if (zst == VMFW_ZIP_ERR_CRC_MISMATCH)
                                            ar->reason = VM_FW_ERR_MEMBER_CHECKSUM;
        else if (staging.write_failed)      ar->reason = VM_FW_ERR_SCRATCH_REFUSED;
        else                                ar->reason = VM_FW_ERR_MEMBER_UNREADABLE;
        snprintf(ar->detail, sizeof ar->detail, "This member: %s.",
                 vm_fw_strerror(ar->reason));
        return;
    }

    file_pread_t fp = { r->cfg->files, scratch };
    vmfw_dmg_reader_t reader;
    vmfw_dmg_status_t dst =
        vmfw_dmg_reader_open(&reader, file_pread, &fp,
                             entry->uncompressed_size,
                             (keys && keys->root_present) ? keys->root : NULL,
                             (keys && keys->root_present)
                                 ? VMFW_DMG_KEY_BLOB_SIZE : 0u);
    if (dst != VMFW_DMG_OK) {
        r->cfg->files->close(r->cfg->files->ctx, scratch, false);
        ar->state = (dst == VMFW_DMG_ERR_KEY_REQUIRED) ? VM_FW_STATE_NEEDS_KEY
                                                       : VM_FW_STATE_FAILED;
        ar->awaiting_key = (dst == VMFW_DMG_ERR_KEY_REQUIRED);
        ar->reason = (dst == VMFW_DMG_ERR_KEY_REQUIRED) ? VM_FW_ERR_KEY_REQUIRED
                                                        : VM_FW_ERR_DMG;
        ar->dmg_reason = dst;
        snprintf(ar->detail, sizeof ar->detail, "This disk image: %s.",
                 vmfw_dmg_strerror(dst));
        return;
    }
    ar->key_supplied = wrapped;

    /* Two calls: the first reports how much room the partition table needs. */
    size_t needed = 0;
    vmfw_dmg_info_t info;
    (void)vmfw_dmg_probe(&reader, NULL, 0, &needed, &info);
    if (needed == 0 || needed > MANIFEST_CAP * 8u) {
        r->cfg->files->close(r->cfg->files->ctx, scratch, false);
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = VM_FW_ERR_DMG;
        ar->dmg_reason = VMFW_DMG_ERR_BAD_KOLY;
        /* A wrong key surfaces here first: no koly, so no size. */
        set_detail(ar->detail, sizeof ar->detail,
                   wrapped
                     ? "Decryption did not produce a disk image. That almost "
                       "always means the key belongs to a different device or "
                       "build -- the iPhone1,1 page for the same build carries "
                       "a different root filesystem key."
                     : "This member does not contain a readable disk image.");
        return;
    }

    uint8_t *xml = (uint8_t *)malloc(needed);
    if (!xml) {
        r->cfg->files->close(r->cfg->files->ctx, scratch, false);
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = VM_FW_ERR_OUT_OF_MEMORY;
        set_detail(ar->detail, sizeof ar->detail,
                   vm_fw_strerror(VM_FW_ERR_OUT_OF_MEMORY));
        return;
    }

    dst = vmfw_dmg_probe(&reader, xml, needed, NULL, &info);
    if (dst != VMFW_DMG_OK) {
        free(xml);
        r->cfg->files->close(r->cfg->files->ctx, scratch, false);
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = VM_FW_ERR_DMG;
        ar->dmg_reason = dst;
        snprintf(ar->detail, sizeof ar->detail, "This disk image: %s.",
                 vmfw_dmg_strerror(dst));
        return;
    }

    /*
     * The decrypted image is a whole disk -- driver map, partition map, an
     * ATAPI driver and free space -- and only the Apple_HFSX partition
     * reproduces the accepted hash. Selecting the whole disk instead would
     * produce a 433,317,888-byte file that looks nearly right and is not.
     */
    uint32_t part = 0;
    dst = vmfw_dmg_find_partition(&info, "Apple_HFSX", &part);
    if (dst != VMFW_DMG_OK) {
        free(xml);
        r->cfg->files->close(r->cfg->files->ctx, scratch, false);
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = VM_FW_ERR_NO_ROOT_PARTITION;
        set_detail(ar->detail, sizeof ar->detail,
                   vm_fw_strerror(VM_FW_ERR_NO_ROOT_PARTITION));
        return;
    }

    uint64_t table_bytes = 204u + (uint64_t)info.partitions[part].chunk_count * 40u;
    uint8_t *table = (uint8_t *)malloc((size_t)table_bytes);
    if (!table) {
        free(xml);
        r->cfg->files->close(r->cfg->files->ctx, scratch, false);
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = VM_FW_ERR_OUT_OF_MEMORY;
        set_detail(ar->detail, sizeof ar->detail,
                   vm_fw_strerror(VM_FW_ERR_OUT_OF_MEMORY));
        return;
    }

    void *handle = r->cfg->files->open(r->cfg->files->ctx,
                                       vm_fw_artefact_filename(which));
    if (!handle) {
        free(table);
        free(xml);
        r->cfg->files->close(r->cfg->files->ctx, scratch, false);
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = VM_FW_ERR_OUTPUT_REFUSED;
        set_detail(ar->detail, sizeof ar->detail,
                   vm_fw_strerror(VM_FW_ERR_OUTPUT_REFUSED));
        return;
    }

    uint64_t expect = info.partitions[part].sector_count * VMFW_DMG_SECTOR_SIZE;
    report_progress(r, which, VM_FW_STAGE_EXPANDING, 0, expect);

    out_sink_t o;
    out_sink_init(&o, r, handle, which, VM_FW_STAGE_EXPANDING, expect);

    uint64_t produced = 0;
    dst = vmfw_dmg_extract_partition(&reader, &info, xml, needed, part,
                                     table, (size_t)table_bytes,
                                     out_write, &o, &produced);

    r->cfg->files->close(r->cfg->files->ctx, handle, dst == VMFW_DMG_OK);
    r->cfg->files->close(r->cfg->files->ctx, scratch, false);
    free(table);
    free(xml);

    if (dst != VMFW_DMG_OK) {
        ar->state = VM_FW_STATE_FAILED;
        ar->reason = o.cancel_requested ? VM_FW_ERR_CANCELLED : VM_FW_ERR_DMG;
        ar->dmg_reason = dst;
        snprintf(ar->detail, sizeof ar->detail, "This disk image: %s.",
                 o.cancel_requested ? "cancelled" : vmfw_dmg_strerror(dst));
        return;
    }

    finish_hash(r, ar, &o, which);

    if (ar->state == VM_FW_STATE_VERIFIED) {
        snprintf(ar->detail, sizeof ar->detail,
                 "Decrypted and expanded the Apple_HFSX partition: %llu "
                 "bytes, SHA-256 matches the known-good rootfs.img for this "
                 "build.", (unsigned long long)ar->produced);
    } else if (ar->state == VM_FW_STATE_MISMATCH) {
        snprintf(ar->detail, sizeof ar->detail,
                 "Produced %llu bytes, but they are not the known-good "
                 "rootfs.img for %s %s. The key may belong to a different "
                 "device or build.", (unsigned long long)ar->produced,
                 r->rep->product_type, r->rep->build);
    } else {
        snprintf(ar->detail, sizeof ar->detail,
                 "Produced %llu bytes. S5LBox has no reference hash for "
                 "%s %s, so this is unpacked but unverified.",
                 (unsigned long long)ar->produced,
                 r->rep->product_type, r->rep->build);
    }
}

/* ------------------------------------------------------------------------ */
/* The run                                                                   */
/* ------------------------------------------------------------------------ */

uint64_t vm_fw_import_peak_memory(void) {
    /*
     * A bound for a 3.x IPSW, not a measurement of an arbitrary one: the
     * kernelcache member (4 MB) and its expansion (7.9 MB) are live at the same
     * moment, plus the disk image's partition table. Everything at 433 MB
     * scale streams.
     */
    return 16ull * 1024ull * 1024ull;
}

vm_fw_status_t vm_fw_import_run(const vm_fw_import_t *cfg,
                                vm_fw_report_t *report) {
    if (!report) return VM_FW_ERR_INVALID_ARGUMENT;
    memset(report, 0, sizeof *report);
    for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++)
        report->artefacts[i].state = VM_FW_STATE_NOT_STARTED;

    if (!cfg || !cfg->pread) {
        report->status = VM_FW_ERR_INVALID_ARGUMENT;
        set_detail(report->detail, sizeof report->detail,
                   vm_fw_strerror(VM_FW_ERR_INVALID_ARGUMENT));
        return report->status;
    }

    run_t r;
    memset(&r, 0, sizeof r);
    r.cfg = cfg;
    r.rep = report;

    report_progress(&r, VM_FW_KERNEL, VM_FW_STAGE_OPENING, 0, 0);

    vmfw_zip_status_t zst = vmfw_zip_open(&r.zip, cfg->pread, cfg->pread_ctx,
                                          cfg->size);
    if (zst != VMFW_ZIP_OK) {
        report->status = (zst == VMFW_ZIP_ERR_NOT_ZIP) ? VM_FW_ERR_NOT_AN_ARCHIVE
                       : (zst == VMFW_ZIP_ERR_READ)    ? VM_FW_ERR_ARCHIVE_UNREADABLE
                                                       : VM_FW_ERR_ARCHIVE_MALFORMED;
        snprintf(report->detail, sizeof report->detail, "%s",
                 vmfw_zip_strerror(zst));
        return report->status;
    }

    count_t counter = { 0 };
    (void)vmfw_zip_iterate(&r.zip, count_visit, &counter);
    report->member_count = counter.count;

    report_progress(&r, VM_FW_KERNEL, VM_FW_STAGE_READING_MANIFEST, 0, 0);

    manifest_t man;
    vm_fw_status_t st = read_manifest(&r, &man);
    if (st != VM_FW_OK) {
        report->status = st;
        snprintf(report->detail, sizeof report->detail, "%s", vm_fw_strerror(st));
        return st;
    }

    /*
     * The emulator is an S5L8900 machine. An IPSW for anything else will
     * unpack into files its gate rejects, so the useful answer is the specific
     * one: what this archive is FOR.
     */
    if (report->platform[0] && strcmp(report->platform, "s5l8900x") != 0) {
        report->status = VM_FW_ERR_UNSUPPORTED_DEVICE;
        snprintf(report->detail, sizeof report->detail,
                 "This is %s %s (%s, %s). S5LBox emulates the S5L8900 -- the "
                 "iPhone 2G and 3G -- so this firmware will not run on it.",
                 report->product_type, report->build,
                 report->product_version[0] ? report->product_version : "?",
                 report->platform);
        for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
            report->artefacts[i].state = VM_FW_STATE_FAILED;
            report->artefacts[i].reason = VM_FW_ERR_UNSUPPORTED_DEVICE;
            set_detail(report->artefacts[i].detail,
                       sizeof report->artefacts[i].detail,
                       "Not attempted: this firmware is for a different SoC.");
        }
        return report->status;
    }

    r.ref = find_reference(report->product_type, report->build);
    report->reference_build = (r.ref != NULL);

    report_progress(&r, VM_FW_KERNEL, VM_FW_STAGE_LOCATING, 0, 0);

    /* Kernel. */
    vmfw_zip_entry_t entry;
    bool have = false;
    if (man.kernel_member[0])
        have = (vmfw_zip_find(&r.zip, man.kernel_member, &entry) == VMFW_ZIP_OK);
    if (!have)
        have = find_member_containing(&r, "kernelcache", NULL, NULL, &entry);

    if (!have) {
        vm_fw_artefact_report_t *ar = &report->artefacts[VM_FW_KERNEL];
        ar->state = VM_FW_STATE_NOT_IN_ARCHIVE;
        ar->reason = VM_FW_ERR_MEMBER_MISSING;
        set_detail(ar->detail, sizeof ar->detail,
                   "This archive contains no kernelcache.");
    } else if (cancelled(&r)) {
        report->status = VM_FW_ERR_CANCELLED;
        return report->status;
    } else {
        import_img3_artefact(&r, VM_FW_KERNEL, &entry,
                             cfg->keys ? &cfg->keys->kernel : NULL);
    }

    /*
     * Device tree. Restore.plist does not name it, so this is the one member
     * found by pattern -- but the board tag comes from the manifest, so an
     * archive carrying both an m68ap and an n82ap tree cannot pick the wrong
     * one. `.production` breaks the remaining tie against a development tree.
     */
    have = false;
    if (report->board[0])
        have = find_member_containing(&r, "DeviceTree", report->board,
                                      ".production", &entry);
    if (!have)
        have = find_member_containing(&r, "DeviceTree", NULL, ".production",
                                      &entry);

    if (!have) {
        vm_fw_artefact_report_t *ar = &report->artefacts[VM_FW_DEVICE_TREE];
        ar->state = VM_FW_STATE_NOT_IN_ARCHIVE;
        ar->reason = VM_FW_ERR_MEMBER_MISSING;
        set_detail(ar->detail, sizeof ar->detail,
                   "This archive contains no device tree for this board.");
    } else if (cancelled(&r)) {
        report->status = VM_FW_ERR_CANCELLED;
        return report->status;
    } else {
        import_img3_artefact(&r, VM_FW_DEVICE_TREE, &entry,
                             cfg->keys ? &cfg->keys->device_tree : NULL);
    }

    /* Root filesystem. */
    have = false;
    if (man.root_member[0])
        have = (vmfw_zip_find(&r.zip, man.root_member, &entry) == VMFW_ZIP_OK);

    if (!have) {
        vm_fw_artefact_report_t *ar = &report->artefacts[VM_FW_ROOT_FILESYSTEM];
        ar->state = VM_FW_STATE_NOT_IN_ARCHIVE;
        ar->reason = VM_FW_ERR_MEMBER_MISSING;
        set_detail(ar->detail, sizeof ar->detail,
                   man.root_member[0]
                     ? "The manifest names a root filesystem this archive does "
                       "not contain."
                     : "The manifest does not say which member is the root "
                       "filesystem.");
    } else if (cancelled(&r)) {
        report->status = VM_FW_ERR_CANCELLED;
        return report->status;
    } else {
        import_root_filesystem(&r, &entry, cfg->keys);
    }

    report_progress(&r, VM_FW_ROOT_FILESYSTEM, VM_FW_STAGE_DONE, 0, 0);

    /* Overall summary. Awaiting a key is not a failure -- it is the expected
     * outcome for someone who has not pasted keys yet -- so it does not turn
     * the run's status non-OK. */
    unsigned verified = 0, waiting = 0, failed = 0;
    for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
        switch (report->artefacts[i].state) {
            case VM_FW_STATE_VERIFIED:  verified++; break;
            case VM_FW_STATE_NEEDS_KEY: waiting++;  break;
            case VM_FW_STATE_FAILED:
            case VM_FW_STATE_MISMATCH:
            case VM_FW_STATE_NOT_IN_ARCHIVE: failed++; break;
            default: break;
        }
    }
    snprintf(report->detail, sizeof report->detail,
             "%s %s (%s): %u of 3 verified, %u waiting on a key you supply, "
             "%u could not be produced.",
             report->product_type, report->build,
             report->product_version[0] ? report->product_version : "?",
             verified, waiting, failed);

    report->status = VM_FW_OK;
    return VM_FW_OK;
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

static const char *state_word(vm_fw_state_t s) {
    switch (s) {
        case VM_FW_STATE_NOT_STARTED:    return "not started";
        case VM_FW_STATE_NOT_IN_ARCHIVE: return "not in this IPSW";
        case VM_FW_STATE_FOUND:          return "found";
        case VM_FW_STATE_NEEDS_KEY:      return "needs a key you supply";
        case VM_FW_STATE_EXTRACTED:      return "extracted, unverified";
        case VM_FW_STATE_VERIFIED:       return "verified";
        case VM_FW_STATE_MISMATCH:       return "wrong bytes";
        case VM_FW_STATE_FAILED:         return "failed";
        default:                         return "?";
    }
}

const char *vm_fw_state_word(vm_fw_state_t s);
const char *vm_fw_state_word(vm_fw_state_t s) { return state_word(s); }

size_t vm_fw_report_render(const vm_fw_report_t *rep, char *out, size_t cap) {
    if (!rep) return 0;

    /* Written through a cursor that never advances past `cap`, so a caller can
     * pass a NULL buffer to size the result first. */
    size_t used = 0;
    char scratch[512];

#define EMIT(...) do {                                                     \
        int n_ = snprintf(scratch, sizeof scratch, __VA_ARGS__);           \
        if (n_ > 0) {                                                      \
            size_t n = (size_t)n_;                                         \
            if (n >= sizeof scratch) n = sizeof scratch - 1;               \
            if (out && cap && used < cap - 1) {                            \
                size_t room = cap - 1 - used;                              \
                size_t take = n < room ? n : room;                         \
                memcpy(out + used, scratch, take);                         \
            }                                                              \
            used += n;                                                     \
        }                                                                  \
    } while (0)

    EMIT("IPSW import\n");
    if (rep->manifest_read)
        EMIT("  %s  %s (%s)  board %s  platform %s\n  %u members\n",
             rep->product_type, rep->build,
             rep->product_version[0] ? rep->product_version : "?",
             rep->board[0] ? rep->board : "?",
             rep->platform[0] ? rep->platform : "?",
             rep->member_count);
    else
        EMIT("  the manifest could not be read\n");

    if (rep->status != VM_FW_OK)
        EMIT("  STOPPED: %s\n", vm_fw_strerror(rep->status));
    if (rep->detail[0])
        EMIT("  %s\n", rep->detail);

    for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
        const vm_fw_artefact_report_t *a = &rep->artefacts[i];
        EMIT("\n%s -> %s\n  %s\n",
             vm_fw_artefact_title((vm_fw_artefact_t)i),
             vm_fw_artefact_filename((vm_fw_artefact_t)i),
             state_word(a->state));
        if (a->member[0]) EMIT("  from %s\n", a->member);
        if (a->is_img3)
            EMIT("  IMG3 '%s'%s\n", a->ident,
                 a->encrypted ? ", encrypted" : "");
        if (a->produced) EMIT("  %llu bytes\n", (unsigned long long)a->produced);
        if (a->sha256_valid) {
            char hex[VM_FW_SHA256_LEN * 2 + 1];
            static const char d[] = "0123456789abcdef";
            for (size_t k = 0; k < VM_FW_SHA256_LEN; k++) {
                hex[k * 2]     = d[a->sha256[k] >> 4];
                hex[k * 2 + 1] = d[a->sha256[k] & 0x0fu];
            }
            hex[VM_FW_SHA256_LEN * 2] = '\0';
            EMIT("  sha256 %s\n", hex);
        }
        if (a->detail[0]) EMIT("  %s\n", a->detail);
    }
#undef EMIT

    if (out && cap) {
        size_t term = used < cap - 1 ? used : cap - 1;
        out[term] = '\0';
    }
    return used;
}
