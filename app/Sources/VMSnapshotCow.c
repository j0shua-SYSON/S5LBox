/*
 * S5LBox — copy-on-write overlays. See VMSnapshotCow.h for the contract and
 * for why replay runs newest-first.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMSnapshotCow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The on-disk header, written little-endian field by field rather than by
 * dumping a struct: this file is produced on a phone and may well be read back
 * by a host test, and a struct layout is a promise the compiler makes to
 * itself, not to another toolchain.
 */
#define VM_COW_HEADER_BYTES   32u
#define VM_COW_INDEX_BYTES     8u
#define VM_COW_RECORD_BYTES   (VM_COW_INDEX_BYTES + VM_COW_BLOCK_BYTES)

struct vm_cow {
    vm_block_t  facade;      /* what the machine is given                   */
    vm_block_t  under;       /* a copy, so the caller's may go out of scope */
    FILE       *overlay;
    uint64_t    overlay_bytes;
    uint64_t    image_size;
    uint64_t    block_count;
    uint8_t    *saved;       /* one bit per block: already recorded         */
    uint8_t    *scratch;     /* one block, for read-before-write            */
    bool        poisoned;    /* a save failed; refuse further writes        */
};

const char *vm_cow_status_text(vm_cow_status_t status) {
    switch (status) {
        case VM_COW_OK:            return "ok";
        case VM_COW_BAD_ARGUMENT:  return "bad argument";
        case VM_COW_IO:            return "input/output error";
        case VM_COW_BAD_FORMAT:    return "not a snapshot overlay";
        case VM_COW_SIZE_MISMATCH: return "overlay belongs to a different disk";
        case VM_COW_TRUNCATED:     return "overlay ends mid-record";
        case VM_COW_OUT_OF_MEMORY: return "out of memory";
        default:                   return "unknown status";
    }
}

static void say(char *detail, size_t capacity, const char *text) {
    if (!detail || capacity == 0u) return;
    size_t n = strlen(text);
    if (n >= capacity) n = capacity - 1u;
    memcpy(detail, text, n);
    detail[n] = '\0';
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_le64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8u; i++) p[i] = (uint8_t)(v >> (i * 8u));
}
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; i++) v |= (uint64_t)p[i] << (i * 8u);
    return v;
}

/* ------------------------------------------------ exact block-layer I/O ---
 *
 * The descriptor may satisfy part of a request and may ask to be retried, so
 * every access loops. A short read that was treated as complete would record a
 * block padded with whatever was already in the scratch buffer, and that
 * corruption would only appear when the snapshot was restored -- long after
 * anything could point at the cause.
 */
static bool read_exact(const vm_block_t *b, uint64_t offset,
                       void *dst, size_t want) {
    uint8_t *p = (uint8_t *)dst;
    size_t done = 0;
    unsigned stalls = 0;
    while (done < want) {
        size_t actual = 0;
        vm_block_io_status_t st =
            b->read_at(b->context, offset + done, p + done, want - done, &actual);
        if (st == VM_BLOCK_IO_ERROR) return false;
        if (actual == 0u) {
            /* RETRY forever is a hang, not a retry. */
            if (++stalls > 64u) return false;
            continue;
        }
        stalls = 0;
        done += actual;
    }
    return true;
}

static bool write_exact(const vm_block_t *b, uint64_t offset,
                        const void *src, size_t want, size_t *actual_out) {
    const uint8_t *p = (const uint8_t *)src;
    size_t done = 0;
    unsigned stalls = 0;
    while (done < want) {
        size_t actual = 0;
        vm_block_io_status_t st =
            b->write_at(b->context, offset + done, p + done, want - done, &actual);
        if (st == VM_BLOCK_IO_ERROR) { if (actual_out) *actual_out = done; return false; }
        if (actual == 0u) {
            if (++stalls > 64u) { if (actual_out) *actual_out = done; return false; }
            continue;
        }
        stalls = 0;
        done += actual;
    }
    if (actual_out) *actual_out = done;
    return true;
}

/* ----------------------------------------------------------- the bitmap --- */

static bool bit_get(const uint8_t *map, uint64_t index) {
    return (map[index >> 3] >> (index & 7u)) & 1u;
}
static void bit_set(uint8_t *map, uint64_t index) {
    map[index >> 3] |= (uint8_t)(1u << (index & 7u));
}

/*
 * Rebuild the bitmap from an overlay that already exists. Appending to a
 * snapshot after the app was relaunched has to know which blocks are already
 * in it; without this every block would be saved a second time, and the
 * SECOND copy -- the newer, wrong one -- would be the one replay applied last.
 */
