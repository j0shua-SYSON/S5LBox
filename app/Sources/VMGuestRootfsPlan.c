/* See VMGuestRootfsPlan.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMGuestRootfsPlan.h"

#include "VMDebArchive.h"
#include "payload_tar.h"
#include "sha256.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAN_NAME_ARENA_BYTES \
    ((size_t)ROOTFS_WORK_MAX_ENTRIES * (size_t)ROOTFS_WORK_MAX_PATH)
#define PLAN_SCRIPT_CAPACITY 16384u
#define PLAN_PATH_CAPACITY 1400u

static const char INSTALL_PLIST[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key>\n"
    "  <string>com.j0shua.s5lbox.guest-install</string>\n"
    "  <key>ProgramArguments</key>\n"
    "  <array>\n"
    "    <string>" VM_GUEST_ROOTFS_INSTALL_SCRIPT "</string>\n"
    "  </array>\n"
    "  <key>RunAtLoad</key>\n"
    "  <true/>\n"
    "  <key>KeepAlive</key>\n"
    "  <dict>\n"
    "    <key>SuccessfulExit</key>\n"
    "    <false/>\n"
    "  </dict>\n"
    "  <key>ThrottleInterval</key>\n"
    "  <integer>10</integer>\n"
    "</dict>\n"
    "</plist>\n";

static const char SCRIPT_HEADER[] =
    "#!/bin/bash\n"
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin\n"
    "export PATH\n"
    "state=" VM_GUEST_ROOTFS_STATE_DIRECTORY "\n"
    "packages=" VM_GUEST_ROOTFS_PACKAGE_DIRECTORY "\n"
    "log=/private/var/log/s5lbox-guest-install.log\n"
    "[ -e \"$state/complete\" ] && exit 0\n"
    "exec >>\"$log\" 2>&1\n"
    "echo \"starting s5lbox guest install\"\n"
    "/bin/mkdir -p \"$state\" \"$packages\" /private/var/lib/dpkg/info "
        "/private/var/lib/dpkg/updates || exit 1\n"
    "[ -e /private/var/lib/dpkg/status ] || : >"
        "/private/var/lib/dpkg/status\n"
    "[ -e /private/var/lib/dpkg/available ] || : >"
        "/private/var/lib/dpkg/available\n"
    "install_one() {\n"
    "    [ -f \"$1\" ] || return 0\n"
    "    /usr/bin/dpkg --force-depends --unpack \"$1\" || return 1\n"
    "    /bin/rm -f \"$1\" || return 1\n"
    "}\n";

static const char SCRIPT_FOOTER[] =
    "/usr/bin/dpkg --force-depends --configure -a || exit 1\n"
    "[ -x /usr/bin/uicache ] && /usr/bin/uicache || true\n"
    ": >\"$state/complete.partial\" || exit 1\n"
    "/bin/sync\n"
    "/bin/mv -f \"$state/complete.partial\" \"$state/complete\" || exit 1\n"
    "/bin/sync\n"
    "echo \"s5lbox guest install complete\"\n"
    "[ -x /usr/bin/killall ] && /usr/bin/killall SpringBoard || true\n"
    "exit 0\n";

struct vm_guest_rootfs_plan {
    rootfs_work_entry_t *entries;
    size_t entry_count;
    char *names;
    size_t names_used;
    size_t names_capacity;
    uint8_t **package_bytes;
    size_t package_count;
    payload_tar_t **tars;
    size_t tar_count;
    char *script;
    size_t script_size;
    vm_guest_rootfs_stats_t stats;
    uint8_t manifest_sha256[VM_GUEST_PACKAGE_SHA256_SIZE];
    bool manifest_valid;
};

static void plan_detail(char *detail, size_t capacity, const char *text) {
    if (!detail || capacity == 0u) return;
    (void)snprintf(detail, capacity, "%s", text ? text : "");
    detail[capacity - 1u] = '\0';
}

static void plan_status(vm_guest_rootfs_status_t *out,
                        vm_guest_rootfs_status_t status) {
    if (out) *out = status;
}

void vm_guest_rootfs_plan_close(vm_guest_rootfs_plan_t **slot) {
    if (!slot || !*slot) return;
    vm_guest_rootfs_plan_t *plan = *slot;
    if (plan->tars) {
        for (size_t i = 0u; i < plan->tar_count; i++)
            payload_tar_close(&plan->tars[i]);
    }
    if (plan->package_bytes) {
        for (size_t i = 0u; i < plan->package_count; i++)
            free(plan->package_bytes[i]);
    }
    free(plan->script);
    free(plan->tars);
    free(plan->package_bytes);
    free(plan->names);
    free(plan->entries);
    free(plan);
    *slot = NULL;
}

static vm_guest_rootfs_plan_t *plan_allocate(size_t package_count) {
    vm_guest_rootfs_plan_t *plan =
        (vm_guest_rootfs_plan_t *)calloc(1u, sizeof *plan);
    if (!plan) return NULL;
    plan->entries = (rootfs_work_entry_t *)calloc(
        ROOTFS_WORK_MAX_ENTRIES, sizeof *plan->entries);
    plan->names_capacity = PLAN_NAME_ARENA_BYTES;
    plan->names = (char *)malloc(plan->names_capacity);
    plan->package_bytes = (uint8_t **)calloc(
        package_count, sizeof *plan->package_bytes);
    plan->tars = (payload_tar_t **)calloc(package_count, sizeof *plan->tars);
    plan->script = (char *)malloc(PLAN_SCRIPT_CAPACITY);
    if (!plan->entries || !plan->names || !plan->package_bytes ||
        !plan->tars || !plan->script) {
        vm_guest_rootfs_plan_close(&plan);
        return NULL;
    }
    plan->package_count = package_count;
    plan->script[0] = '\0';
    return plan;
}

static char *plan_name(vm_guest_rootfs_plan_t *plan, const char *text) {
    if (!plan || !text) return NULL;
    size_t length = strlen(text);
    if (plan->names_used > plan->names_capacity ||
        length >= ROOTFS_WORK_MAX_PATH ||
        length + 1u > plan->names_capacity - plan->names_used)
        return NULL;
    char *stored = plan->names + plan->names_used;
    memcpy(stored, text, length + 1u);
    plan->names_used += length + 1u;
    return stored;
}

static bool plan_entry_same(const rootfs_work_entry_t *a,
                            const rootfs_work_entry_t *b) {
    if (a->kind != b->kind || a->permissions != b->permissions ||
        a->owner_id != b->owner_id || a->group_id != b->group_id ||
        a->existing_policy != b->existing_policy ||
        a->content_size != b->content_size)
        return false;
    return a->content_size == 0u ||
           (a->content && b->content &&
            memcmp(a->content, b->content, a->content_size) == 0);
}

static bool plan_add_entry(vm_guest_rootfs_plan_t *plan,
                           const rootfs_work_entry_t *source,
                           const char *path,
                           char *detail, size_t detail_capacity) {
    if (!plan || !source || !path || path[0] != '/') {
        plan_detail(detail, detail_capacity, "a planned entry is invalid");
        return false;
    }
    rootfs_work_entry_t candidate = *source;
    candidate.path = path;
    for (size_t i = 0u; i < plan->entry_count; i++) {
        if (strcmp(plan->entries[i].path, path) != 0) continue;
        if (!plan_entry_same(&plan->entries[i], &candidate)) {
            if (detail && detail_capacity) {
                (void)snprintf(detail, detail_capacity,
                               "conflicting package entries name %.180s", path);
                detail[detail_capacity - 1u] = '\0';
            }
            return false;
        }
        plan->stats.deduplicated_entries++;
        return true;
    }
    if (plan->entry_count >= ROOTFS_WORK_MAX_ENTRIES) {
        plan_detail(detail, detail_capacity,
                    "the merged package overlay exceeds the entry limit");
        return false;
    }
    char *stored = plan_name(plan, path);
    if (!stored) {
        plan_detail(detail, detail_capacity,
                    "the merged package paths exceed the name limit");
        return false;
    }
    candidate.path = stored;
    plan->entries[plan->entry_count++] = candidate;
    if (candidate.kind == ROOTFS_WORK_ENTRY_FILE)
        plan->stats.files++;
    else if (candidate.kind == ROOTFS_WORK_ENTRY_DIRECTORY)
        plan->stats.directories++;
    else if (candidate.kind == ROOTFS_WORK_ENTRY_SYMLINK)
        plan->stats.symlinks++;
    plan->stats.provision_content_bytes += (uint64_t)candidate.content_size;
    return true;
}

static bool plan_map_package_path(char out[ROOTFS_WORK_MAX_PATH],
                                  const char *path) {
    if (!out || !path || path[0] != '/') return false;
    /* dpkg-created data tars conventionally spell members as ./usr/... . The
     * strict tar reader preserves that harmless component, so remove only
     * leading literal /./ components before applying the root allowlist. */
    while (strncmp(path, "/./", 3u) == 0) path += 2u;
    const char *prefix = NULL;
    if (strcmp(path, "/var") == 0 || strncmp(path, "/var/", 5u) == 0)
        prefix = "/private";
    else if (strcmp(path, "/etc") == 0 || strncmp(path, "/etc/", 5u) == 0)
        prefix = "/private";
    else if (strcmp(path, "/bin") != 0 && strncmp(path, "/bin/", 5u) != 0 &&
             strcmp(path, "/usr") != 0 && strncmp(path, "/usr/", 5u) != 0)
        return false;
    int written = snprintf(out, ROOTFS_WORK_MAX_PATH, "%s%s",
                           prefix ? prefix : "", path);
    return written > 0 && (size_t)written < ROOTFS_WORK_MAX_PATH;
}

