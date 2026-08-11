/* See VMGuestPackageManifest.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMGuestPackageManifest.h"

#include "sha256.h"

#include <stdio.h>
#include <string.h>

#define PACKAGE_ARCHIVE_BASE "https://apt.saurik.com/cydia-3.7/debs/"
#define PACKAGE(PACKAGE, VERSION, FILENAME, SIZE, SHA256, ROLES) \
    { PACKAGE, VERSION, FILENAME, PACKAGE_ARCHIVE_BASE FILENAME, \
      UINT64_C(SIZE), SHA256, ROLES }

#define INSTALL VM_GUEST_PACKAGE_INSTALL
#define FOUNDATION (VM_GUEST_PACKAGE_INSTALL | VM_GUEST_PACKAGE_FOUNDATION)

/*
 * This is deliberately a compatibility set, not "newest versions as of some
 * date". dpkg 1.14.25-8 can bootstrap iPhone OS 3, while apt7-lib
 * 0.7.20.2-1 accepts that dpkg. Later apt7-lib revisions require dpkg -9.
 * Foundation entries use data.tar.gz so the host's bounded DEFLATE path can
 * seed dpkg and its tools. Guest dpkg then installs every original package,
 * runs its maintainer scripts, and owns the package database.
 */
static const vm_guest_package_t PACKAGES[] = {
    PACKAGE("apr-lib", "1.3.3-1",
            "apr-lib_1.3.3-1_iphoneos-arm.deb", 63820,
            "3353469dff2600810ceee66e885fb04264a16829b2aa4f147913aa0f40c14ad2",
            INSTALL),
    PACKAGE("apt7-key", "0.7.20.2-1",
            "apt7-key_0.7.20.2-1_iphoneos-arm.deb", 2528,
            "f8a7207c130cbf0853c0399e76cea97847d2a1198409b3c0c1847cffb9da5e4b",
            INSTALL),
    PACKAGE("apt7-lib", "0.7.20.2-1",
            "apt7-lib_0.7.20.2-1_iphoneos-arm.deb", 433484,
            "b8fb485c786e5a83abf4710778fb5c89bd9f6dcd9ced56fc50f363169f9132b7",
            INSTALL),
    PACKAGE("base", "1-3", "base_1-3_iphoneos-arm.deb", 1580,
            "8b000fc5e70efb177ae1e76ce927d0d0922e94c92a56097b3d1d609683c49e2f",
            INSTALL),
    PACKAGE("bash", "4.0.17-6", "bash_4.0.17-6_iphoneos-arm.deb", 354070,
            "0b5f348f3d871a269970217405e1d5c24e5256831ab456e5d010d8ecc4949cfd",
            FOUNDATION),
    PACKAGE("bzip2", "1.0.5-7", "bzip2_1.0.5-7_iphoneos-arm.deb", 22158,
            "4203a21737a3703cef93b75a13711ca1a9a031a153aecb6b9d0935a6c7531b91",
            FOUNDATION),
    PACKAGE("coreutils", "7.2-8", "coreutils_7.2-8_iphoneos-arm.deb", 3579518,
            "f412a75d43631397b371fc762877904cb70b8e2fed9a8315df1ddc86d6749d1b",
            FOUNDATION),
    PACKAGE("coreutils-bin", "7.2-1",
            "coreutils-bin_7.2-1_iphoneos-arm.deb", 1582692,
            "57b888b424828d799315f91e958f24b4c316f71c75f8f42d30f03c91de747f78",
            FOUNDATION),
    PACKAGE("cydia", "1.0.3044-66",
            "cydia_1.0.3044-66_iphoneos-arm.deb", 629500,
            "94769b67e88198012cd1e45163f2f8bd949b4aa927dab1503a03d62a8ee3dba9",
            INSTALL),
    PACKAGE("darwintools", "1-4", "darwintools_1-4_iphoneos-arm.deb", 5004,
            "a944bb1236935e4df3a6cb6e3fbe1ad179336fbe4ffa1c05684a851f45fe7fd3",
            INSTALL),
    PACKAGE("diffutils", "2.8.1-3",
            "diffutils_2.8.1-3_iphoneos-arm.deb", 110376,
            "cadba3f970701333df46b9990773a79905da5f51b77310cfd398375e8b14111b",
            FOUNDATION),
    PACKAGE("dpkg", "1.14.25-8", "dpkg_1.14.25-8_iphoneos-arm.deb", 488966,
            "58e123ead9572335b7c23bbb496a2d4c7a3bbfbc6d0f835fe2f3e23078253260",
            FOUNDATION),
    PACKAGE("essential", "0-1", "essential_0-1_iphoneos-arm.deb", 694,
            "1fd7b099f9022a783f5b62ebfd4d537158016b8ca3e244f96847abf5b89d5f68",
            INSTALL),
    PACKAGE("findutils", "4.2.33-3",
            "findutils_4.2.33-3_iphoneos-arm.deb", 226318,
            "68cf549ca06ec445338133e7871f056c16024cf6cd8b35401004978d82cae775",
            FOUNDATION),
    PACKAGE("grep", "2.5.4-3", "grep_2.5.4-3_iphoneos-arm.deb", 84338,
            "d518772e85b0c2438371913de18678b60fc8c5f999aa3e16ed639b05245cf618",
            INSTALL),
    PACKAGE("gzip", "1.3.12-6", "gzip_1.3.12-6_iphoneos-arm.deb", 60732,
            "f05d5d7d2db4283827173298abfc7dec9949c69c9d20e6c95c445c5e3385b4df",
            FOUNDATION),
    PACKAGE("lzma", "4.32.7-1", "lzma_4.32.7-1_iphoneos-arm.deb", 91862,
            "5964264d9dd4132e6059dcafd550b1948110a72e9dbb20fa5d48cef4ead0cbf6",
            FOUNDATION),
    PACKAGE("ncurses", "5.7-10", "ncurses_5.7-10_iphoneos-arm.deb", 635826,
            "6099596845693d3d7696f4467a145d7bd3217707e809923fcdb019f49796de68",
            FOUNDATION),
    PACKAGE("pam", "32.1-3", "pam_32.1-3_iphoneos-arm.deb", 54076,
            "79fbd483cd19d195185dce11daf78ea0be6332eec5f1ee2dedd1042800f5d191",
            INSTALL),
    PACKAGE("pam-modules", "36.1-4",
            "pam-modules_36.1-4_iphoneos-arm.deb", 5288,
            "4e670b3735d0caf2bed15e6e274e6df0c67d806b2fdb143f8f23db2d2cf733be",
            INSTALL),
    PACKAGE("pcre", "7.9-3", "pcre_7.9-3_iphoneos-arm.deb", 104726,
            "821c5feb1cafee979129be6a7db3d8b063b6ebc2c0dd1cf076193f200a43f74f",
            INSTALL),
    PACKAGE("profile.d", "0-1", "profile.d_0-1_iphoneos-arm.deb", 906,
            "95c3a1537fe2cb1fca01d91f23a5681509e1ac9d5dd5ce950a1bd3c9137290a4",
            FOUNDATION),
    PACKAGE("readline", "6.0-6", "readline_6.0-6_iphoneos-arm.deb", 137154,
            "6b9f35825e372b28e2d0d8cac83207b4a2b1d6fe742d768fd38d67b34927bfac",
            FOUNDATION),
    PACKAGE("sed", "4.1.5-6", "sed_4.1.5-6_iphoneos-arm.deb", 88346,
            "ce52c9213e7c44bad7adcc8f33ccef71fa4879547d0346a424b20873d0a2b618",
            FOUNDATION),
    PACKAGE("shell-cmds", "118-6",
            "shell-cmds_118-6_iphoneos-arm.deb", 10022,
            "1325f1068c1d83ad44ae5282d907416027f8398d1299263fca82c8188a050ef4",
            INSTALL),
    PACKAGE("system-cmds", "433.4-11",
            "system-cmds_433.4-11_iphoneos-arm.deb", 91874,
            "41b3de2f9840223877715b30d984e4ac637569ce36f7e4a41023826661d6ba29",
            INSTALL),
    PACKAGE("tar", "1.19-5", "tar_1.19-5_iphoneos-arm.deb", 231856,
            "32fa385e2f233956c60c4eeb10539548d91ab2cd7f455402d5ccb1ba21ffb02f",
            FOUNDATION),
    PACKAGE("uikittools", "1.0.2995-1",
            "uikittools_1.0.2995-1_iphoneos-arm.deb", 7388,
            "19dbb574f09e20a8f3eddad1f3c4736ac98b2a4690216f3c6bbd6dafbc7c6f65",
            INSTALL),
};

