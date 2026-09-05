/* See VMGuestInstallBuild.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMGuestInstallBuild.h"

#include "VMResumeCheckpoint.h"
#include "VMSnapshotStore.h"
#include "bringup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(S5LBOX_GUEST_INSTALL_TESTING)
/* The policy test needs the real snapshot format and PMU state, not 128 MiB
 * of otherwise irrelevant zero RAM on every CI platform. Product builds never
 * define this test flag and always verify the hardware's exact geometry. */
#define BUILD_CHECKPOINT_RAM_BASE S5L_BRINGUP_PHYS_BASE
#define BUILD_CHECKPOINT_RAM_SIZE UINT32_C(0x00100000)
#else
#define BUILD_CHECKPOINT_RAM_BASE S5L_BRINGUP_PHYS_BASE
#define BUILD_CHECKPOINT_RAM_SIZE S5L_BRINGUP_RAM_SIZE
#endif

typedef struct {
    vm_guest_install_build_progress_t callback;
    void *context;
} build_progress_adapter_t;

/* Extracted from the previously pinned cydia_1.0.3044-66 package, then matched on
 * the retained physical guest that exposed the historical 0755 install. This
 * is the executable data-fork identity, not the .deb archive identity. */
static const uint8_t VM_CYDIA_LEGACY_EXECUTABLE_SHA256[
    IOS3_SHA256_DIGEST_SIZE] = {
    0x4cu, 0xa3u, 0xf7u, 0x0fu, 0xe5u, 0xcbu, 0x67u, 0x73u,
    0x76u, 0x88u, 0xabu, 0x06u, 0x14u, 0xc6u, 0x86u, 0xfeu,
    0x18u, 0xb1u, 0x24u, 0x84u, 0x43u, 0x69u, 0x84u, 0x8bu,
    0x76u, 0x84u, 0x6fu, 0x80u, 0xa5u, 0x2fu, 0x63u, 0x24u
};

/* Cydia 1.0.3172-68 has the upstream package-name encoding fallback. Keep
 * both publisher identities exact; recognizing the current executable must
 * not break maintenance of retained installations or trust arbitrary bytes. */
static const uint8_t VM_CYDIA_EXECUTABLE_SHA256[
    IOS3_SHA256_DIGEST_SIZE] = {
    0x20u, 0x15u, 0x58u, 0x33u, 0x51u, 0xeeu, 0x73u, 0xe0u,
    0xc9u, 0x74u, 0x60u, 0xb8u, 0x90u, 0x37u, 0xf1u, 0x19u,
    0x26u, 0xfeu, 0xaau, 0xccu, 0x42u, 0xbdu, 0xc4u, 0x72u,
    0x5fu, 0x3eu, 0x2au, 0x78u, 0xa1u, 0x91u, 0x6cu, 0x64u
};

/* /usr/bin/gpgv from the exact gnupg_1.4.8-4 archive. The archive remains a
 * download-only input; this identity recognizes a guest that already has the
 * exact executable instead of treating any same-name file as sufficient. */
static const uint8_t VM_APT_VERIFIER_SHA256[
    IOS3_SHA256_DIGEST_SIZE] = {
    0xb9u, 0x1fu, 0xf6u, 0x08u, 0x73u, 0x1bu, 0x6cu, 0x75u,
    0x0cu, 0x87u, 0xacu, 0xdfu, 0x36u, 0xb0u, 0x1fu, 0x58u,
    0xebu, 0x24u, 0x9bu, 0x4eu, 0xeeu, 0xdfu, 0x39u, 0x07u,
    0x73u, 0x47u, 0x2eu, 0x6cu, 0x35u, 0xd1u, 0xbdu, 0x8du
};

static void build_apt_verifier_probe(
    rootfs_work_file_repair_t *probe) {
    memset(probe, 0, sizeof *probe);
    probe->path = "/usr/bin/gpgv";
    probe->expected_size = UINT64_C(428640);
    memcpy(probe->expected_sha256, VM_APT_VERIFIER_SHA256,
           sizeof probe->expected_sha256);
    probe->expected_owner_id = 0u;
    probe->expected_group_id = 0u;
    probe->expected_permissions = 0755u;
    probe->desired_owner_id = 0u;
    probe->desired_group_id = 0u;
    probe->desired_permissions = 0755u;
}

static void build_cydia_privilege_repair(
    rootfs_work_file_repair_t *repair, bool legacy) {
    memset(repair, 0, sizeof *repair);
    repair->path = "/Applications/Cydia.app/Cydia_";
    repair->expected_size = legacy ? UINT64_C(320704) : UINT64_C(330896);
    memcpy(repair->expected_sha256,
           legacy ? VM_CYDIA_LEGACY_EXECUTABLE_SHA256
                  : VM_CYDIA_EXECUTABLE_SHA256,
           sizeof repair->expected_sha256);
    repair->expected_owner_id = 0u;
    repair->expected_group_id = 0u;
    repair->expected_permissions = 0755u;
    repair->desired_owner_id = 0u;
    repair->desired_group_id = 0u;
    repair->desired_permissions = 06755u;
}

static const uint8_t VM_BIGBOSS_SOURCE[] =
    VM_GUEST_ROOTFS_BIGBOSS_SOURCE_LINE;
static const uint8_t VM_IOS3_PARTY_SOURCE[] =
    VM_GUEST_ROOTFS_IOS3_PARTY_SOURCE_LINE;
static const uint8_t VM_APT_COMPAT_CONFIGURATION[] =
    VM_GUEST_ROOTFS_APT_COMPAT_CONTENT;
/* A short-lived compatibility build wrote this equivalent spelling before
 * packet-loss evidence disproved the URL rewrite as a remedy. It expands to
 * the same index URLs in old APT. Accept it on existing test/install media so
 * cache recovery does not needlessly refuse the disk, but never create it. */
static const uint8_t VM_BIGBOSS_SOURCE_COMPAT[] =
    "deb http://apt.thebigboss.org/repofiles/cydia/ "
    "dists/stable/main/binary-iphoneos-arm/\n";

static bool build_managed_file_probe_for(
    rootfs_work_file_repair_t *probe, const char *path,
    const uint8_t *content, size_t content_size) {
    if (!probe || !path || !*path || !content || !content_size) return false;
    memset(probe, 0, sizeof *probe);
    probe->path = path;
    probe->expected_size = content_size;
    if (!ios3_sha256(content, content_size, probe->expected_sha256))
        return false;
    probe->expected_permissions = 0644u;
    probe->desired_permissions = 0644u;
    return true;
}

static bool build_bigboss_source_probe(
    rootfs_work_file_repair_t *probe) {
    return build_managed_file_probe_for(
        probe, VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH,
        VM_BIGBOSS_SOURCE, sizeof VM_BIGBOSS_SOURCE - 1u);
}

static bool build_ios3_party_source_probe(
    rootfs_work_file_repair_t *probe) {
    return build_managed_file_probe_for(
        probe, VM_GUEST_ROOTFS_IOS3_PARTY_SOURCE_PATH,
        VM_IOS3_PARTY_SOURCE, sizeof VM_IOS3_PARTY_SOURCE - 1u);
}

static bool build_apt_compat_probe(
    rootfs_work_file_repair_t *probe) {
    return build_managed_file_probe_for(
        probe, VM_GUEST_ROOTFS_APT_COMPAT_PATH,
        VM_APT_COMPAT_CONFIGURATION,
        sizeof VM_APT_COMPAT_CONFIGURATION - 1u);
}

/* This is the exact complete trusted.gpg carried by the retained historical
 * bootstrap. It already contains the same BigBoss key, so preserve and accept
 * it without embedding or replacing the unrelated legacy repository keys. */
static const uint8_t VM_HISTORICAL_APT_TRUST_SHA256[
    IOS3_SHA256_DIGEST_SIZE] = {
    0xcfu, 0x37u, 0x73u, 0xdfu, 0x09u, 0x25u, 0x0eu, 0x12u,
    0xcfu, 0x08u, 0xedu, 0x0fu, 0x15u, 0x73u, 0xf6u, 0x60u,
    0x56u, 0x13u, 0x2bu, 0xd1u, 0xffu, 0xfeu, 0xc3u, 0x3du,
    0xc7u, 0x48u, 0xe7u, 0x4bu, 0x9cu, 0x18u, 0x23u, 0xbcu
};

static bool build_apt_trust_probe_for(
    rootfs_work_file_repair_t *probe, uint64_t size,
    const uint8_t digest[IOS3_SHA256_DIGEST_SIZE]) {
    if (!probe || !digest || size == 0u) return false;
    memset(probe, 0, sizeof *probe);
    probe->path = VM_GUEST_ROOTFS_TRUSTED_KEYRING_PATH;
    probe->expected_size = size;
    memcpy(probe->expected_sha256, digest, IOS3_SHA256_DIGEST_SIZE);
    probe->expected_permissions = 0644u;
    probe->desired_permissions = 0644u;
    return true;
}

static bool build_bigboss_keyring_probe(
    rootfs_work_file_repair_t *probe) {
    size_t size = 0u;
    const uint8_t *keyring = vm_guest_rootfs_bigboss_keyring(&size);
    uint8_t digest[IOS3_SHA256_DIGEST_SIZE];
    return keyring && size != 0u && ios3_sha256(keyring, size, digest) &&
           build_apt_trust_probe_for(probe, (uint64_t)size, digest);
}

static bool build_historical_keyring_probe(
    rootfs_work_file_repair_t *probe) {
    return build_apt_trust_probe_for(
        probe, UINT64_C(5974), VM_HISTORICAL_APT_TRUST_SHA256);
}

static const uint8_t VM_BIGBOSS_CACHE_CLEANUP[] =
    "#!/bin/sh\n"
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin\n"
    "export PATH\n"
    "state=/private/var/lib/s5lbox\n"
    "marker=$state/cydia-index-cache-v2.complete\n"
    "[ -e \"$marker\" ] && exit 0\n"
    "log=/private/var/log/s5lbox-cydia-index-cache-v2.log\n"
    "exec >>\"$log\" 2>&1\n"
    "for cache in /private/var/lib/apt/lists/apt.thebigboss.org_repofiles_cydia_dists_stable_* /private/var/lib/apt/lists/partial/apt.thebigboss.org_repofiles_cydia_dists_stable_*; do\n"
    "    [ -e \"$cache\" ] || continue\n"
    "    /bin/rm -f \"$cache\" || exit 1\n"
    "done\n"
    "/bin/rm -f /private/var/cache/apt/pkgcache.bin /private/var/cache/apt/srcpkgcache.bin || exit 1\n"
    ": >\"$marker.partial\" || exit 1\n"
    "/bin/sync\n"
    "/bin/mv -f \"$marker.partial\" \"$marker\" || exit 1\n"
    "/bin/sync\n"
    "echo 'cached BigBoss package indexes removed for a clean retry'\n"
    "exit 0\n";

#define VM_BIGBOSS_CACHE_CLEANUP_PATH \
    "/private/var/lib/s5lbox/cydia-index-cache-v2"