static vm_cow_status_t index_existing(vm_cow_t *cow, char *detail, size_t cap) {
    if (fseek(cow->overlay, 0, SEEK_END) != 0) {
        say(detail, cap, "could not measure the overlay");
        return VM_COW_IO;
    }
    long end = ftell(cow->overlay);
    if (end < 0) { say(detail, cap, "could not measure the overlay"); return VM_COW_IO; }
    uint64_t size = (uint64_t)end;
    if (size < VM_COW_HEADER_BYTES) return VM_COW_OK;   /* header only */

    uint64_t body = size - VM_COW_HEADER_BYTES;
    if (body % VM_COW_RECORD_BYTES != 0u) {
        say(detail, cap, "the overlay ends mid-record");
        return VM_COW_TRUNCATED;
    }
    uint64_t records = body / VM_COW_RECORD_BYTES;
    for (uint64_t i = 0; i < records; i++) {
        uint64_t at = VM_COW_HEADER_BYTES + i * VM_COW_RECORD_BYTES;
        if (fseek(cow->overlay, (long)at, SEEK_SET) != 0) return VM_COW_IO;
        uint8_t idx[VM_COW_INDEX_BYTES];
        if (fread(idx, 1, sizeof idx, cow->overlay) != sizeof idx) return VM_COW_IO;
        uint64_t block = get_le64(idx);
        if (block >= cow->block_count) {
            say(detail, cap, "the overlay names a block outside the disk");
            return VM_COW_SIZE_MISMATCH;
        }
        bit_set(cow->saved, block);
    }
    cow->overlay_bytes = size;
    return VM_COW_OK;
}

/* ------------------------------------------------------- the facade I/O --- */

static vm_block_io_status_t cow_read_at(void *context, uint64_t offset,
                                        void *destination, size_t requested,
                                        size_t *actual) {
    vm_cow_t *cow = (vm_cow_t *)context;
    /* Straight through: the live image is always current, and a read that
     * consulted the overlay would be answering with the past. */
    return cow->under.read_at(cow->under.context, offset, destination,
                              requested, actual);
}

/*
 * Save first, then write. The order is the whole point: once the new bytes are
 * down, the old ones are gone and every snapshot that needed them is
 * unrestorable, so a failure to save must STOP the write rather than be
 * reported alongside it.
 */
static vm_block_io_status_t cow_write_at(void *context, uint64_t offset,
                                         const void *source, size_t requested,
                                         size_t *actual) {
    vm_cow_t *cow = (vm_cow_t *)context;
    if (actual) *actual = 0;
    if (cow->poisoned) return VM_BLOCK_IO_ERROR;
    if (requested == 0u) return VM_BLOCK_IO_OK;

    uint64_t first = offset / VM_COW_BLOCK_BYTES;
    uint64_t last  = (offset + (uint64_t)requested - 1u) / VM_COW_BLOCK_BYTES;
    if (last >= cow->block_count) return VM_BLOCK_IO_ERROR;

    for (uint64_t b = first; b <= last; b++) {
        if (bit_get(cow->saved, b)) continue;
        uint64_t at = b * (uint64_t)VM_COW_BLOCK_BYTES;
        if (!read_exact(&cow->under, at, cow->scratch, VM_COW_BLOCK_BYTES)) {
            cow->poisoned = true;
            return VM_BLOCK_IO_ERROR;
        }
        uint8_t idx[VM_COW_INDEX_BYTES];
        put_le64(idx, b);
        if (fwrite(idx, 1, sizeof idx, cow->overlay) != sizeof idx ||
            fwrite(cow->scratch, 1, VM_COW_BLOCK_BYTES, cow->overlay)
                != VM_COW_BLOCK_BYTES) {
            /* A half-written record is why replay checks for a trailing
             * partial rather than trusting the file's length. */
            cow->poisoned = true;
            return VM_BLOCK_IO_ERROR;
        }
        bit_set(cow->saved, b);
        cow->overlay_bytes += VM_COW_RECORD_BYTES;
    }

    size_t done = 0;
    bool ok = write_exact(&cow->under, offset, source, requested, &done);
    if (actual) *actual = done;
    return ok ? VM_BLOCK_IO_OK : VM_BLOCK_IO_ERROR;
}

static vm_block_io_status_t cow_flush(void *context) {
    vm_cow_t *cow = (vm_cow_t *)context;
    /* Overlay first. An image that is durable while its overlay is not
     * describes a past that never happened, and that is precisely the state a
     * restore would then trust. */
    if (fflush(cow->overlay) != 0) return VM_BLOCK_IO_ERROR;
    return cow->under.flush ? cow->under.flush(cow->under.context)
                            : VM_BLOCK_IO_OK;
}