#undef FOUNDATION
#undef INSTALL
#undef PACKAGE

static const size_t PACKAGE_COUNT = sizeof PACKAGES / sizeof PACKAGES[0];

static void package_reason(char *why, size_t capacity,
                           const char *package, const char *message) {
    if (!why || capacity == 0u) return;
    if (package)
        (void)snprintf(why, capacity, "%s %s", package, message);
    else
        (void)snprintf(why, capacity, "%s", message);
    why[capacity - 1u] = '\0';
}

static bool package_token(const char *text, bool package_name) {
    if (!text || !*text) return false;
    size_t length = strlen(text);
    if (length > 63u) return false;
    for (size_t i = 0u; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        bool alpha = c >= (unsigned char)(package_name ? 'a' : 'A') &&
                     c <= (unsigned char)(package_name ? 'z' : 'Z');
        if (!package_name)
            alpha = alpha || (c >= (unsigned char)'a' && c <= (unsigned char)'z');
        bool digit = c >= (unsigned char)'0' && c <= (unsigned char)'9';
        if (!alpha && !digit && c != '.' && c != '+' && c != '-' &&
            (!package_name && c != ':' && c != '~'))
            return false;
    }
    return true;
}

static int package_hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool vm_guest_package_validate_entry(const vm_guest_package_t *package,
                                     char *why, size_t why_capacity) {
    if (why && why_capacity) why[0] = '\0';
    if (!package) {
        package_reason(why, why_capacity, NULL, "package record is missing");
        return false;
    }
    if (!package_token(package->package, true) ||
        !package_token(package->version, false)) {
        package_reason(why, why_capacity, NULL, "invalid package/version token");
        return false;
    }
    if (!package->filename || !package->source_url || !package->sha256_hex ||
        package->size == 0u) {
        package_reason(why, why_capacity, package->package,
                       "has incomplete metadata");
        return false;
    }

    char expected_filename[192];
    int filename_length = snprintf(expected_filename, sizeof expected_filename,
                                   "%s_%s_iphoneos-arm.deb",
                                   package->package, package->version);
    if (filename_length <= 0 ||
        (size_t)filename_length >= sizeof expected_filename ||
        strcmp(package->filename, expected_filename) != 0) {
        package_reason(why, why_capacity, package->package,
                       "has a mismatched filename");
        return false;
    }

    size_t base_length = strlen(PACKAGE_ARCHIVE_BASE);
    if (strncmp(package->source_url, PACKAGE_ARCHIVE_BASE, base_length) != 0 ||
        strcmp(package->source_url + base_length, package->filename) != 0) {
        package_reason(why, why_capacity, package->package,
                       "is not on the pinned HTTPS archive");
        return false;
    }
    if (strlen(package->sha256_hex) != VM_GUEST_PACKAGE_SHA256_HEX_SIZE - 1u) {
        package_reason(why, why_capacity, package->package,
                       "has a non-64-byte digest");
        return false;
    }
    for (size_t i = 0u; i < VM_GUEST_PACKAGE_SHA256_HEX_SIZE - 1u; i++) {
        if (package_hex_value(package->sha256_hex[i]) < 0) {
            package_reason(why, why_capacity, package->package,
                           "has a non-lowercase digest");
            return false;
        }
    }
    const uint32_t allowed = VM_GUEST_PACKAGE_INSTALL |
                             VM_GUEST_PACKAGE_FOUNDATION;
    if ((package->roles & VM_GUEST_PACKAGE_INSTALL) == 0u ||
        (package->roles & ~allowed) != 0u) {
        package_reason(why, why_capacity, package->package,
                       "has invalid install roles");
        return false;
    }
    return true;
}

