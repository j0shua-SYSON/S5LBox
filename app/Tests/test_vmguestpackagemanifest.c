/* S5LBox -- pinned guest package manifest tests. */
#include "VMGuestPackageManifest.h"

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
    CHECK(vm_guest_package_count() == 28u, "package count is %zu, expected 28",
          vm_guest_package_count());
    CHECK(vm_guest_package_total_download_bytes() == UINT64_C(9105102),
          "download total is %llu, expected 9105102",
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
              (package->roles & VM_GUEST_PACKAGE_INSTALL) != 0u,
              "entry %zu is not installable", i);
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
    const vm_guest_package_t *apt = vm_guest_package_find("apt7-lib");
    const vm_guest_package_t *cydia = vm_guest_package_find("cydia");
    CHECK(dpkg && strcmp(dpkg->version, "1.14.25-8") == 0,
          "bootstrap dpkg revision drifted");
    CHECK(apt && strcmp(apt->version, "0.7.20.2-1") == 0,
          "apt revision no longer matches dpkg -8");
    CHECK(cydia && cydia->size == UINT64_C(629500) &&
          strcmp(cydia->sha256_hex,
                 "94769b67e88198012cd1e45163f2f8bd949b4aa927dab1503a03d62a8ee3dba9") == 0,
          "Cydia identity drifted");
    CHECK(vm_guest_package_find("mobilesubstrate") == NULL,
          "phase-one manifest unexpectedly installs MobileSubstrate");
}

static void test_entry_refusals(void) {
    const vm_guest_package_t *shipping = vm_guest_package_find("cydia");
    CHECK(shipping != NULL, "Cydia entry is missing");
    if (!shipping) return;
    vm_guest_package_t changed = *shipping;
    char why[256];

    changed.filename = "../cydia_1.0.3044-66_iphoneos-arm.deb";
    CHECK(!vm_guest_package_validate_entry(&changed, why, sizeof why),
          "path-bearing filename was accepted");

    changed = *shipping;
    changed.source_url =
        "http://apt.saurik.com/cydia-3.7/debs/cydia_1.0.3044-66_iphoneos-arm.deb";
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
        0x4cu, 0x6fu, 0x94u, 0xceu, 0x24u, 0x53u, 0x3cu, 0x20u,
        0x5du, 0x02u, 0xcfu, 0x4fu, 0x9cu, 0xbfu, 0x12u, 0x0eu,
        0x76u, 0x4du, 0xcfu, 0x14u, 0x1cu, 0xb8u, 0xedu, 0x37u,
        0x29u, 0xa1u, 0xd1u, 0xa8u, 0x11u, 0xe8u, 0xaau, 0xc3u
    };
    CHECK(memcmp(manifest, EXPECTED, sizeof manifest) == 0,
          "manifest identity changed");
    printf("manifest-sha256 ");
    for (size_t i = 0u; i < sizeof manifest; i++) printf("%02x", manifest[i]);
    printf("\n");
}

int main(void) {
    printf("== guest package manifest ==\n");
    test_shipping_manifest();
    test_entry_refusals();
    test_digest_and_download_match();
    printf("== guest package manifest: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