/* ------------------------------------------------------------- lifetime --- */

vm_cow_status_t vm_cow_open(vm_cow_t **out, const vm_block_t *under,
                            const char *overlay_path,
                            char *detail, size_t cap) {
    if (!out || !under || !overlay_path || !under->read_at || !under->write_at)
        return VM_COW_BAD_ARGUMENT;
    if (under->size == 0u || (under->size % VM_COW_BLOCK_BYTES) != 0u) {
        say(detail, cap, "the disk is not a whole number of blocks");
        return VM_COW_BAD_ARGUMENT;
    }
    *out = NULL;

    vm_cow_t *cow = (vm_cow_t *)calloc(1, sizeof *cow);
    if (!cow) return VM_COW_OUT_OF_MEMORY;
    cow->under = *under;
    cow->image_size = under->size;
    cow->block_count = under->size / VM_COW_BLOCK_BYTES;
    cow->scratch = (uint8_t *)malloc(VM_COW_BLOCK_BYTES);
    cow->saved = (uint8_t *)calloc((size_t)((cow->block_count + 7u) / 8u), 1u);
    if (!cow->scratch || !cow->saved) {
        free(cow->scratch); free(cow->saved); free(cow);
        return VM_COW_OUT_OF_MEMORY;
    }

    /* "r+b" then fall back to "w+b": an existing overlay is APPENDED to, so a
     * session resumed after a relaunch keeps recording into the same snapshot
     * instead of starting a second one that only holds half the history. */
    bool fresh = false;
    cow->overlay = fopen(overlay_path, "r+b");
    if (!cow->overlay) { cow->overlay = fopen(overlay_path, "w+b"); fresh = true; }
    if (!cow->overlay) {
        free(cow->scratch); free(cow->saved); free(cow);
        say(detail, cap, "could not open the overlay file");
        return VM_COW_IO;
    }

    if (fresh) {
        uint8_t header[VM_COW_HEADER_BYTES];
        memset(header, 0, sizeof header);
        memcpy(header, VM_COW_MAGIC, strlen(VM_COW_MAGIC));
        put_le32(header + 12, VM_COW_VERSION);
        put_le32(header + 16, VM_COW_BLOCK_BYTES);
        put_le64(header + 20, cow->image_size);
        if (fwrite(header, 1, sizeof header, cow->overlay) != sizeof header) {
            fclose(cow->overlay);
            free(cow->scratch); free(cow->saved); free(cow);
            say(detail, cap, "could not write the overlay header");
            return VM_COW_IO;
        }
        cow->overlay_bytes = VM_COW_HEADER_BYTES;
    } else {
        uint8_t header[VM_COW_HEADER_BYTES];
        if (fread(header, 1, sizeof header, cow->overlay) != sizeof header ||
            memcmp(header, VM_COW_MAGIC, strlen(VM_COW_MAGIC)) != 0 ||
            get_le32(header + 12) != VM_COW_VERSION ||
            get_le32(header + 16) != VM_COW_BLOCK_BYTES) {
            fclose(cow->overlay);
            free(cow->scratch); free(cow->saved); free(cow);
            say(detail, cap, "that file is not a snapshot overlay");
            return VM_COW_BAD_FORMAT;
        }
        if (get_le64(header + 20) != cow->image_size) {
            fclose(cow->overlay);
            free(cow->scratch); free(cow->saved); free(cow);
            say(detail, cap, "the overlay was made against a different disk");
            return VM_COW_SIZE_MISMATCH;
        }
        vm_cow_status_t st = index_existing(cow, detail, cap);
        if (st != VM_COW_OK) {
            fclose(cow->overlay);
            free(cow->scratch); free(cow->saved); free(cow);
            return st;
        }
    }

    if (fseek(cow->overlay, 0, SEEK_END) != 0) {
        fclose(cow->overlay);
        free(cow->scratch); free(cow->saved); free(cow);
        return VM_COW_IO;
    }

    cow->facade.context    = cow;
    cow->facade.size       = under->size;
    /* Deliberately NOT the underlying tokens. A block whose contents can be
     * rewound is a different incarnation from one that cannot, and vm_block.h
     * is explicit that zero means "unavailable" rather than "same". */
    cow->facade.identity   = 0;
    cow->facade.generation = 0;
    cow->facade.read_at    = cow_read_at;
    cow->facade.write_at   = cow_write_at;
    cow->facade.flush      = cow_flush;

    *out = cow;
    return VM_COW_OK;
}

const vm_block_t *vm_cow_block(const vm_cow_t *cow) {
    return cow ? &cow->facade : NULL;
}

uint64_t vm_cow_overlay_bytes(const vm_cow_t *cow) {
    return cow ? cow->overlay_bytes : 0u;
}

