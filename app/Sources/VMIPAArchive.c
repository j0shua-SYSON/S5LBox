/* Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMIPAArchive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *out, size_t cap, const char *message) {
    if (out && cap) (void)snprintf(out, cap, "%s", message);
    return false;
}

bool vm_ipa_safe_path(const char *path) {
    if (!path || !*path || *path == '/') return false;
    const char *part = path;
    for (const char *p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && (c < 32 || c > 126 || c == '\\' || c == ':')) return false;
        if (c == '/' || c == 0) {
            size_t n = (size_t)(p - part);
            if (!n || (n == 1 && part[0] == '.') ||
                (n == 2 && part[0] == '.' && part[1] == '.')) return false;
            if (!c || !p[1]) return true;
            part = p + 1;
        }
    }
}

typedef struct {
    vm_ipa_archive_t *plan;
    const char *error;
} walk_t;

static bool visit(void *opaque, const vmfw_zip_entry_t *entry, uint32_t index) {
    (void)index;
    walk_t *walk = opaque;
    vm_ipa_archive_t *p = walk->plan;
    if (!vm_ipa_safe_path(entry->name)) {
        walk->error = "The IPA contains an unsafe or unsupported filename.";
        return false;
    }
    if (strncmp(entry->name, "Payload/", 8) || !entry->name[8]) return true;
    const char *slash = strchr(entry->name + 8, '/');
    size_t root_len = slash ? (size_t)(slash - entry->name) : strlen(entry->name);
    if (root_len < 13 || strncmp(entry->name + root_len - 4, ".app", 4)) {
        walk->error = "Payload must contain exactly one .app bundle.";
        return false;
    }
    if (!p->app_root[0]) {
        memcpy(p->app_root, entry->name, root_len);
        p->app_root[root_len] = 0;
    } else if (strlen(p->app_root) != root_len ||
               strncmp(p->app_root, entry->name, root_len)) {
        walk->error = "An IPA containing multiple apps is not supported.";
        return false;
    }
    unsigned type = entry->unix_mode & 0170000u;
    if ((type && type != 0100000u && type != 0040000u) ||
        (type == 0040000u && !entry->is_directory) ||
        (type == 0100000u && entry->is_directory)) {
        walk->error = "IPA symlinks and special files are not supported.";
        return false;
    }
    if ((!slash || !slash[1]) && !entry->is_directory) {
        walk->error = "The app bundle is not a directory.";
        return false;
    }
    if (p->count == VM_IPA_MAX_FILES ||
        entry->uncompressed_size > VM_IPA_MAX_FILE_BYTES ||
        entry->uncompressed_size > VM_IPA_MAX_BYTES - p->unpacked_bytes ||
        (entry->is_directory && entry->uncompressed_size)) {
        walk->error = "The IPA exceeds the supported size or file-count limit.";
        return false;
    }
    for (unsigned i = 0; i < p->count; i++) {
        const char *a = p->files[i].name, *b = entry->name;
        size_t al = strlen(a), bl = strlen(b);
        if (al && a[al - 1] == '/') al--;
        if (bl && b[bl - 1] == '/') bl--;
        bool same = al == bl;
        for (size_t n = 0; same && n < al; n++) {
            unsigned char ac = (unsigned char)a[n], bc = (unsigned char)b[n];
            if (ac >= 'A' && ac <= 'Z') ac += 'a' - 'A';
            if (bc >= 'A' && bc <= 'Z') bc += 'a' - 'A';
            same = ac == bc;
        }
        if (same) {
            walk->error = "The IPA contains conflicting or duplicate paths.";
            return false;
        }
    }
    if (slash && !strcmp(slash + 1, "Info.plist") && !entry->is_directory)
        p->info_index = p->count;
    p->files[p->count++] = *entry;
    p->unpacked_bytes += entry->uncompressed_size;
    return true;
}

bool vm_ipa_archive_open(const vmfw_zip_t *zip, vm_ipa_archive_t *p,
                         char *detail, size_t cap) {
    if (!p) return fail(detail, cap, "No IPA inspection result was supplied.");
    memset(p, 0, sizeof *p);
    p->info_index = UINT32_MAX;
    p->files = calloc(VM_IPA_MAX_FILES, sizeof *p->files);
    if (!p->files) return fail(detail, cap, "Not enough memory to inspect this IPA.");
    walk_t walk = {p, NULL};
    vmfw_zip_status_t status = vmfw_zip_iterate(zip, visit, &walk);
    if (status != VMFW_ZIP_OK || walk.error || p->info_index == UINT32_MAX) {
        const char *message = walk.error ? walk.error : status != VMFW_ZIP_OK
            ? vmfw_zip_strerror(status) : "No Payload/*.app/Info.plist was found.";
        fail(detail, cap, message);
        vm_ipa_archive_close(p);
        return false;
    }
    if (detail && cap) detail[0] = 0;
    return true;
}

void vm_ipa_archive_close(vm_ipa_archive_t *p) {
    if (!p) return;
    free(p->files);
    memset(p, 0, sizeof *p);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
static bool armv6(uint32_t subtype) {
    subtype &= 0xffffffu;
    return subtype == 0 || subtype == 5 || subtype == 6 || subtype == 7;
}
static bool thin(const uint8_t *b, size_t n, char *out, size_t cap) {
    if (n < 28 || le32(b) != 0xfeedface || le32(b + 4) != 12 ||
        !armv6(le32(b + 8)) || le32(b + 12) != 2)
        return fail(out, cap, "This app needs an ARMv6 iPhone executable; ARMv7-only and ARM64 apps cannot run here.");
    uint32_t commands = le32(b + 16), bytes = le32(b + 20);
    if (bytes > n - 28 || commands > bytes / 8)
        return fail(out, cap, "The app executable has invalid load commands.");
    size_t cursor = 28, end = 28 + bytes;
    for (uint32_t i = 0; i < commands; i++) {
        if (end - cursor < 8) return fail(out, cap, "The app executable is truncated.");
        uint32_t cmd = le32(b + cursor), size = le32(b + cursor + 4);
        if (size < 8 || size % 4 || size > end - cursor)
            return fail(out, cap, "The app executable has an invalid command length.");
        if (cmd == 0x21 || cmd == 0x2c) {
            if (size < 20 || le32(b + cursor + 16))
                return fail(out, cap, "This app is encrypted. S5LBox does not decrypt protected apps.");
        }
        if (cmd == 0x25 && (size < 16 || le32(b + cursor + 8) > 0x00030103))
            return fail(out, cap, "This app requires a newer iOS version than 3.1.3.");
        if (cmd == 0x32 || cmd == 0x24 || cmd == 0x2f || cmd == 0x30)
            return fail(out, cap, "This executable targets an unsupported Apple platform.");
        cursor += size;
    }
    if (cursor != end) return fail(out, cap, "The executable command table is inconsistent.");
    return true;
}
bool vm_ipa_check_executable(const uint8_t *b, size_t n, char *out, size_t cap) {
    if (!b || n < 4) return fail(out, cap, "The app executable is missing or truncated.");
    if (be32(b) != 0xcafebabe) return thin(b, n, out, cap);
    if (n < 8) return fail(out, cap, "The universal executable is truncated.");
    uint32_t count = be32(b + 4);
    if (!count || count > 128 || count > (n - 8) / 20)
        return fail(out, cap, "The universal executable has an invalid slice table.");
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *a = b + 8 + i * 20;
        uint32_t offset = be32(a + 8), size = be32(a + 12);
        if (offset < 8 + count * 20 || offset > n || size > n - offset)
            return fail(out, cap, "The universal executable has an out-of-bounds slice.");
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *previous = b + 8 + j * 20;
            uint32_t start = be32(previous + 8), length = be32(previous + 12);
            if ((uint64_t)offset < (uint64_t)start + length &&
                (uint64_t)start < (uint64_t)offset + size)
                return fail(out, cap, "The universal executable has overlapping slices.");
        }
        if (be32(a) == 12 && armv6(be32(a + 4))) {
            if (!thin(b + offset, size, out, cap)) return false;
            if (be32(a + 4) != le32(b + offset + 8))
                return fail(out, cap, "The executable slice does not match its architecture table.");
            found = true;
        }
    }
    if (found) return true;
    return fail(out, cap, "This IPA has no ARMv6-compatible executable.");
}
