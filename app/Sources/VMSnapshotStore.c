/*
 * S5LBox — the set of snapshots belonging to one machine. See the header for
 * the layout and for why the disk part is copy-on-write.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMSnapshotStore.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif

/* ------------------------------------------------------------------ status */

const char *vm_snapshot_status_text(vm_snapshot_status_t status) {
    switch (status) {
        case VM_SNAPSHOT_OK:            return "ok";
        case VM_SNAPSHOT_BAD_ARGUMENT:  return "bad argument";
        case VM_SNAPSHOT_PATH_TOO_LONG: return "path too long";
        case VM_SNAPSHOT_BAD_ID:        return "malformed snapshot id";
        case VM_SNAPSHOT_IO:            return "input/output error";
        case VM_SNAPSHOT_NOT_FOUND:     return "not found";
        case VM_SNAPSHOT_INCOMPLETE:    return "snapshot is incomplete";
        case VM_SNAPSHOT_TOO_MANY:      return "too many snapshots";
        default:                        return "unknown status";
    }
}

static void say(char *detail, size_t capacity, const char *text) {
    if (!detail || capacity == 0u) return;
    size_t n = strlen(text);
    if (n >= capacity) n = capacity - 1u;
    memcpy(detail, text, n);
    detail[n] = '\0';
}

/* ---------------------------------------------------------------------- id */

/*
 * "20260731T142200Z" or "20260731T142200Z-07". Fixed width by construction, so
 * validation is a shape check rather than a search for bad characters: a
 * blacklist would have to anticipate every traversal spelling, and this
 * anticipates none of them because nothing but digits, 'T', 'Z' and one
 * hyphenated two-digit suffix can pass at all.
 */
bool vm_snapshot_id_valid(const char *id) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n != 16u && n != 19u) return false;
    for (size_t i = 0; i < 8u; i++)
        if (id[i] < '0' || id[i] > '9') return false;
    if (id[8] != 'T') return false;
    for (size_t i = 9; i < 15u; i++)
        if (id[i] < '0' || id[i] > '9') return false;
    if (id[15] != 'Z') return false;
    if (n == 19u) {
        if (id[16] != '-') return false;
        if (id[17] < '0' || id[17] > '9') return false;
        if (id[18] < '0' || id[18] > '9') return false;
    }
    return true;
}

/*
 * Civil date from a Unix day count, computed here rather than taken from
 * gmtime. The three toolchains spell the reentrant form differently and one of
 * them does not have it at all; this is arithmetic, is identical everywhere,
 * and makes the tests independent of the host's timezone database.
 *
 * Proleptic Gregorian, shifting the epoch to 0000-03-01 so that the leap day
 * lands at the end of the cycle and no month-length table is needed.
 */
static void civil_from_days(int64_t days, int *y, unsigned *m, unsigned *d) {
    days += 719468;                       /* 1970-01-01 -> 0000-03-01 */
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    uint64_t doe = (uint64_t)(days - era * 146097);            /* [0, 146096] */
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);  /* [0, 365]   */
    uint64_t mp = (5u * doy + 2u) / 153u;                      /* [0, 11]    */
    uint64_t dd = doy - (153u * mp + 2u) / 5u + 1u;            /* [1, 31]    */
    uint64_t mm = mp + (mp < 10u ? 3u : (uint64_t)-9);         /* [1, 12]    */
    *y = (int)(yy + (mm <= 2u ? 1 : 0));
    *m = (unsigned)mm;
    *d = (unsigned)dd;
}

vm_snapshot_status_t vm_snapshot_id_make(uint64_t created_unix,
                                         unsigned sequence,
                                         char *out, size_t capacity) {
    if (!out || capacity < VM_SNAPSHOT_ID_CAPACITY) return VM_SNAPSHOT_BAD_ARGUMENT;
    if (sequence > 99u) return VM_SNAPSHOT_BAD_ARGUMENT;

    int64_t days = (int64_t)(created_unix / 86400u);
    unsigned secs = (unsigned)(created_unix % 86400u);
    int year = 0; unsigned month = 0, day = 0;
    civil_from_days(days, &year, &month, &day);
    if (year < 1000 || year > 9999) return VM_SNAPSHOT_BAD_ARGUMENT;

    /* Built in one call rather than pieces so the width cannot drift. */
    int written = snprintf(out, capacity, "%04d%02u%02uT%02u%02u%02uZ",
                           year, month, day,
                           secs / 3600u, (secs / 60u) % 60u, secs % 60u);
    if (written < 0 || (size_t)written >= capacity) return VM_SNAPSHOT_PATH_TOO_LONG;
    if (sequence != 0u) {
        int more = snprintf(out + written, capacity - (size_t)written,
                            "-%02u", sequence);
        if (more < 0 || (size_t)(written + more) >= capacity)
            return VM_SNAPSHOT_PATH_TOO_LONG;
    }
    return vm_snapshot_id_valid(out) ? VM_SNAPSHOT_OK : VM_SNAPSHOT_BAD_ID;
}

