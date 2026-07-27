/*
 * S5LBox — turning a user's own IPSW into the three files the emulator accepts.
 *
 * WHAT THIS REPLACES. Until now the app's Firmware section listed three file
 * names and asked the user to produce them by hand, which in practice meant
 * following docs/BOOT_CHAIN.md on a desktop with five command-line tools. This
 * does the parts of that a program can do: open the archive, read Apple's own
 * manifest to learn which member is which, unwrap the IMG3 containers,
 * decompress the kernel, expand the root filesystem's partition, hash
 * everything and say what came out.
 *
 * WHAT IT CANNOT DO, STATED FIRST BECAUSE IT IS THE WHOLE SHAPE OF THIS FILE.
 * Every payload in a 3.x IPSW is AES-encrypted, and the keys are NOT in the
 * IPSW. They are not derivable from it either -- they were recovered from the
 * hardware by researchers and published per build and per device. Measured on
 * the real 7E18 iPhone1,2 archive: the kernelcache, the device tree, iBoot and
 * every boot image carry a KBAG with cryptState 1 and 128-bit keys, and the
 * key/IV stored in that KBAG do not decrypt the payload -- they are wrapped, so
 * decrypting with them produces noise rather than "complzss".
 *
 * So this code:
 *   - ships no keys, embeds no key table, and performs no network access;
 *   - accepts keys the USER supplies, and labels them that way everywhere;
 *   - when a key is absent, says exactly which artefact needs one and what kind
 *     it is, rather than failing vaguely or hanging.
 *
 * A user with no keys still gets: the archive opened, the device and build
 * identified from Restore.plist, every member located, each container parsed,
 * and a precise list of what remains. A user with keys gets the three files,
 * verified byte-for-byte against known-good hashes when the build is one we
 * have hashes for.
 *
 * WHY IT IS PLAIN C. Same reason as VMTouchMap, VMOptions, VMTouchQueue,
 * VMInstances and VMButtonQueue: this is where silent wrongness lives, every
 * length in it comes from a file off the internet, and a host CI runner can
 * test it. No UIKit, no Foundation. The Objective-C shell is a caller.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_VM_FIRMWARE_IMPORT_H
#define S5LBOX_VM_FIRMWARE_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "VMFirmwareFormats.h"

/* ------------------------------------------------------------------------ */
/* What we are trying to produce                                             */
/* ------------------------------------------------------------------------ */
/*
 * The emulator accepts exactly three files, gated on size and SHA-256 before
 * anything is opened. These are them, and the names are the ones
 * docs/BOOT_CHAIN.md's regeneration procedure writes.
 */
typedef enum {
    VM_FW_KERNEL = 0,
    VM_FW_DEVICE_TREE,
    VM_FW_ROOT_FILESYSTEM,
    VM_FW_ARTEFACT_COUNT
} vm_fw_artefact_t;

/* "kernel.macho", "devicetree.bin", "rootfs.img". */
const char *vm_fw_artefact_filename(vm_fw_artefact_t which);
/* "Kernel", "Device tree", "Root filesystem" -- for a table row. */
const char *vm_fw_artefact_title(vm_fw_artefact_t which);

/* ------------------------------------------------------------------------ */
/* Outcomes                                                                  */
/* ------------------------------------------------------------------------ */
/*
 * Separated from the status below on purpose. The state is what the user sees
 * on a row; the status is why. "Needs a key" is not a failure and must not be
 * presented as one -- it is the expected outcome for a user who has not pasted
 * keys yet, and it is actionable.
 */
typedef enum {
    VM_FW_STATE_NOT_STARTED = 0,
    VM_FW_STATE_NOT_IN_ARCHIVE, /* the manifest or directory does not have it */
    VM_FW_STATE_FOUND,          /* located; nothing produced (no output sink) */
    VM_FW_STATE_NEEDS_KEY,      /* located, encrypted, and no key was given   */
    VM_FW_STATE_EXTRACTED,      /* produced, but no reference hash to check   */
    VM_FW_STATE_VERIFIED,       /* produced AND identical to the known-good   */
    VM_FW_STATE_MISMATCH,       /* produced AND not the known-good bytes      */
    VM_FW_STATE_FAILED          /* refused; `reason` says which refusal       */
} vm_fw_state_t;

