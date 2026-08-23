/*
 * S5LBox -- verified guest-package rootfs-plan tests.
 *
 * The ordinary fixture is authored byte-for-byte here. It proves the complete
 * package identity -> ar -> gzip -> tar -> remapped rootfs-entry path without
 * embedding any third-party package. S5LBOX_GUEST_PACKAGE_DIR optionally adds
 * a read-only integration pass over the locally acquired original packages.
 */
#include "VMGuestRootfsPlan.h"

#include "VMFirmwareFormats.h"
#include "sha256.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition, ...) do {                                         \
    checks++;                                                              \
    if (!(condition)) {                                                    \
        failures++;                                                        \
        printf("  FAIL %s:%d: ", __func__, __LINE__);                    \
        printf(__VA_ARGS__);                                               \
        printf("\n");                                                     \
    }                                                                      \
} while (0)

#define TAR_BLOCK 512u
#define TAR_CAPACITY 16384u
#define DEB_CAPACITY 65536u

typedef struct {
    uint8_t bytes[TAR_CAPACITY];
    size_t used;
} tar_fixture_t;

typedef struct {
    uint8_t bytes[DEB_CAPACITY];
    size_t size;
    size_t data_header;
} deb_fixture_t;

static void little32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8u);
    out[2] = (uint8_t)(value >> 16u);
    out[3] = (uint8_t)(value >> 24u);
}

static bool append_bytes(uint8_t *out, size_t capacity, size_t *offset,
                         const void *bytes, size_t size) {
    if (!out || !offset || (!bytes && size != 0u) || *offset > capacity ||
        size > capacity - *offset)
        return false;
    if (size != 0u) memcpy(out + *offset, bytes, size);
    *offset += size;
    return true;
}

static void tar_octal(uint8_t *field, size_t size,
                      unsigned long long value) {
    for (size_t i = size - 1u; i-- > 0u;) {
        field[i] = (uint8_t)('0' + (value & 7u));
        value >>= 3u;
    }
    field[size - 1u] = '\0';
}

static bool tar_add(tar_fixture_t *tar, const char *name, char type,
                    unsigned mode, const void *body, size_t body_size,
                    const char *link) {
    if (!tar || !name || (!body && body_size != 0u) ||
        tar->used > TAR_CAPACITY - TAR_BLOCK || strlen(name) >= 100u ||
        (link && strlen(link) >= 100u))
        return false;
    size_t padded = (body_size + TAR_BLOCK - 1u) / TAR_BLOCK * TAR_BLOCK;
    if (padded > TAR_CAPACITY - tar->used - TAR_BLOCK) return false;
    uint8_t *header = tar->bytes + tar->used;
    memset(header, 0, TAR_BLOCK);
    memcpy(header, name, strlen(name));
    tar_octal(header + 100u, 8u, mode);
    tar_octal(header + 108u, 8u, 0u);
    tar_octal(header + 116u, 8u, 0u);
    tar_octal(header + 124u, 12u, body_size);
    tar_octal(header + 136u, 12u, 0u);
    memset(header + 148u, ' ', 8u);
    header[156u] = (uint8_t)type;
    if (link) memcpy(header + 157u, link, strlen(link));
    memcpy(header + 257u, "ustar  ", 8u);
    unsigned long checksum = 0u;
    for (size_t i = 0u; i < TAR_BLOCK; i++) checksum += header[i];
    tar_octal(header + 148u, 7u, checksum);
    header[155u] = ' ';
    tar->used += TAR_BLOCK;
    if (body_size != 0u) memcpy(tar->bytes + tar->used, body, body_size);
    tar->used += padded;
    return true;
}

static bool make_tar_variant(tar_fixture_t *tar, bool ncurses_alias) {
    static const uint8_t TOOL[] = {'t', 'o', 'o', 'l', '\n'};
    static const uint8_t CONFIG[] = {'o', 'k', '\n'};
    if (!tar) return false;
    memset(tar, 0, sizeof *tar);
    if (!(tar_add(tar, "./var/", '5', 0755u, NULL, 0u, NULL) &&
           tar_add(tar, "./var/lib/", '5', 0755u, NULL, 0u, NULL) &&
           tar_add(tar, "./var/lib/testpkg/", '5', 0755u, NULL, 0u, NULL) &&
           tar_add(tar, "./var/lib/testpkg/state", '0', 0644u,
                   TOOL, sizeof TOOL, NULL) &&
           tar_add(tar, "./etc/", '5', 0755u, NULL, 0u, NULL) &&
           tar_add(tar, "./etc/testpkg.conf", '0', 0644u,
                   CONFIG, sizeof CONFIG, NULL) &&
           tar_add(tar, "./usr/", '5', 0755u, NULL, 0u, NULL) &&
           tar_add(tar, "./usr/bin/", '5', 0755u, NULL, 0u, NULL) &&
           tar_add(tar, "./usr/bin/testpkg", '0', 0755u,
                   TOOL, sizeof TOOL, NULL) &&
           tar_add(tar, "./bin/", '5', 0755u, NULL, 0u, NULL) &&
           tar_add(tar, "./bin/testpkg", '2', 0777u, NULL, 0u,
                   "/usr/bin/testpkg")))
        return false;
    if (ncurses_alias &&
        !(tar_add(tar, "./usr/lib/", '5', 0755u, NULL, 0u, NULL) &&
          tar_add(tar, "./usr/lib/libncurses.5.dylib", '0', 0755u,
                  TOOL, sizeof TOOL, NULL) &&
          tar_add(tar, "./usr/lib/_ncurses/", '5', 0755u,
                  NULL, 0u, NULL) &&
          tar_add(tar, "./usr/lib/_ncurses/libcurses.dylib", '2', 0777u,
                  NULL, 0u, "libncurses.5.dylib") &&
          tar_add(tar, "./usr/lib/_ncurses/libncurses.dylib", '2', 0777u,
                  NULL, 0u, "libncurses.5.dylib")))
        return false;
    if (tar->used > TAR_CAPACITY - 2u * TAR_BLOCK) return false;
    memset(tar->bytes + tar->used, 0, 2u * TAR_BLOCK);
    tar->used += 2u * TAR_BLOCK;
    return true;
}

