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
#define PLAN_SCRIPT_CAPACITY 24576u
#define PLAN_PATH_CAPACITY 1400u

static const char CYDIA_SOURCE_LIST[] =
    VM_GUEST_ROOTFS_SAURIK_SOURCE_LINE;
static const char BIGBOSS_SOURCE_LIST[] =
    VM_GUEST_ROOTFS_BIGBOSS_SOURCE_LINE;
static const char IOS3_PARTY_SOURCE_LIST[] =
    VM_GUEST_ROOTFS_IOS3_PARTY_SOURCE_LINE;
static const char APT_COMPAT_CONFIGURATION[] =
    VM_GUEST_ROOTFS_APT_COMPAT_CONTENT;

/* Public signing key from the retained iPhone OS 3 bootstrap artifact. Its
 * OpenPGP fingerprint is A9C96A37115894A23B894107694D17D38764B4F4 and its
 * exact bytes verified BigBoss's 2026-08-17 Release.gpg. Keep the binary
 * keyring form: the pinned APT 0.7 gpgv method reads only
 * /etc/apt/trusted.gpg and does not scan trusted.gpg.d. */
static const uint8_t BIGBOSS_TRUSTED_KEYRING[] =
    "\x99\x01\xa2\x04\x48\x10\x72\x94\x11\x04\x00\x85\xb6\x56\x48\xdb"
    "\xb5\xfe\xd8\x73\xbe\x9d\xac\x12\xf9\x21\x5e\x2c\x66\x3f\xd6\x06"
    "\xdf\xe5\x04\x0d\x5f\xff\x6f\x35\xd7\x64\xe3\xee\xf1\x7e\x8c\x67"
    "\x0e\x2b\x3a\xf6\x75\xca\x55\x91\xfa\x84\xb4\xa7\x6c\xaa\x9d\x6c"
    "\x09\xc3\x44\xbb\xb3\xbc\x49\x68\x19\xd0\xad\x9f\x3d\xf0\x56\xec"
    "\xe6\xee\x08\x3c\x3e\xf8\xc4\xfb\xa1\x0e\x33\x8d\x18\xe1\x34\x5d"
    "\x6c\x2f\x1b\xf6\xa0\xe4\xbd\xf6\x08\xd7\x6f\x48\xd4\xe5\xa6\xee"
    "\x24\x60\xeb\xbd\x3c\x1a\x59\x5f\x86\x40\xf0\x2c\x43\x8d\x88\x37"
    "\x95\xbf\x11\xc1\x5a\x13\x7d\xfc\xf6\xf6\x6b\x00\xa0\xcd\xdd\x99"
    "\x74\xd2\xc4\x0b\xc7\x2d\xc2\xb6\x2e\xf4\x6f\x92\x08\x21\xb6\xe6"
    "\xd7\x03\xff\x71\xf6\x0c\x3b\xfc\x78\x0d\x3c\xdb\x20\xcd\x57\x64"
    "\x46\x43\xc4\x5d\xe6\x0d\xa0\x53\xa9\x6d\x2c\x5c\x7a\xc1\x8e\xe4"
    "\xcc\xe8\x06\x70\xeb\x5d\x23\x81\xd0\x03\x61\x91\xc0\x8d\x7d\x75"
    "\x14\x71\x64\x25\x46\xad\x89\x4f\x90\x42\xb3\x64\x80\x5c\x7c\xa3"
    "\x1c\x00\x56\xca\x2c\x77\xa5\xa6\x6a\x07\xb6\x56\x3a\x61\xc8\x6e"
    "\xcd\x6f\x84\x1a\xbe\xe9\x06\x73\x7e\xaf\xc7\x86\xb5\x92\xc0\xf1"
    "\xa7\x32\xe6\x89\x71\xae\x02\x08\x85\x6e\x80\x30\x5b\x49\xbc\x0d"
    "\x78\xaa\xea\xed\x9a\x35\xb3\x05\x81\xf6\xe7\x4e\x8b\xb7\x69\xf7"
    "\xce\x44\xe2\x03\xfe\x2e\xe1\x8b\x2e\xd1\x97\x3a\x5b\xfe\xc6\x31"
    "\xa5\xd1\xa1\x01\x20\x5a\x6b\x65\x50\x47\x60\x2d\x57\x84\x46\x2e"
    "\x47\x83\xe3\x69\x9b\x54\x01\x9f\xfc\x92\xee\x51\xe3\x11\x14\x41"
    "\x0a\xc3\x4d\xf6\xe4\x0f\xff\xdb\x7a\x1e\xe4\x96\xaf\x94\x62\x61"
    "\x55\x71\x4e\x8c\x79\x10\x9e\x63\x38\x0a\x0b\x55\x27\xdf\x61\xf6"
    "\xc5\xcc\x04\x52\x6d\x74\x60\xf9\xbd\x0f\xce\x9c\x9c\x8e\x96\x05"
    "\x38\x82\x09\x7f\xd3\x0e\xe4\x9c\x0e\xd4\x41\x26\x2c\x0f\x0b\xc5"
    "\x9f\xa0\x7f\x1e\xc0\xbc\x87\x11\xe4\x0b\x86\xf4\x9d\x9e\x3e\xf2"
    "\xcf\xce\x77\x5d\x09\xb4\x20\x42\x69\x67\x42\x6f\x73\x73\x20\x3c"
    "\x62\x69\x67\x62\x6f\x73\x73\x40\x74\x68\x65\x62\x69\x67\x62\x6f"
    "\x73\x73\x2e\x6f\x72\x67\x3e\x88\x60\x04\x13\x11\x02\x00\x20\x05"
    "\x02\x48\x10\x72\x94\x02\x1b\x03\x06\x0b\x09\x08\x07\x03\x02\x04"
    "\x15\x02\x08\x03\x04\x16\x02\x03\x01\x02\x1e\x01\x02\x17\x80\x00"
    "\x0a\x09\x10\x69\x4d\x17\xd3\x87\x64\xb4\xf4\xaf\x6f\x00\x9f\x54"
    "\x60\xd5\x4f\x6a\xdb\xd1\x8a\x8b\x1f\x91\x55\xb4\x19\xc2\xec\x47"
    "\x67\x14\x93\x00\xa0\xa1\xeb\xde\x91\xae\xed\xd4\x15\xee\x9c\x55"
    "\x74\x15\xd8\x26\xc8\x0a\xea\x04\xde\xb0\x02\x00\x03\xb9\x02\x0d"
    "\x04\x48\x10\x72\x99\x10\x08\x00\xc2\x70\x34\x96\x63\xe4\xb4\xd2"
    "\x9c\x26\x70\x28\x47\x13\xd0\x0f\x7c\xeb\x84\xa4\xb9\xd5\x5f\x0b"
    "\x78\x31\x4e\x1c\x2a\x98\x44\x76\xf0\x1f\x6a\x59\x34\xd6\xf0\x75"
    "\x99\x91\x0e\xc9\x6f\x30\xa6\x12\x43\x0a\xc7\x83\xd0\x9e\xf3\xee"
    "\x20\x2e\x61\x44\x8b\xd9\x20\x4e\x89\x5b\xc0\x6e\xd2\x1d\xe3\x24"
    "\x2e\xc0\xaa\xdd\xc0\xbd\x06\x38\xa8\x31\x95\xb6\x40\x52\x43\x1a"
    "\xe8\x88\x75\x17\xaf\x98\xd9\x9a\x20\xa2\x4d\xc7\x86\x22\xa3\x29"
    "\xec\x7e\x8c\x38\xfa\x1b\x78\x40\xbb\xef\x7e\x79\x28\x5f\x93\x2c"
    "\x8f\x55\xc2\x03\xb0\xba\x19\x09\xfa\x56\x25\x19\x09\xf5\x6c\x5c"
    "\xc8\x8e\x54\xe2\x80\x58\xa2\x11\x5c\x1a\x9c\x4b\xee\xce\xad\x25"
    "\x2e\x23\x7e\x3d\xef\xb7\x12\x51\x9c\xfc\x32\xa9\x14\x5d\x0c\x14"
    "\x53\xbc\xdc\xad\xca\x95\xd5\x4a\x28\xec\x02\x20\xbe\x1a\xe2\x22"
    "\x91\x21\x4d\xfe\x29\x74\x26\xc9\x57\x47\x52\x13\xca\x99\x83\x7d"
    "\x1f\xc7\xfa\x57\x3d\xc2\xff\x28\xe6\xed\x92\x7e\x7c\xff\x9d\x11"
    "\x01\xc3\x72\x2d\x90\xeb\xd7\x94\xb6\x76\x76\x1f\x98\xd4\x5a\x60"
    "\xe3\x1b\x30\x41\xdb\xa9\x5f\x84\x41\x37\x13\x16\x1f\x22\x8a\xf2"
    "\x62\x1a\x52\x38\x7e\x28\x5f\x3f\x00\x03\x07\x07\xff\x4e\x24\xb5"
    "\x0a\xa1\x28\xb3\x02\xe3\xe4\xdd\x76\x94\xe9\xf5\x89\x01\x14\xef"
    "\x73\x93\x97\x6e\xb8\x27\xc7\x4b\x79\xf4\x5a\x0c\x2b\x8b\x59\x7d"
    "\x71\xa4\x2e\x95\x72\x04\x43\x29\x8c\x05\x85\xac\x0f\x39\x26\x1f"
    "\xa8\xc1\x50\x2c\xbe\x5b\x32\xe5\x8e\x70\x84\x47\x34\xd0\xfc\xb0"
    "\xa5\xa3\x11\x24\x6c\x75\x0e\xc7\x8c\xfe\xf3\x8f\x01\x78\x92\x3c"
    "\xc8\xf2\x8c\xc7\xd9\x17\x80\x00\x6e\xd1\xc4\xc2\xe6\x42\x2a\xe3"
    "\xd1\xee\x15\x4e\x0a\x5d\xef\x87\x59\xbd\xe4\x21\x3e\x5f\x54\x15"
    "\x28\xbb\xdf\xd5\x0d\x75\x08\x9b\x50\x3b\xc1\x58\xa1\x0d\x39\x0a"
    "\x01\xfa\x63\x4c\x19\x81\xb4\x0d\x88\x7e\x4d\x1a\xc8\xc9\x27\x5b"
    "\xd3\x76\xfb\x3a\x92\x36\xaa\xe5\x30\x88\x3a\x6c\x0a\x98\x5d\xf7"
    "\x9d\x56\xd8\x84\x1a\x8d\xfa\xee\xe2\xe9\x7b\xe4\xf5\xcb\xb3\xdf"
    "\xa8\x94\x4c\x71\x16\x15\x80\x50\xd5\x0c\x9f\x3c\x87\x17\x22\xb4"
    "\xad\xdd\x69\xe3\x29\xb5\x5d\x7d\xbb\xed\xb6\x54\x93\xbf\xc2\x3e"
    "\xe4\x32\xc6\x40\xb2\xfa\x00\x4b\xbd\x49\xf8\x75\x5e\x3f\xf4\x80"
    "\x43\x4e\x50\x49\xb7\x1b\x00\x11\xe7\xd4\x68\xe3\x3d\xe3\xfd\x54"
    "\xdf\xac\x0d\x7f\xd9\xa0\x3f\xf0\xc0\x03\xdc\x0a\x55\x88\x49\x04"
    "\x18\x11\x02\x00\x09\x05\x02\x48\x10\x72\x99\x02\x1b\x0c\x00\x0a"
    "\x09\x10\x69\x4d\x17\xd3\x87\x64\xb4\xf4\x05\xdd\x00\xa0\xa2\x4c"
    "\x2c\x36\xb2\x28\x74\x08\x0d\x55\x90\x18\x89\x95\xd8\x76\x32\x97"
    "\xcf\xed\x00\x9f\x5c\x7d\xb6\xfc\x82\xc8\x6c\xdc\xd2\xfd\x64\x5f"
    "\x41\xf4\xf4\x64\x74\x54\xac\x84\xb0\x02\x00\x03";