static bool plan_add_tar(vm_guest_rootfs_plan_t *plan, payload_tar_t *tar,
                         char *detail, size_t detail_capacity) {
    const rootfs_work_entry_t *entries = payload_tar_entries(tar);
    size_t count = payload_tar_entry_count(tar);
    for (size_t i = 0u; i < count; i++) {
        char mapped[ROOTFS_WORK_MAX_PATH];
        if (!plan_map_package_path(mapped, entries[i].path)) {
            if (detail && detail_capacity) {
                (void)snprintf(detail, detail_capacity,
                               "foundation package path %.180s is outside its allowed roots",
                               entries[i].path);
                detail[detail_capacity - 1u] = '\0';
            }
            return false;
        }
        rootfs_work_entry_t candidate = entries[i];
        if (candidate.kind == ROOTFS_WORK_ENTRY_DIRECTORY)
            candidate.existing_policy = ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
        else if (candidate.kind == ROOTFS_WORK_ENTRY_SYMLINK)
            candidate.existing_policy =
                ROOTFS_WORK_EXISTING_REUSE_IDENTICAL_SYMLINK;
        if (!plan_add_entry(plan, &candidate, mapped,
                            detail, detail_capacity))
            return false;
    }
    payload_tar_stats_t stats;
    payload_tar_get_stats(tar, &stats);
    plan->stats.hardlinks_materialised += stats.hardlinks_materialised;
    return true;
}

