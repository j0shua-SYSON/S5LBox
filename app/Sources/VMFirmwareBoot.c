/* See VMFirmwareBoot.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMFirmwareBoot.h"

#include "file_block.h"
#include "ios3_bringup_gate.h"
#include "rootfs_work.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vm_firmware_boot {
    file_block_t     *media;
    s5l_bringup_md_t *bridges;   /* ~300 KiB; heap, never a thread stack */
};

/* -------------------------------------------------------------------------- */

static bool join_path(char *out, size_t capacity, const char *directory,
                      const char *name) {
    if (!out || !capacity || !directory || !name) return false;
    size_t n = strlen(directory);
    /* Both separators are accepted so the same code works against a Windows
     * host path in the test suite and a POSIX one on the phone. */
    bool has_separator = n > 0u && (directory[n - 1u] == '/' ||
                                    directory[n - 1u] == '\\');
    int written = snprintf(out, capacity, "%s%s%s", directory,
                           has_separator ? "" : "/", name);
    return written > 0 && (size_t)written < capacity;
}

/* Size of a regular file, or 0 for absent/empty/unreadable. Deliberately does
 * not distinguish those: to this layer they are the same answer, "you cannot
 * boot this", and a size is all the caller shows. */
static uint64_t file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0u;
    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    fclose(f);
    return size > 0 ? (uint64_t)size : 0u;
}

static uint8_t *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buffer = (uint8_t *)malloc((size_t)size);
    if (!buffer) { fclose(f); return NULL; }
    size_t got = fread(buffer, 1u, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buffer); return NULL; }
    *out_size = got;
    return buffer;
}

static void set_detail(char *out, size_t capacity, const char *text) {
    if (!out || !capacity) return;
    (void)snprintf(out, capacity, "%s", text ? text : "");
    out[capacity - 1u] = '\0';
}

/* -------------------------------------------------------------------------- */

void vm_firmware_boot_probe(const char *directory,
                            vm_firmware_boot_state_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->readiness = VM_FW_BOOT_INCOMPLETE;

    if (!directory || !*directory) {
        set_detail(out->detail, sizeof out->detail,
                   "This app has no directory to look for firmware in.");
        return;
    }

    char path[1024];
    if (join_path(path, sizeof path, directory, VM_FW_BOOT_KERNEL_FILE))
        out->kernel_size = file_size(path);
    if (join_path(path, sizeof path, directory, VM_FW_BOOT_DEVICETREE_FILE))
        out->devicetree_size = file_size(path);
    if (join_path(path, sizeof path, directory, VM_FW_BOOT_ROOTFS_FILE))
        out->rootfs_size = file_size(path);
    if (join_path(path, sizeof path, directory, VM_FW_BOOT_WORK_FILE))
        out->work_size = file_size(path);

    out->kernel_present     = out->kernel_size > 0u;
    out->devicetree_present = out->devicetree_size > 0u;
    out->rootfs_present     = out->rootfs_size > 0u;
    out->work_present       = out->work_size > 0u;

    if (!out->kernel_present || !out->devicetree_present ||
        !out->rootfs_present) {
        /* Name every missing file at once rather than one per attempt: a user
         * who imported two of three should not have to boot three times to
         * learn that. */
        char missing[192];
        missing[0] = '\0';
        size_t used = 0u;
        static const struct { const char *name; size_t offset; } files[] = {
            { VM_FW_BOOT_KERNEL_FILE,
              offsetof(vm_firmware_boot_state_t, kernel_present) },
            { VM_FW_BOOT_DEVICETREE_FILE,
              offsetof(vm_firmware_boot_state_t, devicetree_present) },
            { VM_FW_BOOT_ROOTFS_FILE,
              offsetof(vm_firmware_boot_state_t, rootfs_present) },
        };
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
            const bool *present =
                (const bool *)((const char *)out + files[i].offset);
            if (*present) continue;
            int written = snprintf(missing + used, sizeof missing - used,
                                   "%s%s", used ? ", " : "", files[i].name);
            if (written < 0 || (size_t)written >= sizeof missing - used) break;
            used += (size_t)written;
        }
        (void)snprintf(out->detail, sizeof out->detail,
                       "Missing firmware: %s. Import an IPSW from Settings.",
                       missing);
        out->detail[sizeof out->detail - 1u] = '\0';
        return;
    }

    if (!out->work_present) {
        out->readiness = VM_FW_BOOT_NEEDS_WORK_IMAGE;
        set_detail(out->detail, sizeof out->detail,
                   "The writable root filesystem has not been prepared yet.");
        return;
    }

    out->readiness = VM_FW_BOOT_READY;
}