static size_t make_gzip(uint8_t *out, size_t capacity,
                        const uint8_t *payload, size_t payload_size) {
    if (!out || !payload || payload_size > 65535u ||
        capacity < payload_size + 23u)
        return 0u;
    static const uint8_t HEADER[10] = {
        0x1fu, 0x8bu, 8u, 0u, 0u, 0u, 0u, 0u, 2u, 3u
    };
    size_t offset = 0u;
    if (!append_bytes(out, capacity, &offset, HEADER, sizeof HEADER)) return 0u;
    out[offset++] = 0x01u;
    uint16_t length = (uint16_t)payload_size;
    uint16_t inverse = (uint16_t)~length;
    out[offset++] = (uint8_t)length;
    out[offset++] = (uint8_t)(length >> 8u);
    out[offset++] = (uint8_t)inverse;
    out[offset++] = (uint8_t)(inverse >> 8u);
    if (!append_bytes(out, capacity, &offset, payload, payload_size)) return 0u;
    little32(out + offset, vmfw_crc32(0u, payload, payload_size));
    little32(out + offset + 4u, (uint32_t)payload_size);
    return offset + 8u;
}

static bool ar_member(deb_fixture_t *deb, const char *name,
                      const uint8_t *content, size_t content_size,
                      size_t *header_offset) {
    if (!deb || !name || !content || strlen(name) > 16u ||
        deb->size > DEB_CAPACITY - 60u)
        return false;
    uint8_t header[60];
    memset(header, ' ', sizeof header);
    memcpy(header, name, strlen(name));
    memcpy(header + 16u, "0", 1u);
    memcpy(header + 28u, "0", 1u);
    memcpy(header + 34u, "0", 1u);
    memcpy(header + 40u, "100644", 6u);
    char size_text[16];
    int length = snprintf(size_text, sizeof size_text, "%zu", content_size);
    if (length <= 0 || (size_t)length > 10u) return false;
    memcpy(header + 48u, size_text, (size_t)length);
    header[58u] = '`';
    header[59u] = '\n';
    if (header_offset) *header_offset = deb->size;
    if (!append_bytes(deb->bytes, sizeof deb->bytes, &deb->size,
                      header, sizeof header) ||
        !append_bytes(deb->bytes, sizeof deb->bytes, &deb->size,
                      content, content_size))
        return false;
    if ((content_size & 1u) != 0u) {
        static const uint8_t PAD = '\n';
        if (!append_bytes(deb->bytes, sizeof deb->bytes, &deb->size,
                          &PAD, 1u))
            return false;
    }
    return true;
}

static bool make_deb_variant(deb_fixture_t *deb, bool ncurses_alias) {
    static const uint8_t AR_MAGIC[8] = {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
    static const uint8_t VERSION[4] = {'2', '.', '0', '\n'};
    tar_fixture_t tar;
    uint8_t gzip[TAR_CAPACITY + 32u];
    if (!deb || !make_tar_variant(&tar, ncurses_alias)) return false;
    size_t gzip_size = make_gzip(gzip, sizeof gzip, tar.bytes, tar.used);
    if (gzip_size == 0u) return false;
    memset(deb, 0, sizeof *deb);
    return append_bytes(deb->bytes, sizeof deb->bytes, &deb->size,
                        AR_MAGIC, sizeof AR_MAGIC) &&
           ar_member(deb, "debian-binary", VERSION, sizeof VERSION, NULL) &&
           ar_member(deb, "control.tar.gz", gzip, gzip_size, NULL) &&
           ar_member(deb, "data.tar.gz", gzip, gzip_size, &deb->data_header);
}

static bool make_deb(deb_fixture_t *deb) {
    return make_deb_variant(deb, false);
}

static void digest_hex(const uint8_t *bytes, size_t size,
                       char out[VM_GUEST_PACKAGE_SHA256_HEX_SIZE]) {
    static const char HEX[] = "0123456789abcdef";
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE];
    bool ok = ios3_sha256(bytes, size, digest);
    CHECK(ok, "could not hash fixture");
    for (size_t i = 0u; i < sizeof digest; i++) {
        out[i * 2u] = HEX[digest[i] >> 4u];
        out[i * 2u + 1u] = HEX[digest[i] & 15u];
    }
    out[VM_GUEST_PACKAGE_SHA256_HEX_SIZE - 1u] = '\0';
}

