/* Per-machine disk capacity. Missing records retain legacy sizing; malformed
 * records never silently select another capacity. Copyright (c) 2026 j0shua-SYSON. */
#ifndef S5LBOX_VM_DISK_SIZE_H
#define S5LBOX_VM_DISK_SIZE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define VM_DISK_SIZE_FILE "disk-size-v1"
#define VM_DISK_GIB UINT64_C(1073741824)

static inline const char *vm_disk_size_record(unsigned gib) {
    switch (gib) {
    case 2: return "s5lbox-disk-v1 2\n";
    case 4: return "s5lbox-disk-v1 4\n";
    case 8: return "s5lbox-disk-v1 8\n";
    default: return NULL;
    }
}

static inline bool vm_disk_size_parse(const void *data, size_t size, uint64_t *out) {
    if (!data || !out) return false;
    for (unsigned gib = 2; gib <= 8; gib *= 2) {
        const char *record = vm_disk_size_record(gib);
        if (size == strlen(record) && !memcmp(data, record, size)) {
            *out = gib * VM_DISK_GIB;
            return true;
        }
    }
    return false;
}

/* Read-only, bounded, leaves output unchanged on failure. A missing record is
 * the sole legacy case (zero); permission/read errors are not missing files. */
static inline bool vm_disk_size_read(const char *directory, uint64_t *out) {
    char path[4096], data[32];
    if (!directory || !directory[0] || !out) return false;
    int n = snprintf(path, sizeof path, "%s/%s", directory, VM_DISK_SIZE_FILE);
    if (n < 0 || (size_t)n >= sizeof path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) { if (errno != ENOENT) return false; *out = 0; return true; }
    size_t used = fread(data, 1, sizeof data, f);
    bool ok = !ferror(f) && feof(f);
    if (fclose(f)) ok = false;
    return ok && vm_disk_size_parse(data, used, out);
}
#endif