static bool plan_script_append(vm_guest_rootfs_plan_t *plan,
                               const char *text) {
    if (!plan || !text) return false;
    size_t length = strlen(text);
    if (length + 1u > PLAN_SCRIPT_CAPACITY - plan->script_size) return false;
    memcpy(plan->script + plan->script_size, text, length + 1u);
    plan->script_size += length;
    return true;
}

static bool plan_script_package(vm_guest_rootfs_plan_t *plan,
                                const vm_guest_package_t *package) {
    return plan_script_append(plan, "install_one \"$packages/") &&
           plan_script_append(plan, package->filename) &&
           plan_script_append(plan, "\" || exit 1\n");
}

static bool plan_build_script(vm_guest_rootfs_plan_t *plan,
                              const vm_guest_package_input_t *inputs,
                              size_t input_count) {
    if (!plan_script_append(plan, SCRIPT_HEADER)) return false;
    for (unsigned pass = 0u; pass < 2u; pass++) {
        for (size_t i = 0u; i < input_count; i++) {
            bool foundation =
                (inputs[i].package->roles & VM_GUEST_PACKAGE_FOUNDATION) != 0u;
            if (foundation != (pass == 0u)) continue;
            if (!plan_script_package(plan, inputs[i].package)) return false;
        }
    }
    return plan_script_append(plan, SCRIPT_FOOTER);
}