/* -------------------------------------------------------------------------- */

vm_firmware_boot_t *vm_firmware_boot_create(void) {
    vm_firmware_boot_t *boot =
        (vm_firmware_boot_t *)calloc(1u, sizeof *boot);
    if (!boot) return NULL;
    boot->bridges = (s5l_bringup_md_t *)calloc(1u, sizeof *boot->bridges);
    boot->media = file_block_create();
    if (!boot->bridges || !boot->media) {
        vm_firmware_boot_destroy(&boot);
        return NULL;
    }
    return boot;
}

void vm_firmware_boot_destroy(vm_firmware_boot_t **slot) {
    if (!slot || !*slot) return;
    vm_firmware_boot_t *boot = *slot;
    if (boot->media) {
        /* The bridges borrowed this descriptor, so every borrow must have
         * ended -- which it has, because the caller frees the machine first. */
        (void)file_block_flush(boot->media);
        (void)file_block_close(boot->media);
        (void)file_block_destroy(&boot->media);
    }
    free(boot->bridges);
    free(boot);
    *slot = NULL;
}

bool vm_firmware_boot_start(vm_firmware_boot_t *boot,
                            s5l8900_t *machine,
                            const char *directory,
                            vm_firmware_boot_report_t *report) {
    if (!report) return false;
    memset(report, 0, sizeof *report);
    set_detail(report->summary, sizeof report->summary, "not started");

    if (!boot || !boot->bridges || !boot->media || !machine || !directory) {
        set_detail(report->detail, sizeof report->detail,
                   "Internal error: the firmware boot path was not set up.");
        set_detail(report->summary, sizeof report->summary, "internal error");
        return false;
    }

    vm_firmware_boot_state_t state;
    vm_firmware_boot_probe(directory, &state);
    if (state.readiness != VM_FW_BOOT_READY) {
        set_detail(report->detail, sizeof report->detail, state.detail);
        set_detail(report->summary, sizeof report->summary,
                   state.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE
                       ? "root filesystem not prepared"
                       : "firmware incomplete");
        return false;
    }

    char kernel_path[1024], tree_path[1024], work_path[1024];
    if (!join_path(kernel_path, sizeof kernel_path, directory,
                   VM_FW_BOOT_KERNEL_FILE) ||
        !join_path(tree_path, sizeof tree_path, directory,
                   VM_FW_BOOT_DEVICETREE_FILE) ||
        !join_path(work_path, sizeof work_path, directory,
                   VM_FW_BOOT_WORK_FILE)) {
        set_detail(report->detail, sizeof report->detail,
                   "The firmware directory path is too long to use.");
        set_detail(report->summary, sizeof report->summary, "path too long");
        return false;
    }

    /* The work image first: it is the only step that can fail because another
     * machine is already using the file, and discovering that after reading
     * 8 MB of kernel wastes the user's time for no reason. */
    file_block_status_t opened =
        file_block_open(boot->media, work_path, state.work_size);
    if (opened != FILE_BLOCK_STATUS_OK) {
        (void)snprintf(report->detail, sizeof report->detail,
                       "Cannot open the root filesystem work image: %s.",
                       file_block_strerror(opened));
        report->detail[sizeof report->detail - 1u] = '\0';
        set_detail(report->summary, sizeof report->summary,
                   "root filesystem unavailable");
        return false;
    }

    size_t kernel_size = 0u, tree_size = 0u;
    uint8_t *kernel = slurp(kernel_path, &kernel_size);
    uint8_t *tree = kernel ? slurp(tree_path, &tree_size) : NULL;
    if (!kernel || !tree) {
        free(kernel);
        free(tree);
        (void)file_block_close(boot->media);
        set_detail(report->detail, sizeof report->detail,
                   kernel ? "Cannot read devicetree.bin."
                          : "Cannot read kernel.macho.");
        set_detail(report->summary, sizeof report->summary,
                   "firmware unreadable");
        return false;
    }

    s5l_bringup_request_t request;
    memset(&request, 0, sizeof request);
    request.kernel = kernel;
    request.kernel_size = kernel_size;
    request.devicetree = tree;
    request.devicetree_size = tree_size;
    request.root_media = file_block_get(boot->media);
    /* One place decides both which kernel is acceptable and where its
     * memory-disk call sites are, so the bridges can never be armed against a
     * kernel that was authorized somewhere else. */
    ios3_bringup_gate_configure(&request, NULL);

    s5l_bringup_status_t status =
        s5l_bringup(machine, &request, boot->bridges, &report->bringup);

    /* Guest DRAM holds its own copy of everything, so the files go now
     * regardless of the outcome. */
    free(kernel);
    free(tree);

    if (status != S5L_BRINGUP_OK) {
        (void)file_block_close(boot->media);
        (void)snprintf(report->detail, sizeof report->detail,
                       "Could not start Apple's kernel (%s at %s): %s",
                       s5l_bringup_status_name(status),
                       s5l_bringup_stage_name(report->bringup.stage),
                       report->bringup.detail);
        report->detail[sizeof report->detail - 1u] = '\0';
        (void)snprintf(report->summary, sizeof report->summary,
                       "firmware boot failed: %s",
                       s5l_bringup_status_name(status));
        report->summary[sizeof report->summary - 1u] = '\0';
        return false;
    }

    report->ok = true;
    (void)snprintf(report->summary, sizeof report->summary,
                   "iPhone OS 3.1.3 kernel, root on /dev/md0");
    report->summary[sizeof report->summary - 1u] = '\0';
    return true;
}