static void make_named_package(
    vm_guest_package_t *package,
    char sha[VM_GUEST_PACKAGE_SHA256_HEX_SIZE],
    char url[256], const deb_fixture_t *deb,
    const char *name, const char *version, const char *filename) {
    digest_hex(deb->bytes, deb->size, sha);
    int written = snprintf(url, 256u,
        "https://apt.saurik.com/cydia-3.7/debs/%s", filename);
    CHECK(written > 0 && written < 256, "fixture URL was truncated");
    package->package = name;
    package->version = version;
    package->filename = filename;
    package->source_url = url;
    package->size = deb->size;
    package->sha256_hex = sha;
    package->roles = VM_GUEST_PACKAGE_INSTALL | VM_GUEST_PACKAGE_FOUNDATION;
}

static void make_package(vm_guest_package_t *package,
                         char sha[VM_GUEST_PACKAGE_SHA256_HEX_SIZE],
                         char url[256], const deb_fixture_t *deb) {
    make_named_package(package, sha, url, deb, "testpkg", "1-1",
                       "testpkg_1-1_iphoneos-arm.deb");
}

static const rootfs_work_entry_t *find_entry(
    const vm_guest_rootfs_plan_t *plan, const char *path) {
    const rootfs_work_entry_t *entries = vm_guest_rootfs_plan_entries(plan);
    size_t count = vm_guest_rootfs_plan_entry_count(plan);
    for (size_t i = 0u; i < count; i++)
        if (strcmp(entries[i].path, path) == 0) return &entries[i];
    return NULL;
}