static bool plan_add_directory(vm_guest_rootfs_plan_t *plan,
                               const char *path,
                               char *detail, size_t detail_capacity) {
    rootfs_work_entry_t entry;
    memset(&entry, 0, sizeof entry);
    entry.kind = ROOTFS_WORK_ENTRY_DIRECTORY;
    entry.permissions = 0755u;
    entry.existing_policy = ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
    return plan_add_entry(plan, &entry, path, detail, detail_capacity);
}

static bool plan_add_file(vm_guest_rootfs_plan_t *plan, const char *path,
                          const uint8_t *bytes, size_t size,
                          uint16_t permissions,
                          char *detail, size_t detail_capacity) {
    rootfs_work_entry_t entry;
    memset(&entry, 0, sizeof entry);
    entry.kind = ROOTFS_WORK_ENTRY_FILE;
    entry.path = path;
    entry.content = bytes;
    entry.content_size = size;
    entry.permissions = permissions;
    return plan_add_entry(plan, &entry, path, detail, detail_capacity);
}

static bool plan_add_runtime_entries(
    vm_guest_rootfs_plan_t *plan,
    const vm_guest_package_input_t *inputs, size_t input_count,
    char *detail, size_t detail_capacity) {
    static const char *DIRECTORIES[] = {
        "/private/var/lib",
        "/private/var/lib/dpkg",
        VM_GUEST_ROOTFS_STATE_DIRECTORY,
        VM_GUEST_ROOTFS_PACKAGE_DIRECTORY,
        "/private/var/log",
        "/usr/libexec",
        "/System/Library/LaunchDaemons"
    };
    for (size_t i = 0u; i < sizeof DIRECTORIES / sizeof DIRECTORIES[0]; i++) {
        if (!plan_add_directory(plan, DIRECTORIES[i],
                                detail, detail_capacity))
            return false;
    }
    if (!plan_add_file(plan, "/private/var/lib/dpkg/status", NULL, 0u, 0644u,
                       detail, detail_capacity) ||
        !plan_add_file(plan, "/private/var/lib/dpkg/available", NULL, 0u, 0644u,
                       detail, detail_capacity))
        return false;

    for (size_t i = 0u; i < input_count; i++) {
        char path[ROOTFS_WORK_MAX_PATH];
        int written = snprintf(path, sizeof path, "%s/%s",
                               VM_GUEST_ROOTFS_PACKAGE_DIRECTORY,
                               inputs[i].package->filename);
        if (written <= 0 || (size_t)written >= sizeof path ||
            !plan_add_file(plan, path, plan->package_bytes[i], inputs[i].size,
                           0644u, detail, detail_capacity))
            return false;
    }
    return plan_add_file(plan, VM_GUEST_ROOTFS_INSTALL_SCRIPT,
                         (const uint8_t *)plan->script, plan->script_size,
                         0755u, detail, detail_capacity) &&
           plan_add_file(plan, VM_GUEST_ROOTFS_LAUNCHD_PLIST,
                         (const uint8_t *)INSTALL_PLIST,
                         sizeof INSTALL_PLIST - 1u, 0644u,
                         detail, detail_capacity);
}

static bool plan_hash_text(ios3_sha256_context_t *context,
                           const char *text) {
    return text && ios3_sha256_update(context, text, strlen(text));
}