/* -------------------------------------------------------------------- paths */

vm_snapshot_status_t vm_snapshot_dir(const char *machine_dir,
                                     char *out, size_t capacity) {
    if (!machine_dir || !out || capacity == 0u) return VM_SNAPSHOT_BAD_ARGUMENT;
    int n = snprintf(out, capacity, "%s/snapshots", machine_dir);
    if (n < 0 || (size_t)n >= capacity) { out[0] = '\0'; return VM_SNAPSHOT_PATH_TOO_LONG; }
    return VM_SNAPSHOT_OK;
}

vm_snapshot_status_t vm_snapshot_path(const char *snapshots_dir,
                                      const char *id,
                                      bool partial,
                                      char *out, size_t capacity) {
    if (!snapshots_dir || !out || capacity == 0u) return VM_SNAPSHOT_BAD_ARGUMENT;
    if (!vm_snapshot_id_valid(id)) return VM_SNAPSHOT_BAD_ID;
    int n = snprintf(out, capacity, "%s/%s%s", snapshots_dir, id,
                     partial ? ".partial" : "");
    if (n < 0 || (size_t)n >= capacity) { out[0] = '\0'; return VM_SNAPSHOT_PATH_TOO_LONG; }
    return VM_SNAPSHOT_OK;
}

vm_snapshot_status_t vm_snapshot_member_path(const char *snapshots_dir,
                                             const char *id,
                                             bool partial,
                                             const char *leaf,
                                             char *out, size_t capacity) {
    if (!snapshots_dir || !leaf || !out || capacity == 0u)
        return VM_SNAPSHOT_BAD_ARGUMENT;
    if (!vm_snapshot_id_valid(id)) return VM_SNAPSHOT_BAD_ID;
    /* The leaf is one of this header's four constants and never user input;
     * checked anyway, because "never" is a property of today's callers. */
    if (strchr(leaf, '/') || strchr(leaf, '\\') || strcmp(leaf, "..") == 0)
        return VM_SNAPSHOT_BAD_ARGUMENT;
    int n = snprintf(out, capacity, "%s/%s%s/%s", snapshots_dir, id,
                     partial ? ".partial" : "", leaf);
    if (n < 0 || (size_t)n >= capacity) { out[0] = '\0'; return VM_SNAPSHOT_PATH_TOO_LONG; }
    return VM_SNAPSHOT_OK;
}

/* --------------------------------------------------------------------- meta */

vm_snapshot_status_t vm_snapshot_meta_write(const char *path,
                                            const vm_snapshot_info_t *info,
                                            char *detail, size_t cap) {
    if (!path || !info) return VM_SNAPSHOT_BAD_ARGUMENT;
    FILE *f = fopen(path, "wb");
    if (!f) { say(detail, cap, "could not create the meta file"); return VM_SNAPSHOT_IO; }
    int n = fprintf(f,
                    "version 1\n"
                    "id %s\n"
                    "created %llu\n"
                    "retired %llu\n",
                    info->id,
                    (unsigned long long)info->created_unix,
                    (unsigned long long)info->retired);
    /* fclose can be where a buffered write finally fails, so its result is
     * part of the answer rather than cleanup. */
    bool closed = (fclose(f) == 0);
    if (n < 0 || !closed) {
        say(detail, cap, "could not write the meta file");
        return VM_SNAPSHOT_IO;
    }
    return VM_SNAPSHOT_OK;
}

vm_snapshot_status_t vm_snapshot_meta_read(const char *path,
                                           vm_snapshot_info_t *out,
                                           char *detail, size_t cap) {
    if (!path || !out) return VM_SNAPSHOT_BAD_ARGUMENT;
    FILE *f = fopen(path, "rb");
    if (!f) { say(detail, cap, "no meta file"); return VM_SNAPSHOT_NOT_FOUND; }

    memset(out, 0, sizeof *out);
    bool have_id = false, have_created = false, have_retired = false;
    char line[256];
    while (fgets(line, (int)sizeof line, f)) {
        char key[32];
        char value[VM_SNAPSHOT_ID_CAPACITY];
        unsigned long long number = 0;
        if (sscanf(line, "%31s %llu", key, &number) == 2) {
            if (strcmp(key, "created") == 0) { out->created_unix = number; have_created = true; continue; }
            if (strcmp(key, "retired") == 0) { out->retired = number; have_retired = true; continue; }
        }
        if (sscanf(line, "%31s %23s", key, value) == 2 && strcmp(key, "id") == 0) {
            if (!vm_snapshot_id_valid(value)) {
                fclose(f);
                say(detail, cap, "meta names a malformed id");
                return VM_SNAPSHOT_BAD_ID;
            }
            memcpy(out->id, value, strlen(value) + 1u);
            have_id = true;
        }
        /* Unknown keys are ignored so a newer build's file still loads. */
    }
    fclose(f);
    if (!have_id || !have_created || !have_retired) {
        say(detail, cap, "meta is missing a required key");
        return VM_SNAPSHOT_INCOMPLETE;
    }
    return VM_SNAPSHOT_OK;
}