static void test_complete_synthetic_plan(void) {
    deb_fixture_t deb;
    CHECK(make_deb(&deb), "could not build synthetic package");
    vm_guest_package_t package;
    vm_guest_package_t cache_tool;
    char sha[VM_GUEST_PACKAGE_SHA256_HEX_SIZE];
    char cache_tool_sha[VM_GUEST_PACKAGE_SHA256_HEX_SIZE];
    char url[256];
    char cache_tool_url[256];
    make_package(&package, sha, url, &deb);
    make_named_package(&cache_tool, cache_tool_sha, cache_tool_url, &deb,
                       "apt7", "0.7.20.2-1",
                       "apt7_0.7.20.2-1_iphoneos-arm.deb");
    cache_tool.roles = VM_GUEST_PACKAGE_APT_CACHE_TOOL;
    vm_guest_package_input_t inputs[] = {
        {&package, deb.bytes, deb.size},
        {&cache_tool, deb.bytes, deb.size}
    };
    char detail[VM_GUEST_ROOTFS_DETAIL_CAPACITY];
    vm_guest_rootfs_status_t status = VM_GUEST_ROOTFS_ERR_ARGUMENT;
    vm_guest_rootfs_plan_t *plan = vm_guest_rootfs_plan_open(
        inputs, sizeof inputs / sizeof inputs[0],
        &status, detail, sizeof detail);
    CHECK(plan != NULL && status == VM_GUEST_ROOTFS_OK,
          "valid plan refused: %s / %s",
          vm_guest_rootfs_status_text(status), detail);
    if (!plan) return;

    const rootfs_work_entry_t *var_state =
        find_entry(plan, "/private/var/lib/testpkg/state");
    const rootfs_work_entry_t *etc_config =
        find_entry(plan, "/private/etc/testpkg.conf");
    const rootfs_work_entry_t *usr_tool =
        find_entry(plan, "/usr/bin/testpkg");
    const rootfs_work_entry_t *link = find_entry(plan, "/bin/testpkg");
    CHECK(var_state && etc_config && usr_tool && link,
          "one or more remapped foundation entries are missing");
    CHECK(find_entry(plan, "/var/lib/testpkg/state") == NULL &&
          find_entry(plan, "/etc/testpkg.conf") == NULL,
          "an alias path was not remapped to its physical directory");
    CHECK(link && link->kind == ROOTFS_WORK_ENTRY_SYMLINK &&
          link->existing_policy == ROOTFS_WORK_EXISTING_REUSE_IDENTICAL_SYMLINK,
          "foundation symlink does not fail closed against an existing target");
    const rootfs_work_entry_t *usr_directory = find_entry(plan, "/usr");
    CHECK(usr_directory &&
          usr_directory->existing_policy == ROOTFS_WORK_EXISTING_REUSE_DIRECTORY,
          "foundation directory is not reusable on the stock rootfs");

    const rootfs_work_entry_t *apt_directory =
        find_entry(plan, "/private/etc/apt");
    const rootfs_work_entry_t *source_directory =
        find_entry(plan, "/private/etc/apt/sources.list.d");
    const rootfs_work_entry_t *apt_compat_directory =
        find_entry(plan, VM_GUEST_ROOTFS_APT_COMPAT_DIRECTORY);
    const rootfs_work_entry_t *cydia_source =
        find_entry(plan, VM_GUEST_ROOTFS_SAURIK_SOURCE_PATH);
    const rootfs_work_entry_t *bigboss_source =
        find_entry(plan, VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH);
    const rootfs_work_entry_t *ios3_party_source =
        find_entry(plan, VM_GUEST_ROOTFS_IOS3_PARTY_SOURCE_PATH);
    const rootfs_work_entry_t *apt_compat =
        find_entry(plan, VM_GUEST_ROOTFS_APT_COMPAT_PATH);
    const rootfs_work_entry_t *trusted_keyring =
        find_entry(plan, VM_GUEST_ROOTFS_TRUSTED_KEYRING_PATH);
    static const char EXPECTED_CYDIA_SOURCE[] =
        VM_GUEST_ROOTFS_SAURIK_SOURCE_LINE;
    static const char EXPECTED_BIGBOSS_SOURCE[] =
        VM_GUEST_ROOTFS_BIGBOSS_SOURCE_LINE;
    static const char EXPECTED_IOS3_PARTY_SOURCE[] =
        VM_GUEST_ROOTFS_IOS3_PARTY_SOURCE_LINE;
    static const char EXPECTED_APT_COMPAT[] =
        VM_GUEST_ROOTFS_APT_COMPAT_CONTENT;
    CHECK(apt_directory && source_directory && apt_compat_directory &&
          apt_directory->kind == ROOTFS_WORK_ENTRY_DIRECTORY &&
          source_directory->kind == ROOTFS_WORK_ENTRY_DIRECTORY &&
          apt_compat_directory->kind == ROOTFS_WORK_ENTRY_DIRECTORY &&
          apt_directory->permissions == 0755u &&
          source_directory->permissions == 0755u &&
          apt_compat_directory->permissions == 0755u &&
          apt_directory->existing_policy ==
              ROOTFS_WORK_EXISTING_REUSE_DIRECTORY &&
          source_directory->existing_policy ==
              ROOTFS_WORK_EXISTING_REUSE_DIRECTORY &&
          apt_compat_directory->existing_policy ==
              ROOTFS_WORK_EXISTING_REUSE_DIRECTORY,
          "the official source parent directories are absent or unsafe");
    CHECK(cydia_source && cydia_source->kind == ROOTFS_WORK_ENTRY_FILE &&
          cydia_source->permissions == 0644u &&
          cydia_source->owner_id == 0u && cydia_source->group_id == 0u &&
          cydia_source->existing_policy == ROOTFS_WORK_EXISTING_REFUSE &&
          cydia_source->content_size == sizeof EXPECTED_CYDIA_SOURCE - 1u &&
          memcmp(cydia_source->content, EXPECTED_CYDIA_SOURCE,
                 sizeof EXPECTED_CYDIA_SOURCE - 1u) == 0,
          "the official period-compatible Cydia source is not exact root:root 0644 data");
    CHECK(bigboss_source &&
          bigboss_source->kind == ROOTFS_WORK_ENTRY_FILE &&
          bigboss_source->permissions == 0644u &&
          bigboss_source->owner_id == 0u &&
          bigboss_source->group_id == 0u &&
          bigboss_source->existing_policy == ROOTFS_WORK_EXISTING_REFUSE &&
          bigboss_source->content_size ==
              sizeof EXPECTED_BIGBOSS_SOURCE - 1u &&
          memcmp(bigboss_source->content, EXPECTED_BIGBOSS_SOURCE,
                 sizeof EXPECTED_BIGBOSS_SOURCE - 1u) == 0,
          "the BigBoss distribution source is not exact root:root 0644 data");
    CHECK(ios3_party_source &&
          ios3_party_source->kind == ROOTFS_WORK_ENTRY_FILE &&
          ios3_party_source->permissions == 0644u &&
          ios3_party_source->owner_id == 0u &&
          ios3_party_source->group_id == 0u &&
          ios3_party_source->existing_policy == ROOTFS_WORK_EXISTING_REFUSE &&
          ios3_party_source->content_size ==
              sizeof EXPECTED_IOS3_PARTY_SOURCE - 1u &&
          memcmp(ios3_party_source->content, EXPECTED_IOS3_PARTY_SOURCE,
                 sizeof EXPECTED_IOS3_PARTY_SOURCE - 1u) == 0,
          "the iOS 3 repository source is not exact root:root 0644 data");
    CHECK(apt_compat && apt_compat->kind == ROOTFS_WORK_ENTRY_FILE &&
          apt_compat->permissions == 0644u &&
          apt_compat->owner_id == 0u && apt_compat->group_id == 0u &&
          apt_compat->existing_policy == ROOTFS_WORK_EXISTING_REFUSE &&
          apt_compat->content_size == sizeof EXPECTED_APT_COMPAT - 1u &&
          memcmp(apt_compat->content, EXPECTED_APT_COMPAT,
                 sizeof EXPECTED_APT_COMPAT - 1u) == 0,
          "the legacy APT no-PDiff configuration is not exact root:root 0644 data");
    size_t expected_keyring_size = 0u;
    const uint8_t *expected_keyring =
        vm_guest_rootfs_bigboss_keyring(&expected_keyring_size);
    uint8_t keyring_digest[IOS3_SHA256_DIGEST_SIZE];
    static const uint8_t EXPECTED_KEYRING_DIGEST[
        IOS3_SHA256_DIGEST_SIZE] = {
        0x0du, 0x01u, 0xddu, 0x89u, 0x07u, 0x22u, 0xaeu, 0x15u,
        0x91u, 0x6cu, 0x77u, 0x30u, 0xe8u, 0x9au, 0xbau, 0x8cu,
        0x41u, 0xa8u, 0xceu, 0xaeu, 0xa5u, 0x02u, 0x93u, 0xf8u,
        0xbbu, 0x4au, 0xc9u, 0xd2u, 0x79u, 0x5fu, 0xecu, 0xb5u
    };
    CHECK(expected_keyring && expected_keyring_size == 1164u &&
          ios3_sha256(expected_keyring, expected_keyring_size,
                      keyring_digest) &&
          memcmp(keyring_digest, EXPECTED_KEYRING_DIGEST,
                 sizeof keyring_digest) == 0,
          "the embedded BigBoss public key has the wrong identity");
    CHECK(trusted_keyring &&
          trusted_keyring->kind == ROOTFS_WORK_ENTRY_FILE &&
          trusted_keyring->permissions == 0644u &&
          trusted_keyring->owner_id == 0u &&
          trusted_keyring->group_id == 0u &&
          trusted_keyring->existing_policy == ROOTFS_WORK_EXISTING_REFUSE &&
          trusted_keyring->content_size == expected_keyring_size &&
          memcmp(trusted_keyring->content, expected_keyring,
                 expected_keyring_size) == 0,
          "the legacy APT keyring is not exact root:root 0644 key data");

    char cache_path[ROOTFS_WORK_MAX_PATH];
    int written = snprintf(cache_path, sizeof cache_path, "%s/%s",
                           VM_GUEST_ROOTFS_PACKAGE_DIRECTORY,
                           package.filename);
    CHECK(written > 0 && (size_t)written < sizeof cache_path,
          "fixture cache path was truncated");
    const rootfs_work_entry_t *cached = find_entry(plan, cache_path);
    CHECK(cached && cached->content_size == deb.size &&
          memcmp(cached->content, deb.bytes, deb.size) == 0,
          "the exact package was not retained in the guest cache");
    uint8_t first = cached ? cached->content[0] : 0u;
    deb.bytes[0] ^= 0xffu;
    CHECK(cached && cached->content[0] == first,
          "the plan aliases caller-owned package memory");
    char cache_tool_path[ROOTFS_WORK_MAX_PATH];
    written = snprintf(cache_tool_path, sizeof cache_tool_path, "%s/%s",
                       VM_GUEST_ROOTFS_PACKAGE_DIRECTORY,
                       cache_tool.filename);
    CHECK(written > 0 && (size_t)written < sizeof cache_tool_path,
          "cache-tool path was truncated");
    const rootfs_work_entry_t *cached_tool =
        find_entry(plan, cache_tool_path);
    CHECK(cached_tool && cached_tool->content_size == deb.size,
          "the authenticated APT cache-tool archive was not staged");

    const rootfs_work_entry_t *script =
        find_entry(plan, VM_GUEST_ROOTFS_INSTALL_SCRIPT);
    const rootfs_work_entry_t *plist =
        find_entry(plan, VM_GUEST_ROOTFS_LAUNCHD_PLIST);
    CHECK(script && script->permissions == 0755u &&
          script->content_size != 0u &&
          strstr((const char *)script->content,
                 "testpkg_1-1_iphoneos-arm.deb") != NULL,
          "the first-boot script does not install the fixture package");
    CHECK(script &&
          strstr((const char *)script->content,
                 "install_one \"$packages/apt7_0.7.20.2-1_iphoneos-arm.deb\"") == NULL &&
          strstr((const char *)script->content,
                 "cache_archive=\"$packages/apt7_0.7.20.2-1_iphoneos-arm.deb\"") != NULL &&
          strstr((const char *)script->content,
                 "/usr/bin/dpkg-deb --extract \"$cache_archive\" \"$cache_stage\" || exit 1") != NULL &&
          strstr((const char *)script->content,
                 "exec \"$apt_get\" -o Acquire::PDiffs=false -o Acquire::http::Timeout=30 -o Acquire::Retries=1 update") != NULL &&
          strstr((const char *)script->content,
                 "exceeded 180 seconds; terminating it") != NULL &&
          strstr((const char *)script->content,
                 "\"$apt_cache\" gencaches || exit 1") != NULL &&
          strstr((const char *)script->content,
                 "\"$apt_cache\" stats >/dev/null 2>&1 || exit 1") != NULL,
          "the tool-only APT archive is installed or does not build and validate caches");
    CHECK(plist && plist->content_size != 0u &&
          strstr((const char *)plist->content,
                 "com.j0shua.s5lbox.guest-install") != NULL,
          "the launchd job is absent");
    CHECK(script &&
          strstr((const char *)script->content,
                 "/bin/su --login --command /usr/bin/uicache mobile || exit 1") != NULL,
          "the first-boot script does not refresh the mobile user's icon cache");
    CHECK(script &&
          strstr((const char *)script->content,
                 "/usr/bin/uicache || true") == NULL,
          "the first-boot script still hides an icon-cache failure");
    const char *configured = script
        ? strstr((const char *)script->content,
                 "/usr/bin/dpkg --force-depends --configure -a || exit 1")
        : NULL;
    const char *apt_cache_ready = script
        ? strstr((const char *)script->content,
                 "Cydia APT caches built outside the application watchdog")
        : NULL;
    const char *cydia_owner = script
        ? strstr((const char *)script->content,
                 "/bin/chown 0:0 \"$cydia\" || exit 1")
        : NULL;
    const char *cydia_mode = script
        ? strstr((const char *)script->content,
                 "/bin/chmod 6755 \"$cydia\" || exit 1")
        : NULL;
    const char *cydia_mode_check = script
        ? strstr((const char *)script->content,
                 "[ -u \"$cydia\" ] && [ -g \"$cydia\" ] || exit 1")
        : NULL;
    CHECK(configured && apt_cache_ready && cydia_owner && cydia_mode &&
          cydia_mode_check && configured < apt_cache_ready &&
          apt_cache_ready < cydia_owner && cydia_owner < cydia_mode &&
          cydia_mode < cydia_mode_check,
          "Cydia caches are not validated before its root 6755 executable is published");
    const char *mobile_cache = script
        ? strstr((const char *)script->content,
                 "/bin/grep -aq 'com.saurik.Cydia' \"$cache\" || exit 1")
        : NULL;
    const char *springboard_ready = script
        ? strstr((const char *)script->content,
                 "/usr/bin/killall -0 SpringBoard >/dev/null 2>&1")
        : NULL;
    const char *icon_cache = script
        ? strstr((const char *)script->content,
                 "[ -s \"$icon_cache/com.saurik.Cydia\" ] || exit 1")
        : NULL;
    const char *respring = script
        ? strstr((const char *)script->content,
                 "/usr/bin/killall SpringBoard || exit 1")
        : NULL;
    const char *completion = script
        ? strstr((const char *)script->content,
                 ": >\"$state/complete.partial\" || exit 1")
        : NULL;
    CHECK(springboard_ready && mobile_cache && icon_cache && respring &&
          completion && cydia_mode_check < springboard_ready &&
          springboard_ready < mobile_cache && mobile_cache < icon_cache &&
          icon_cache < respring &&
          respring < completion,
          "the install can complete before SpringBoard is ready or Cydia is visibly cached and resprung");
    CHECK(script &&
          strstr((const char *)script->content,
                 "/usr/bin/killall SpringBoard || true") == NULL &&
          strstr((const char *)script->content,
                 "[ \"$attempt\" -lt 60 ] || exit 1") != NULL,
          "the first-boot cache refresh still hides a missing SpringBoard process");

    vm_guest_rootfs_stats_t stats;
    vm_guest_rootfs_plan_get_stats(plan, &stats);
    CHECK(stats.packages == 2u && stats.foundation_packages == 1u &&
          stats.entries == vm_guest_rootfs_plan_entry_count(plan) &&
          stats.download_bytes == package.size + cache_tool.size &&
          stats.files != 0u &&
          stats.directories != 0u && stats.symlinks == 1u,
          "unexpected plan stats: packages=%zu foundation=%zu entries=%zu",
          stats.packages, stats.foundation_packages, stats.entries);
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE];
    CHECK(vm_guest_rootfs_plan_manifest_sha256(plan, digest),
          "plan manifest identity was not produced");
    static const uint8_t EXPECTED[VM_GUEST_PACKAGE_SHA256_SIZE] = {
        0x44u, 0x0au, 0x33u, 0xc9u, 0x98u, 0x2bu, 0x71u, 0x2bu,
        0xf9u, 0x32u, 0x9bu, 0x43u, 0x97u, 0xe9u, 0xbdu, 0x6eu,
        0x6cu, 0xf8u, 0xe3u, 0xa4u, 0x51u, 0xc5u, 0x63u, 0xc2u,
        0xc9u, 0xb9u, 0xcdu, 0x77u, 0x60u, 0x05u, 0xffu, 0xa4u
    };
    CHECK(memcmp(digest, EXPECTED, sizeof digest) == 0,
          "synthetic plan identity changed");
    printf("synthetic-plan-sha256 ");
    for (size_t i = 0u; i < sizeof digest; i++) printf("%02x", digest[i]);
    printf("\n");
    vm_guest_rootfs_plan_close(&plan);
    CHECK(plan == NULL, "close did not clear the plan slot");
}

