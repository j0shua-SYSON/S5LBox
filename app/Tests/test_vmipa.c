/* Synthetic archives only. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMIPAArchive.h"
#include "VMFirmwareFixtures.h"
#include "VMFirmwareTest.h"

static uint8_t archive[8192];
static size_t archive_size;
static size_t read_archive(void *ctx, uint64_t offset, uint8_t *out, size_t count) {
    (void)ctx;
    if (offset > archive_size || count > archive_size - offset) return 0;
    memcpy(out, archive + offset, count);
    return count;
}
static bool inspect(void) {
    vmfw_zip_t zip;
    vm_ipa_archive_t plan;
    char detail[256];
    if (vmfw_zip_open(&zip, read_archive, NULL, archive_size) != VMFW_ZIP_OK) return false;
    bool ok = vm_ipa_archive_open(&zip, &plan, detail, sizeof detail);
    if (ok) vm_ipa_archive_close(&plan);
    return ok;
}
static void macho(uint8_t *b, uint32_t subtype) {
    memset(b, 0, 64);
    fx_w32le(b, 0xfeedface); fx_w32le(b + 4, 12);
    fx_w32le(b + 8, subtype); fx_w32le(b + 12, 2);
}
int main(void) {
    vmfw_test_t state = {0}, *t = &state;
    char detail[256];
    VMFW_T_SECTION(t, "IPA paths and archive policy");
    const char *bad[] = {"", "/a", "../a", "a/../b", "a/./b", "a//b", "a\\b", "a:b", "a\001b"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        VMFW_T_CHECK(t, !vm_ipa_safe_path(bad[i]), "unsafe path accepted: %s", bad[i]);
    VMFW_T_CHECK(t, vm_ipa_safe_path("Payload/Test.app/data/"), "valid path refused");
    static const uint8_t content[] = "test";
    fx_zip_member_t members[3] = {
        {"Payload/Test.app/Info.plist", content, 4, false, false},
        {"Payload/Test.app/Test", content, 4, true, true},
        {"iTunesMetadata.plist", content, 4, false, false}
    };
    fx_zip_layout_t layout;
    archive_size = fx_zip_build(archive, sizeof archive, members, 3, &layout);
    VMFW_T_CHECK(t, inspect(), "valid bundle refused");
    fx_w16le(archive + layout.cd_offset + 4, (3u << 8) | 20u);
    fx_w32le(archive + layout.cd_offset + 38, 0120777u << 16);
    VMFW_T_CHECK(t, !inspect(), "symlink accepted");
    const char *conflicts[] = {"Payload/Test.app/info.plist", "Payload/Other.app/Info.plist", "Payload/Test.app/../oops"};
    for (size_t i = 0; i < sizeof conflicts / sizeof conflicts[0]; i++) {
        members[1].name = conflicts[i];
        archive_size = fx_zip_build(archive, sizeof archive, members, 3, &layout);
        VMFW_T_CHECK(t, !inspect(), "bad member accepted: %s", conflicts[i]);
    }
    members[1].name = "Payload/Test.app/Test";
    archive_size = fx_zip_build(archive, sizeof archive, members, 3, &layout);
    fx_w32le(archive + layout.cd_offset + 24, VM_IPA_MAX_FILE_BYTES + 1);
    VMFW_T_CHECK(t, !inspect(), "oversize file accepted");

    VMFW_T_SECTION(t, "Mach-O compatibility");
    uint8_t thin[64], fat[256];
    macho(thin, 6);
    VMFW_T_CHECK(t, vm_ipa_check_executable(thin, 28, detail, sizeof detail), "ARMv6 refused: %s", detail);
    fx_w32le(thin + 8, 9);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 28, detail, sizeof detail), "ARMv7 accepted");
    macho(thin, 6);
    fx_w32le(thin, 0xfeedfacf);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 28, detail, sizeof detail), "ARM64 accepted");
    macho(thin, 6);
    fx_w32le(thin + 16, 1); fx_w32le(thin + 20, 20);
    fx_w32le(thin + 28, 0x21); fx_w32le(thin + 32, 20); fx_w32le(thin + 44, 1);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 48, detail, sizeof detail), "encrypted executable accepted");
    fx_w32le(thin + 44, 0);
    VMFW_T_CHECK(t, vm_ipa_check_executable(thin, 48, detail, sizeof detail), "unencrypted executable refused");
    fx_w32le(thin + 20, 16); fx_w32le(thin + 28, 0x25); fx_w32le(thin + 32, 16);
    fx_w32le(thin + 36, 0x00040000);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 44, detail, sizeof detail), "iOS4 minimum accepted");
    fx_w32le(thin + 36, 0x00030103);
    VMFW_T_CHECK(t, vm_ipa_check_executable(thin, 44, detail, sizeof detail), "OS3 minimum refused");
    fx_w32le(thin + 32, 80);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 44, detail, sizeof detail), "command overflow accepted");
    memset(fat, 0, sizeof fat);
    fx_w32be(fat, 0xcafebabe); fx_w32be(fat + 4, 2);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *a = fat + 8 + i * 20;
        fx_w32be(a, 12); fx_w32be(a + 4, i ? 9 : 6);
        fx_w32be(a + 8, 64 + i * 64); fx_w32be(a + 12, 28);
        macho(fat + 64 + i * 64, i ? 9 : 6);
    }
    VMFW_T_CHECK(t, vm_ipa_check_executable(fat, sizeof fat, detail, sizeof detail), "valid universal refused");
    fx_w32be(fat + 36, 250);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(fat, sizeof fat, detail, sizeof detail), "bad later slice ignored");
    fx_w32be(fat + 36, 64);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(fat, sizeof fat, detail, sizeof detail), "overlapping slices accepted");
    fx_w32be(fat + 36, 128); fx_w32be(fat + 32, 6);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(fat, sizeof fat, detail, sizeof detail), "mismatched architecture accepted");
    printf("IPA inspection: %u checks, %u failures\n", t->checks, t->failures);
    return t->failures ? 1 : 0;
}