static bool plan_compute_manifest(vm_guest_rootfs_plan_t *plan,
                                  const vm_guest_package_input_t *inputs,
                                  size_t input_count) {
    ios3_sha256_context_t context;
    if (!ios3_sha256_init(&context) ||
        !plan_hash_text(&context, "s5lbox-guest-rootfs-plan 1\n") ||
        !plan_hash_text(&context,
                        "aliases /etc=/private/etc /var=/private/var\n"))
        return false;
    for (size_t i = 0u; i < input_count; i++) {
        const vm_guest_package_t *package = inputs[i].package;
        if (!plan_hash_text(&context, package->filename) ||
            !plan_hash_text(&context, "\t") ||
            !plan_hash_text(&context, package->sha256_hex) ||
            !plan_hash_text(&context, "\t"))
            return false;
        char ending[64];
        int length = snprintf(ending, sizeof ending, "%llu\t%u\n",
                              (unsigned long long)package->size,
                              (unsigned)package->roles);
        if (length <= 0 || (size_t)length >= sizeof ending ||
            !ios3_sha256_update(&context, ending, (size_t)length))
            return false;
    }
    static const char SCRIPT_LABEL[] = "script\n";
    static const char PLIST_LABEL[] = "plist\n";
    return ios3_sha256_update(&context, SCRIPT_LABEL,
                              sizeof SCRIPT_LABEL - 1u) &&
           ios3_sha256_update(&context, plan->script, plan->script_size) &&
           ios3_sha256_update(&context, PLIST_LABEL,
                              sizeof PLIST_LABEL - 1u) &&
           ios3_sha256_update(&context, INSTALL_PLIST,
                              sizeof INSTALL_PLIST - 1u) &&
           ios3_sha256_final(&context, plan->manifest_sha256);
}