static void test_identity_and_compression_refusals(void) {
    deb_fixture_t deb;
    CHECK(make_deb(&deb), "could not build refusal fixture");
    vm_guest_package_t package;
    char sha[VM_GUEST_PACKAGE_SHA256_HEX_SIZE];
    char url[256];
    make_package(&package, sha, url, &deb);
    vm_guest_package_input_t input = {&package, deb.bytes, deb.size};
    char detail[VM_GUEST_ROOTFS_DETAIL_CAPACITY];
    vm_guest_rootfs_status_t status = VM_GUEST_ROOTFS_OK;

    sha[63] = sha[63] == '0' ? '1' : '0';
    vm_guest_rootfs_plan_t *plan = vm_guest_rootfs_plan_open(
        &input, 1u, &status, detail, sizeof detail);
    CHECK(plan == NULL && status == VM_GUEST_ROOTFS_ERR_PACKAGE_IDENTITY,
          "wrong SHA was not refused: %s / %s",
          vm_guest_rootfs_status_text(status), detail);
    vm_guest_rootfs_plan_close(&plan);

    make_package(&package, sha, url, &deb);
    package.size = (uint64_t)VM_GUEST_ROOTFS_MAX_DOWNLOAD_BYTES + 1u;
    input.size = (size_t)VM_GUEST_ROOTFS_MAX_DOWNLOAD_BYTES + 1u;
    status = VM_GUEST_ROOTFS_OK;
    plan = vm_guest_rootfs_plan_open(&input, 1u, &status,
                                     detail, sizeof detail);
    CHECK(plan == NULL && status == VM_GUEST_ROOTFS_ERR_LIMIT,
          "oversized package set was not bounded: %s / %s",
          vm_guest_rootfs_status_text(status), detail);
    vm_guest_rootfs_plan_close(&plan);

    make_package(&package, sha, url, &deb);
    input.size = deb.size;
    memset(deb.bytes + deb.data_header, ' ', 16u);
    memcpy(deb.bytes + deb.data_header, "data.tar.lzma", 13u);
    make_package(&package, sha, url, &deb);
    input.bytes = deb.bytes;
    input.size = deb.size;
    status = VM_GUEST_ROOTFS_OK;
    plan = vm_guest_rootfs_plan_open(&input, 1u, &status,
                                     detail, sizeof detail);
    CHECK(plan == NULL && status == VM_GUEST_ROOTFS_ERR_COMPRESSION,
          "non-gzip foundation was not refused: %s / %s",
          vm_guest_rootfs_status_text(status), detail);
    vm_guest_rootfs_plan_close(&plan);
}

