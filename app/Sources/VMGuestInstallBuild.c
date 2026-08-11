/* See VMGuestInstallBuild.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMGuestInstallBuild.h"

#include "VMSnapshotStore.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    vm_guest_install_build_progress_t callback;
    void *context;
} build_progress_adapter_t;

static void build_detail(char *detail, size_t capacity, const char *text) {
    if (!detail || capacity == 0u) return;
    (void)snprintf(detail, capacity, "%s", text ? text : "");
    detail[capacity - 1u] = '\0';
}

static void build_result_clear(vm_guest_install_build_result_t *result) {
    if (result) memset(result, 0, sizeof *result);
}

static void build_progress(vm_guest_install_build_progress_t callback,
                           void *context,
                           vm_guest_install_build_phase_t phase,
                           uint64_t completed, uint64_t total) {
    if (callback) callback(context, phase, completed, total);
}

static void build_rootfs_progress(void *opaque, uint64_t done,
                                  uint64_t total) {
    build_progress_adapter_t *adapter = (build_progress_adapter_t *)opaque;
    if (!adapter) return;
    build_progress(adapter->callback, adapter->context,
                   VM_GUEST_INSTALL_BUILD_COPYING, done, total);
}

static bool build_join(char out[VM_GUEST_INSTALL_PATH_CAPACITY],
                       const char *directory, const char *leaf) {
    if (!out || !directory || !*directory || !leaf || !*leaf) return false;
    size_t length = strlen(directory);
    const char *separator = directory[length - 1u] == '/' ||
                            directory[length - 1u] == '\\' ? "" : "/";
    int written = snprintf(out, VM_GUEST_INSTALL_PATH_CAPACITY,
                           "%s%s%s", directory, separator, leaf);
    return written > 0 &&
           (size_t)written < VM_GUEST_INSTALL_PATH_CAPACITY;
}

static vm_guest_install_build_status_t build_snapshot_gate(
    const char *work_directory, vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity) {
    char directory[VM_GUEST_INSTALL_PATH_CAPACITY];
    vm_snapshot_status_t path = vm_snapshot_dir(
        work_directory, directory, sizeof directory);
    if (path != VM_SNAPSHOT_OK) {
        build_detail(detail, detail_capacity,
                     "The machine snapshot path is too long to inspect.");
        return VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS;
    }
    vm_snapshot_info_t snapshots[VM_SNAPSHOT_MAX];
    size_t count = 0u;
    char snapshot_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_snapshot_status_t listed = vm_snapshot_list(
        directory, snapshots, VM_SNAPSHOT_MAX, &count,
        snapshot_detail, sizeof snapshot_detail);
    if (listed != VM_SNAPSHOT_OK) {
        build_detail(detail, detail_capacity,
                     snapshot_detail[0] ? snapshot_detail
                                        : vm_snapshot_status_text(listed));
        return VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS;
    }
    if (result) result->historical_snapshots = count;
    if (count != 0u) {
        build_detail(detail, detail_capacity,
                     "Delete this machine's historical snapshots before replacing its guest disk.");
        return VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS;
    }
    return VM_GUEST_INSTALL_BUILD_OK;
}

vm_guest_install_build_status_t
vm_guest_install_build_from_directory(
    const char *work_directory, const char *package_directory,
    vm_guest_install_build_progress_t progress, void *progress_context,
    vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity) {
    build_result_clear(result);
    build_detail(detail, detail_capacity, "");
    if (!work_directory || !*work_directory) {
        build_detail(detail, detail_capacity,
                     "The machine work directory is missing.");
        return VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 0u, 1u);
    vm_guest_install_result_t recovered;
    char transaction_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_guest_install_status_t recovery = vm_guest_install_recover(
        work_directory, &recovered, transaction_detail,
        sizeof transaction_detail);
    if (recovery != VM_GUEST_INSTALL_OK) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0] ? transaction_detail
                                           : vm_guest_install_status_text(recovery));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (result) result->transaction = recovered;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 1u, 1u);
    if (recovered.committed) {
        if (result) {
            result->already_installed = true;
            if (recovered.has_manifest)
                memcpy(result->manifest_sha256, recovered.manifest_sha256,
                       VM_GUEST_INSTALL_SHA256_SIZE);
        }
        build_progress(progress, progress_context,
                       VM_GUEST_INSTALL_BUILD_COMPLETE, 1u, 1u);
        return VM_GUEST_INSTALL_BUILD_OK;
    }

    vm_guest_install_build_status_t snapshot_gate = build_snapshot_gate(
        work_directory, result, detail, detail_capacity);
    if (snapshot_gate != VM_GUEST_INSTALL_BUILD_OK) return snapshot_gate;
    if (!package_directory || !*package_directory) {
        build_detail(detail, detail_capacity,
                     "The verified package directory is missing.");
        return VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PLANNING, 0u, 1u);
    vm_guest_rootfs_status_t plan_status = VM_GUEST_ROOTFS_ERR_ARGUMENT;
    char plan_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_guest_rootfs_plan_t *plan = vm_guest_rootfs_plan_open_directory(
        package_directory, &plan_status, plan_detail, sizeof plan_detail);
    if (!plan) {
        build_detail(detail, detail_capacity,
                     plan_detail[0] ? plan_detail
                                    : vm_guest_rootfs_status_text(plan_status));
        return VM_GUEST_INSTALL_BUILD_ERR_PACKAGES;
    }
    if (result) vm_guest_rootfs_plan_get_stats(plan, &result->plan);
    uint8_t manifest[VM_GUEST_INSTALL_SHA256_SIZE];
    if (!vm_guest_rootfs_plan_manifest_sha256(plan, manifest)) {
        vm_guest_rootfs_plan_close(&plan);
        build_detail(detail, detail_capacity,
                     "The guest rootfs plan has no stable manifest identity.");
        return VM_GUEST_INSTALL_BUILD_ERR_MANIFEST;
    }
    if (result) memcpy(result->manifest_sha256, manifest, sizeof manifest);
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PLANNING, 1u, 1u);

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 0u, 1u);
    vm_guest_install_result_t prepared;
    vm_guest_install_status_t preparation = vm_guest_install_prepare_stage(
        work_directory, &prepared, transaction_detail,
        sizeof transaction_detail);
    if (preparation != VM_GUEST_INSTALL_OK || prepared.committed) {
        vm_guest_rootfs_plan_close(&plan);
        if (preparation == VM_GUEST_INSTALL_OK && prepared.committed) {
            build_detail(detail, detail_capacity,
                         "The machine became installed while its package plan was being built.");
        } else {
            build_detail(detail, detail_capacity,
                         transaction_detail[0]
                             ? transaction_detail
                             : vm_guest_install_status_text(preparation));
        }
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    char stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    if (!build_join(live, work_directory, VM_GUEST_INSTALL_LIVE_FILE) ||
        !vm_guest_install_stage_image_path(stage, sizeof stage,
                                           work_directory)) {
        vm_guest_rootfs_plan_close(&plan);
        build_detail(detail, detail_capacity,
                     "The live or staged guest-disk path is too long.");
        return VM_GUEST_INSTALL_BUILD_ERR_PATH;
    }
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 1u, 1u);

    rootfs_work_options_t options;
    memset(&options, 0, sizeof options);
    options.preserve_fstab = true;
    options.growth_bytes = VM_GUEST_INSTALL_ADDITIONAL_GROWTH_BYTES;
    options.entries = vm_guest_rootfs_plan_entries(plan);
    options.entry_count = vm_guest_rootfs_plan_entry_count(plan);
    build_progress_adapter_t adapter = {progress, progress_context};
    options.progress = build_rootfs_progress;
    options.progress_ctx = &adapter;
    rootfs_work_result_t rootfs;
    rootfs_work_status_t rootfs_status = rootfs_work_create(
        live, stage, &options, &rootfs);
    if (result) result->rootfs = rootfs;
    vm_guest_rootfs_plan_close(&plan);
    if (rootfs_status != ROOTFS_WORK_OK || !rootfs.published) {
        build_detail(detail, detail_capacity,
                     rootfs.detail[0] ? rootfs.detail
                                      : rootfs_work_status_name(rootfs_status));
        return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 0u, 1u);
    vm_guest_install_result_t published;
    vm_guest_install_status_t publication = vm_guest_install_publish(
        work_directory, manifest, &published, transaction_detail,
        sizeof transaction_detail);
    if (result) result->transaction = published;
    if (publication != VM_GUEST_INSTALL_OK || !published.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(publication));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 1u, 1u);
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_COMPLETE, 1u, 1u);
    return VM_GUEST_INSTALL_BUILD_OK;
}

const char *vm_guest_install_build_status_text(
    vm_guest_install_build_status_t status) {
    switch (status) {
        case VM_GUEST_INSTALL_BUILD_OK:              return "ok";
        case VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT:    return "invalid argument";
        case VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION: return "transaction recovery";
        case VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS:   return "historical snapshots";
        case VM_GUEST_INSTALL_BUILD_ERR_PACKAGES:    return "package plan";
        case VM_GUEST_INSTALL_BUILD_ERR_MANIFEST:    return "manifest identity";
        case VM_GUEST_INSTALL_BUILD_ERR_PATH:        return "path unavailable";
        case VM_GUEST_INSTALL_BUILD_ERR_ROOTFS:      return "rootfs construction";
        case VM_GUEST_INSTALL_BUILD_ERR_PUBLISH:     return "transaction publish";
        default:                                     return "unknown status";
    }
}

const char *vm_guest_install_build_phase_text(
    vm_guest_install_build_phase_t phase) {
    switch (phase) {
        case VM_GUEST_INSTALL_BUILD_RECOVERING: return "Checking installation";
        case VM_GUEST_INSTALL_BUILD_PLANNING:   return "Verifying packages";
        case VM_GUEST_INSTALL_BUILD_STAGING:    return "Preparing guest disk";
        case VM_GUEST_INSTALL_BUILD_COPYING:    return "Building guest disk";
        case VM_GUEST_INSTALL_BUILD_PUBLISHING: return "Installing guest disk";
        case VM_GUEST_INSTALL_BUILD_COMPLETE:   return "Installation ready";
        default:                                return "Installing";
    }
}
