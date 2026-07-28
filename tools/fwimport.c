/*
 * S5LBox — run the app's IPSW importer over a real archive, from a terminal.
 *
 * WHY THIS EXISTS. app/Sources/VMFirmwareImport.c has 1,126 assertions behind
 * it, and every one of them runs against a fixture this project built. That
 * proves the parser does what the parser was written to do; it does not prove
 * it can open Apple's actual 239 MB IPSW, whose central directory, manifest,
 * IMG3 containers and UDIF wrapper were produced by a toolchain nobody here
 * has seen. Those are different claims, and only this one answers the question
 * a user actually asks, which is "does importing my IPSW work".
 *
 * IDENTIFY-ONLY BY DEFAULT, and that is the safe half on purpose: with no
 * output paths it opens nothing for writing, produces no file, and needs no
 * key. It reads the archive, reports the device and build out of Restore.plist,
 * locates every member, parses each container and says what each artefact
 * would still need. That is exactly the state the app's Firmware screen shows
 * a user who has an IPSW and no keys, so a disagreement between this and the
 * app is a bug in one of them.
 *
 * IT NEVER WRITES TO THE ARCHIVE. The file is opened "rb" and only ever pread.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
/*
 * fseeko()/off_t are POSIX, not C11, and this target is built with
 * C_EXTENSIONS NO -- so on a strict-conformance Linux build neither is
 * declared unless the feature test macro asks for them. The Windows arm uses
 * _fseeki64 and needs none of this.
 */
#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  include <sys/types.h>
#endif

#include "VMFirmwareImport.h"

#include <stdio.h>
#include <string.h>

typedef struct { FILE *f; uint64_t size; uint64_t reads, bytes; } src_t;

static size_t src_pread(void *ctx, uint64_t off, uint8_t *buf, size_t len) {
    src_t *s = ctx;
    if (off >= s->size) return 0;
    if (len > s->size - off) len = (size_t)(s->size - off);
#if defined(_WIN32)
    if (_fseeki64(s->f, (long long)off, SEEK_SET) != 0) return 0;
#else
    if (fseeko(s->f, (off_t)off, SEEK_SET) != 0) return 0;
#endif
    size_t got = fread(buf, 1, len, s->f);
    s->reads++;
    s->bytes += got;
    return got;
}


/* ------------------------------------------------------------------------ */
/* Writing the three files out, when keys were supplied                      */
/* ------------------------------------------------------------------------ */
/*
 * The smallest sink that satisfies vm_fw_files_t. It writes into ONE directory
 * given on the command line, and it will not touch anything outside it: the
 * importer only ever asks for the three fixed names, and a name containing a
 * separator or a dot-dot is refused here rather than trusted.
 *
 * It never writes into firmware/ unless told to. The canonical inputs are not
 * this tool's to overwrite.
 */
typedef struct { char dir[1024]; } out_ctx_t;

static bool safe_name(const char *n) {
    if (!n || !*n || *n == '.') return false;
    for (const char *p = n; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') return false;
    return true;
}

static void *out_open(void *ctx, const char *name) {
    out_ctx_t *o = ctx;
    char path[1200];
    if (!safe_name(name)) return NULL;
    snprintf(path, sizeof path, "%s/%s", o->dir, name);
    return fopen(path, "wb");
}
static bool out_write(void *ctx, void *h, const uint8_t *d, size_t n) {
    (void)ctx;
    return h && fwrite(d, 1, n, (FILE *)h) == n;
}
static size_t out_pread(void *ctx, void *h, uint64_t off, uint8_t *b, size_t n) {
    (void)ctx;
    if (!h) return 0;
    if (fflush((FILE *)h) != 0) return 0;
#if defined(_WIN32)
    if (_fseeki64((FILE *)h, (long long)off, SEEK_SET) != 0) return 0;
#else
    if (fseeko((FILE *)h, (off_t)off, SEEK_SET) != 0) return 0;
#endif
    size_t got = fread(b, 1, n, (FILE *)h);
    (void)fseek((FILE *)h, 0, SEEK_END);
    return got;
}
static void out_close(void *ctx, void *h, bool keep) {
    (void)ctx; (void)keep;
    if (h) fclose((FILE *)h);
}

static const char *state_name(vm_fw_state_t st) {
    switch (st) {
        case VM_FW_STATE_NOT_STARTED:     return "not started";
        case VM_FW_STATE_NOT_IN_ARCHIVE:  return "NOT IN ARCHIVE";
        case VM_FW_STATE_FOUND:           return "found (identify only)";
        case VM_FW_STATE_NEEDS_KEY:       return "NEEDS A KEY";
        case VM_FW_STATE_EXTRACTED:       return "extracted";
        case VM_FW_STATE_VERIFIED:        return "VERIFIED against known-good";
        case VM_FW_STATE_MISMATCH:        return "MISMATCH";
        case VM_FW_STATE_FAILED:          return "FAILED";
    }
    return "?";
}

static const char *artefact_name(unsigned i) {
    switch (i) {
        case VM_FW_KERNEL:          return "kernel.macho";
        case VM_FW_DEVICE_TREE:     return "devicetree.bin";
        case VM_FW_ROOT_FILESYSTEM: return "rootfs.img";
    }
    return "?";
}

static void on_progress(void *ctx, vm_fw_artefact_t which,
                        vm_fw_stage_t stage, uint64_t done, uint64_t total) {
    (void)ctx; (void)which; (void)stage; (void)done; (void)total;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: fwimport <archive.ipsw> [options]\n"
            "  --out <dir>            produce the three files there\n"
            "  --kernel <key> <iv>    IMG3 key and IV for the kernelcache\n"
            "  --devicetree <key> <iv>  the same for the device tree\n"
            "  --rootfs <key>         the root filesystem key\n"
            "\n"
            "Keys are taken from the command line and held in memory only.\n"
            "They are never written anywhere, and nor is the archive.\n"
            "\n"
            "Identify-only: reads the archive, reports what is in it and what\n"
            "each artefact would still need. Writes nothing, needs no key, and\n"
            "never opens the archive for writing.\n");
        return 2;
    }

    src_t src;
    memset(&src, 0, sizeof src);
    src.f = fopen(argv[1], "rb");
    if (!src.f) { perror(argv[1]); return 1; }