static void test_ncurses_preinst_alias_is_left_for_guest_dpkg(void) {
    /* Model only the conflicting archive paths. No package-maintainer binary
     * is embedded in this fixture. */
    deb_fixture_t deb;
    CHECK(make_deb_variant(&deb, true),
          "could not build the ncurses-alias fixture");
    vm_guest_package_t package;
    char sha[VM_GUEST_PACKAGE_SHA256_HEX_SIZE];
    char url[256];
    make_named_package(&package, sha, url, &deb, "ncurses", "5.7-10",
                       "ncurses_5.7-10_iphoneos-arm.deb");
    vm_guest_package_input_t input = {&package, deb.bytes, deb.size};
    char detail[VM_GUEST_ROOTFS_DETAIL_CAPACITY];
    vm_guest_rootfs_status_t status = VM_GUEST_ROOTFS_ERR_ARGUMENT;
    vm_guest_rootfs_plan_t *plan = vm_guest_rootfs_plan_open(
        &input, 1u, &status, detail, sizeof detail);
    CHECK(plan != NULL && status == VM_GUEST_ROOTFS_OK,
          "ncurses-alias plan refused: %s / %s",
          vm_guest_rootfs_status_text(status), detail);
    if (!plan) return;

    CHECK(find_entry(plan, "/usr/lib/libncurses.5.dylib") != NULL,
          "ordinary ncurses foundation content was not seeded");
    CHECK(find_entry(plan, "/usr/lib/_ncurses") == NULL &&
          find_entry(plan, "/usr/lib/_ncurses/libcurses.dylib") == NULL &&
          find_entry(plan, "/usr/lib/_ncurses/libncurses.dylib") == NULL,
          "the ncurses preinst-owned alias subtree was preseeded");
    vm_guest_rootfs_plan_close(&plan);

    make_named_package(&package, sha, url, &deb, "ncurses", "5.7-11",
                       "ncurses_5.7-11_iphoneos-arm.deb");
    input.package = &package;
    status = VM_GUEST_ROOTFS_ERR_ARGUMENT;
    plan = vm_guest_rootfs_plan_open(&input, 1u, &status,
                                     detail, sizeof detail);
    CHECK(plan != NULL && status == VM_GUEST_ROOTFS_OK,
          "nearby ncurses revision fixture refused: %s / %s",
          vm_guest_rootfs_status_text(status), detail);
    CHECK(plan && find_entry(plan, "/usr/lib/_ncurses") != NULL,
          "an unaudited ncurses revision inherited the compatibility rule");
    vm_guest_rootfs_plan_close(&plan);
}

