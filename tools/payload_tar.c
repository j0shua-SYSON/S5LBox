/*
 * S5LBox — payload tar reader. See payload_tar.h for why this is a tar.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "payload_tar.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAR_BLOCK 512u

/* ustar header field offsets, from POSIX.1-1988. Named rather than counted so
 * a reader can check them against the standard instead of against this file. */
#define TAR_OFF_NAME      0u
#define TAR_LEN_NAME    100u
#define TAR_OFF_MODE    100u
#define TAR_LEN_MODE      8u
#define TAR_OFF_SIZE    124u
#define TAR_LEN_SIZE     12u
#define TAR_OFF_CHKSUM  148u
#define TAR_LEN_CHKSUM    8u
#define TAR_OFF_TYPE    156u
#define TAR_OFF_LINK    157u
#define TAR_LEN_LINK    100u
#define TAR_OFF_PREFIX  345u
#define TAR_LEN_PREFIX  155u

struct payload_tar {
    uint8_t  *image;          /* the whole archive, owned                    */
    size_t    image_size;
    /* Entry storage. `path` strings live in one arena so each entry's name is
     * a pointer into it and nothing needs freeing individually. */
    rootfs_work_entry_t *entries;
    size_t    entry_count;
    char     *names;
    size_t    names_used;
    size_t    names_cap;
    payload_tar_stats_t stats;
};

static void say(char *detail, size_t cap, const char *fmt, ...) {
    if (!detail || cap == 0u) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(detail, cap, fmt, ap);
    va_end(ap);
}

/* An octal field, which tar pads with spaces or NULs in either position. */
static bool octal_field(const uint8_t *p, size_t n, uint64_t *out) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    if (i == n) { *out = 0u; return true; }        /* all padding is zero */
    for (; i < n; i++) {
        if (p[i] == ' ' || p[i] == '\0') break;
        if (p[i] < '0' || p[i] > '7') return false;
        if (v > (UINT64_MAX >> 3)) return false;
        v = (v << 3) | (uint64_t)(p[i] - '0');
    }
    *out = v;
    return true;
}

/*
 * The header checksum, computed the way tar does: the sum of every unsigned
 * byte with the checksum field itself read as spaces. Verified because a
 * truncated or corrupted archive is otherwise indistinguishable from one that
 * simply ended, and provisioning half a payload into a filesystem is worse
 * than refusing the whole thing.
 */
static bool checksum_ok(const uint8_t *h) {
    uint64_t stored = 0;
    if (!octal_field(h + TAR_OFF_CHKSUM, TAR_LEN_CHKSUM, &stored)) return false;
    uint64_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++)
        sum += (i >= TAR_OFF_CHKSUM && i < TAR_OFF_CHKSUM + TAR_LEN_CHKSUM)
                   ? (uint64_t)' ' : (uint64_t)h[i];
    return sum == stored;
}

static bool all_zero(const uint8_t *h) {
    for (size_t i = 0; i < TAR_BLOCK; i++) if (h[i]) return false;
    return true;
}

/* Printable ASCII and no '\\', which is what rootfs_work.c accepts. */
static bool printable_path(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20u || (unsigned char)*s > 0x7eu || *s == '\\')
            return false;
    return true;
}

static bool traverses(const char *s) {
    if (s[0] == '/') return true;
    if (!strncmp(s, "../", 3) || !strcmp(s, "..")) return true;
    return strstr(s, "/../") != NULL ||
           (strlen(s) >= 3 && !strcmp(s + strlen(s) - 3, "/.."));
}

static char *arena_put(payload_tar_t *t, const char *prefix, const char *name) {
    size_t pl = prefix ? strlen(prefix) : 0u;
    size_t nl = strlen(name);
    size_t need = pl + nl + 2u;              /* '/' + text + NUL */
    if (t->names_used + need > t->names_cap) return NULL;
    char *out = t->names + t->names_used;
    int wrote = snprintf(out, t->names_cap - t->names_used, "%s%s%s",
                         pl ? prefix : "", (pl && prefix[pl - 1u] == '/') ? "" : "/",
                         name);
    if (wrote < 0 || (size_t)wrote >= t->names_cap - t->names_used) return NULL;
    t->names_used += (size_t)wrote + 1u;
    return out;
}

void payload_tar_close(payload_tar_t **slot) {
    if (!slot || !*slot) return;
    payload_tar_t *t = *slot;
    free(t->image);
    free(t->entries);
    free(t->names);
    free(t);
    *slot = NULL;
}