vm_guest_rootfs_plan_t *
vm_guest_rootfs_plan_open(const vm_guest_package_input_t *inputs,
                          size_t input_count,
                          vm_guest_rootfs_status_t *out_status,
                          char *detail, size_t detail_capacity) {
    plan_status(out_status, VM_GUEST_ROOTFS_ERR_ARGUMENT);
    plan_detail(detail, detail_capacity, "");
    if (!inputs || input_count == 0u ||
        input_count > VM_GUEST_ROOTFS_MAX_PACKAGES) {
        plan_detail(detail, detail_capacity,
                    "the package input count is invalid");
        return NULL;
    }

    vm_guest_rootfs_plan_t *plan = plan_allocate(input_count);
    if (!plan) {
        plan_status(out_status, VM_GUEST_ROOTFS_ERR_MEMORY);
        plan_detail(detail, detail_capacity, "out of memory for the rootfs plan");
        return NULL;
    }

    for (size_t i = 0u; i < input_count; i++) {
        const vm_guest_package_t *package = inputs[i].package;
        char manifest_detail[VM_GUEST_ROOTFS_DETAIL_CAPACITY];
        if (!vm_guest_package_validate_entry(package, manifest_detail,
                                             sizeof manifest_detail)) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_MANIFEST);
            plan_detail(detail, detail_capacity, manifest_detail);
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
        if (!inputs[i].bytes || inputs[i].size != package->size) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_PACKAGE_IDENTITY);
            if (detail && detail_capacity) {
                (void)snprintf(detail, detail_capacity,
                               "%.160s has the wrong byte count", package->filename);
                detail[detail_capacity - 1u] = '\0';
            }
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
        if (package->size > VM_GUEST_ROOTFS_MAX_DOWNLOAD_BYTES -
                            plan->stats.download_bytes) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_LIMIT);
            plan_detail(detail, detail_capacity,
                        "the package set exceeds the download-byte limit");
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
        for (size_t j = 0u; j < i; j++) {
            if (strcmp(package->package, inputs[j].package->package) == 0) {
                plan_status(out_status, VM_GUEST_ROOTFS_ERR_MANIFEST);
                plan_detail(detail, detail_capacity,
                            "the package inputs contain a duplicate name");
                vm_guest_rootfs_plan_close(&plan);
                return NULL;
            }
        }

        uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE];
        if (!ios3_sha256(inputs[i].bytes, inputs[i].size, digest) ||
            !vm_guest_package_download_matches(package, inputs[i].size,
                                               digest)) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_PACKAGE_IDENTITY);
            if (detail && detail_capacity) {
                (void)snprintf(detail, detail_capacity,
                               "%.160s failed its SHA-256 gate", package->filename);
                detail[detail_capacity - 1u] = '\0';
            }
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }

        plan->package_bytes[i] = (uint8_t *)malloc(inputs[i].size);
        if (!plan->package_bytes[i]) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_MEMORY);
            plan_detail(detail, detail_capacity,
                        "out of memory while retaining package bytes");
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
        memcpy(plan->package_bytes[i], inputs[i].bytes, inputs[i].size);
        plan->stats.packages++;
        plan->stats.download_bytes += package->size;

        vm_deb_archive_t archive;
        vm_deb_status_t deb = vm_deb_archive_open(
            &archive, plan->package_bytes[i], inputs[i].size);
        if (deb != VM_DEB_OK) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_ARCHIVE);
            if (detail && detail_capacity) {
                (void)snprintf(detail, detail_capacity,
                               "%.140s is not an accepted Debian archive: %s",
                               package->filename, vm_deb_status_text(deb));
                detail[detail_capacity - 1u] = '\0';
            }
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
        if (archive.control.compression != VM_DEB_COMPRESSION_GZIP) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_COMPRESSION);
            if (detail && detail_capacity) {
                (void)snprintf(detail, detail_capacity,
                               "%.140s does not use control.tar.gz",
                               package->filename);
                detail[detail_capacity - 1u] = '\0';
            }
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }

        if ((package->roles & VM_GUEST_PACKAGE_FOUNDATION) == 0u) continue;
        plan->stats.foundation_packages++;
        if (archive.data.compression != VM_DEB_COMPRESSION_GZIP) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_COMPRESSION);
            if (detail && detail_capacity) {
                (void)snprintf(detail, detail_capacity,
                               "foundation package %.130s does not use data.tar.gz",
                               package->filename);
                detail[detail_capacity - 1u] = '\0';
            }
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
        uint32_t tar_size = 0u;
        if (vm_deb_gzip_size(&archive.data, &tar_size) != VM_DEB_OK ||
            tar_size == 0u || tar_size > PAYLOAD_TAR_MAX_BYTES) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_COMPRESSION);
            plan_detail(detail, detail_capacity,
                        "a foundation data tar has an invalid expanded size");
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
        uint8_t *tar_bytes = (uint8_t *)malloc(tar_size);
        if (!tar_bytes) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_MEMORY);
            plan_detail(detail, detail_capacity,
                        "out of memory while expanding a foundation package");
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
        vmfw_mem_sink_t sink;
        vmfw_mem_sink_init(&sink, tar_bytes, tar_size);
        uint64_t produced = 0u;
        vmfw_inflate_status_t inflate_status = VMFW_INFLATE_OK;
        vm_deb_status_t inflated = vm_deb_gzip_inflate(
            &archive.data, vmfw_mem_sink_write, &sink,
            PAYLOAD_TAR_MAX_BYTES, &produced, &inflate_status);
        if (inflated != VM_DEB_OK || produced != tar_size ||
            sink.len != tar_size) {
            free(tar_bytes);
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_COMPRESSION);
            if (detail && detail_capacity) {
                (void)snprintf(detail, detail_capacity,
                               "%.130s data.tar.gz refused: %s",
                               package->filename, vm_deb_status_text(inflated));
                detail[detail_capacity - 1u] = '\0';
            }
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }

        char tar_detail[PAYLOAD_TAR_DETAIL_CAPACITY];
        payload_tar_t *tar = payload_tar_open_memory(
            tar_bytes, tar_size, "", tar_detail, sizeof tar_detail);
        free(tar_bytes);
        if (!tar) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_TAR);
            plan_detail(detail, detail_capacity, tar_detail);
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
        plan->tars[plan->tar_count++] = tar;
        plan->stats.foundation_tar_bytes += tar_size;
        if (!plan_add_tar(plan, tar, detail, detail_capacity)) {
            plan_status(out_status, VM_GUEST_ROOTFS_ERR_ENTRY);
            vm_guest_rootfs_plan_close(&plan);
            return NULL;
        }
    }

    if (plan->stats.foundation_packages == 0u) {
        plan_status(out_status, VM_GUEST_ROOTFS_ERR_MANIFEST);
        plan_detail(detail, detail_capacity,
                    "the rootfs plan has no dpkg foundation packages");
        vm_guest_rootfs_plan_close(&plan);
        return NULL;
    }
    if (!plan_build_script(plan, inputs, input_count)) {
        plan_status(out_status, VM_GUEST_ROOTFS_ERR_SCRIPT);
        plan_detail(detail, detail_capacity,
                    "the first-boot install script exceeds its bound");
        vm_guest_rootfs_plan_close(&plan);
        return NULL;
    }
    if (!plan_add_runtime_entries(plan, inputs, input_count,
                                  detail, detail_capacity)) {
        plan_status(out_status, VM_GUEST_ROOTFS_ERR_ENTRY);
        vm_guest_rootfs_plan_close(&plan);
        return NULL;
    }
    if (!plan_compute_manifest(plan, inputs, input_count)) {
        plan_status(out_status, VM_GUEST_ROOTFS_ERR_MANIFEST);
        plan_detail(detail, detail_capacity,
                    "the rootfs manifest digest could not be computed");
        vm_guest_rootfs_plan_close(&plan);
        return NULL;
    }
    plan->manifest_valid = true;
    plan->stats.entries = plan->entry_count;
    plan_status(out_status, VM_GUEST_ROOTFS_OK);
    return plan;
}