size_t vm_guest_package_count(void) {
    return PACKAGE_COUNT;
}

const vm_guest_package_t *vm_guest_package_at(size_t index) {
    return index < PACKAGE_COUNT ? &PACKAGES[index] : NULL;
}

const vm_guest_package_t *vm_guest_package_find(const char *package) {
    if (!package) return NULL;
    for (size_t i = 0u; i < PACKAGE_COUNT; i++) {
        if (strcmp(PACKAGES[i].package, package) == 0) return &PACKAGES[i];
    }
    return NULL;
}

bool vm_guest_package_manifest_validate(char *why, size_t why_capacity) {
    if (why && why_capacity) why[0] = '\0';
    if (PACKAGE_COUNT == 0u) {
        package_reason(why, why_capacity, NULL, "package manifest is empty");
        return false;
    }
    for (size_t i = 0u; i < PACKAGE_COUNT; i++) {
        if (!vm_guest_package_validate_entry(&PACKAGES[i], why, why_capacity))
            return false;
        for (size_t j = 0u; j < i; j++) {
            if (strcmp(PACKAGES[i].package, PACKAGES[j].package) == 0 ||
                strcmp(PACKAGES[i].filename, PACKAGES[j].filename) == 0) {
                package_reason(why, why_capacity, PACKAGES[i].package,
                               "is duplicated in the manifest");
                return false;
            }
        }
    }
    return true;
}