const rootfs_work_entry_t *payload_tar_entries(const payload_tar_t *t) {
    return t ? t->entries : NULL;
}
size_t payload_tar_entry_count(const payload_tar_t *t) {
    return t ? t->entry_count : 0u;
}
void payload_tar_get_stats(const payload_tar_t *t, payload_tar_stats_t *out) {
    if (!out) return;
    if (t) *out = t->stats; else memset(out, 0, sizeof *out);
}

payload_tar_t *payload_tar_open(const char *path, const char *prefix,
                                char *detail, size_t cap) {
    if (!path || !*path) { say(detail, cap, "no payload path given"); return NULL; }

    FILE *f = fopen(path, "rb");
    if (!f) { say(detail, cap, "cannot open payload %s", path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); say(detail, cap, "cannot size %s", path); return NULL; }
    long end = ftell(f);
    if (end <= 0) { fclose(f); say(detail, cap, "payload %s is empty", path); return NULL; }
    rewind(f);

    payload_tar_t *t = (payload_tar_t *)calloc(1u, sizeof *t);
    if (!t) { fclose(f); say(detail, cap, "out of memory"); return NULL; }
    t->image_size = (size_t)end;
    t->image = (uint8_t *)malloc(t->image_size);
    /* One name arena sized from the archive: every path is shorter than the
     * header it came from, so the block count is a safe upper bound. */
    t->names_cap = t->image_size / TAR_BLOCK * 300u + 4096u;
    t->names = (char *)malloc(t->names_cap);
    t->entries = (rootfs_work_entry_t *)calloc(ROOTFS_WORK_MAX_ENTRIES,
                                               sizeof *t->entries);
    if (!t->image || !t->names || !t->entries) {
        fclose(f); payload_tar_close(&t); say(detail, cap, "out of memory");
        return NULL;
    }
    if (fread(t->image, 1u, t->image_size, f) != t->image_size) {
        fclose(f); payload_tar_close(&t);
        say(detail, cap, "short read on %s", path);
        return NULL;
    }
    fclose(f);

    size_t off = 0;
    while (off + TAR_BLOCK <= t->image_size) {
        const uint8_t *h = t->image + off;
        if (all_zero(h)) break;                       /* end-of-archive */
        if (!checksum_ok(h)) {
            say(detail, cap,
                "payload header at offset %zu fails its checksum; the archive "
                "is truncated or corrupt", off);
            payload_tar_close(&t);
            return NULL;
        }

        char name[TAR_LEN_PREFIX + 1u + TAR_LEN_NAME + 1u];
        char pfx[TAR_LEN_PREFIX + 1u], base[TAR_LEN_NAME + 1u];
        memcpy(pfx, h + TAR_OFF_PREFIX, TAR_LEN_PREFIX); pfx[TAR_LEN_PREFIX] = '\0';
        memcpy(base, h + TAR_OFF_NAME, TAR_LEN_NAME);    base[TAR_LEN_NAME]   = '\0';
        if (pfx[0]) (void)snprintf(name, sizeof name, "%s/%s", pfx, base);
        else        (void)snprintf(name, sizeof name, "%s", base);

        uint64_t mode = 0, size = 0;
        if (!octal_field(h + TAR_OFF_MODE, TAR_LEN_MODE, &mode) ||
            !octal_field(h + TAR_OFF_SIZE, TAR_LEN_SIZE, &size)) {
            say(detail, cap, "payload member \"%s\" has a malformed octal field",
                name);
            payload_tar_close(&t);
            return NULL;
        }
        char type = (char)h[TAR_OFF_TYPE];
        char link[TAR_LEN_LINK + 1u];
        memcpy(link, h + TAR_OFF_LINK, TAR_LEN_LINK); link[TAR_LEN_LINK] = '\0';

        size_t body = ((size_t)size + TAR_BLOCK - 1u) / TAR_BLOCK * TAR_BLOCK;
        const uint8_t *content = h + TAR_BLOCK;
        if (off + TAR_BLOCK + body > t->image_size) {
            say(detail, cap,
                "payload member \"%s\" claims %llu bytes but the archive ends "
                "first", name, (unsigned long long)size);
            payload_tar_close(&t);
            return NULL;
        }
        off += TAR_BLOCK + body;

        /* Metadata-only members: GNU/pax extensions and device nodes. Skipped
         * rather than refused -- an archive carrying them is not malformed and
         * the payload does not need them -- but a LONG NAME extension would
         * change the next member's name, and silently ignoring that would
         * provision a file under the wrong path. So those two are fatal. */
        if (type == 'L' || type == 'K') {
            say(detail, cap,
                "payload uses GNU long-name extensions, which this reader does "
                "not implement; repack with paths under 100 characters");
            payload_tar_close(&t);
            return NULL;
        }
        if (type == 'x' || type == 'g' || type == 'V' ||
            type == '3' || type == '4' || type == '6') continue;

        /* Strip the trailing slash a directory member carries. */
        size_t nl = strlen(name);
        while (nl && name[nl - 1u] == '/') name[--nl] = '\0';
        if (nl == 0u) continue;                        /* "./" and friends */
        if (!strcmp(name, ".")) continue;

        if (traverses(name) || !printable_path(name)) {
            say(detail, cap,
                "payload member \"%s\" is not a safe printable relative path",
                name);
            payload_tar_close(&t);
            return NULL;
        }
        if (t->entry_count >= ROOTFS_WORK_MAX_ENTRIES) {
            say(detail, cap, "payload has more than %u members",
                (unsigned)ROOTFS_WORK_MAX_ENTRIES);
            payload_tar_close(&t);
            return NULL;
        }

        rootfs_work_entry_t *e = &t->entries[t->entry_count];
        memset(e, 0, sizeof *e);
        e->path = arena_put(t, prefix, name);
        if (!e->path) {
            say(detail, cap, "payload paths exceed this reader's name arena");
            payload_tar_close(&t);
            return NULL;
        }
        e->permissions = (uint32_t)(mode & 07777u);

        if (type == '5') {
            e->kind = ROOTFS_WORK_ENTRY_DIRECTORY;
            t->stats.directories++;
        } else if (type == '2') {
            if (!link[0] || !printable_path(link)) {
                say(detail, cap,
                    "payload symlink \"%s\" has an empty or non-printable "
                    "target", name);
                payload_tar_close(&t);
                return NULL;
            }
            e->kind = ROOTFS_WORK_ENTRY_SYMLINK;
            e->content = (const uint8_t *)arena_put(t, "", link);
            if (!e->content) {
                say(detail, cap, "payload link targets exceed the name arena");
                payload_tar_close(&t);
                return NULL;
            }
            /* arena_put prepends '/', which is right for a member path and
             * wrong for a link target -- a relative "bzip2" must stay
             * relative. Step past it. */
            e->content += 1;
            e->content_size = strlen((const char *)e->content);
            t->stats.symlinks++;
        } else if (type == '1') {
            /*
             * A HARD LINK, MATERIALISED AS A COPY, and this is an
             * approximation stated rather than hidden. HFS+ hard links are a
             * private indirect-node mechanism the provisioner does not
             * implement, and the payload contains exactly one
             * (bin/uncompress -> bin/gunzip). Copying the target's bytes gives
             * a filesystem where both paths work and read identically; what is
             * lost is that a write through one would be seen through the other,
             * which nothing on a read-mostly system does. The count is reported
             * so this never passes unnoticed.
             */
            const rootfs_work_entry_t *src = NULL;
            for (size_t i = 0; i < t->entry_count; i++) {
                const char *p = t->entries[i].path;
                /* link names are archive-relative; entry paths carry prefix+'/' */
                size_t plen = strlen(p), llen = strlen(link);
                if (plen >= llen && !strcmp(p + plen - llen, link) &&
                    t->entries[i].kind == ROOTFS_WORK_ENTRY_FILE) {
                    src = &t->entries[i];
                    break;
                }
            }
            if (!src) {
                say(detail, cap,
                    "payload hard link \"%s\" names \"%s\", which is not an "
                    "earlier regular member", name, link);
                payload_tar_close(&t);
                return NULL;
            }
            e->kind = ROOTFS_WORK_ENTRY_FILE;
            e->content = src->content;
            e->content_size = src->content_size;
            t->stats.hardlinks_materialised++;
            t->stats.content_bytes += e->content_size;
        } else if (type == '0' || type == '\0') {
            e->kind = ROOTFS_WORK_ENTRY_FILE;
            if (size > (uint64_t)ROOTFS_WORK_MAX_ENTRY_BYTES) {
                say(detail, cap,
                    "payload member \"%s\" is %llu bytes, over the %u-byte "
                    "limit", name, (unsigned long long)size,
                    (unsigned)ROOTFS_WORK_MAX_ENTRY_BYTES);
                payload_tar_close(&t);
                return NULL;
            }
            e->content = size ? content : NULL;
            e->content_size = (size_t)size;
            t->stats.files++;
            t->stats.content_bytes += (size_t)size;
        } else {
            continue;                                  /* unknown, skipped */
        }
        t->entry_count++;
    }

    if (t->entry_count == 0u) {
        say(detail, cap, "payload %s contains no usable members", path);
        payload_tar_close(&t);
        return NULL;
    }
    return t;
}
