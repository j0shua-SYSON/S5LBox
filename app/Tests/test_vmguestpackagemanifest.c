/* S5LBox -- pinned guest package manifest tests. */
#include "VMGuestPackageManifest.h"
#include "VMGuestPackageFile.h"

#include <errno.h>
#include <stdio.h>
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

static void test_shipping_manifest(void) {
    char why[256];
    CHECK(vm_guest_package_manifest_validate(why, sizeof why),
          "shipping manifest refused: %s", why);
    CHECK(vm_guest_package_count() == 30u, "package count is %zu, expected 30",
          vm_guest_package_count());
    CHECK(vm_guest_package_total_download_bytes() == UINT64_C(10245014),
          "download total is %llu, expected 10245014",
          (unsigned long long)vm_guest_package_total_download_bytes());
    CHECK(vm_guest_package_at(vm_guest_package_count()) == NULL,
          "out-of-range package lookup succeeded");
    CHECK(vm_guest_package_find(NULL) == NULL &&
          vm_guest_package_find("not-a-package") == NULL,
          "missing package lookup succeeded");

    size_t foundation = 0u;
    uint64_t foundation_bytes = 0u;
    for (size_t i = 0u; i < vm_guest_package_count(); i++) {
        const vm_guest_package_t *package = vm_guest_package_at(i);
        CHECK(package != NULL &&
              (((package->roles & VM_GUEST_PACKAGE_INSTALL) != 0u) !=
               ((package->roles &
                 VM_GUEST_PACKAGE_APT_CACHE_TOOL) != 0u)),
              "entry %zu has no unique provisioning role", i);
        if (package &&
            (package->roles & VM_GUEST_PACKAGE_FOUNDATION) != 0u) {
            foundation++;
            foundation_bytes += package->size;
        }
    }
    CHECK(foundation == 14u && foundation_bytes == UINT64_C(7610780),
          "foundation is %zu packages / %llu bytes", foundation,
          (unsigned long long)foundation_bytes);

    const vm_guest_package_t *dpkg = vm_guest_package_find("dpkg");
    const vm_guest_package_t *apt_tool = vm_guest_package_find("apt7");
    const vm_guest_package_t *apt = vm_guest_package_find("apt7-lib");
    const vm_guest_package_t *cydia = vm_guest_package_find("cydia");
    const vm_guest_package_t *gnupg = vm_guest_package_find("gnupg");
    CHECK(dpkg && strcmp(dpkg->version, "1.14.25-8") == 0,
          "bootstrap dpkg revision drifted");
    CHECK(apt && strcmp(apt->version, "0.7.20.2-1") == 0,
          "apt revision no longer matches dpkg -8");
    CHECK(apt_tool && strcmp(apt_tool->version, "0.7.20.2-1") == 0 &&
          apt_tool->roles == VM_GUEST_PACKAGE_APT_CACHE_TOOL &&
          apt_tool->size == UINT64_C(664620) &&
          strcmp(apt_tool->sha256_hex,
                 "3864ac7542ff2c28bc0e4915c0781d8a680ae557c6150563831f476e701aa1a8") == 0,
          "the ABI-matched APT cache-tool identity drifted");
    CHECK(cydia && strcmp(cydia->version, "1.0.3172-68") == 0 &&
          cydia->size == UINT64_C(634106) &&
          strcmp(cydia->sha256_hex,
                 "6d3688421873a976b37ebe0af1c467da2add4dee6876b1aba50fa78ae7ab3ecb") == 0,
          "Cydia package-name encoding fix identity drifted");
    CHECK(gnupg && strcmp(gnupg->version, "1.4.8-4") == 0 &&
          gnupg->size == UINT64_C(470686) &&
          strcmp(gnupg->sha256_hex,
                 "9ead71f65ad62e95b31cc821dd6e7b6a3dbd930a7a49e96f280a2988288f5187") == 0,
          "legacy APT signature-verifier identity drifted");
    CHECK(vm_guest_package_find("mobilesubstrate") == NULL,
          "phase-one manifest unexpectedly installs MobileSubstrate");
}