/*
 * A named reason for every refusal. Nothing in this file returns a generic
 * failure: a user who is told "could not import" learns nothing, and a
 * developer reading a bug report learns less.
 */
typedef enum {
    VM_FW_OK = 0,
    VM_FW_ERR_INVALID_ARGUMENT,
    VM_FW_ERR_OUT_OF_MEMORY,
    VM_FW_ERR_CANCELLED,

    /* the archive */
    VM_FW_ERR_NOT_AN_ARCHIVE,
    VM_FW_ERR_ARCHIVE_UNREADABLE,
    VM_FW_ERR_ARCHIVE_MALFORMED,

    /* the manifest */
    VM_FW_ERR_NO_MANIFEST,        /* no Restore.plist                        */
    VM_FW_ERR_MANIFEST_TOO_BIG,
    VM_FW_ERR_MANIFEST_MALFORMED,
    VM_FW_ERR_MANIFEST_INCOMPLETE,/* parses, but does not name what we need  */
    VM_FW_ERR_UNSUPPORTED_DEVICE, /* not an S5L8900 iPhone                   */

    /* a member */
    VM_FW_ERR_MEMBER_MISSING,
    VM_FW_ERR_MEMBER_UNREADABLE,
    VM_FW_ERR_MEMBER_CHECKSUM,    /* the zip's own CRC disagrees             */
    VM_FW_ERR_MEMBER_TOO_BIG,     /* larger than we will hold in memory      */

    /* the containers */
    VM_FW_ERR_NOT_IMG3,
    VM_FW_ERR_IMG3_MALFORMED,
    VM_FW_ERR_IMG3_NO_PAYLOAD,    /* parsed, but carries no DATA tag         */
    VM_FW_ERR_KEY_REQUIRED,       /* encrypted and no key was supplied       */
    VM_FW_ERR_KEY_WRONG_LENGTH,
    VM_FW_ERR_KEY_NOT_HEX,
    VM_FW_ERR_DECRYPT_FAILED,
    VM_FW_ERR_NOT_COMPRESSED,     /* decrypted, but not a complzss blob      */
    VM_FW_ERR_DECOMPRESS_FAILED,

    /* the disk image */
    VM_FW_ERR_DMG,                /* see the artefact's dmg_reason           */
    VM_FW_ERR_NO_ROOT_PARTITION,

    /* output */
    VM_FW_ERR_OUTPUT_REFUSED,
    VM_FW_ERR_SCRATCH_REFUSED
} vm_fw_status_t;

const char *vm_fw_strerror(vm_fw_status_t st);

/* ------------------------------------------------------------------------ */
/* Keys the user supplies                                                    */
/* ------------------------------------------------------------------------ */
/*
 * There is deliberately no loader here: no file format, no bundled table, no
 * fetch. The only way a key enters this program is a caller passing one in,
 * and the only way it enters the app is the user typing or pasting it.
 *
 * The IMG3 artefacts take a key and an IV as separate hex strings, because the
 * published values are separate and the IV in the container is a different,
 * wrapped value that would silently corrupt the first block. The root
 * filesystem takes one 72-character string, which is a 16-byte AES key followed
 * by a 20-byte HMAC key.
 */
#define VM_FW_MAX_KEY_BYTES 32u

typedef struct {
    bool     present;
    unsigned key_bits;                    /* 128, 192 or 256                 */
    uint8_t  key[VM_FW_MAX_KEY_BYTES];
    uint8_t  iv[16];
} vm_fw_img3_key_t;

typedef struct {
    vm_fw_img3_key_t kernel;
    vm_fw_img3_key_t device_tree;
    bool             root_present;
    uint8_t          root[VMFW_DMG_KEY_BLOB_SIZE];
} vm_fw_keys_t;

void vm_fw_keys_clear(vm_fw_keys_t *keys);

/*
 * Parse a user-typed key. Whitespace anywhere is ignored, case does not matter,
 * and anything else is refused -- a key that was silently trimmed to a valid
 * prefix would decrypt into garbage and be blamed on the IPSW.
 */