/* -------------------------------------------------------------------------- */

bool vm_firmware_boot_provision(const char *directory,
                                char *detail, size_t detail_capacity) {
    char source_path[1024], work_path[1024];
    rootfs_work_options_t options;
    rootfs_work_result_t result;

    set_detail(detail, detail_capacity, "");

    if (!directory || !*directory ||
        !join_path(source_path, sizeof source_path, directory,
                   VM_FW_BOOT_ROOTFS_FILE) ||
        !join_path(work_path, sizeof work_path, directory,
                   VM_FW_BOOT_WORK_FILE)) {
        set_detail(detail, detail_capacity,
                   "The firmware directory path is unusable.");
        return false;
    }

    memset(&options, 0, sizeof options);
    memset(&result, 0, sizeof result);
    /* The stock image names /dev/disk0s1, which is NAND this machine does not
     * model; without this line launchd fails fsck and halts. */
    options.fstab_line = ROOTFS_WORK_DEFAULT_FSTAB;
    options.growth_bytes = VM_FW_BOOT_GROWTH_BYTES;
    /* This machine has no GPU, so QuartzCore must be told to software-render
     * or SpringBoard's compositor has nowhere to go. */
    options.ca_software_render = true;

    rootfs_work_status_t status =
        rootfs_work_create(source_path, work_path, &options, &result);
    if (status != ROOTFS_WORK_OK) {
        (void)snprintf(detail, detail_capacity,
                       "Could not prepare the root filesystem (%s at %s): %s",
                       rootfs_work_status_name(status),
                       rootfs_work_stage_name(result.stage),
                       result.detail);
        if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
        return false;
    }
    return true;
}