bool vm_guest_package_expected_sha256(
    const vm_guest_package_t *package,
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE]) {
    if (digest) memset(digest, 0, VM_GUEST_PACKAGE_SHA256_SIZE);
    if (!package || !digest || !package->sha256_hex ||
        strlen(package->sha256_hex) != VM_GUEST_PACKAGE_SHA256_HEX_SIZE - 1u)
        return false;
    uint8_t parsed[VM_GUEST_PACKAGE_SHA256_SIZE];
    for (size_t i = 0u; i < VM_GUEST_PACKAGE_SHA256_SIZE; i++) {
        int high = package_hex_value(package->sha256_hex[i * 2u]);
        int low = package_hex_value(package->sha256_hex[i * 2u + 1u]);
        if (high < 0 || low < 0) return false;
        parsed[i] = (uint8_t)((unsigned)high << 4u | (unsigned)low);
    }
    memcpy(digest, parsed, sizeof parsed);
    return true;
}

bool vm_guest_package_download_matches(
    const vm_guest_package_t *package, uint64_t size,
    const uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE]) {
    if (!package || !digest || size != package->size) return false;
    uint8_t expected[VM_GUEST_PACKAGE_SHA256_SIZE];
    return vm_guest_package_expected_sha256(package, expected) &&
           memcmp(expected, digest, sizeof expected) == 0;
}

static bool package_hash_text(ios3_sha256_context_t *context,
                              const char *text) {
    return text && ios3_sha256_update(context, text, strlen(text));
}

bool vm_guest_package_manifest_sha256(
    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE]) {
    if (digest) memset(digest, 0, VM_GUEST_PACKAGE_SHA256_SIZE);
    if (!digest || !vm_guest_package_manifest_validate(NULL, 0u)) return false;

    ios3_sha256_context_t context;
    if (!ios3_sha256_init(&context) ||
        !package_hash_text(&context, "s5lbox-guest-package-manifest 1\n"))
        return false;
    for (size_t i = 0u; i < PACKAGE_COUNT; i++) {
        const vm_guest_package_t *package = &PACKAGES[i];
        const char *fields[] = {
            package->package, package->version, package->filename,
            package->source_url, package->sha256_hex
        };
        for (size_t field = 0u; field < sizeof fields / sizeof fields[0]; field++) {
            if (!package_hash_text(&context, fields[field]) ||
                !package_hash_text(&context, "\t"))
                return false;
        }
        char ending[64];
        int length = snprintf(ending, sizeof ending, "%llu\t%u\n",
                              (unsigned long long)package->size,
                              (unsigned)package->roles);
        if (length <= 0 || (size_t)length >= sizeof ending ||
            !ios3_sha256_update(&context, ending, (size_t)length))
            return false;
    }
    return ios3_sha256_final(&context, digest);
}

uint64_t vm_guest_package_total_download_bytes(void) {
    uint64_t total = 0u;
    for (size_t i = 0u; i < PACKAGE_COUNT; i++) {
        if (UINT64_MAX - total < PACKAGES[i].size) return 0u;
        total += PACKAGES[i].size;
    }
    return total;
}