vm_fw_status_t vm_fw_keys_set_img3(vm_fw_keys_t *keys, vm_fw_artefact_t which,
                                   const char *key_hex, const char *iv_hex);
vm_fw_status_t vm_fw_keys_set_root(vm_fw_keys_t *keys, const char *key_hex);

/* Shared by the two above; exposed because the UI wants to validate as the
 * user types rather than only on submit. `want` is the exact byte count. */
vm_fw_status_t vm_fw_parse_hex(const char *text, uint8_t *out, size_t want);

/* ------------------------------------------------------------------------ */
/* Where the results go                                                      */
/* ------------------------------------------------------------------------ */
/*
 * A four-call file abstraction rather than stdio, so the core has no file API
 * in it and the tests can run the whole pipeline -- including the 433 MB root
 * filesystem path -- against memory.
 *
 * `open` receives the file name the artefact should end up as, or a name ending
 * in ".part" for the one large intermediate. Returning NULL means "do not
 * produce this", which is how a caller asks for identification only; that is
 * reported as VM_FW_STATE_FOUND and is not an error.
 *
 * `close` is called exactly once per successful `open`. `keep` is false when
 * the file is an intermediate or when production failed partway, and the
 * implementation is expected to delete it -- a half-written rootfs.img left in
 * the firmware directory is worse than none, because the emulator's own gate
 * would reject it only after the user had trusted it.
 */
typedef struct {
    void  *(*open)(void *ctx, const char *name);
    bool   (*write)(void *ctx, void *handle, const uint8_t *data, size_t len);
    size_t (*pread)(void *ctx, void *handle, uint64_t offset,
                    uint8_t *buf, size_t len);
    void   (*close)(void *ctx, void *handle, bool keep);
    void  *ctx;
} vm_fw_files_t;

/* ------------------------------------------------------------------------ */
/* Progress and cancellation                                                 */
/* ------------------------------------------------------------------------ */
/*
 * Import must not block the main thread, so the app runs it on a queue and
 * needs both a way to draw a bar and a way to stop. `total` is 0 when the
 * amount of work is not yet known.
 */
typedef enum {
    VM_FW_STAGE_OPENING = 0,
    VM_FW_STAGE_READING_MANIFEST,
    VM_FW_STAGE_LOCATING,
    VM_FW_STAGE_EXTRACTING,
    VM_FW_STAGE_DECRYPTING,
    VM_FW_STAGE_DECOMPRESSING,
    VM_FW_STAGE_EXPANDING,
    VM_FW_STAGE_VERIFYING,
    VM_FW_STAGE_DONE
} vm_fw_stage_t;

const char *vm_fw_stage_name(vm_fw_stage_t stage);

typedef void (*vm_fw_progress_fn)(void *ctx, vm_fw_artefact_t which,
                                  vm_fw_stage_t stage,
                                  uint64_t done, uint64_t total);

/* Polled between units of work. Returning true stops the import with
 * VM_FW_ERR_CANCELLED and every partly-written output discarded. */
typedef bool (*vm_fw_cancel_fn)(void *ctx);

/* ------------------------------------------------------------------------ */
/* The report                                                                */
/* ------------------------------------------------------------------------ */
#define VM_FW_DETAIL_LEN 320u
#define VM_FW_SHA256_LEN 32u

typedef struct {
    vm_fw_state_t  state;
    vm_fw_status_t reason;

    /* The IPSW member this came from, as named in the archive. Empty if the
     * manifest never named one. */
    char member[VMFW_ZIP_MAX_NAME];

    /* One sentence in plain language: what happened, and if something is
     * still needed, what the user can do about it. Always NUL-terminated. */
    char detail[VM_FW_DETAIL_LEN];

    uint64_t member_size;       /* bytes in the archive, uncompressed        */
    uint64_t produced;          /* bytes written out                         */

    /* IMG3 facts, when the member was one. */
    bool     is_img3;
    char     ident[8];          /* "krnl", "dtre"                            */
    bool     encrypted;
    unsigned key_bits;
    bool     key_supplied;

    /* True when a key was needed and none was given. The UI keys its
     * "paste a key" affordance off this rather than off the state, so a row
     * that failed for a different reason does not offer the wrong fix. */
    bool     awaiting_key;

    uint8_t  sha256[VM_FW_SHA256_LEN];
    bool     sha256_valid;
    bool     reference_known;   /* we have a known-good hash for this build  */
    bool     matches_reference;

    /* Non-zero when `reason` is VM_FW_ERR_DMG, so the disk-image layer's own
     * named refusal is not flattened into one status. */
    vmfw_dmg_status_t dmg_reason;
} vm_fw_artefact_report_t;