static uint8_t *plan_read_package(const char *path, uint64_t expected,
                                  size_t *out_size) {
    if (out_size) *out_size = 0u;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    long end = ftell(file);
    if (end <= 0 || (uint64_t)end != expected ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    uint8_t *bytes = (uint8_t *)malloc((size_t)end);
    if (!bytes) {
        (void)fclose(file);
        return NULL;
    }
    bool read_ok = fread(bytes, 1u, (size_t)end, file) == (size_t)end;
    bool close_ok = fclose(file) == 0;
    if (!read_ok || !close_ok) {
        free(bytes);
        return NULL;
    }
    *out_size = (size_t)end;
    return bytes;
}

vm_guest_rootfs_plan_t *
vm_guest_rootfs_plan_open_directory(const char *directory,
                                    vm_guest_rootfs_status_t *out_status,
                                    char *detail, size_t detail_capacity) {
    plan_status(out_status, VM_GUEST_ROOTFS_ERR_ARGUMENT);
    plan_detail(detail, detail_capacity, "");
    if (!directory || !*directory ||
        !vm_guest_package_manifest_validate(detail, detail_capacity)) {
        plan_status(out_status, directory && *directory
                                  ? VM_GUEST_ROOTFS_ERR_MANIFEST
                                  : VM_GUEST_ROOTFS_ERR_ARGUMENT);
        return NULL;
    }
    size_t count = vm_guest_package_count();
    if (count == 0u || count > VM_GUEST_ROOTFS_MAX_PACKAGES) {
        plan_status(out_status, VM_GUEST_ROOTFS_ERR_MANIFEST);
        plan_detail(detail, detail_capacity,
                    "the shipping package count is outside the plan limit");
        return NULL;
    }
    vm_guest_package_input_t *inputs =
        (vm_guest_package_input_t *)calloc(count, sizeof *inputs);
    if (!inputs) {
        plan_status(out_status, VM_GUEST_ROOTFS_ERR_MEMORY);
        plan_detail(detail, detail_capacity,
                    "out of memory for package input records");
        return NULL;
    }
    bool loaded = true;
    for (size_t i = 0u; i < count; i++) {
        const vm_guest_package_t *package = vm_guest_package_at(i);
        char path[PLAN_PATH_CAPACITY];
        size_t directory_length = strlen(directory);
        const char *separator =
            directory[directory_length - 1u] == '/' ||
            directory[directory_length - 1u] == '\\' ? "" : "/";
        int written = snprintf(path, sizeof path, "%s%s%s", directory,
                               separator, package->filename);
        if (written <= 0 || (size_t)written >= sizeof path) {
            loaded = false;
        } else {
            inputs[i].package = package;
            inputs[i].bytes = plan_read_package(path, package->size,
                                                &inputs[i].size);
            loaded = inputs[i].bytes != NULL;
        }
        if (!loaded) {
            if (detail && detail_capacity) {
                (void)snprintf(detail, detail_capacity,
                               "could not read exact package %.160s",
                               package->filename);
                detail[detail_capacity - 1u] = '\0';
            }
            break;
        }
    }
    vm_guest_rootfs_plan_t *plan = NULL;
    if (loaded)
        plan = vm_guest_rootfs_plan_open(inputs, count, out_status,
                                         detail, detail_capacity);
    else
        plan_status(out_status, VM_GUEST_ROOTFS_ERR_PACKAGE_IO);
    for (size_t i = 0u; i < count; i++) free((void *)inputs[i].bytes);
    free(inputs);
    return plan;
}

const rootfs_work_entry_t *
vm_guest_rootfs_plan_entries(const vm_guest_rootfs_plan_t *plan) {
    return plan ? plan->entries : NULL;
}

size_t vm_guest_rootfs_plan_entry_count(const vm_guest_rootfs_plan_t *plan) {
    return plan ? plan->entry_count : 0u;
}

void vm_guest_rootfs_plan_get_stats(const vm_guest_rootfs_plan_t *plan,
                                    vm_guest_rootfs_stats_t *out) {
    if (!out) return;
    if (plan) *out = plan->stats;
    else memset(out, 0, sizeof *out);
}

bool vm_guest_rootfs_plan_manifest_sha256(
    const vm_guest_rootfs_plan_t *plan,
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE]) {
    if (digest) memset(digest, 0, VM_GUEST_PACKAGE_SHA256_SIZE);
    if (!plan || !digest || !plan->manifest_valid) return false;
    memcpy(digest, plan->manifest_sha256, VM_GUEST_PACKAGE_SHA256_SIZE);
    return true;
}