const uint8_t *vm_guest_rootfs_bigboss_keyring(size_t *out_size) {
    if (out_size) *out_size = sizeof BIGBOSS_TRUSTED_KEYRING - 1u;
    return BIGBOSS_TRUSTED_KEYRING;
}

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

static const char SCRIPT_CONFIGURE[] =
    "/usr/bin/dpkg --force-depends --configure -a || exit 1\n";

static const char SCRIPT_FOOTER[] =
    "cydia=/Applications/Cydia.app/Cydia_\n"
    "[ -f \"$cydia\" ] || exit 1\n"
    "/bin/chown 0:0 \"$cydia\" || exit 1\n"
    "/bin/chmod 6755 \"$cydia\" || exit 1\n"
    "[ -u \"$cydia\" ] && [ -g \"$cydia\" ] || exit 1\n"
    "[ -x /usr/bin/uicache ] || exit 1\n"
    "attempt=0\n"
    "while [ \"$attempt\" -lt 60 ]; do\n"
    "    if /usr/bin/killall -0 SpringBoard >/dev/null 2>&1; then\n"
    "        break\n"
    "    fi\n"
    "    attempt=$((attempt + 1))\n"
    "    /bin/sleep 1\n"
    "done\n"
    "[ \"$attempt\" -lt 60 ] || exit 1\n"
    "/bin/su --login --command /usr/bin/uicache mobile || exit 1\n"
    "cache=/private/var/mobile/Library/Caches/"
        "com.apple.mobile.installation.plist\n"
    "/bin/grep -aq 'com.saurik.Cydia' \"$cache\" || exit 1\n"
    "icon_cache=/private/var/mobile/Library/Caches/SpringBoardIconCache\n"
    "[ -s \"$icon_cache/com.saurik.Cydia\" ] || exit 1\n"
    "/usr/bin/killall SpringBoard || exit 1\n"
    ": >\"$state/complete.partial\" || exit 1\n"
    "/bin/sync\n"
    "/bin/mv -f \"$state/complete.partial\" \"$state/complete\" || exit 1\n"
    "/bin/sync\n"
    "echo \"s5lbox guest install complete\"\n"
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