typedef struct {
    vm_fw_status_t status;

    bool     manifest_read;
    char     product_type[32];      /* "iPhone1,2"  */
    char     product_version[32];   /* "3.1.3"      */
    char     build[16];             /* "7E18"       */
    char     board[16];             /* "n82ap"      */
    char     platform[16];          /* "s5l8900x"   */

    /* True when this build is one we hold reference hashes for, so a VERIFIED
     * state is possible at all. Everything still works without it; the
     * artefacts just come out EXTRACTED. */
    bool     reference_build;

    uint32_t member_count;
    char     detail[VM_FW_DETAIL_LEN];

    vm_fw_artefact_report_t artefacts[VM_FW_ARTEFACT_COUNT];
} vm_fw_report_t;

/* ------------------------------------------------------------------------ */
/* Running it                                                                */
/* ------------------------------------------------------------------------ */
typedef struct {
    /* The IPSW, as random access. The app wraps a file descriptor. */
    vmfw_pread_fn pread;
    void         *pread_ctx;
    uint64_t      size;

    /* Optional. NULL `files` means identify only. */
    const vm_fw_files_t *files;

    /* Optional. NULL means no key is available for anything. */
    const vm_fw_keys_t *keys;

    vm_fw_progress_fn progress;
    void             *progress_ctx;
    vm_fw_cancel_fn   cancel;
    void             *cancel_ctx;
} vm_fw_import_t;

/*
 * Run the whole import. Always fills `report` -- including on failure, because
 * the per-artefact detail is the entire point and a caller that got only a
 * status code would have to guess which of the three went wrong.
 *
 * Returns the overall status. That status is VM_FW_OK when the import ran to
 * completion, EVEN IF artefacts are still awaiting keys: not having a key is a
 * state of the world, not a malfunction. Callers decide what to show from the
 * per-artefact states.
 */
vm_fw_status_t vm_fw_import_run(const vm_fw_import_t *import,
                                vm_fw_report_t *report);

/*
 * Peak heap the run will ask for, in bytes, so a caller can decide whether to
 * attempt it. Dominated by the kernel path, which holds the 4 MB member and its
 * 7.9 MB expansion at once.
 */
uint64_t vm_fw_import_peak_memory(void);

/* Render a report as plain text, for the results screen and for a bug report.
 * Returns the number of characters that would have been written. */
size_t vm_fw_report_render(const vm_fw_report_t *report, char *out, size_t cap);

/*
 * Does a produced artefact match a reference?
 *
 * Exposed rather than left inline because it is the single decision that turns
 * "we unpacked something" into "this is the file the emulator accepts", and an
 * inline version cannot be tested: driving it through a whole import can only
 * ever supply digests that differ everywhere, so a comparison shortened to
 * sixteen bytes -- or one that stopped checking the length -- would pass every
 * end-to-end test ever written. Constructing that case needs crafted inputs,
 * and crafted inputs need a callable function.
 *
 * Both the length and the whole digest must match. The length is not
 * redundant: it is the only half of the pair that a caller can check cheaply
 * while a 433 MB file is still being written.
 */
bool vm_fw_reference_matches(uint64_t produced_size,
                             const uint8_t produced_sha256[VM_FW_SHA256_LEN],
                             uint64_t reference_size,
                             const uint8_t reference_sha256[VM_FW_SHA256_LEN]);

#endif /* S5LBOX_VM_FIRMWARE_IMPORT_H */