#if defined(_WIN32)
    _fseeki64(src.f, 0, SEEK_END); src.size = (uint64_t)_ftelli64(src.f);
#else
    fseeko(src.f, 0, SEEK_END); src.size = (uint64_t)ftello(src.f);
#endif

    printf("archive    : %s\n", argv[1]);
    printf("size       : %llu bytes\n", (unsigned long long)src.size);
    printf("peak heap  : %llu bytes if it were asked to produce files\n",
           (unsigned long long)vm_fw_import_peak_memory());

    /* Options. Absent keys leave the run in identify-only, which is the
     * default and the safe half. */
    vm_fw_keys_t keys;
    vm_fw_keys_clear(&keys);
    bool have_keys = false;
    const char *outdir = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            outdir = argv[++i];
        } else if (!strcmp(argv[i], "--kernel") && i + 2 < argc) {
            if (vm_fw_keys_set_img3(&keys, VM_FW_KERNEL, argv[i + 1],
                                    argv[i + 2]) != VM_FW_OK) {
                fprintf(stderr, "fwimport: bad kernel key or iv\n");
                return 2;
            }
            i += 2; have_keys = true;
        } else if (!strcmp(argv[i], "--devicetree") && i + 2 < argc) {
            if (vm_fw_keys_set_img3(&keys, VM_FW_DEVICE_TREE, argv[i + 1],
                                    argv[i + 2]) != VM_FW_OK) {
                fprintf(stderr, "fwimport: bad device tree key or iv\n");
                return 2;
            }
            i += 2; have_keys = true;
        } else if (!strcmp(argv[i], "--rootfs") && i + 1 < argc) {
            if (vm_fw_keys_set_root(&keys, argv[++i]) != VM_FW_OK) {
                fprintf(stderr, "fwimport: bad root filesystem key\n");
                return 2;
            }
            have_keys = true;
        } else {
            fprintf(stderr, "fwimport: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    out_ctx_t octx;
    vm_fw_files_t files;
    memset(&octx, 0, sizeof octx);
    memset(&files, 0, sizeof files);
    if (outdir) {
        snprintf(octx.dir, sizeof octx.dir, "%s", outdir);
        files.open = out_open; files.write = out_write;
        files.pread = out_pread; files.close = out_close;
        files.ctx = &octx;
    }

    vm_fw_import_t in;
    memset(&in, 0, sizeof in);
    in.pread       = src_pread;
    in.pread_ctx   = &src;
    in.size        = src.size;
    in.files       = outdir ? &files : NULL;
    in.keys        = have_keys ? &keys : NULL;
    in.progress    = on_progress;

    vm_fw_report_t rep;
    memset(&rep, 0, sizeof rep);
    vm_fw_status_t st = vm_fw_import_run(&in, &rep);

    printf("status     : %d (%s)\n", (int)st,
           st == VM_FW_OK ? "ran to completion" : "refused");
    printf("reads      : %llu, %llu bytes\n",
           (unsigned long long)src.reads, (unsigned long long)src.bytes);
    printf("manifest   : %s\n", rep.manifest_read ? "read" : "NOT READ");
    if (rep.manifest_read) {
        printf("  product  : %s  %s  build %s\n",
               rep.product_type, rep.product_version, rep.build);
        printf("  board    : %s  platform %s\n", rep.board, rep.platform);
        printf("  known    : %s\n", rep.reference_build
               ? "yes -- reference hashes exist, so VERIFIED is reachable"
               : "no reference hashes for this build");
    }
    printf("members    : %u\n", rep.member_count);
    if (rep.detail[0]) printf("detail     : %s\n", rep.detail);

    puts("");
    for (unsigned i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
        const vm_fw_artefact_report_t *a = &rep.artefacts[i];
        printf("%-15s %s\n", artefact_name(i), state_name(a->state));
        if (a->member[0]) printf("    member   %s\n", a->member);
        if (a->is_img3)
            printf("    IMG3     ident '%.4s'  %s  %u-bit key\n",
                   a->ident, a->encrypted ? "ENCRYPTED" : "plaintext",
                   a->key_bits);
        if (a->awaiting_key)
            printf("    -> this one is waiting on a key you supply\n");
        if (a->detail[0]) printf("    detail   %s\n", a->detail);
    }

    /* The keys go out of scope here and are in no file, no log and no
     * environment variable -- the same promise the app makes. */
    vm_fw_keys_clear(&keys);
    fclose(src.f);
    return st == VM_FW_OK ? 0 : 1;
}