static void test_real_packages_when_supplied(void) {
    const char *directory = getenv("S5LBOX_GUEST_PACKAGE_DIR");
    if (!directory || !*directory) {
        printf("real-package-plan SKIP (S5LBOX_GUEST_PACKAGE_DIR unset)\n");
        return;
    }
    char detail[VM_GUEST_ROOTFS_DETAIL_CAPACITY];
    vm_guest_rootfs_status_t status = VM_GUEST_ROOTFS_ERR_ARGUMENT;
    vm_guest_rootfs_plan_t *plan = vm_guest_rootfs_plan_open_directory(
        directory, &status, detail, sizeof detail);
    CHECK(plan != NULL && status == VM_GUEST_ROOTFS_OK,
          "real package plan refused: %s / %s",
          vm_guest_rootfs_status_text(status), detail);
    if (!plan) return;
    vm_guest_rootfs_stats_t stats;
    vm_guest_rootfs_plan_get_stats(plan, &stats);
    CHECK(stats.packages == 30u && stats.foundation_packages == 14u,
          "real plan has %zu packages / %zu foundation packages",
          stats.packages, stats.foundation_packages);
    CHECK(stats.entries != 0u && stats.entries <= ROOTFS_WORK_MAX_ENTRIES,
          "real plan has an invalid entry count: %zu", stats.entries);
    CHECK(find_entry(plan, "/usr/lib/libncurses.5.dylib") != NULL &&
          find_entry(plan, "/usr/lib/_ncurses") == NULL &&
          find_entry(plan, "/usr/lib/_ncurses/libcurses.dylib") == NULL &&
          find_entry(plan, "/usr/lib/_ncurses/libncurses.dylib") == NULL,
          "the real ncurses plan violates its preinst alias invariant");
    CHECK(find_entry(
              plan,
              VM_GUEST_ROOTFS_PACKAGE_DIRECTORY
              "/apt7_0.7.20.2-1_iphoneos-arm.deb") != NULL,
          "the real plan did not stage its authenticated APT cache tool");
    printf("real-package-plan packages=%zu foundation=%zu entries=%zu "
           "deduplicated=%zu payload-bytes=%llu\n",
           stats.packages, stats.foundation_packages, stats.entries,
           stats.deduplicated_entries,
           (unsigned long long)stats.provision_content_bytes);

    const char *source = getenv("S5LBOX_ROOTFS_SOURCE");
    const char *destination = getenv("S5LBOX_ROOTFS_DESTINATION");
    bool has_source = source && *source;
    bool has_destination = destination && *destination;
    CHECK(has_source == has_destination,
          "real disk test needs both S5LBOX_ROOTFS_SOURCE and "
          "S5LBOX_ROOTFS_DESTINATION");
    if (has_source && has_destination) {
        rootfs_work_options_t options;
        memset(&options, 0, sizeof options);
        options.growth_bytes = UINT64_C(32) * 1024u * 1024u;
        options.entries = vm_guest_rootfs_plan_entries(plan);
        options.entry_count = vm_guest_rootfs_plan_entry_count(plan);
        rootfs_work_result_t result;
        rootfs_work_status_t disk = rootfs_work_create(
            source, destination, &options, &result);
        CHECK(disk == ROOTFS_WORK_OK && result.published,
              "real HFS plan refused at %s: %s / system=%d cleanup=%d",
              rootfs_work_stage_name(result.stage), result.detail,
              result.system_error, result.cleanup_system_error);
        if (disk == ROOTFS_WORK_OK) {
            CHECK(result.provision_entries + result.provision_reused_entries ==
                  stats.entries,
                  "HFS applied %u and reused %u of %zu entries",
                  result.provision_entries, result.provision_reused_entries,
                  stats.entries);
            printf("real-rootfs-plan final-bytes=%llu created=%u reused=%u "
                   "blocks=%u leaf-splits=%u index-splits=%u\n",
                   (unsigned long long)result.final_size,
                   result.provision_entries, result.provision_reused_entries,
                   result.provision_blocks, result.provision_leaf_splits,
                   result.provision_index_splits);
        }
    } else {
        printf("real-rootfs-plan SKIP (source/destination unset)\n");
    }
    vm_guest_rootfs_plan_close(&plan);
}

int main(void) {
    printf("== guest rootfs plan ==\n");
    test_complete_synthetic_plan();
    test_identity_and_compression_refusals();
    test_ncurses_preinst_alias_is_left_for_guest_dpkg();
    test_real_packages_when_supplied();
    printf("== guest rootfs plan: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