static void test_entry_refusals(void) {
    const vm_guest_package_t *shipping = vm_guest_package_find("cydia");
    CHECK(shipping != NULL, "Cydia entry is missing");
    if (!shipping) return;
    vm_guest_package_t changed = *shipping;
    char why[256];

    changed.filename = "../cydia_1.0.3172-68_iphoneos-arm.deb";
    CHECK(!vm_guest_package_validate_entry(&changed, why, sizeof why),
          "path-bearing filename was accepted");

    changed = *shipping;
    changed.source_url =
        "http://apt.saurik.com/cydia-3.7/debs/cydia_1.0.3172-68_iphoneos-arm.deb";
    CHECK(!vm_guest_package_validate_entry(&changed, why, sizeof why),
          "non-HTTPS source was accepted");

    changed = *shipping;
    char uppercase[VM_GUEST_PACKAGE_SHA256_HEX_SIZE];
    memcpy(uppercase, shipping->sha256_hex, sizeof uppercase);
    uppercase[0] = 'A';
    changed.sha256_hex = uppercase;
    CHECK(!vm_guest_package_validate_entry(&changed, why, sizeof why),
          "uppercase digest was accepted");

    changed = *shipping;
    changed.roles = VM_GUEST_PACKAGE_FOUNDATION;
    CHECK(!vm_guest_package_validate_entry(&changed, why, sizeof why),
          "foundation without install role was accepted");

    changed = *shipping;
    changed.roles = VM_GUEST_PACKAGE_APT_CACHE_TOOL;
    CHECK(vm_guest_package_validate_entry(&changed, why, sizeof why),
          "a tool-only authenticated archive was refused: %s", why);
    changed.roles = VM_GUEST_PACKAGE_INSTALL |
                    VM_GUEST_PACKAGE_APT_CACHE_TOOL;
    CHECK(!vm_guest_package_validate_entry(&changed, why, sizeof why),
          "one archive was allowed to be both installed and tool-only");
    changed.roles = VM_GUEST_PACKAGE_FOUNDATION |
                    VM_GUEST_PACKAGE_APT_CACHE_TOOL;
    CHECK(!vm_guest_package_validate_entry(&changed, why, sizeof why),
          "a tool-only archive was allowed to seed the foundation");

    changed = *shipping;
    changed.size = 0u;
    CHECK(!vm_guest_package_validate_entry(&changed, why, sizeof why),
          "zero-byte package was accepted");
}

static void test_digest_and_download_match(void) {
    const vm_guest_package_t *cydia = vm_guest_package_find("cydia");
    CHECK(cydia != NULL, "Cydia entry is missing");
    if (!cydia) return;
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE];
    CHECK(vm_guest_package_expected_sha256(cydia, digest),
          "Cydia digest did not decode");
    CHECK(vm_guest_package_download_matches(cydia, cydia->size, digest),
          "exact Cydia evidence did not match");
    CHECK(!vm_guest_package_download_matches(cydia, cydia->size - 1u, digest),
          "wrong Cydia size matched");
    digest[31] ^= 1u;
    CHECK(!vm_guest_package_download_matches(cydia, cydia->size, digest),
          "wrong Cydia digest matched");

    uint8_t manifest[VM_GUEST_PACKAGE_SHA256_SIZE];
    CHECK(vm_guest_package_manifest_sha256(manifest),
          "manifest digest failed");
    static const uint8_t EXPECTED[VM_GUEST_PACKAGE_SHA256_SIZE] = {
        0x32u, 0xa4u, 0xd5u, 0x1fu, 0xc5u, 0xedu, 0xe2u, 0xa6u,
        0xcfu, 0xbdu, 0x0fu, 0x98u, 0xaau, 0x9eu, 0xe3u, 0x26u,
        0x23u, 0xa3u, 0x69u, 0x1du, 0x0au, 0xf9u, 0xa2u, 0x19u,
        0xcdu, 0x6fu, 0x5au, 0x4cu, 0x19u, 0x13u, 0xd4u, 0xa8u
    };
    CHECK(memcmp(manifest, EXPECTED, sizeof manifest) == 0,
          "manifest identity changed");
    printf("manifest-sha256 ");
    for (size_t i = 0u; i < sizeof manifest; i++) printf("%02x", manifest[i]);
    printf("\n");
}

static void test_downloaded_file_gate(void) {
    static const char PATH[] = "vmguestpackagefile-fixture";
    static const char ABC_SHA256[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    vm_guest_package_t package = {
        "fixture", "1", "fixture_1_iphoneos-arm.deb",
        "https://example.invalid/fixture_1_iphoneos-arm.deb",
        UINT64_C(3), ABC_SHA256, VM_GUEST_PACKAGE_INSTALL
    };
    (void)remove(PATH);
    FILE *file = fopen(PATH, "wb");
    bool written = false;
    if (file) {
        written = fwrite("abc", 1u, 3u, file) == 3u;
        if (fclose(file) != 0) written = false;
    }
    CHECK(written,
          "could not create package-file fixture: %s", strerror(errno));
    if (!written) {
        (void)remove(PATH);
        return;
    }

    uint64_t size = 0u;
    char detail[160];
    CHECK(vm_guest_package_file_verify(&package, PATH, &size,
                                       detail, sizeof detail) ==
              VM_GUEST_PACKAGE_FILE_OK && size == 3u,
          "exact downloaded file refused: %s", detail);

    package.size = 4u;
    CHECK(vm_guest_package_file_verify(&package, PATH, &size,
                                       detail, sizeof detail) ==
              VM_GUEST_PACKAGE_FILE_ERR_SIZE,
          "wrong downloaded size was accepted");
    package.size = 3u;
    package.sha256_hex =
        "aa7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK(vm_guest_package_file_verify(&package, PATH, &size,
                                       detail, sizeof detail) ==
              VM_GUEST_PACKAGE_FILE_ERR_DIGEST,
          "wrong downloaded digest was accepted");
    CHECK(vm_guest_package_file_verify(&package, "does-not-exist", &size,
                                       detail, sizeof detail) ==
              VM_GUEST_PACKAGE_FILE_ERR_OPEN,
          "missing downloaded file was accepted");
    (void)remove(PATH);
}

int main(void) {
    printf("== guest package manifest ==\n");
    test_shipping_manifest();
    test_entry_refusals();
    test_digest_and_download_match();
    test_downloaded_file_gate();
    printf("== guest package manifest: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