const char *vm_guest_rootfs_status_text(vm_guest_rootfs_status_t status) {
    switch (status) {
        case VM_GUEST_ROOTFS_OK:                   return "ok";
        case VM_GUEST_ROOTFS_ERR_ARGUMENT:         return "invalid argument";
        case VM_GUEST_ROOTFS_ERR_MANIFEST:         return "invalid manifest";
        case VM_GUEST_ROOTFS_ERR_PACKAGE_IO:       return "package I/O failure";
        case VM_GUEST_ROOTFS_ERR_PACKAGE_IDENTITY: return "package identity mismatch";
        case VM_GUEST_ROOTFS_ERR_ARCHIVE:          return "invalid Debian archive";
        case VM_GUEST_ROOTFS_ERR_COMPRESSION:      return "invalid package compression";
        case VM_GUEST_ROOTFS_ERR_TAR:              return "invalid package data tar";
        case VM_GUEST_ROOTFS_ERR_ENTRY:            return "conflicting rootfs overlay";
        case VM_GUEST_ROOTFS_ERR_LIMIT:            return "rootfs plan limit";
        case VM_GUEST_ROOTFS_ERR_MEMORY:           return "out of memory";
        case VM_GUEST_ROOTFS_ERR_SCRIPT:           return "install script limit";
        default:                                   return "unknown status";
    }
}