vm_cow_status_t vm_cow_flush(vm_cow_t *cow) {
    if (!cow) return VM_COW_BAD_ARGUMENT;
    return cow_flush(cow) == VM_BLOCK_IO_OK ? VM_COW_OK : VM_COW_IO;
}

vm_cow_status_t vm_cow_close(vm_cow_t **slot) {
    if (!slot || !*slot) return VM_COW_BAD_ARGUMENT;
    vm_cow_t *cow = *slot;
    vm_cow_status_t st = VM_COW_OK;
    if (fflush(cow->overlay) != 0) st = VM_COW_IO;
    /* fclose is where a buffered write finally fails, so its result is part of
     * the answer rather than cleanup. */
    if (fclose(cow->overlay) != 0) st = VM_COW_IO;
    free(cow->scratch);
    free(cow->saved);
    free(cow);
    *slot = NULL;
    return st;
}

/* --------------------------------------------------------------- replay --- */

vm_cow_status_t vm_cow_replay(const char *overlay_path, const vm_block_t *onto,
                              void (*progress)(void *, uint64_t, uint64_t),
                              void *progress_ctx,
                              char *detail, size_t cap) {
    if (!overlay_path || !onto || !onto->write_at) return VM_COW_BAD_ARGUMENT;

    FILE *f = fopen(overlay_path, "rb");
    if (!f) { say(detail, cap, "could not open the overlay"); return VM_COW_IO; }

    uint8_t header[VM_COW_HEADER_BYTES];
    if (fread(header, 1, sizeof header, f) != sizeof header ||
        memcmp(header, VM_COW_MAGIC, strlen(VM_COW_MAGIC)) != 0 ||
        get_le32(header + 12) != VM_COW_VERSION ||
        get_le32(header + 16) != VM_COW_BLOCK_BYTES) {
        fclose(f);
        say(detail, cap, "that file is not a snapshot overlay");
        return VM_COW_BAD_FORMAT;
    }
    if (get_le64(header + 20) != onto->size) {
        fclose(f);
        say(detail, cap, "the overlay was made against a different disk");
        return VM_COW_SIZE_MISMATCH;
    }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return VM_COW_IO; }
    long end = ftell(f);
    if (end < 0) { fclose(f); return VM_COW_IO; }
    uint64_t body = (uint64_t)end - VM_COW_HEADER_BYTES;
    if (body % VM_COW_RECORD_BYTES != 0u) {
        /* Reported, never skipped. A trailing partial record means a crash
         * during append, and treating it as end-of-file silently restores a
         * past that was only half recorded. */
        fclose(f);
        say(detail, cap, "the overlay ends mid-record");
        return VM_COW_TRUNCATED;
    }
    uint64_t records = body / VM_COW_RECORD_BYTES;
    if (fseek(f, (long)VM_COW_HEADER_BYTES, SEEK_SET) != 0) {
        fclose(f); return VM_COW_IO;
    }

    uint8_t *block = (uint8_t *)malloc(VM_COW_BLOCK_BYTES);
    if (!block) { fclose(f); return VM_COW_OUT_OF_MEMORY; }

    uint64_t total = records * (uint64_t)VM_COW_BLOCK_BYTES;
    if (progress) progress(progress_ctx, 0, total);

    for (uint64_t i = 0; i < records; i++) {
        uint8_t idx[VM_COW_INDEX_BYTES];
        if (fread(idx, 1, sizeof idx, f) != sizeof idx ||
            fread(block, 1, VM_COW_BLOCK_BYTES, f) != VM_COW_BLOCK_BYTES) {
            free(block); fclose(f);
            say(detail, cap, "the overlay could not be read to the end");
            return VM_COW_IO;
        }
        uint64_t at = get_le64(idx) * (uint64_t)VM_COW_BLOCK_BYTES;
        if (at + VM_COW_BLOCK_BYTES > onto->size) {
            free(block); fclose(f);
            say(detail, cap, "the overlay names a block outside the disk");
            return VM_COW_SIZE_MISMATCH;
        }
        if (!write_exact(onto, at, block, VM_COW_BLOCK_BYTES, NULL)) {
            free(block); fclose(f);
            say(detail, cap, "could not write a block back to the disk");
            return VM_COW_IO;
        }
        if (progress) progress(progress_ctx, (i + 1u) * VM_COW_BLOCK_BYTES, total);
    }

    free(block);
    fclose(f);
    if (onto->flush && onto->flush(onto->context) != VM_BLOCK_IO_OK) {
        say(detail, cap, "the disk could not be made durable after replay");
        return VM_COW_IO;
    }
    return VM_COW_OK;
}