/* --------------------------------------------------------------- enumerate */

static uint64_t file_size_or_zero(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t n = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end > 0) n = (uint64_t)end;
    }
    fclose(f);
    return n;
}

/* Sum of the members this store writes, rather than a directory walk: the set
 * is fixed and known, and a walk would follow whatever else ended up there. */
static uint64_t snapshot_bytes(const char *snapshots_dir, const char *id) {
    static const char *const LEAVES[] = {
        VM_SNAPSHOT_META_FILE, VM_SNAPSHOT_STATE_FILE,
        VM_SNAPSHOT_MD_FILE,   VM_SNAPSHOT_COW_FILE,
    };
    uint64_t total = 0;
    for (size_t i = 0; i < sizeof LEAVES / sizeof LEAVES[0]; i++) {
        char p[VM_FW_BOOT_PATH_CAPACITY];
        if (vm_snapshot_member_path(snapshots_dir, id, false, LEAVES[i],
                                    p, sizeof p) != VM_SNAPSHOT_OK)
            continue;
        total += file_size_or_zero(p);
    }
    return total;
}

static void insert_newest_first(vm_snapshot_info_t *out, size_t *count,
                                size_t capacity, const vm_snapshot_info_t *item) {
    if (*count >= capacity) return;
    size_t at = *count;
    /* Ids are fixed width and sort lexically, which IS newest-first order
     * reversed -- so one strcmp decides placement and no clock is read. */
    while (at > 0 && strcmp(out[at - 1].id, item->id) < 0) {
        out[at] = out[at - 1];
        at--;
    }
    out[at] = *item;
    (*count)++;
}

vm_snapshot_status_t vm_snapshot_list(const char *snapshots_dir,
                                      vm_snapshot_info_t *out,
                                      size_t capacity,
                                      size_t *count,
                                      char *detail, size_t cap) {
    if (!snapshots_dir || !out || !count || capacity == 0u)
        return VM_SNAPSHOT_BAD_ARGUMENT;
    *count = 0;

#if defined(_WIN32)
    char pattern[VM_FW_BOOT_PATH_CAPACITY];
    if (snprintf(pattern, sizeof pattern, "%s\\*", snapshots_dir) < 0)
        return VM_SNAPSHOT_PATH_TOO_LONG;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) {
        /* No directory yet is "no snapshots", not a failure: a machine that
         * has never been snapshotted is the ordinary case. */
        if (GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND)
            return VM_SNAPSHOT_OK;
        say(detail, cap, "could not list the snapshots directory");
        return VM_SNAPSHOT_IO;
    }
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const char *name = found.cFileName;
#else
    DIR *dir = opendir(snapshots_dir);
    if (!dir) return VM_SNAPSHOT_OK;      /* same reasoning as above */
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
#endif
        /* `.partial` fails the shape check, so in-progress and crash-left
         * directories are skipped without a special case. */
        if (!vm_snapshot_id_valid(name)) continue;

        vm_snapshot_info_t info;
        char meta[VM_FW_BOOT_PATH_CAPACITY];
        if (vm_snapshot_member_path(snapshots_dir, name, false,
                                    VM_SNAPSHOT_META_FILE,
                                    meta, sizeof meta) != VM_SNAPSHOT_OK)
            continue;
        if (vm_snapshot_meta_read(meta, &info, NULL, 0) != VM_SNAPSHOT_OK)
            continue;                     /* incomplete: not offerable */
        if (strcmp(info.id, name) != 0) continue;   /* meta must agree */

        info.bytes = snapshot_bytes(snapshots_dir, name);
        info.complete = true;
        if (*count >= capacity) {
#if defined(_WIN32)
            FindClose(h);
#else
            closedir(dir);
#endif
            say(detail, cap, "more snapshots than the listing buffer holds");
            return VM_SNAPSHOT_TOO_MANY;
        }
        insert_newest_first(out, count, capacity, &info);
#if defined(_WIN32)
    } while (FindNextFileA(h, &found));
    FindClose(h);
#else
    }
    closedir(dir);
#endif
    return VM_SNAPSHOT_OK;
}
