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

static bool make_tar(tar_fixture_t *tar) {
    static const uint8_t TOOL[] = {'t', 'o', 'o', 'l', '\n'};
    static const uint8_t CONFIG[] = {'o', 'k', '\n'};
    if (!tar) return false;
    memset(tar, 0, sizeof *tar);
    return tar_add(tar, "./var/", '5', 0755u, NULL, 0u, NULL) &&
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
                   "/usr/bin/testpkg") &&
           tar->used <= TAR_CAPACITY - 2u * TAR_BLOCK &&
           (memset(tar->bytes + tar->used, 0, 2u * TAR_BLOCK),
            tar->used += 2u * TAR_BLOCK, true);
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

static bool make_deb(deb_fixture_t *deb) {
    static const uint8_t AR_MAGIC[8] = {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
    static const uint8_t VERSION[4] = {'2', '.', '0', '\n'};
    tar_fixture_t tar;
    uint8_t gzip[TAR_CAPACITY + 32u];
    if (!deb || !make_tar(&tar)) return false;
    size_t gzip_size = make_gzip(gzip, sizeof gzip, tar.bytes, tar.used);
    if (gzip_size == 0u) return false;
    memset(deb, 0, sizeof *deb);
    return append_bytes(deb->bytes, sizeof deb->bytes, &deb->size,
                        AR_MAGIC, sizeof AR_MAGIC) &&
           ar_member(deb, "debian-binary", VERSION, sizeof VERSION, NULL) &&
           ar_member(deb, "control.tar.gz", gzip, gzip_size, NULL) &&
           ar_member(deb, "data.tar.gz", gzip, gzip_size, &deb->data_header);
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

static void make_package(vm_guest_package_t *package,
                         char sha[VM_GUEST_PACKAGE_SHA256_HEX_SIZE],
                         char url[256], const deb_fixture_t *deb) {
    static const char NAME[] = "testpkg";
    static const char VERSION[] = "1-1";
    static const char FILENAME[] = "testpkg_1-1_iphoneos-arm.deb";
    digest_hex(deb->bytes, deb->size, sha);
    int written = snprintf(url, 256u,
        "https://apt.saurik.com/cydia-3.7/debs/%s", FILENAME);
    CHECK(written > 0 && written < 256, "fixture URL was truncated");
    package->package = NAME;
    package->version = VERSION;
    package->filename = FILENAME;
    package->source_url = url;
    package->size = deb->size;
    package->sha256_hex = sha;
    package->roles = VM_GUEST_PACKAGE_INSTALL | VM_GUEST_PACKAGE_FOUNDATION;
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
    char sha[VM_GUEST_PACKAGE_SHA256_HEX_SIZE];
    char url[256];
    make_package(&package, sha, url, &deb);
    vm_guest_package_input_t input = {&package, deb.bytes, deb.size};
    char detail[VM_GUEST_ROOTFS_DETAIL_CAPACITY];
    vm_guest_rootfs_status_t status = VM_GUEST_ROOTFS_ERR_ARGUMENT;
    vm_guest_rootfs_plan_t *plan = vm_guest_rootfs_plan_open(
        &input, 1u, &status, detail, sizeof detail);
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

    const rootfs_work_entry_t *script =
        find_entry(plan, VM_GUEST_ROOTFS_INSTALL_SCRIPT);
    const rootfs_work_entry_t *plist =
        find_entry(plan, VM_GUEST_ROOTFS_LAUNCHD_PLIST);
    CHECK(script && script->permissions == 0755u &&
          script->content_size != 0u &&
          strstr((const char *)script->content,
                 "testpkg_1-1_iphoneos-arm.deb") != NULL,
          "the first-boot script does not install the fixture package");
    CHECK(plist && plist->content_size != 0u &&
          strstr((const char *)plist->content,
                 "com.j0shua.s5lbox.guest-install") != NULL,
          "the launchd job is absent");

    vm_guest_rootfs_stats_t stats;
    vm_guest_rootfs_plan_get_stats(plan, &stats);
    CHECK(stats.packages == 1u && stats.foundation_packages == 1u &&
          stats.entries == vm_guest_rootfs_plan_entry_count(plan) &&
          stats.download_bytes == package.size && stats.files != 0u &&
          stats.directories != 0u && stats.symlinks == 1u,
          "unexpected plan stats: packages=%zu foundation=%zu entries=%zu",
          stats.packages, stats.foundation_packages, stats.entries);
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE];
    CHECK(vm_guest_rootfs_plan_manifest_sha256(plan, digest),
          "plan manifest identity was not produced");
    static const uint8_t EXPECTED[VM_GUEST_PACKAGE_SHA256_SIZE] = {
        0x20u, 0xe8u, 0x22u, 0x05u, 0x55u, 0x76u, 0xc3u, 0x35u,
        0x78u, 0x40u, 0xdfu, 0xaeu, 0xf8u, 0x4du, 0xadu, 0x20u,
        0xfbu, 0x3eu, 0x7bu, 0x7fu, 0x58u, 0xfdu, 0x0du, 0x7bu,
        0x2cu, 0xc0u, 0xe9u, 0xf3u, 0x0cu, 0xc5u, 0xc5u, 0x1eu
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
    CHECK(stats.packages == 28u && stats.foundation_packages == 14u,
          "real plan has %zu packages / %zu foundation packages",
          stats.packages, stats.foundation_packages);
    CHECK(stats.entries != 0u && stats.entries <= ROOTFS_WORK_MAX_ENTRIES,
          "real plan has an invalid entry count: %zu", stats.entries);
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
    test_real_packages_when_supplied();
    printf("== guest rootfs plan: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