/* Foundation payloads only seed enough files to start the guest's dpkg. The
 * pinned ncurses preinst owns this compatibility alias: it removes the old
 * path and recreates it as a symlink to /usr/lib before unpacking. Preseeding
 * the archive's directory form makes unlink fail with EISDIR and the following
 * symlink fail with EEXIST. Leave that exact subtree absent so the original
 * package and its maintainer script remain the authority for the final shape. */
static bool plan_defer_path_to_guest_dpkg(
    const vm_guest_package_t *package, const char *path) {
    static const char NCURSES_ALIAS[] = "/usr/lib/_ncurses";
    if (!package || !path ||
        strcmp(package->package, "ncurses") != 0 ||
        strcmp(package->version, "5.7-10") != 0 ||
        strcmp(package->filename,
               "ncurses_5.7-10_iphoneos-arm.deb") != 0)
        return false;
    size_t alias_length = sizeof NCURSES_ALIAS - 1u;
    return strcmp(path, NCURSES_ALIAS) == 0 ||
           (strncmp(path, NCURSES_ALIAS, alias_length) == 0 &&
            path[alias_length] == '/');
}

static bool plan_add_tar(vm_guest_rootfs_plan_t *plan,
                         const vm_guest_package_t *package,
                         payload_tar_t *tar,
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
        if (plan_defer_path_to_guest_dpkg(package, mapped)) continue;
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

static bool plan_script_apt_cache_tool(
    vm_guest_rootfs_plan_t *plan,
    const vm_guest_package_t *package) {
    return plan_script_append(plan, "cache_archive=\"$packages/") &&
           plan_script_append(plan, package->filename) &&
           plan_script_append(plan, "\"\n") &&
           plan_script_append(
               plan,
               "cache_stage=\"$state/apt-cache-tool.stage\"\n"
               "apt_get=/usr/libexec/s5lbox-apt-get\n"
               "apt_cache=/usr/libexec/s5lbox-apt-cache\n"
               "if [ -f \"$cache_archive\" ]; then\n"
               "    /bin/rm -rf \"$cache_stage\" || exit 1\n"
               "    /bin/mkdir -p \"$cache_stage\" || exit 1\n"
               "    /usr/bin/dpkg-deb --extract \"$cache_archive\" \"$cache_stage\" || exit 1\n"
               "    [ -x \"$cache_stage/usr/bin/apt-get\" ] || exit 1\n"
               "    [ -x \"$cache_stage/usr/bin/apt-cache\" ] || exit 1\n"
               "    /bin/cp \"$cache_stage/usr/bin/apt-get\" \"$apt_get.partial\" || exit 1\n"
               "    /bin/cp \"$cache_stage/usr/bin/apt-cache\" \"$apt_cache.partial\" || exit 1\n"
               "    /bin/chown 0:0 \"$apt_get.partial\" \"$apt_cache.partial\" || exit 1\n"
               "    /bin/chmod 0755 \"$apt_get.partial\" \"$apt_cache.partial\" || exit 1\n"
               "    /bin/mv -f \"$apt_get.partial\" \"$apt_get\" || exit 1\n"
               "    /bin/mv -f \"$apt_cache.partial\" \"$apt_cache\" || exit 1\n"
               "    /bin/rm -rf \"$cache_stage\" || exit 1\n"
               "fi\n"
               "[ -x \"$apt_get\" ] && [ -x \"$apt_cache\" ] || exit 1\n"
               "/bin/rm -f /private/var/cache/apt/pkgcache.bin /private/var/cache/apt/srcpkgcache.bin || exit 1\n"
               "run_apt_update() {\n"
               "    update_token=\"$state/apt-update.$$.running\"\n"
               "    /bin/rm -f \"$update_token\"\n"
               "    (\n"
               "        exec \"$apt_get\" -o Acquire::PDiffs=false -o Acquire::http::Timeout=30 -o Acquire::Retries=1 update\n"
               "    ) &\n"
               "    apt_pid=$!\n"
               "    : >\"$update_token\" || { kill -TERM \"$apt_pid\" >/dev/null 2>&1; wait \"$apt_pid\" >/dev/null 2>&1; return 1; }\n"
               "    (\n"
               "        /bin/sleep 180\n"
               "        [ -e \"$update_token\" ] || exit 0\n"
               "        echo \"APT refresh attempt $1 exceeded 180 seconds; terminating it\"\n"
               "        kill -TERM \"$apt_pid\" >/dev/null 2>&1 || exit 0\n"
               "        /bin/sleep 5\n"
               "        [ -e \"$update_token\" ] || exit 0\n"
               "        kill -KILL \"$apt_pid\" >/dev/null 2>&1 || true\n"
               "    ) &\n"
               "    watchdog_pid=$!\n"
               "    wait \"$apt_pid\"\n"
               "    apt_status=$?\n"
               "    /bin/rm -f \"$update_token\"\n"
               "    kill -TERM \"$watchdog_pid\" >/dev/null 2>&1 || true\n"
               "    return \"$apt_status\"\n"
               "}\n"
               "attempt=1\n"
               "while [ \"$attempt\" -le 3 ]; do\n"
               "    run_apt_update \"$attempt\" && break\n"
               "    attempt=$((attempt + 1))\n"
               "    [ \"$attempt\" -le 3 ] && /bin/sleep 10\n"
               "done\n"
               "[ \"$attempt\" -le 3 ] || echo 'APT refresh incomplete; rebuilding from retained lists'\n"
               "/bin/rm -f /private/var/cache/apt/pkgcache.bin /private/var/cache/apt/srcpkgcache.bin || exit 1\n"
               "\"$apt_cache\" gencaches || exit 1\n"
               "[ -s /private/var/cache/apt/pkgcache.bin ] || exit 1\n"
               "[ -s /private/var/cache/apt/srcpkgcache.bin ] || exit 1\n"
               "\"$apt_cache\" stats >/dev/null 2>&1 || exit 1\n"
               "/bin/sync\n"
               "/bin/rm -f \"$cache_archive\" || exit 1\n"
               "echo 'Cydia APT caches built outside the application watchdog'\n");
}

static bool plan_build_script(vm_guest_rootfs_plan_t *plan,
                              const vm_guest_package_input_t *inputs,
                              size_t input_count) {
    if (!plan_script_append(plan, SCRIPT_HEADER)) return false;
    for (unsigned pass = 0u; pass < 2u; pass++) {
        for (size_t i = 0u; i < input_count; i++) {
            if ((inputs[i].package->roles & VM_GUEST_PACKAGE_INSTALL) == 0u)
                continue;
            bool foundation =
                (inputs[i].package->roles & VM_GUEST_PACKAGE_FOUNDATION) != 0u;
            if (foundation != (pass == 0u)) continue;
            if (!plan_script_package(plan, inputs[i].package)) return false;
        }
    }
    if (!plan_script_append(plan, SCRIPT_CONFIGURE)) return false;
    size_t cache_tools = 0u;
    for (size_t i = 0u; i < input_count; i++) {
        if ((inputs[i].package->roles &
             VM_GUEST_PACKAGE_APT_CACHE_TOOL) == 0u)
            continue;
        if (++cache_tools != 1u ||
            !plan_script_apt_cache_tool(plan, inputs[i].package))
            return false;
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
        "/private/etc/apt",
        "/private/etc/apt/sources.list.d",
        VM_GUEST_ROOTFS_APT_COMPAT_DIRECTORY,
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
                       detail, detail_capacity) ||
        !plan_add_file(plan, VM_GUEST_ROOTFS_SAURIK_SOURCE_PATH,
                       (const uint8_t *)CYDIA_SOURCE_LIST,
                       sizeof CYDIA_SOURCE_LIST - 1u, 0644u,
                       detail, detail_capacity) ||
        !plan_add_file(plan, VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH,
                       (const uint8_t *)BIGBOSS_SOURCE_LIST,
                       sizeof BIGBOSS_SOURCE_LIST - 1u, 0644u,
                       detail, detail_capacity) ||
        !plan_add_file(plan, VM_GUEST_ROOTFS_IOS3_PARTY_SOURCE_PATH,
                       (const uint8_t *)IOS3_PARTY_SOURCE_LIST,
                       sizeof IOS3_PARTY_SOURCE_LIST - 1u, 0644u,
                       detail, detail_capacity) ||
        !plan_add_file(plan, VM_GUEST_ROOTFS_APT_COMPAT_PATH,
                       (const uint8_t *)APT_COMPAT_CONFIGURATION,
                       sizeof APT_COMPAT_CONFIGURATION - 1u, 0644u,
                       detail, detail_capacity) ||
        !plan_add_file(plan, VM_GUEST_ROOTFS_TRUSTED_KEYRING_PATH,
                       BIGBOSS_TRUSTED_KEYRING,
                       sizeof BIGBOSS_TRUSTED_KEYRING - 1u, 0644u,
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
        !plan_hash_text(&context, "s5lbox-guest-rootfs-plan 8\n") ||
        !plan_hash_text(&context,
                        "aliases /etc=/private/etc /var=/private/var\n") ||
        !plan_hash_text(
            &context,
            "source " VM_GUEST_ROOTFS_SAURIK_SOURCE_PATH "\n") ||
        !ios3_sha256_update(&context, CYDIA_SOURCE_LIST,
                            sizeof CYDIA_SOURCE_LIST - 1u) ||
        !plan_hash_text(
            &context,
            "source " VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH "\n") ||
        !ios3_sha256_update(&context, BIGBOSS_SOURCE_LIST,
                            sizeof BIGBOSS_SOURCE_LIST - 1u) ||
        !plan_hash_text(
            &context,
            "source " VM_GUEST_ROOTFS_IOS3_PARTY_SOURCE_PATH "\n") ||
        !ios3_sha256_update(&context, IOS3_PARTY_SOURCE_LIST,
                            sizeof IOS3_PARTY_SOURCE_LIST - 1u) ||
        !plan_hash_text(
            &context,
            "apt-config " VM_GUEST_ROOTFS_APT_COMPAT_PATH "\n") ||
        !ios3_sha256_update(&context, APT_COMPAT_CONFIGURATION,
                            sizeof APT_COMPAT_CONFIGURATION - 1u) ||
        !plan_hash_text(
            &context,
            "keyring " VM_GUEST_ROOTFS_TRUSTED_KEYRING_PATH "\n") ||
        !ios3_sha256_update(&context, BIGBOSS_TRUSTED_KEYRING,
                            sizeof BIGBOSS_TRUSTED_KEYRING - 1u))
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
        if (!plan_add_tar(plan, package, tar, detail, detail_capacity)) {
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