static const uint8_t VM_BIGBOSS_CACHE_CLEANUP_PLIST[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key>\n"
    "  <string>com.j0shua.s5lbox.cydia-index-cache-v2</string>\n"
    "  <key>ProgramArguments</key>\n"
    "  <array>\n"
    "    <string>" VM_BIGBOSS_CACHE_CLEANUP_PATH "</string>\n"
    "  </array>\n"
    "  <key>RunAtLoad</key>\n"
    "  <true/>\n"
    "</dict>\n"
    "</plist>\n";

static const uint8_t VM_APT_TRUST_CACHE_CLEANUP[] =
    "#!/bin/sh\n"
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin\n"
    "export PATH\n"
    "state=/private/var/lib/s5lbox\n"
    "marker=$state/apt-trust-v1.complete\n"
    "[ -e \"$marker\" ] && exit 0\n"
    "log=/private/var/log/s5lbox-apt-trust-v1.log\n"
    "exec >>\"$log\" 2>&1\n"
    "for cache in /private/var/lib/apt/lists/apt.thebigboss.org_repofiles_cydia_dists_stable_* /private/var/lib/apt/lists/partial/apt.thebigboss.org_repofiles_cydia_dists_stable_*; do\n"
    "    [ -e \"$cache\" ] || continue\n"
    "    /bin/rm -f \"$cache\" || exit 1\n"
    "done\n"
    "/bin/rm -f /private/var/cache/apt/pkgcache.bin /private/var/cache/apt/srcpkgcache.bin || exit 1\n"
    ": >\"$marker.partial\" || exit 1\n"
    "/bin/sync\n"
    "/bin/mv -f \"$marker.partial\" \"$marker\" || exit 1\n"
    "/bin/sync\n"
    "echo 'APT trust installed; cached BigBoss indexes removed'\n"
    "exit 0\n";

#define VM_APT_TRUST_CACHE_CLEANUP_PATH \
    "/private/var/lib/s5lbox/apt-trust-v1"

static const uint8_t VM_APT_TRUST_CACHE_CLEANUP_PLIST[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key>\n"
    "  <string>com.j0shua.s5lbox.apt-trust-v1</string>\n"
    "  <key>ProgramArguments</key>\n"
    "  <array>\n"
    "    <string>" VM_APT_TRUST_CACHE_CLEANUP_PATH "</string>\n"
    "  </array>\n"
    "  <key>RunAtLoad</key>\n"
    "  <true/>\n"
    "</dict>\n"
    "</plist>\n";

#define VM_APT_VERIFIER_PACKAGE_DIRECTORY \
    "/private/var/lib/s5lbox/apt-verifier-v1"
#define VM_APT_VERIFIER_PACKAGE_PATH \
    VM_APT_VERIFIER_PACKAGE_DIRECTORY "/gnupg_1.4.8-4_iphoneos-arm.deb"
#define VM_APT_VERIFIER_INSTALL_PATH \
    "/private/var/lib/s5lbox/apt-verifier-v1-install"

static const uint8_t VM_APT_VERIFIER_INSTALL[] =
    "#!/bin/sh\n"
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin\n"
    "export PATH\n"
    "state=/private/var/lib/s5lbox\n"
    "marker=$state/apt-verifier-v1.complete\n"
    "[ -e \"$marker\" ] && exit 0\n"
    "package=" VM_APT_VERIFIER_PACKAGE_PATH "\n"
    "log=/private/var/log/s5lbox-apt-verifier-v1.log\n"
    "exec >>\"$log\" 2>&1\n"
    "echo 'installing the legacy APT signature verifier'\n"
    "[ -f \"$package\" ] || exit 1\n"
    "/usr/bin/dpkg --force-depends --install \"$package\" || exit 1\n"
    "/usr/bin/dpkg --status gnupg >/dev/null 2>&1 || exit 1\n"
    "[ -x /usr/bin/gpgv ] || exit 1\n"
    "/usr/bin/gpgv --version >/dev/null 2>&1 || exit 1\n"
    "for cache in /private/var/lib/apt/lists/apt.thebigboss.org_repofiles_cydia_dists_stable_* /private/var/lib/apt/lists/partial/apt.thebigboss.org_repofiles_cydia_dists_stable_*; do\n"
    "    [ -e \"$cache\" ] || continue\n"
    "    /bin/rm -f \"$cache\" || exit 1\n"
    "done\n"
    "/bin/rm -f /private/var/cache/apt/pkgcache.bin /private/var/cache/apt/srcpkgcache.bin || exit 1\n"
    ": >\"$marker.partial\" || exit 1\n"
    "/bin/sync\n"
    "/bin/mv -f \"$marker.partial\" \"$marker\" || exit 1\n"
    "/bin/sync\n"
    "/bin/rm -f \"$package\" || exit 1\n"
    "echo 'legacy APT signature verifier installed'\n"
    "exit 0\n";

static const uint8_t VM_APT_VERIFIER_INSTALL_PLIST[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key>\n"
    "  <string>com.j0shua.s5lbox.apt-verifier-v1</string>\n"
    "  <key>ProgramArguments</key>\n"
    "  <array>\n"
    "    <string>" VM_APT_VERIFIER_INSTALL_PATH "</string>\n"
    "  </array>\n"
    "  <key>RunAtLoad</key>\n"
    "  <true/>\n"
    "</dict>\n"
    "</plist>\n";

#define VM_CYDIA_CACHE_V3_STATE_DIRECTORY \
    "/private/var/lib/s5lbox/cydia-cache-v3"
#define VM_CYDIA_CACHE_V3_PACKAGE_PATH \
    VM_CYDIA_CACHE_V3_STATE_DIRECTORY "/apt7_0.7.20.2-1_iphoneos-arm.deb"
#define VM_CYDIA_CACHE_V3_RUN_PATH \
    "/private/var/lib/s5lbox/cydia-cache-v3-run"

/* Exact v3 bytes are retained as the fail-closed identity for superseding an
 * already-staged, unbounded worker. Unknown contents are never overwritten. */
static const uint8_t VM_CYDIA_CACHE_V3_RUN[] =
    "#!/bin/sh\n"
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin\n"
    "export PATH\n"
    "state=" VM_CYDIA_CACHE_V3_STATE_DIRECTORY "\n"
    "marker=$state/complete\n"
    "[ -e \"$marker\" ] && exit 0\n"
    "package=" VM_CYDIA_CACHE_V3_PACKAGE_PATH "\n"
    "stage=$state/extract.partial\n"
    "apt_get=$state/apt-get\n"
    "apt_cache=$state/apt-cache\n"
    "cydia=/Applications/Cydia.app/Cydia_\n"
    "log=/private/var/log/s5lbox-cydia-cache-v3.log\n"
    "exec >>\"$log\" 2>&1\n"
    "echo 'building Cydia caches outside the application watchdog'\n"
    "[ -f \"$cydia\" ] || exit 1\n"
    "/bin/chown 0:0 \"$cydia\" || exit 1\n"
    "/bin/chmod 0600 \"$cydia\" || exit 1\n"
    "if [ -f \"$package\" ]; then\n"
    "    /bin/rm -rf \"$stage\" || exit 1\n"
    "    /bin/mkdir -p \"$stage\" || exit 1\n"
    "    /usr/bin/dpkg-deb --extract \"$package\" \"$stage\" || exit 1\n"
    "    [ -x \"$stage/usr/bin/apt-get\" ] || exit 1\n"
    "    [ -x \"$stage/usr/bin/apt-cache\" ] || exit 1\n"
    "    /bin/cp \"$stage/usr/bin/apt-get\" \"$apt_get.partial\" || exit 1\n"
    "    /bin/cp \"$stage/usr/bin/apt-cache\" \"$apt_cache.partial\" || exit 1\n"
    "    /bin/chown 0:0 \"$apt_get.partial\" \"$apt_cache.partial\" || exit 1\n"
    "    /bin/chmod 0755 \"$apt_get.partial\" \"$apt_cache.partial\" || exit 1\n"
    "    /bin/mv -f \"$apt_get.partial\" \"$apt_get\" || exit 1\n"
    "    /bin/mv -f \"$apt_cache.partial\" \"$apt_cache\" || exit 1\n"
    "    /bin/rm -rf \"$stage\" || exit 1\n"
    "fi\n"
    "[ -x \"$apt_get\" ] && [ -x \"$apt_cache\" ] || exit 1\n"
    "/bin/rm -f /private/var/cache/apt/pkgcache.bin /private/var/cache/apt/srcpkgcache.bin || exit 1\n"
    "attempt=0\n"
    "while [ \"$attempt\" -lt 3 ]; do\n"
    "    \"$apt_get\" -o Acquire::http::Timeout=30 -o Acquire::Retries=3 update && break\n"
    "    attempt=$((attempt + 1))\n"
    "    /bin/sleep 10\n"
    "done\n"
    "[ \"$attempt\" -lt 3 ] || echo 'APT refresh incomplete; trying authenticated retained lists'\n"
    "/bin/rm -f /private/var/cache/apt/pkgcache.bin /private/var/cache/apt/srcpkgcache.bin || exit 1\n"
    "\"$apt_cache\" gencaches || exit 1\n"
    "[ -s /private/var/cache/apt/pkgcache.bin ] || exit 1\n"
    "[ -s /private/var/cache/apt/srcpkgcache.bin ] || exit 1\n"
    "\"$apt_cache\" stats >/dev/null 2>&1 || exit 1\n"
    "/bin/chown 0:0 \"$cydia\" || exit 1\n"
    "/bin/chmod 6755 \"$cydia\" || exit 1\n"
    "[ -u \"$cydia\" ] && [ -g \"$cydia\" ] || exit 1\n"
    ": >\"$marker.partial\" || exit 1\n"
    "/bin/sync\n"
    "/bin/mv -f \"$marker.partial\" \"$marker\" || exit 1\n"
    "/bin/sync\n"
    "/bin/rm -f \"$package\" || exit 1\n"
    "echo 'Cydia cache build complete'\n"
    "exit 0\n";

static const uint8_t VM_CYDIA_CACHE_V3_SUPERSEDED[] =
    "#!/bin/sh\n"
    "# Superseded by the bounded cydia-cache-v4 worker.\n"
    "exit 0\n";

#define VM_CYDIA_CACHE_STATE_DIRECTORY \
    "/private/var/lib/s5lbox/cydia-cache-v4"
#define VM_CYDIA_CACHE_PACKAGE_PATH \
    VM_CYDIA_CACHE_STATE_DIRECTORY "/apt7_0.7.20.2-1_iphoneos-arm.deb"
#define VM_CYDIA_CACHE_RUN_PATH \
    "/private/var/lib/s5lbox/cydia-cache-v4-run"

static const uint8_t VM_CYDIA_CACHE_RUN[] =
    "#!/bin/sh\n"
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin\n"
    "export PATH\n"
    "state=" VM_CYDIA_CACHE_STATE_DIRECTORY "\n"
    "marker=$state/complete\n"
    "[ -e \"$marker\" ] && exit 0\n"
    "package=" VM_CYDIA_CACHE_PACKAGE_PATH "\n"
    "stage=$state/extract.partial\n"
    "apt_get=$state/apt-get\n"
    "apt_cache=$state/apt-cache\n"
    "cydia=/Applications/Cydia.app/Cydia_\n"
    "log=/private/var/log/s5lbox-cydia-cache-v4.log\n"
    "exec >>\"$log\" 2>&1\n"
    "echo 'building Cydia caches with a bounded legacy APT worker'\n"
    "[ -f \"$cydia\" ] || exit 1\n"
    "/bin/chown 0:0 \"$cydia\" || exit 1\n"
    "/bin/chmod 0600 \"$cydia\" || exit 1\n"
    "if [ -f \"$package\" ]; then\n"
    "    /bin/rm -rf \"$stage\" || exit 1\n"
    "    /bin/mkdir -p \"$stage\" || exit 1\n"
    "    /usr/bin/dpkg-deb --extract \"$package\" \"$stage\" || exit 1\n"
    "    [ -x \"$stage/usr/bin/apt-get\" ] || exit 1\n"
    "    [ -x \"$stage/usr/bin/apt-cache\" ] || exit 1\n"
    "    /bin/cp \"$stage/usr/bin/apt-get\" \"$apt_get.partial\" || exit 1\n"
    "    /bin/cp \"$stage/usr/bin/apt-cache\" \"$apt_cache.partial\" || exit 1\n"
    "    /bin/chown 0:0 \"$apt_get.partial\" \"$apt_cache.partial\" || exit 1\n"
    "    /bin/chmod 0755 \"$apt_get.partial\" \"$apt_cache.partial\" || exit 1\n"
    "    /bin/mv -f \"$apt_get.partial\" \"$apt_get\" || exit 1\n"
    "    /bin/mv -f \"$apt_cache.partial\" \"$apt_cache\" || exit 1\n"
    "    /bin/rm -rf \"$stage\" || exit 1\n"
    "fi\n"
    "[ -x \"$apt_get\" ] && [ -x \"$apt_cache\" ] || exit 1\n"
    "for cache in /private/var/lib/apt/lists/partial/* /private/var/lib/apt/lists/apt.thebigboss.org_*Packages.diff_Index* /private/var/lib/apt/lists/*.FAILED; do\n"
    "    [ -f \"$cache\" ] || continue\n"
    "    /bin/rm -f \"$cache\" || exit 1\n"
    "done\n"
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
    "[ \"$attempt\" -le 3 ] || echo 'APT refresh incomplete; trying retained lists'\n"
    "/bin/rm -f /private/var/cache/apt/pkgcache.bin /private/var/cache/apt/srcpkgcache.bin || exit 1\n"
    "\"$apt_cache\" gencaches || exit 1\n"
    "[ -s /private/var/cache/apt/pkgcache.bin ] || exit 1\n"
    "[ -s /private/var/cache/apt/srcpkgcache.bin ] || exit 1\n"
    "\"$apt_cache\" stats >/dev/null 2>&1 || exit 1\n"
    "ios3_list=missing\n"
    "for cache in /private/var/lib/apt/lists/ios3.party_*Packages; do\n"
    "    [ -s \"$cache\" ] && ios3_list=present\n"
    "done\n"
    "echo \"ios3.party package list: $ios3_list\"\n"
    "/bin/chown 0:0 \"$cydia\" || exit 1\n"
    "/bin/chmod 6755 \"$cydia\" || exit 1\n"
    "[ -u \"$cydia\" ] && [ -g \"$cydia\" ] || exit 1\n"
    ": >\"$marker.partial\" || exit 1\n"
    "/bin/sync\n"
    "/bin/mv -f \"$marker.partial\" \"$marker\" || exit 1\n"
    "/bin/sync\n"
    "/bin/rm -f \"$package\" || exit 1\n"
    "echo 'Cydia cache build complete'\n"
    "exit 0\n";

static const uint8_t VM_CYDIA_CACHE_V4_SUPERSEDED[] =
    "#!/bin/sh\n"
    "# Superseded by the offline-first cydia-cache-v5 worker.\n"
    "exit 0\n";

#define VM_CYDIA_CACHE_V5_STATE_DIRECTORY \
    "/private/var/lib/s5lbox/cydia-cache-v5"
#define VM_CYDIA_CACHE_V5_PACKAGE_PATH \
    VM_CYDIA_CACHE_V5_STATE_DIRECTORY "/apt7_0.7.20.2-1_iphoneos-arm.deb"
#define VM_CYDIA_CACHE_V5_RUN_PATH \
    "/private/var/lib/s5lbox/cydia-cache-v5-run"

static const uint8_t VM_CYDIA_CACHE_V5_RUN_HEAD[] =
    "#!/bin/sh\n"
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin\n"
    "export PATH\n"
    "state=" VM_CYDIA_CACHE_V5_STATE_DIRECTORY "\n"
    "marker=$state/complete\n"
    "[ -e \"$marker\" ] && exit 0\n"
    "package=" VM_CYDIA_CACHE_V5_PACKAGE_PATH "\n"
    "stage=$state/extract.partial\n"
    "apt_get=$state/apt-get\n"
    "apt_cache=$state/apt-cache\n"
    "cydia=/Applications/Cydia.app/Cydia_\n"
    "live_pkgcache=/private/var/cache/apt/pkgcache.bin\n"
    "live_srcpkgcache=/private/var/cache/apt/srcpkgcache.bin\n"
    "staged_pkgcache=$state/pkgcache.bin.partial\n"
    "staged_srcpkgcache=$state/srcpkgcache.bin.partial\n"
    "backup_pkgcache=$state/pkgcache.bin.backup\n"
    "backup_srcpkgcache=$state/srcpkgcache.bin.backup\n"
    "log=/private/var/log/s5lbox-cydia-cache-v5.log\n"
    "exec >>\"$log\" 2>&1\n"
    "echo 'recovering Cydia caches from retained repository lists first'\n"
    "[ -f \"$cydia\" ] || exit 1\n"
    "/bin/chown 0:0 \"$cydia\" || exit 1\n"
    "/bin/chmod 6755 \"$cydia\" || exit 1\n"
    "[ -u \"$cydia\" ] && [ -g \"$cydia\" ] || exit 1\n"
    "if [ -f \"$package\" ]; then\n"
    "    /bin/rm -rf \"$stage\" || exit 1\n"
    "    /bin/mkdir -p \"$stage\" || exit 1\n"
    "    /usr/bin/dpkg-deb --extract \"$package\" \"$stage\" || exit 1\n"
    "    [ -x \"$stage/usr/bin/apt-get\" ] || exit 1\n"
    "    [ -x \"$stage/usr/bin/apt-cache\" ] || exit 1\n"
    "    /bin/cp \"$stage/usr/bin/apt-get\" \"$apt_get.partial\" || exit 1\n"
    "    /bin/cp \"$stage/usr/bin/apt-cache\" \"$apt_cache.partial\" || exit 1\n"
    "    /bin/chown 0:0 \"$apt_get.partial\" \"$apt_cache.partial\" || exit 1\n"
    "    /bin/chmod 0755 \"$apt_get.partial\" \"$apt_cache.partial\" || exit 1\n"
    "    /bin/mv -f \"$apt_get.partial\" \"$apt_get\" || exit 1\n"
    "    /bin/mv -f \"$apt_cache.partial\" \"$apt_cache\" || exit 1\n"
    "    /bin/rm -rf \"$stage\" || exit 1\n"
    "fi\n"
    "[ -x \"$apt_get\" ] && [ -x \"$apt_cache\" ] || exit 1\n"
    "for cache in /private/var/lib/apt/lists/partial/* /private/var/lib/apt/lists/apt.thebigboss.org_*Packages.diff_Index* /private/var/lib/apt/lists/*.FAILED; do\n"
    "    [ -f \"$cache\" ] || continue\n"
    "    /bin/rm -f \"$cache\" || exit 1\n"
    "done\n"
    "cache_valid() {\n"
    "    [ -s \"$1\" ] || return 1\n"
    "    [ -s \"$2\" ] || return 1\n"
    "    \"$apt_cache\" -o Dir::Cache::pkgcache=\"$1\" -o Dir::Cache::srcpkgcache=\"$2\" stats >/dev/null 2>&1 || return 1\n"
    "    found_list=0\n"
    "    for cache in /private/var/lib/apt/lists/*_Packages; do\n"
    "        [ -s \"$cache\" ] && found_list=1\n"
    "    done\n"
    "    [ \"$found_list\" -eq 1 ] || return 1\n"
    "    return 0\n"
    "}\n"
    "generate_caches() {\n"
    "    /bin/rm -f \"$staged_pkgcache\" \"$staged_srcpkgcache\" || return 1\n"
    "    \"$apt_cache\" -o Dir::Cache::pkgcache=\"$staged_pkgcache\" -o Dir::Cache::srcpkgcache=\"$staged_srcpkgcache\" gencaches || return 1\n"
    "    cache_valid \"$staged_pkgcache\" \"$staged_srcpkgcache\"\n"
    "}\n"
    "restore_caches() {\n"
    "    if [ -s \"$backup_pkgcache\" ] && [ -s \"$backup_srcpkgcache\" ]; then\n"
    "        /bin/mv -f \"$backup_pkgcache\" \"$live_pkgcache\" || return 1\n"
    "        /bin/mv -f \"$backup_srcpkgcache\" \"$live_srcpkgcache\" || return 1\n"
    "    else\n"
    "        /bin/rm -f \"$live_pkgcache\" \"$live_srcpkgcache\" || return 1\n"
    "    fi\n"
    "    return 0\n"
    "}\n"
    "publish_caches() {\n"
    "    /bin/mkdir -p /private/var/cache/apt || return 1\n"
    "    /bin/rm -f \"$backup_pkgcache\" \"$backup_srcpkgcache\" || return 1\n"
    "    if cache_valid \"$live_pkgcache\" \"$live_srcpkgcache\"; then\n"
    "        /bin/cp \"$live_pkgcache\" \"$backup_pkgcache\" || return 1\n"
    "        /bin/cp \"$live_srcpkgcache\" \"$backup_srcpkgcache\" || return 1\n"
    "    fi\n"
    "    /bin/mv -f \"$staged_pkgcache\" \"$live_pkgcache\" || return 1\n"
    "    if ! /bin/mv -f \"$staged_srcpkgcache\" \"$live_srcpkgcache\"; then\n"
    "        restore_caches || true\n"
    "        return 1\n"
    "    fi\n"
    "    if ! cache_valid \"$live_pkgcache\" \"$live_srcpkgcache\"; then\n"
    "        restore_caches || true\n"
    "        return 1\n"
    "    fi\n"
    "    /bin/rm -f \"$backup_pkgcache\" \"$backup_srcpkgcache\" || return 1\n"
    "    return 0\n"
    "}\n";

static const uint8_t VM_CYDIA_CACHE_V5_RUN_TAIL[] =
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
    "        echo 'APT refresh exceeded 180 guest seconds; terminating it once'\n"
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
    "    wait \"$watchdog_pid\" >/dev/null 2>&1 || true\n"
    "    return \"$apt_status\"\n"
    "}\n"
    "if generate_caches; then\n"
    "    publish_caches || exit 1\n"
    "    echo 'retained repository lists produced valid Cydia caches'\n"
    "else\n"
    "    echo 'retained lists were insufficient; attempting one bounded repository refresh'\n"
    "    if run_apt_update; then\n"
    "        echo 'bounded repository refresh completed'\n"
    "    else\n"
    "        echo 'bounded repository refresh incomplete; rebuilding from every usable retained list'\n"
    "    fi\n"
    "    generate_caches || exit 1\n"
    "    publish_caches || exit 1\n"
    "fi\n"
    "list_count=0\n"
    "for cache in /private/var/lib/apt/lists/*_Packages; do\n"
    "    [ -s \"$cache\" ] || continue\n"
    "    list_count=$((list_count + 1))\n"
    "done\n"
    "echo \"usable repository package lists: $list_count\"\n"
    "[ \"$list_count\" -gt 0 ] || exit 1\n"
    "/bin/rm -f \"$package\" || exit 1\n"
    ": >\"$marker.partial\" || exit 1\n"
    "/bin/sync\n"
    "/bin/mv -f \"$marker.partial\" \"$marker\" || exit 1\n"
    "/bin/sync\n"
    "echo 'Cydia cache recovery complete'\n"
    "exit 0\n";

enum {
    VM_CYDIA_CACHE_V5_RUN_SIZE =
        sizeof VM_CYDIA_CACHE_V5_RUN_HEAD - 1u +
        sizeof VM_CYDIA_CACHE_V5_RUN_TAIL - 1u
};

static bool build_cydia_v5_run(uint8_t *buffer, size_t capacity,
                               size_t *size_out) {
    if (size_out) *size_out = VM_CYDIA_CACHE_V5_RUN_SIZE;
    if (!buffer || capacity < VM_CYDIA_CACHE_V5_RUN_SIZE + 1u)
        return false;
    size_t offset = sizeof VM_CYDIA_CACHE_V5_RUN_HEAD - 1u;
    memcpy(buffer, VM_CYDIA_CACHE_V5_RUN_HEAD, offset);
    memcpy(buffer + offset, VM_CYDIA_CACHE_V5_RUN_TAIL,
           sizeof VM_CYDIA_CACHE_V5_RUN_TAIL - 1u);
    buffer[VM_CYDIA_CACHE_V5_RUN_SIZE] = '\0';
    return true;
}

static const uint8_t VM_CYDIA_CACHE_V5_RUN_PLIST[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key>\n"
    "  <string>com.j0shua.s5lbox.cydia-cache-v5</string>\n"
    "  <key>ProgramArguments</key>\n"
    "  <array>\n"
    "    <string>" VM_CYDIA_CACHE_V5_RUN_PATH "</string>\n"
    "  </array>\n"
    "  <key>RunAtLoad</key>\n"
    "  <true/>\n"
    "</dict>\n"
    "</plist>\n";

static void build_cydia_v3_supersede_rewrite(
    rootfs_work_file_rewrite_t *rewrite) {
    memset(rewrite, 0, sizeof *rewrite);
    rewrite->path = VM_CYDIA_CACHE_V3_RUN_PATH;
    rewrite->expected_content = VM_CYDIA_CACHE_V3_RUN;
    rewrite->expected_content_size = sizeof VM_CYDIA_CACHE_V3_RUN - 1u;
    rewrite->desired_content = VM_CYDIA_CACHE_V3_SUPERSEDED;
    rewrite->desired_content_size =
        sizeof VM_CYDIA_CACHE_V3_SUPERSEDED - 1u;
    rewrite->owner_id = 0u;
    rewrite->group_id = 0u;
    rewrite->permissions = 0755u;
}

static void build_cydia_v4_supersede_rewrite(
    rootfs_work_file_rewrite_t *rewrite) {
    memset(rewrite, 0, sizeof *rewrite);
    rewrite->path = VM_CYDIA_CACHE_RUN_PATH;
    rewrite->expected_content = VM_CYDIA_CACHE_RUN;
    rewrite->expected_content_size = sizeof VM_CYDIA_CACHE_RUN - 1u;
    rewrite->desired_content = VM_CYDIA_CACHE_V4_SUPERSEDED;
    rewrite->desired_content_size =
        sizeof VM_CYDIA_CACHE_V4_SUPERSEDED - 1u;
    rewrite->owner_id = 0u;
    rewrite->group_id = 0u;
    rewrite->permissions = 0755u;
}

enum { VM_CYDIA_MAINTENANCE_MAX_ENTRIES = 22u };

static size_t build_cydia_maintenance_entries(
    rootfs_work_entry_t *entries, size_t capacity,
    bool include_source, bool create_source,
    bool include_trust, bool create_keyring,
    bool include_verifier, const uint8_t *verifier_package,
    size_t verifier_package_size,
    bool include_cache, const uint8_t *cache_package,
    size_t cache_package_size, const uint8_t *cache_run,
    size_t cache_run_size,
    bool create_ios3_source, bool create_apt_compat) {
    static const char *directories[] = {
        "/private/etc/apt",
        "/private/etc/apt/sources.list.d",
        "/private/var/lib/s5lbox",
        "/private/var/log",
        "/System/Library/LaunchDaemons"
    };
    const size_t directory_count =
        sizeof directories / sizeof directories[0];
    const size_t required = directory_count +
        (include_source ? 2u + (create_source ? 1u : 0u) : 0u) +
        (include_trust ? 2u + (create_keyring ? 1u : 0u) : 0u) +
        (include_verifier ? 4u : 0u) +
        (include_cache ? 5u + (create_ios3_source ? 1u : 0u) +
                             (create_apt_compat ? 1u : 0u) : 0u);
    if (include_verifier &&
        (!verifier_package || verifier_package_size == 0u))
        return required;
    if (include_cache &&
        (!cache_package || cache_package_size == 0u ||
         !cache_run || cache_run_size == 0u))
        return required;
    if (!entries || capacity < required) return required;
    memset(entries, 0, required * sizeof entries[0]);
    size_t count = 0u;
    for (size_t i = 0u; i < directory_count; i++) {
        entries[count].kind = ROOTFS_WORK_ENTRY_DIRECTORY;
        entries[count].path = directories[i];
        entries[count].permissions = 0755u;
        entries[count].existing_policy =
            ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
        count++;
    }
    if (include_source && create_source) {
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path = VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH;
        entries[count].content = VM_BIGBOSS_SOURCE;
        entries[count].content_size = sizeof VM_BIGBOSS_SOURCE - 1u;
        entries[count].permissions = 0644u;
        count++;
    }
    if (include_source) {
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        /* Cydia-era bootstraps may stash /usr/libexec behind a symlink. The
         * helper belongs with the private state it manages. */
        entries[count].path = VM_BIGBOSS_CACHE_CLEANUP_PATH;
        entries[count].content = VM_BIGBOSS_CACHE_CLEANUP;
        entries[count].content_size = sizeof VM_BIGBOSS_CACHE_CLEANUP - 1u;
        entries[count].permissions = 0755u;
        count++;
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path =
            "/System/Library/LaunchDaemons/"
            "com.j0shua.s5lbox.cydia-index-cache-v2.plist";
        entries[count].content = VM_BIGBOSS_CACHE_CLEANUP_PLIST;
        entries[count].content_size =
            sizeof VM_BIGBOSS_CACHE_CLEANUP_PLIST - 1u;
        entries[count].permissions = 0644u;
        count++;
    }
    if (include_trust && create_keyring) {
        size_t keyring_size = 0u;
        const uint8_t *keyring =
            vm_guest_rootfs_bigboss_keyring(&keyring_size);
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path = VM_GUEST_ROOTFS_TRUSTED_KEYRING_PATH;
        entries[count].content = keyring;
        entries[count].content_size = keyring_size;
        entries[count].permissions = 0644u;
        count++;
    }
    if (include_trust) {
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path = VM_APT_TRUST_CACHE_CLEANUP_PATH;
        entries[count].content = VM_APT_TRUST_CACHE_CLEANUP;
        entries[count].content_size =
            sizeof VM_APT_TRUST_CACHE_CLEANUP - 1u;
        entries[count].permissions = 0755u;
        count++;
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path =
            "/System/Library/LaunchDaemons/"
            "com.j0shua.s5lbox.apt-trust-v1.plist";
        entries[count].content = VM_APT_TRUST_CACHE_CLEANUP_PLIST;
        entries[count].content_size =
            sizeof VM_APT_TRUST_CACHE_CLEANUP_PLIST - 1u;
        entries[count].permissions = 0644u;
        count++;
    }
    if (include_verifier) {
        entries[count].kind = ROOTFS_WORK_ENTRY_DIRECTORY;
        entries[count].path = VM_APT_VERIFIER_PACKAGE_DIRECTORY;
        entries[count].permissions = 0755u;
        entries[count].existing_policy =
            ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
        count++;
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path = VM_APT_VERIFIER_PACKAGE_PATH;
        entries[count].content = verifier_package;
        entries[count].content_size = verifier_package_size;
        entries[count].permissions = 0644u;
        count++;
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path = VM_APT_VERIFIER_INSTALL_PATH;
        entries[count].content = VM_APT_VERIFIER_INSTALL;
        entries[count].content_size =
            sizeof VM_APT_VERIFIER_INSTALL - 1u;
        entries[count].permissions = 0755u;
        count++;
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path =
            "/System/Library/LaunchDaemons/"
            "com.j0shua.s5lbox.apt-verifier-v1.plist";
        entries[count].content = VM_APT_VERIFIER_INSTALL_PLIST;
        entries[count].content_size =
            sizeof VM_APT_VERIFIER_INSTALL_PLIST - 1u;
        entries[count].permissions = 0644u;
        count++;
    }
    if (include_cache) {
        entries[count].kind = ROOTFS_WORK_ENTRY_DIRECTORY;
        entries[count].path = VM_GUEST_ROOTFS_APT_COMPAT_DIRECTORY;
        entries[count].permissions = 0755u;
        entries[count].existing_policy =
            ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
        count++;
        if (create_ios3_source) {
            entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
            entries[count].path = VM_GUEST_ROOTFS_IOS3_PARTY_SOURCE_PATH;
            entries[count].content = VM_IOS3_PARTY_SOURCE;
            entries[count].content_size = sizeof VM_IOS3_PARTY_SOURCE - 1u;
            entries[count].permissions = 0644u;
            count++;
        }
        if (create_apt_compat) {
            entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
            entries[count].path = VM_GUEST_ROOTFS_APT_COMPAT_PATH;
            entries[count].content = VM_APT_COMPAT_CONFIGURATION;
            entries[count].content_size =
                sizeof VM_APT_COMPAT_CONFIGURATION - 1u;
            entries[count].permissions = 0644u;
            count++;
        }
        entries[count].kind = ROOTFS_WORK_ENTRY_DIRECTORY;
        entries[count].path = VM_CYDIA_CACHE_V5_STATE_DIRECTORY;
        entries[count].permissions = 0755u;
        entries[count].existing_policy =
            ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
        count++;
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path = VM_CYDIA_CACHE_V5_PACKAGE_PATH;
        entries[count].content = cache_package;
        entries[count].content_size = cache_package_size;
        entries[count].permissions = 0644u;
        count++;
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path = VM_CYDIA_CACHE_V5_RUN_PATH;
        entries[count].content = cache_run;
        entries[count].content_size = cache_run_size;
        entries[count].permissions = 0755u;
        count++;
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path =
            "/System/Library/LaunchDaemons/"
            "com.j0shua.s5lbox.cydia-cache-v5.plist";
        entries[count].content = VM_CYDIA_CACHE_V5_RUN_PLIST;
        entries[count].content_size =
            sizeof VM_CYDIA_CACHE_V5_RUN_PLIST - 1u;
        entries[count].permissions = 0644u;
        count++;
    }
    return count;
}

static size_t build_bigboss_source_entries(
    rootfs_work_entry_t *entries, size_t capacity, bool create_source) {
    return build_cydia_maintenance_entries(
        entries, capacity, true, create_source, false, false,
        false, NULL, 0u, false, NULL, 0u, NULL, 0u, false, false);
}

static size_t build_apt_trust_entries(
    rootfs_work_entry_t *entries, size_t capacity, bool create_keyring) {
    return build_cydia_maintenance_entries(
        entries, capacity, false, false, true, create_keyring,
        false, NULL, 0u, false, NULL, 0u, NULL, 0u, false, false);
}

static size_t build_apt_verifier_entries(
    rootfs_work_entry_t *entries, size_t capacity,
    const uint8_t *package, size_t package_size) {
    return build_cydia_maintenance_entries(
        entries, capacity, false, false, false, false,
        true, package, package_size, false, NULL, 0u, NULL, 0u,
        false, false);
}

static size_t build_cydia_cache_entries(
    rootfs_work_entry_t *entries, size_t capacity,
    const uint8_t *package, size_t package_size,
    bool create_ios3_source, bool create_apt_compat) {
    static uint8_t test_cache_run[VM_CYDIA_CACHE_V5_RUN_SIZE + 1u];
    size_t test_cache_run_size = 0u;
    if (!build_cydia_v5_run(test_cache_run, sizeof test_cache_run,
                            &test_cache_run_size))
        return 0u;
    return build_cydia_maintenance_entries(
        entries, capacity, false, false, false, false,
        false, NULL, 0u, true, package, package_size,
        test_cache_run, test_cache_run_size,
        create_ios3_source, create_apt_compat);
}

#if defined(S5LBOX_GUEST_INSTALL_TESTING)
void vm_guest_install_build_test_cydia_privilege_repair(
    rootfs_work_file_repair_t *repair, bool legacy) {
    if (!repair) return;
    build_cydia_privilege_repair(repair, legacy);
}

size_t vm_guest_install_build_test_bigboss_source_entries(
    rootfs_work_entry_t *entries, size_t capacity, bool create_source) {
    return build_bigboss_source_entries(entries, capacity, create_source);
}

size_t vm_guest_install_build_test_apt_trust_entries(
    rootfs_work_entry_t *entries, size_t capacity, bool create_keyring) {
    return build_apt_trust_entries(entries, capacity, create_keyring);
}

size_t vm_guest_install_build_test_apt_verifier_entries(
    rootfs_work_entry_t *entries, size_t capacity,
    const uint8_t *package, size_t package_size) {
    return build_apt_verifier_entries(
        entries, capacity, package, package_size);
}

size_t vm_guest_install_build_test_cydia_cache_entries(
    rootfs_work_entry_t *entries, size_t capacity,
    const uint8_t *package, size_t package_size,
    bool create_ios3_source, bool create_apt_compat) {
    return build_cydia_cache_entries(
        entries, capacity, package, package_size,
        create_ios3_source, create_apt_compat);
}

void vm_guest_install_build_test_cydia_v3_rewrite(
    rootfs_work_file_rewrite_t *rewrite) {
    if (!rewrite) return;
    build_cydia_v3_supersede_rewrite(rewrite);
}

void vm_guest_install_build_test_cydia_v4_rewrite(
    rootfs_work_file_rewrite_t *rewrite) {
    if (!rewrite) return;
    build_cydia_v4_supersede_rewrite(rewrite);
}
#endif

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

static bool build_regular_file_size(const char *path, uint64_t *out_size) {
    if (out_size) *out_size = 0u;
    if (!path || !*path || !out_size) return false;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0 || (st.st_mode & _S_IFREG) == 0 ||
        st.st_size <= 0)
        return false;
#else
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return false;
#endif
    *out_size = (uint64_t)st.st_size;
    return true;
}

static uint8_t *build_load_apt_verifier_package(
    const char *package_directory, size_t *out_size,
    char *detail, size_t detail_capacity) {
    if (out_size) *out_size = 0u;
    const vm_guest_package_t *package = vm_guest_package_find("gnupg");
    if (!package_directory || !*package_directory || !out_size || !package ||
        strcmp(package->version, "1.4.8-4") != 0 ||
        package->size == 0u || package->size > SIZE_MAX) {
        build_detail(detail, detail_capacity,
                     "The pinned APT signature-verifier package is unavailable.");
        return NULL;
    }
    char path[VM_GUEST_INSTALL_PATH_CAPACITY];
    if (!build_join(path, package_directory, package->filename)) {
        build_detail(detail, detail_capacity,
                     "The APT signature-verifier package path is too long.");
        return NULL;
    }
    size_t size = (size_t)package->size;
    uint8_t *bytes = (uint8_t *)malloc(size);
    if (!bytes) {
        build_detail(detail, detail_capacity,
                     "The APT signature-verifier package could not be retained in memory.");
        return NULL;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        free(bytes);
        build_detail(detail, detail_capacity,
                     "The downloaded APT signature-verifier package cannot be opened.");
        return NULL;
    }
    size_t used = 0u;
    bool read_ok = true;
    while (read_ok && used < size) {
        size_t amount = fread(bytes + used, 1u, size - used, file);
        if (amount == 0u) {
            read_ok = false;
            break;
        }
        used += amount;
    }
    int extra = read_ok ? fgetc(file) : EOF;
    if ((read_ok && extra != EOF) || ferror(file) || fclose(file) != 0)
        read_ok = false;
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE];
    if (!read_ok || used != size ||
        !ios3_sha256(bytes, size, digest) ||
        !vm_guest_package_download_matches(package, size, digest)) {
        free(bytes);
        build_detail(detail, detail_capacity,
                     "The APT signature-verifier package changed while it was being read.");
        return NULL;
    }
    *out_size = size;
    return bytes;
}

static uint8_t *build_load_cydia_cache_package(
    const char *package_directory, size_t *out_size,
    char *detail, size_t detail_capacity) {
    if (out_size) *out_size = 0u;
    const vm_guest_package_t *package = vm_guest_package_find("apt7");
    if (!package_directory || !*package_directory || !out_size || !package ||
        strcmp(package->version, "0.7.20.2-1") != 0 ||
        strcmp(package->filename,
               "apt7_0.7.20.2-1_iphoneos-arm.deb") != 0 ||
        package->roles != VM_GUEST_PACKAGE_APT_CACHE_TOOL ||
        package->size == 0u || package->size > SIZE_MAX) {
        build_detail(detail, detail_capacity,
                     "The pinned Cydia cache-tool package is unavailable.");
        return NULL;
    }
    char path[VM_GUEST_INSTALL_PATH_CAPACITY];
    if (!build_join(path, package_directory, package->filename)) {
        build_detail(detail, detail_capacity,
                     "The Cydia cache-tool package path is too long.");
        return NULL;
    }
    size_t size = (size_t)package->size;
    uint8_t *bytes = (uint8_t *)malloc(size);
    if (!bytes) {
        build_detail(detail, detail_capacity,
                     "The Cydia cache-tool package could not be retained in memory.");
        return NULL;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        free(bytes);
        build_detail(detail, detail_capacity,
                     "The downloaded Cydia cache-tool package cannot be opened.");
        return NULL;
    }
    size_t used = 0u;
    bool read_ok = true;
    while (read_ok && used < size) {
        size_t amount = fread(bytes + used, 1u, size - used, file);
        if (amount == 0u) {
            read_ok = false;
            break;
        }
        used += amount;
    }
    int extra = read_ok ? fgetc(file) : EOF;
    if ((read_ok && extra != EOF) || ferror(file) || fclose(file) != 0)
        read_ok = false;
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE];
    if (!read_ok || used != size ||
        !ios3_sha256(bytes, size, digest) ||
        !vm_guest_package_download_matches(package, size, digest)) {
        free(bytes);
        build_detail(detail, detail_capacity,
                     "The Cydia cache-tool package changed while it was being read.");
        return NULL;
    }
    *out_size = size;
    return bytes;
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

static bool build_transaction_matches_install(
    const vm_guest_install_result_t *transaction,
    const vm_guest_install_result_t *install) {
    return transaction && install &&
           (!transaction->committed ||
            (transaction->has_manifest && install->has_manifest &&
             memcmp(transaction->manifest_sha256, install->manifest_sha256,
                    VM_GUEST_INSTALL_SHA256_SIZE) == 0));
}

static vm_guest_install_build_status_t build_rootfs_refusal(
    rootfs_work_status_t status, const rootfs_work_result_t *rootfs,
    char *detail, size_t detail_capacity) {
    if (status == ROOTFS_WORK_HFS_INVALID && rootfs &&
        strstr(rootfs->detail, "not cleanly unmounted") != NULL) {
        build_detail(detail, detail_capacity,
                     "Guest-disk maintenance needs a clean guest shutdown. Reopen this machine, hold Power, slide to power off, wait until the guest halts, return to Machines, and try again. S5LBox will not guess-repair this unjournaled HFS disk.");
        return VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN;
    }
    build_detail(detail, detail_capacity,
                 rootfs && rootfs->detail[0]
                     ? rootfs->detail : rootfs_work_status_name(status));
    return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
}

static bool build_repair_proves_allocation_reconciliation(
    const rootfs_work_result_t *preflight,
    const rootfs_work_result_t *repair, uint64_t live_size) {
    if (!preflight || !repair || live_size == 0u ||
        preflight->status != ROOTFS_WORK_HFS_INVALID ||
        repair->status != ROOTFS_WORK_OK || repair->published ||
        preflight->source_size != live_size ||
        repair->source_size != live_size || repair->final_size != live_size ||
        !preflight->source_allocation_free_count_mismatch ||
        !repair->source_allocation_free_count_mismatch ||
        preflight->source_allocation_bitmap_used !=
            repair->source_allocation_bitmap_used ||
        preflight->source_allocation_header_used !=
            repair->source_allocation_header_used ||
        preflight->source_cleanly_unmounted !=
            repair->source_cleanly_unmounted)
        return false;

    /* The scanner has already performed a raw strict re-audit before it can
     * return OK. Keep the app-level publication gate independent: every
     * planned mutation must also be reported as applied, and the original
     * bitmap/header disagreement must have caused either a bitmap change or a
     * redundant freeBlocks change. A topology-only identity pass cannot make
     * an allocation mismatch safe to publish. */
    if (repair->catalog_backlinks_repairable !=
            repair->catalog_backlinks_repaired ||
        repair->catalog_topology_nodes_repairable !=
            repair->catalog_topology_nodes_repaired ||
        repair->catalog_extent_records_repairable !=
            repair->catalog_extent_records_repaired ||
        repair->allocation_bits_repairable !=
            repair->allocation_bits_repaired ||
        repair->allocation_free_count_repairable !=
            repair->allocation_free_count_repaired)
        return false;
    return repair->allocation_bits_repaired != 0u ||
           repair->allocation_free_count_repaired != 0u;
}

#if defined(S5LBOX_GUEST_INSTALL_TESTING)
bool vm_guest_install_build_test_allocation_repair_proven(
    const rootfs_work_result_t *preflight,
    const rootfs_work_result_t *repair, uint64_t live_size) {
    return build_repair_proves_allocation_reconciliation(
        preflight, repair, live_size);
}
#endif

/* The strict read-only probe admits this path only for a stale redundant
 * freeBlocks count. The powered-off scanner then proves the complete disk, so
 * it may also canonicalize derivable catalog topology or reconcile other
 * crash-consistent allocation state. Ambiguity is refused by that scanner;
 * nothing is published unless its raw strict re-audit succeeds. */
static vm_guest_install_build_status_t build_repair_allocation_accounting(
    const char *work_directory, uint64_t live_size,
    const rootfs_work_result_t *accounting_probe,
    vm_guest_install_build_progress_t progress, void *progress_context,
    vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity) {
    char stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    char transaction_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    if (!vm_guest_recovery_stage_image_path(
            stage, sizeof stage, work_directory)) {
        build_detail(detail, detail_capacity,
                     "The filesystem-accounting recovery path is too long.");
        return VM_GUEST_INSTALL_BUILD_ERR_PATH;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 0u, 1u);
    vm_guest_install_result_t transaction;
    vm_guest_install_status_t transaction_status =
        vm_guest_recovery_prepare_stage(
            work_directory, &transaction, transaction_detail,
            sizeof transaction_detail);
    if (transaction_status != VM_GUEST_INSTALL_OK || transaction.committed) {
        build_detail(detail, detail_capacity,
                     transaction_status == VM_GUEST_INSTALL_OK
                         ? "Filesystem recovery unexpectedly became committed before its clone was prepared."
                         : (transaction_detail[0]
                                ? transaction_detail
                                : vm_guest_install_status_text(
                                      transaction_status)));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    transaction_status = vm_guest_recovery_clone_live_to_stage(
        work_directory, transaction_detail, sizeof transaction_detail);
    if (transaction_status != VM_GUEST_INSTALL_OK) {
        char discard_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
        vm_guest_install_status_t discarded =
            vm_guest_recovery_discard_stage(
                work_directory, discard_detail, sizeof discard_detail);
        if (discarded != VM_GUEST_INSTALL_OK) {
            (void)snprintf(
                detail, detail_capacity,
                "The filesystem recovery clone failed and its inert stage could not be removed: %.120s",
                discard_detail[0] ? discard_detail
                                  : vm_guest_install_status_text(discarded));
        } else {
            build_detail(detail, detail_capacity,
                         transaction_detail[0]
                             ? transaction_detail
                             : vm_guest_install_status_text(
                                   transaction_status));
        }
        if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    rootfs_work_result_t repair;
    rootfs_work_status_t repair_status =
        rootfs_work_repair_powered_off_clone(stage, &repair);
    if (result) result->filesystem_recovery = repair;
    bool geometry_ok = repair.source_size == live_size &&
                       repair.final_size == live_size;
    bool proven_repair = repair_status == ROOTFS_WORK_OK &&
        build_repair_proves_allocation_reconciliation(
            accounting_probe, &repair, live_size);
    if (!proven_repair) {
        char discard_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
        vm_guest_install_status_t discarded =
            vm_guest_recovery_discard_stage(
                work_directory, discard_detail, sizeof discard_detail);
        if (discarded != VM_GUEST_INSTALL_OK) {
            (void)snprintf(
                detail, detail_capacity,
                "The unpublished filesystem repair was refused and its stage could not be removed: %.112s",
                discard_detail[0] ? discard_detail
                                  : vm_guest_install_status_text(discarded));
            if (detail && detail_capacity)
                detail[detail_capacity - 1u] = '\0';
            return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
        }
        if (repair_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(
                repair_status, &repair, detail, detail_capacity);
        if (geometry_ok) {
            (void)snprintf(
                detail, detail_capacity,
                "Filesystem repair evidence mismatch: source bitmap/header "
                "%u/%u blocks; topology %u/%u, fork records %u/%u, bitmap "
                "bits %u/%u, and free count %u/%u. The clone was not "
                "published.",
                repair.source_allocation_bitmap_used,
                repair.source_allocation_header_used,
                repair.catalog_topology_nodes_repairable,
                repair.catalog_topology_nodes_repaired,
                repair.catalog_extent_records_repairable,
                repair.catalog_extent_records_repaired,
                repair.allocation_bits_repairable,
                repair.allocation_bits_repaired,
                repair.allocation_free_count_repairable,
                repair.allocation_free_count_repaired);
            if (detail && detail_capacity)
                detail[detail_capacity - 1u] = '\0';
        } else {
            build_detail(
                detail, detail_capacity,
                "The repaired clone changed guest-disk geometry; it was not published.");
        }
        return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 0u, 1u);
    transaction_status = vm_guest_recovery_publish(
        work_directory, &transaction, transaction_detail,
        sizeof transaction_detail);
    if (result) result->filesystem_recovery_transaction = transaction;
    if (transaction_status != VM_GUEST_INSTALL_OK || !transaction.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(transaction_status));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    if (!transaction.cleanup_complete) {
        transaction_status = vm_guest_recovery_recover(
            work_directory, &transaction, transaction_detail,
            sizeof transaction_detail);
        if (result) result->filesystem_recovery_transaction = transaction;
        if (transaction_status != VM_GUEST_INSTALL_OK ||
            !transaction.committed || !transaction.cleanup_complete) {
            build_detail(detail, detail_capacity,
                         transaction_detail[0]
                             ? transaction_detail
                             : vm_guest_install_status_text(
                                   transaction_status));
            return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
        }
    }
    if (result) result->filesystem_repaired = true;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 1u, 1u);
    return VM_GUEST_INSTALL_BUILD_OK;
}

static bool build_rootfs_is_unclean(rootfs_work_status_t status,
                                    const rootfs_work_result_t *rootfs) {
    return status == ROOTFS_WORK_HFS_INVALID && rootfs &&
           strstr(rootfs->detail, "not cleanly unmounted") != NULL;
}

static bool build_authorize_unclean_source(
    const char *work_directory, uint64_t live_size,
    bool *allow_unclean_source,
    vm_guest_install_build_result_t *result) {
    if (!allow_unclean_source) return false;
    if (*allow_unclean_source) return true;
    char checkpoint_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_resume_checkpoint_state_t checkpoint =
        vm_resume_checkpoint_probe_state(
            work_directory, live_size, BUILD_CHECKPOINT_RAM_BASE,
            BUILD_CHECKPOINT_RAM_SIZE, checkpoint_detail,
            sizeof checkpoint_detail);
    if (checkpoint != VM_RESUME_CHECKPOINT_POWERED_OFF) return false;
    *allow_unclean_source = true;
    if (result) result->powered_off_checkpoint_witnessed = true;
    return true;
}

static rootfs_work_status_t build_probe_file_repair(
    const char *work_directory, const char *live, uint64_t live_size,
    const rootfs_work_file_repair_t *repair,
    rootfs_work_file_repair_state_t *state,
    bool *allow_unclean_source,
    vm_guest_install_build_result_t *result,
    rootfs_work_result_t *probe) {
    bool authorized = allow_unclean_source && *allow_unclean_source;
    rootfs_work_status_t status = rootfs_work_probe_file_repair_policy(
        live, repair, authorized, authorized, state, probe);
    if (!build_rootfs_is_unclean(status, probe) ||
        !build_authorize_unclean_source(
            work_directory, live_size, allow_unclean_source, result))
        return status;
    return rootfs_work_probe_file_repair_policy(
        live, repair, true, true, state, probe);
}

static rootfs_work_status_t build_probe_file_rewrite(
    const char *work_directory, const char *live, uint64_t live_size,
    const rootfs_work_file_rewrite_t *rewrite,
    rootfs_work_file_rewrite_state_t *state,
    bool *allow_unclean_source,
    vm_guest_install_build_result_t *result,
    rootfs_work_result_t *probe) {
    bool authorized = allow_unclean_source && *allow_unclean_source;
    rootfs_work_status_t status = rootfs_work_probe_file_rewrite_policy(
        live, rewrite, authorized, authorized, state, probe);
    if (!build_rootfs_is_unclean(status, probe) ||
        !build_authorize_unclean_source(
            work_directory, live_size, allow_unclean_source, result))
        return status;
    return rootfs_work_probe_file_rewrite_policy(
        live, rewrite, true, true, state, probe);
}

static rootfs_work_status_t build_validate_source(
    const char *work_directory, const char *live, uint64_t live_size,
    bool *allow_unclean_source,
    vm_guest_install_build_result_t *result,
    rootfs_work_result_t *preflight) {
    rootfs_work_status_t status = rootfs_work_validate_source_ex(
        live, allow_unclean_source && *allow_unclean_source, preflight);
    if (!build_rootfs_is_unclean(status, preflight) ||
        !build_authorize_unclean_source(
            work_directory, live_size, allow_unclean_source, result))
        return status;
    return rootfs_work_validate_source_ex(live, true, preflight);
}

static vm_guest_install_build_status_t build_maintain_install(
    const char *work_directory, const char *package_directory,
    const vm_guest_install_result_t *install,
    const vm_guest_install_result_t *storage,
    const vm_guest_install_result_t *privilege,
    const vm_guest_install_result_t *sources,
    const vm_guest_install_result_t *sources_v2,
    const vm_guest_install_result_t *apt_trust,
    const vm_guest_install_result_t *apt_verifier,
    const vm_guest_install_result_t *cydia_cache,
    vm_guest_install_build_progress_t progress, void *progress_context,
    vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity) {
    if (!install || !install->committed || !install->has_manifest ||
        !storage || !privilege || !sources || !sources_v2 || !apt_trust ||
        !apt_verifier || !cydia_cache ||
        !build_transaction_matches_install(storage, install) ||
        !build_transaction_matches_install(privilege, install) ||
        !build_transaction_matches_install(sources, install) ||
        !build_transaction_matches_install(sources_v2, install) ||
        !build_transaction_matches_install(apt_trust, install) ||
        !build_transaction_matches_install(apt_verifier, install) ||
        !build_transaction_matches_install(cydia_cache, install)) {
        build_detail(detail, detail_capacity,
                     "A guest-disk maintenance record does not match the committed installation.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (!install->cleanup_complete ||
        (storage->committed && !storage->cleanup_complete) ||
        (privilege->committed && !privilege->cleanup_complete) ||
        (sources->committed && !sources->cleanup_complete) ||
        (sources_v2->committed && !sources_v2->cleanup_complete) ||
        (apt_trust->committed && !apt_trust->cleanup_complete) ||
        (apt_verifier->committed && !apt_verifier->cleanup_complete) ||
        (cydia_cache->committed && !cydia_cache->cleanup_complete)) {
        build_detail(detail, detail_capacity,
                     "A committed guest-disk transaction still has cleanup residue; no new maintenance transaction was started.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    uint64_t live_size = 0u;
    if (!build_join(live, work_directory, VM_GUEST_INSTALL_LIVE_FILE) ||
        !build_regular_file_size(live, &live_size)) {
        build_detail(detail, detail_capacity,
                     "The committed installation has no valid live guest disk.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    bool grow_storage = live_size < VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES;
    if (grow_storage && storage->committed) {
        build_detail(detail, detail_capacity,
                     "The storage-upgrade record is committed, but the live guest disk is still smaller than 2 GiB.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    /* A clean HFS unmount can still leave its redundant freeBlocks value or
     * allocation bitmap one transaction behind the catalog. Detect only the
     * exact header/bitmap disagreement before any file-specific probe; the
     * complete powered-off scanner then determines which fully reconstructed
     * state is stale. Repair remains confined to the same unpublished-clone
     * transaction used by powered-off boot recovery. A dirty source receives
     * this privilege only when its exact powered-off checkpoint independently
     * authorizes the existing narrow exception. */
    bool allow_unclean_source = false;
    bool source_validated = false;
    bool snapshot_gate_passed = false;
    rootfs_work_result_t accounting_probe;
    rootfs_work_status_t accounting_status = build_validate_source(
        work_directory, live, live_size, &allow_unclean_source,
        result, &accounting_probe);
    if (result) result->rootfs = accounting_probe;
    if (accounting_status == ROOTFS_WORK_OK) {
        source_validated = true;
    } else if (accounting_status == ROOTFS_WORK_HFS_INVALID &&
               accounting_probe.source_allocation_free_count_mismatch &&
               (accounting_probe.source_cleanly_unmounted ||
                allow_unclean_source)) {
        vm_guest_install_build_status_t snapshot_gate = build_snapshot_gate(
            work_directory, result, detail, detail_capacity);
        if (snapshot_gate != VM_GUEST_INSTALL_BUILD_OK) return snapshot_gate;
        snapshot_gate_passed = true;
        vm_guest_install_build_status_t repaired =
            build_repair_allocation_accounting(
                work_directory, live_size, &accounting_probe,
                progress, progress_context, result, detail,
                detail_capacity);
        if (repaired != VM_GUEST_INSTALL_BUILD_OK) return repaired;
        accounting_status = build_validate_source(
            work_directory, live, live_size, &allow_unclean_source,
            result, &accounting_probe);
        if (result) result->rootfs = accounting_probe;
        if (accounting_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(
                accounting_status, &accounting_probe,
                detail, detail_capacity);
        source_validated = true;
    }

    rootfs_work_file_repair_t repair;
    build_cydia_privilege_repair(&repair, false);
    rootfs_work_file_repair_state_t repair_state =
        ROOTFS_WORK_FILE_REPAIR_MISSING;
    bool repair_needed = false;
    bool source_needed = false;
    bool trust_needed = false;
    bool verifier_needed = false;
    bool cache_needed = false;
    bool cache_ios3_source_create = false;
    bool cache_apt_compat_create = false;
    bool cache_v3_rewrite_needed = false;
    bool cache_v4_rewrite_needed = false;
    bool trust_create = false;
    rootfs_work_file_rewrite_t cache_v3_rewrite;
    rootfs_work_file_rewrite_t cache_v4_rewrite;
    build_cydia_v3_supersede_rewrite(&cache_v3_rewrite);
    build_cydia_v4_supersede_rewrite(&cache_v4_rewrite);
    bool source_preflighted = false;
    char transaction_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];

    if (privilege->committed) {
        if (result) result->cydia_privileges_verified = true;
    } else {
        rootfs_work_result_t probe;
        rootfs_work_status_t probe_status = build_probe_file_repair(
            work_directory, live, live_size, &repair, &repair_state,
            &allow_unclean_source, result, &probe);
        if (probe_status == ROOTFS_WORK_FILE_REPAIR_MISMATCH) {
            build_cydia_privilege_repair(&repair, true);
            repair_state = ROOTFS_WORK_FILE_REPAIR_MISSING;
            probe_status = build_probe_file_repair(
                work_directory, live, live_size, &repair, &repair_state,
                &allow_unclean_source, result, &probe);
        }
        if (result) result->rootfs = probe;
        if (probe_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(probe_status, &probe,
                                        detail, detail_capacity);
        source_preflighted = true;
        repair_needed = repair_state == ROOTFS_WORK_FILE_REPAIR_NEEDED;
        if (repair_state == ROOTFS_WORK_FILE_REPAIR_SATISFIED) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_privilege_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->privilege_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK || !confirmed.committed) {
                build_detail(detail, detail_capacity,
                             transaction_detail[0]
                                 ? transaction_detail
                                 : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
            }
            if (result) result->cydia_privileges_verified = true;
        }
    }

    rootfs_work_file_repair_t source_probe;
    bool source_create = false;
    if (sources_v2->committed) {
        if (result) result->cydia_sources_verified = true;
    } else {
        if (!build_bigboss_source_probe(&source_probe)) {
            build_detail(detail, detail_capacity,
                         "The BigBoss source identity could not be computed.");
            return VM_GUEST_INSTALL_BUILD_ERR_MANIFEST;
        }
        rootfs_work_file_repair_state_t source_state =
            ROOTFS_WORK_FILE_REPAIR_MISSING;
        rootfs_work_result_t probe;
        rootfs_work_status_t probe_status = build_probe_file_repair(
            work_directory, live, live_size, &source_probe, &source_state,
            &allow_unclean_source, result, &probe);
        if (probe_status == ROOTFS_WORK_FILE_REPAIR_MISMATCH &&
            build_managed_file_probe_for(
                &source_probe, VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH,
                VM_BIGBOSS_SOURCE_COMPAT,
                sizeof VM_BIGBOSS_SOURCE_COMPAT - 1u)) {
            source_state = ROOTFS_WORK_FILE_REPAIR_MISSING;
            probe_status = build_probe_file_repair(
                work_directory, live, live_size, &source_probe, &source_state,
                &allow_unclean_source, result, &probe);
        }
        if (result) result->rootfs = probe;
        if (probe_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(probe_status, &probe,
                                        detail, detail_capacity);
        source_preflighted = true;
        source_create = source_state == ROOTFS_WORK_FILE_REPAIR_MISSING;
        if (!source_create &&
            source_state != ROOTFS_WORK_FILE_REPAIR_SATISFIED) {
            build_detail(detail, detail_capacity,
                         "The managed BigBoss source has unexpected metadata or contents.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        /* An already-correct source can coexist with an HTML list cached
         * after a compressed-index transfer failed. The v2 disk transaction
         * installs a one-shot cache purge; the source URL itself is unchanged
         * because changing its spelling does not change APT's fallback URLs. */
        source_needed = true;
    }

    if (apt_trust->committed) {
        if (result) result->apt_trust_verified = true;
    } else {
        rootfs_work_file_repair_t trust_probe;
        if (!build_bigboss_keyring_probe(&trust_probe)) {
            build_detail(detail, detail_capacity,
                         "The BigBoss public-key identity could not be computed.");
            return VM_GUEST_INSTALL_BUILD_ERR_MANIFEST;
        }
        rootfs_work_file_repair_state_t trust_state =
            ROOTFS_WORK_FILE_REPAIR_MISSING;
        rootfs_work_result_t probe;
        rootfs_work_status_t probe_status = build_probe_file_repair(
            work_directory, live, live_size, &trust_probe, &trust_state,
            &allow_unclean_source, result, &probe);
        if (probe_status == ROOTFS_WORK_FILE_REPAIR_MISMATCH) {
            if (!build_historical_keyring_probe(&trust_probe)) {
                build_detail(detail, detail_capacity,
                             "The historical APT keyring identity could not be prepared.");
                return VM_GUEST_INSTALL_BUILD_ERR_MANIFEST;
            }
            trust_state = ROOTFS_WORK_FILE_REPAIR_MISSING;
            probe_status = build_probe_file_repair(
                work_directory, live, live_size, &trust_probe, &trust_state,
                &allow_unclean_source, result, &probe);
        }
        if (result) result->rootfs = probe;
        if (probe_status == ROOTFS_WORK_FILE_REPAIR_MISMATCH) {
            build_detail(detail, detail_capacity,
                         "The existing APT trusted.gpg is not an accepted historical keyring and was not overwritten.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        if (probe_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(probe_status, &probe,
                                        detail, detail_capacity);
        if (trust_state != ROOTFS_WORK_FILE_REPAIR_MISSING &&
            trust_state != ROOTFS_WORK_FILE_REPAIR_SATISFIED) {
            build_detail(detail, detail_capacity,
                         "The APT trusted.gpg has unsupported metadata and was not changed.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        source_preflighted = true;
        trust_create = trust_state == ROOTFS_WORK_FILE_REPAIR_MISSING;
        /* Even an already-correct keyring gets this versioned transaction:
         * its one-shot guest helper removes only the failed BigBoss cache. */
        trust_needed = true;
    }

    if (apt_verifier->committed) {
        /* The durable host marker means the exact package and retryable guest
         * job are already on disk. Do not rewrite or require a second HFS
         * scan before that job has had a chance to run. */
        if (result) result->apt_verifier_staged = true;
    } else {
        rootfs_work_file_repair_t verifier_probe;
        build_apt_verifier_probe(&verifier_probe);
        rootfs_work_file_repair_state_t verifier_state =
            ROOTFS_WORK_FILE_REPAIR_MISSING;
        rootfs_work_result_t verifier_result;
        rootfs_work_status_t verifier_status = build_probe_file_repair(
            work_directory, live, live_size, &verifier_probe, &verifier_state,
            &allow_unclean_source, result, &verifier_result);
        if (result) result->rootfs = verifier_result;
        if (verifier_status == ROOTFS_WORK_FILE_REPAIR_MISMATCH) {
            build_detail(detail, detail_capacity,
                         "The existing /usr/bin/gpgv is not the pinned iPhone OS 3 verifier and was not overwritten.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        if (verifier_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(verifier_status, &verifier_result,
                                        detail, detail_capacity);
        source_preflighted = true;
        if (verifier_state == ROOTFS_WORK_FILE_REPAIR_SATISFIED) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_apt_verifier_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->apt_verifier_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK ||
                !confirmed.committed) {
                build_detail(
                    detail, detail_capacity,
                    transaction_detail[0]
                        ? transaction_detail
                        : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
            }
            if (result) result->apt_verifier_verified = true;
        } else if (verifier_state == ROOTFS_WORK_FILE_REPAIR_MISSING ||
                   verifier_state == ROOTFS_WORK_FILE_REPAIR_NEEDED) {
            verifier_needed = true;
        } else {
            build_detail(detail, detail_capacity,
                         "The APT signature verifier has unsupported metadata and was not changed.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
    }

    if (cydia_cache->committed) {
        if (result) result->cydia_cache_staged = true;
    } else {
        rootfs_work_file_repair_t ios3_source_probe;
        rootfs_work_file_repair_state_t ios3_source_state =
            ROOTFS_WORK_FILE_REPAIR_MISSING;
        rootfs_work_result_t ios3_source_result;
        if (!build_ios3_party_source_probe(&ios3_source_probe)) {
            build_detail(detail, detail_capacity,
                         "The iOS 3 repository source identity could not be computed.");
            return VM_GUEST_INSTALL_BUILD_ERR_MANIFEST;
        }
        rootfs_work_status_t ios3_source_status = build_probe_file_repair(
            work_directory, live, live_size, &ios3_source_probe,
            &ios3_source_state, &allow_unclean_source, result,
            &ios3_source_result);
        if (result) result->rootfs = ios3_source_result;
        if (ios3_source_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(ios3_source_status,
                                        &ios3_source_result,
                                        detail, detail_capacity);
        if (ios3_source_state != ROOTFS_WORK_FILE_REPAIR_MISSING &&
            ios3_source_state != ROOTFS_WORK_FILE_REPAIR_SATISFIED) {
            build_detail(detail, detail_capacity,
                         "The managed iOS 3 repository source has unexpected metadata or contents.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        cache_ios3_source_create =
            ios3_source_state == ROOTFS_WORK_FILE_REPAIR_MISSING;

        rootfs_work_file_repair_t apt_compat_probe;
        rootfs_work_file_repair_state_t apt_compat_state =
            ROOTFS_WORK_FILE_REPAIR_MISSING;
        rootfs_work_result_t apt_compat_result;
        if (!build_apt_compat_probe(&apt_compat_probe)) {
            build_detail(detail, detail_capacity,
                         "The legacy APT compatibility identity could not be computed.");
            return VM_GUEST_INSTALL_BUILD_ERR_MANIFEST;
        }
        rootfs_work_status_t apt_compat_status = build_probe_file_repair(
            work_directory, live, live_size, &apt_compat_probe,
            &apt_compat_state, &allow_unclean_source, result,
            &apt_compat_result);
        if (result) result->rootfs = apt_compat_result;
        if (apt_compat_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(apt_compat_status,
                                        &apt_compat_result,
                                        detail, detail_capacity);
        if (apt_compat_state != ROOTFS_WORK_FILE_REPAIR_MISSING &&
            apt_compat_state != ROOTFS_WORK_FILE_REPAIR_SATISFIED) {
            build_detail(detail, detail_capacity,
                         "The managed legacy APT compatibility file has unexpected metadata or contents.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        cache_apt_compat_create =
            apt_compat_state == ROOTFS_WORK_FILE_REPAIR_MISSING;

        rootfs_work_file_rewrite_state_t cache_v3_rewrite_state =
            ROOTFS_WORK_FILE_REWRITE_MISSING;
        rootfs_work_result_t cache_v3_rewrite_result;
        rootfs_work_status_t cache_v3_rewrite_status =
            build_probe_file_rewrite(
                work_directory, live, live_size, &cache_v3_rewrite,
                &cache_v3_rewrite_state, &allow_unclean_source,
                result, &cache_v3_rewrite_result);
        if (result) result->rootfs = cache_v3_rewrite_result;
        if (cache_v3_rewrite_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(cache_v3_rewrite_status,
                                        &cache_v3_rewrite_result,
                                        detail, detail_capacity);
        cache_v3_rewrite_needed =
            cache_v3_rewrite_state == ROOTFS_WORK_FILE_REWRITE_NEEDED;
        if (!cache_v3_rewrite_needed &&
            cache_v3_rewrite_state != ROOTFS_WORK_FILE_REWRITE_MISSING &&
            cache_v3_rewrite_state != ROOTFS_WORK_FILE_REWRITE_SATISFIED) {
            build_detail(detail, detail_capacity,
                         "The legacy Cydia cache worker has unsupported contents or metadata.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }

        rootfs_work_file_rewrite_state_t cache_v4_rewrite_state =
            ROOTFS_WORK_FILE_REWRITE_MISSING;
        rootfs_work_result_t cache_v4_rewrite_result;
        rootfs_work_status_t cache_v4_rewrite_status =
            build_probe_file_rewrite(
                work_directory, live, live_size, &cache_v4_rewrite,
                &cache_v4_rewrite_state, &allow_unclean_source,
                result, &cache_v4_rewrite_result);
        if (result) result->rootfs = cache_v4_rewrite_result;
        if (cache_v4_rewrite_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(cache_v4_rewrite_status,
                                        &cache_v4_rewrite_result,
                                        detail, detail_capacity);
        cache_v4_rewrite_needed =
            cache_v4_rewrite_state == ROOTFS_WORK_FILE_REWRITE_NEEDED;
        if (!cache_v4_rewrite_needed &&
            cache_v4_rewrite_state != ROOTFS_WORK_FILE_REWRITE_MISSING &&
            cache_v4_rewrite_state != ROOTFS_WORK_FILE_REWRITE_SATISFIED) {
            build_detail(detail, detail_capacity,
                         "The bounded v4 Cydia cache worker has unsupported contents or metadata.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        source_preflighted = true;
        cache_needed = true;
    }

    if (!grow_storage && !repair_needed && !source_needed && !trust_needed &&
        !verifier_needed && !cache_needed) {
        build_progress(progress, progress_context,
                       VM_GUEST_INSTALL_BUILD_COMPLETE, 1u, 1u);
        return VM_GUEST_INSTALL_BUILD_OK;
    }

    uint8_t *verifier_package = NULL;
    size_t verifier_package_size = 0u;
    uint8_t *cache_package = NULL;
    size_t cache_package_size = 0u;
    if (verifier_needed) {
        if (!package_directory || !*package_directory) {
            build_detail(detail, detail_capacity,
                         "The exact legacy signature-verifier package must be downloaded before this guest can be repaired.");
            return VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT;
        }
        verifier_package = build_load_apt_verifier_package(
            package_directory, &verifier_package_size,
            detail, detail_capacity);
        if (!verifier_package)
            return VM_GUEST_INSTALL_BUILD_ERR_PACKAGES;
    }
    if (cache_needed) {
        if (!package_directory || !*package_directory) {
            free(verifier_package);
            build_detail(detail, detail_capacity,
                         "The exact legacy Cydia cache-tool package must be downloaded before this guest can be repaired.");
            return VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT;
        }
        cache_package = build_load_cydia_cache_package(
            package_directory, &cache_package_size,
            detail, detail_capacity);
        if (!cache_package) {
            free(verifier_package);
            return VM_GUEST_INSTALL_BUILD_ERR_PACKAGES;
        }
    }

    if (!snapshot_gate_passed) {
        vm_guest_install_build_status_t snapshot_gate = build_snapshot_gate(
            work_directory, result, detail, detail_capacity);
        if (snapshot_gate != VM_GUEST_INSTALL_BUILD_OK) {
            free(verifier_package);
            free(cache_package);
            return snapshot_gate;
        }
    }
    if (!source_preflighted && !source_validated) {
        rootfs_work_result_t preflight;
        rootfs_work_status_t preflight_status = build_validate_source(
            work_directory, live, live_size, &allow_unclean_source,
            result, &preflight);
        if (result) result->rootfs = preflight;
        if (preflight_status != ROOTFS_WORK_OK) {
            free(verifier_package);
            free(cache_package);
            return build_rootfs_refusal(preflight_status, &preflight,
                                        detail, detail_capacity);
        }
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 0u, 1u);
    vm_guest_install_result_t prepared;
    vm_guest_install_status_t preparation;
    if (grow_storage) {
        preparation = vm_guest_storage_prepare_stage(
            work_directory, &prepared, transaction_detail,
            sizeof transaction_detail);
    } else if (repair_needed) {
        preparation = vm_guest_privilege_prepare_stage(
            work_directory, &prepared, transaction_detail,
            sizeof transaction_detail);
    } else if (source_needed) {
        preparation = vm_guest_sources_v2_prepare_stage(
            work_directory, &prepared, transaction_detail,
            sizeof transaction_detail);
    } else if (trust_needed) {
        preparation = vm_guest_apt_trust_prepare_stage(
            work_directory, &prepared, transaction_detail,
            sizeof transaction_detail);
    } else if (verifier_needed) {
        preparation = vm_guest_apt_verifier_prepare_stage(
            work_directory, &prepared, transaction_detail,
            sizeof transaction_detail);
    } else {
        preparation = vm_guest_cydia_cache_prepare_stage(
            work_directory, &prepared, transaction_detail,
            sizeof transaction_detail);
    }
    if (preparation != VM_GUEST_INSTALL_OK || prepared.committed) {
        free(verifier_package);
        free(cache_package);
        build_detail(detail, detail_capacity,
                     preparation == VM_GUEST_INSTALL_OK
                         ? "Guest-disk maintenance became committed while its disk was being prepared."
                         : (transaction_detail[0]
                                ? transaction_detail
                                : vm_guest_install_status_text(preparation)));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    char stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    bool stage_ok;
    if (grow_storage) {
        stage_ok = vm_guest_storage_stage_image_path(
            stage, sizeof stage, work_directory);
    } else if (repair_needed) {
        stage_ok = vm_guest_privilege_stage_image_path(
            stage, sizeof stage, work_directory);
    } else if (source_needed) {
        stage_ok = vm_guest_sources_v2_stage_image_path(
            stage, sizeof stage, work_directory);
    } else if (trust_needed) {
        stage_ok = vm_guest_apt_trust_stage_image_path(
            stage, sizeof stage, work_directory);
    } else if (verifier_needed) {
        stage_ok = vm_guest_apt_verifier_stage_image_path(
            stage, sizeof stage, work_directory);
    } else {
        stage_ok = vm_guest_cydia_cache_stage_image_path(
            stage, sizeof stage, work_directory);
    }
    if (!stage_ok) {
        free(verifier_package);
        free(cache_package);
        build_detail(detail, detail_capacity,
                     "The guest-disk maintenance stage path is too long.");
        return VM_GUEST_INSTALL_BUILD_ERR_PATH;
    }
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 1u, 1u);

    rootfs_work_options_t options;
    memset(&options, 0, sizeof options);
    options.preserve_fstab = true;
    options.allow_unclean_source = allow_unclean_source;
    options.repair_catalog_backlinks =
        allow_unclean_source &&
        (repair_needed || source_needed || trust_needed || verifier_needed ||
         cache_needed);
    if (grow_storage)
        options.minimum_volume_bytes = VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES;
    if (repair_needed) {
        options.file_repairs = &repair;
        options.file_repair_count = 1u;
    }
    rootfs_work_file_rewrite_t cache_rewrites[2];
    size_t cache_rewrite_count = 0u;
    if (cache_v3_rewrite_needed)
        cache_rewrites[cache_rewrite_count++] = cache_v3_rewrite;
    if (cache_v4_rewrite_needed)
        cache_rewrites[cache_rewrite_count++] = cache_v4_rewrite;
    if (cache_rewrite_count != 0u) {
        options.file_rewrites = cache_rewrites;
        options.file_rewrite_count = cache_rewrite_count;
    }
    rootfs_work_entry_t
        maintenance_entries[VM_CYDIA_MAINTENANCE_MAX_ENTRIES];
    uint8_t cache_run[VM_CYDIA_CACHE_V5_RUN_SIZE + 1u];
    size_t cache_run_size = 0u;
    if (cache_needed &&
        !build_cydia_v5_run(cache_run, sizeof cache_run, &cache_run_size)) {
        free(verifier_package);
        free(cache_package);
        build_detail(detail, detail_capacity,
                     "The Cydia cache recovery helper could not be assembled.");
        return VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT;
    }
    size_t maintenance_entry_count = 0u;
    if (source_needed || trust_needed || verifier_needed || cache_needed) {
        maintenance_entry_count = build_cydia_maintenance_entries(
            maintenance_entries, VM_CYDIA_MAINTENANCE_MAX_ENTRIES,
            source_needed, source_create, trust_needed, trust_create,
            verifier_needed, verifier_package, verifier_package_size,
            cache_needed, cache_package, cache_package_size,
            cache_run, cache_run_size,
            cache_ios3_source_create, cache_apt_compat_create);
        options.entries = maintenance_entries;
        options.entry_count = maintenance_entry_count;
    }
    build_progress_adapter_t adapter = {progress, progress_context};
    options.progress = build_rootfs_progress;
    options.progress_ctx = &adapter;
    rootfs_work_result_t rootfs;
    rootfs_work_status_t rootfs_status = rootfs_work_create(
        live, stage, &options, &rootfs);
    free(verifier_package);
    verifier_package = NULL;
    free(cache_package);
    cache_package = NULL;
    if (result) result->rootfs = rootfs;
    if (rootfs_status != ROOTFS_WORK_OK || !rootfs.published ||
        (grow_storage &&
         rootfs.final_size < VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES) ||
        (repair_needed && rootfs.file_repairs_applied != 1u) ||
        (cache_rewrite_count != 0u &&
         rootfs.file_rewrites_applied != cache_rewrite_count) ||
        ((source_needed || trust_needed || verifier_needed || cache_needed) &&
         (rootfs.provision_entries <
              2u * ((source_needed ? 1u : 0u) +
                    (trust_needed ? 1u : 0u)) +
                  (verifier_needed ? 3u : 0u) +
                  (cache_needed ? 3u : 0u) ||
          rootfs.provision_entries + rootfs.provision_reused_entries !=
              maintenance_entry_count))) {
        if (rootfs_status == ROOTFS_WORK_OK) {
            build_detail(detail, detail_capacity,
                         "The completed guest-disk clone did not contain the requested maintenance result.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        return build_rootfs_refusal(rootfs_status, &rootfs,
                                    detail, detail_capacity);
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 0u, 1u);
    vm_guest_install_result_t published;
    vm_guest_install_status_t publication;
    if (grow_storage) {
        publication = vm_guest_storage_publish(
            work_directory, install->manifest_sha256, &published,
            transaction_detail, sizeof transaction_detail);
    } else if (repair_needed) {
        publication = vm_guest_privilege_publish(
            work_directory, install->manifest_sha256, &published,
            transaction_detail, sizeof transaction_detail);
    } else if (source_needed) {
        publication = vm_guest_sources_v2_publish(
            work_directory, install->manifest_sha256, &published,
            transaction_detail, sizeof transaction_detail);
    } else if (trust_needed) {
        publication = vm_guest_apt_trust_publish(
            work_directory, install->manifest_sha256, &published,
            transaction_detail, sizeof transaction_detail);
    } else if (verifier_needed) {
        publication = vm_guest_apt_verifier_publish(
            work_directory, install->manifest_sha256, &published,
            transaction_detail, sizeof transaction_detail);
    } else {
        publication = vm_guest_cydia_cache_publish(
            work_directory, install->manifest_sha256, &published,
            transaction_detail, sizeof transaction_detail);
    }
    if (grow_storage) {
        if (result) result->storage_transaction = published;
    } else if (repair_needed) {
        if (result) result->privilege_transaction = published;
    } else if (source_needed && result) {
        result->sources_v2_transaction = published;
    } else if (trust_needed && result) {
        result->apt_trust_transaction = published;
    } else if (verifier_needed && result) {
        result->apt_verifier_transaction = published;
    } else if (result) {
        result->cydia_cache_transaction = published;
    }
    if (publication != VM_GUEST_INSTALL_OK || !published.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(publication));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    if (result && grow_storage) result->storage_upgraded = true;
    if (repair_needed) {
        if (result) result->cydia_privileges_repaired = true;
        if (grow_storage) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_privilege_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->privilege_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK || !confirmed.committed) {
                build_detail(detail, detail_capacity,
                             transaction_detail[0]
                                 ? transaction_detail
                                 : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
            }
        }
        if (result) result->cydia_privileges_verified = true;
    }
    if (source_needed) {
        if (result) result->cydia_sources_added = true;
        if (!sources->committed) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_sources_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->sources_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK ||
                !confirmed.committed) {
                build_detail(detail, detail_capacity,
                             transaction_detail[0]
                                 ? transaction_detail
                                 : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
            }
        }
        if (grow_storage || repair_needed) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_sources_v2_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->sources_v2_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK ||
                !confirmed.committed) {
                build_detail(detail, detail_capacity,
                             transaction_detail[0]
                                 ? transaction_detail
                                 : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
            }
        }
        if (result) result->cydia_sources_verified = true;
    }
    if (trust_needed) {
        if (result && trust_create) result->apt_trust_installed = true;
        if (grow_storage || repair_needed || source_needed) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_apt_trust_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->apt_trust_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK ||
                !confirmed.committed) {
                build_detail(detail, detail_capacity,
                             transaction_detail[0]
                                 ? transaction_detail
                                 : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
            }
        }
        if (result) result->apt_trust_verified = true;
    }
    if (verifier_needed) {
        if (result) result->apt_verifier_staged = true;
        if (grow_storage || repair_needed || source_needed || trust_needed) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_apt_verifier_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->apt_verifier_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK ||
                !confirmed.committed) {
                build_detail(
                    detail, detail_capacity,
                    transaction_detail[0]
                        ? transaction_detail
                        : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
            }
        }
    }
    if (cache_needed) {
        if (result) result->cydia_cache_staged = true;
        if (grow_storage || repair_needed || source_needed || trust_needed ||
            verifier_needed) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_cydia_cache_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->cydia_cache_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK ||
                !confirmed.committed) {
                build_detail(
                    detail, detail_capacity,
                    transaction_detail[0]
                        ? transaction_detail
                        : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
            }
        }
    }
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 1u, 1u);
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_COMPLETE, 1u, 1u);
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
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 0u, 7u);
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;
    vm_guest_install_result_t sources;
    char transaction_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_guest_install_status_t maintenance_recovery =
        vm_guest_maintenance_recover(
            work_directory, &privilege, &storage, &sources, transaction_detail,
            sizeof transaction_detail);
    if (maintenance_recovery != VM_GUEST_INSTALL_OK) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(maintenance_recovery));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (result) {
        result->privilege_transaction = privilege;
        result->storage_transaction = storage;
        result->sources_transaction = sources;
    }
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 2u, 7u);
    vm_guest_install_result_t sources_v2;
    maintenance_recovery = vm_guest_sources_v2_recover(
        work_directory, &sources_v2, transaction_detail,
        sizeof transaction_detail);
    if (maintenance_recovery != VM_GUEST_INSTALL_OK) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(maintenance_recovery));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (result) result->sources_v2_transaction = sources_v2;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 3u, 7u);
    vm_guest_install_result_t apt_trust;
    maintenance_recovery = vm_guest_apt_trust_recover(
        work_directory, &apt_trust, transaction_detail,
        sizeof transaction_detail);
    if (maintenance_recovery != VM_GUEST_INSTALL_OK) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(maintenance_recovery));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (result) result->apt_trust_transaction = apt_trust;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 4u, 7u);
    vm_guest_install_result_t apt_verifier;
    maintenance_recovery = vm_guest_apt_verifier_recover(
        work_directory, &apt_verifier, transaction_detail,
        sizeof transaction_detail);
    if (maintenance_recovery != VM_GUEST_INSTALL_OK) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(maintenance_recovery));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (result) result->apt_verifier_transaction = apt_verifier;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 5u, 7u);
    vm_guest_install_result_t cydia_cache;
    maintenance_recovery = vm_guest_cydia_cache_recover(
        work_directory, &cydia_cache, transaction_detail,
        sizeof transaction_detail);
    if (maintenance_recovery != VM_GUEST_INSTALL_OK) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(maintenance_recovery));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (result) result->cydia_cache_transaction = cydia_cache;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 6u, 7u);
    vm_guest_install_result_t recovered;
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
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 7u, 7u);
    if (recovered.committed) {
        if (result) {
            result->already_installed = true;
            if (recovered.has_manifest)
                memcpy(result->manifest_sha256, recovered.manifest_sha256,
                       VM_GUEST_INSTALL_SHA256_SIZE);
        }
        return build_maintain_install(
            work_directory, package_directory, &recovered, &storage,
            &privilege, &sources, &sources_v2, &apt_trust, &apt_verifier,
            &cydia_cache,
            progress, progress_context, result, detail, detail_capacity);
    }
    if (storage.committed || privilege.committed || sources.committed ||
        sources_v2.committed || apt_trust.committed ||
        apt_verifier.committed || cydia_cache.committed) {
        build_detail(detail, detail_capacity,
                     "A guest-disk maintenance record exists without a committed guest installation.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
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
    options.minimum_volume_bytes = VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES;
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
    vm_guest_install_result_t sources_confirmed;
    vm_guest_install_status_t sources_confirmation =
        vm_guest_sources_confirm(
            work_directory, manifest, &sources_confirmed, transaction_detail,
            sizeof transaction_detail);
    if (result) result->sources_transaction = sources_confirmed;
    if (sources_confirmation != VM_GUEST_INSTALL_OK ||
        !sources_confirmed.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(sources_confirmation));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    sources_confirmation = vm_guest_sources_v2_confirm(
        work_directory, manifest, &sources_confirmed, transaction_detail,
        sizeof transaction_detail);
    if (result) result->sources_v2_transaction = sources_confirmed;
    if (sources_confirmation != VM_GUEST_INSTALL_OK ||
        !sources_confirmed.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(sources_confirmation));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    vm_guest_install_result_t trust_confirmed;
    vm_guest_install_status_t trust_confirmation =
        vm_guest_apt_trust_confirm(
            work_directory, manifest, &trust_confirmed, transaction_detail,
            sizeof transaction_detail);
    if (result) result->apt_trust_transaction = trust_confirmed;
    if (trust_confirmation != VM_GUEST_INSTALL_OK ||
        !trust_confirmed.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(trust_confirmation));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    vm_guest_install_result_t verifier_confirmed;
    vm_guest_install_status_t verifier_confirmation =
        vm_guest_apt_verifier_confirm(
            work_directory, manifest, &verifier_confirmed,
            transaction_detail, sizeof transaction_detail);
    if (result) result->apt_verifier_transaction = verifier_confirmed;
    if (verifier_confirmation != VM_GUEST_INSTALL_OK ||
        !verifier_confirmed.committed) {
        build_detail(
            detail, detail_capacity,
            transaction_detail[0]
                ? transaction_detail
                : vm_guest_install_status_text(verifier_confirmation));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    vm_guest_install_result_t cache_confirmed;
    vm_guest_install_status_t cache_confirmation =
        vm_guest_cydia_cache_confirm(
            work_directory, manifest, &cache_confirmed,
            transaction_detail, sizeof transaction_detail);
    if (result) result->cydia_cache_transaction = cache_confirmed;
    if (cache_confirmation != VM_GUEST_INSTALL_OK ||
        !cache_confirmed.committed) {
        build_detail(
            detail, detail_capacity,
            transaction_detail[0]
                ? transaction_detail
                : vm_guest_install_status_text(cache_confirmation));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    if (result) {
        result->cydia_sources_verified = true;
        result->apt_trust_installed = true;
        result->apt_trust_verified = true;
        result->apt_verifier_staged = true;
        result->cydia_cache_staged = true;
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
        case VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN:
            return "guest shutdown required";
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
