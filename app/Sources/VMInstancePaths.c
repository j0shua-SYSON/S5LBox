/*
 * S5LBox — VMInstancePaths. See the header for the layout and the reasoning
 * behind sharing the originals and not the work image.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMInstancePaths.h"

#include "VMInstances.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* snprintf into a caller's buffer that may not exist. Every refusal here has
 * to be sayable, and a caller that does not want the sentence passes NULL. */
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void say(char *out, size_t capacity, const char *fmt, ...) {
    va_list ap;
    if (!out || !capacity) return;
    va_start(ap, fmt);
    (void)vsnprintf(out, capacity, fmt, ap);
    va_end(ap);
    out[capacity - 1u] = '\0';
}

const char *vm_instance_paths_status_text(vm_instance_paths_status_t status) {
    switch (status) {
        case VM_INSTANCE_PATHS_OK:       return "ok";
        case VM_INSTANCE_PATHS_ERR_NULL: return "a required value was missing";
        case VM_INSTANCE_PATHS_ERR_ID:
            return "the machine identifier is malformed";
        case VM_INSTANCE_PATHS_ERR_TOO_LONG:
            return "the path to this machine's files is too long";
    }
    return "unknown";
}

/* Join, accepting either separator so the same code works against a Windows
 * host path under the test suite and a POSIX one on the phone. Returns false
 * on truncation rather than producing a shorter path that is a real and
 * different location. */
static bool join(char *out, size_t capacity, const char *directory,
                 const char *name) {
    if (!out || !capacity || !directory || !name) return false;
    size_t n = strlen(directory);
    bool separated = n > 0u && (directory[n - 1u] == '/' ||
                                directory[n - 1u] == '\\');
    int written = snprintf(out, capacity, "%s%s%s", directory,
                           separated ? "" : "/", name);
    return written > 0 && (size_t)written < capacity;
}

static bool copy_into(char *out, size_t capacity, const char *text) {
    int written = snprintf(out, capacity, "%s", text);
    return written >= 0 && (size_t)written < capacity;
}

/*
 * Size of a regular file, or 0 for absent, empty or unreadable.
 *
 * A near-copy of VMFirmwareBoot.c's, and deliberately not shared with it: that
 * one is static inside a translation unit whose link closure is emucore, the
 * file-block layer and the rootfs provisioner, and dragging all of that into a
 * test about string joining would make the test slow enough that nobody runs
 * it. Six lines of fopen is the cheaper duplication.
 */
static uint64_t regular_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0u;
    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    fclose(f);
    return size > 0 ? (uint64_t)size : 0u;
}

vm_instance_paths_status_t
vm_instance_paths_derive(const char *firmware_directory,
                         const char *machines_directory,
                         const char *instance_id,
                         vm_instance_paths_t *out) {
    if (!out) return VM_INSTANCE_PATHS_ERR_NULL;
    memset(out, 0, sizeof *out);

    if (!firmware_directory || !*firmware_directory ||
        !machines_directory || !*machines_directory || !instance_id)
        return VM_INSTANCE_PATHS_ERR_NULL;

    /*
     * The id becomes a path component, so it is checked by the same function
     * the machine list validates it with rather than by a second rule that
     * could drift from it. Sixteen lower-case hex digits contains no separator
     * and no dot, which is what makes the join below safe against a crafted
     * or corrupted machines.txt.
     */
    if (vm_instance_id_check(instance_id) != VM_INSTANCE_OK)
        return VM_INSTANCE_PATHS_ERR_ID;

    if (!copy_into(out->firmware, sizeof out->firmware, firmware_directory) ||
        !join(out->machine, sizeof out->machine, machines_directory,
              instance_id) ||
        !join(out->work_image, sizeof out->work_image, out->machine,
              VM_FW_BOOT_WORK_FILE) ||
        !join(out->legacy_work_image, sizeof out->legacy_work_image,
              firmware_directory, VM_FW_BOOT_WORK_FILE)) {
        memset(out, 0, sizeof *out);
        return VM_INSTANCE_PATHS_ERR_TOO_LONG;
    }
    return VM_INSTANCE_PATHS_OK;
}

void vm_instance_paths_to_boot(const vm_instance_paths_t *paths,
                               vm_firmware_boot_paths_t *out) {
    if (!out) return;
    if (!paths) {
        memset(out, 0, sizeof *out);
        return;
    }
    (void)vm_firmware_boot_paths_split(out, paths->firmware, paths->machine);
}

vm_instance_work_plan_t
vm_instance_work_plan(const vm_instance_paths_t *paths) {
    if (!paths || !paths->work_image[0]) return VM_INSTANCE_WORK_PROVISION;
    if (regular_file_size(paths->work_image) > 0u)
        return VM_INSTANCE_WORK_PRIVATE;
    if (paths->legacy_work_image[0] &&
        regular_file_size(paths->legacy_work_image) > 0u)
        return VM_INSTANCE_WORK_ADOPT;
    return VM_INSTANCE_WORK_PROVISION;
}

bool vm_instance_work_adopt(const vm_instance_paths_t *paths,
                            char *detail, size_t detail_capacity) {
    if (detail && detail_capacity) detail[0] = '\0';
    if (!paths) return false;

    /*
     * Re-decide rather than trust the caller's earlier answer. This is the one
     * function here that destroys information -- after it, the file is not
     * where it was -- and the guard that stops it overwriting a work image
     * this machine already has must be read from the disk at the moment of the
     * move, not from a plan computed before whatever else happened since.
     */
    if (vm_instance_work_plan(paths) != VM_INSTANCE_WORK_ADOPT) {
        say(detail, detail_capacity,
            "There is nothing to adopt: this machine already has a root "
            "filesystem, or the shared one is gone.");
        return false;
    }

    /*
     * A zero-byte destination is "not a file" as far as the plan is concerned
     * -- it is what an interrupted copy leaves -- but it is still a directory
     * entry, and rename() over an existing entry succeeds on POSIX and fails
     * with EEXIST on Windows. Removing it first makes the two hosts agree, and
     * it is provably safe: the plan above returned ADOPT, which it only does
     * when this path holds no bytes.
     */
    (void)remove(paths->work_image);

    if (rename(paths->legacy_work_image, paths->work_image) != 0) {
        /*
         * Nothing has moved. Name the path the image is still at: it is the
         * user's guest disk and the only recovery is knowing where it is.
         * strerror() rather than a guess at the cause, because the two likely
         * ones -- a different volume, and a file another process still has
         * open -- need different things done about them.
         */
        say(detail, detail_capacity,
            "Could not move the existing root filesystem into this machine "
            "(%s). It is still at %s and nothing has been lost.",
            strerror(errno), paths->legacy_work_image);
        return false;
    }

    say(detail, detail_capacity,
        "This machine adopted the root filesystem that was shared by every "
        "machine before now. Other machines will be given their own.");
    return true;
}
