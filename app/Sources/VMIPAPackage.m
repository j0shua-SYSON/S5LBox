// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMIPAPackage.h"
#import "VMIPAArchive.h"
#import "VMGuestInstall.h"
#import "VMGuest.h"
#import "VMResumeCheckpoint.h"
#import "VMSnapshotStore.h"
#include "rootfs_work.h"
#include "sha256.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static BOOL IPAError(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"com.j0shua.S5LBox.IPA"
        code:1 userInfo:@{NSLocalizedDescriptionKey:message ?: @"The IPA could not be installed."}];
    return NO;
}
static size_t IPAPread(void *ctx, uint64_t offset, uint8_t *bytes, size_t length) {
    ssize_t got = pread(*(int *)ctx, bytes, length, (off_t)offset);
    return got < 0 ? 0 : (size_t)got;
}
static bool IPASink(void *ctx, const uint8_t *bytes, size_t size) {
    return fwrite(bytes, 1, size, (FILE *)ctx) == size;
}
static BOOL IPAString(id value) { return [value isKindOfClass:[NSString class]] && [value length] > 0; }
static BOOL IPAVersionSupported(id version) {
    if (!version) return YES; // Early iPhone OS apps sometimes omit this key.
    if (!IPAString(version)) return NO;
    NSArray *parts = [version componentsSeparatedByString:@"."];
    if (parts.count > 3) return NO;
    unsigned result = 0;
    NSCharacterSet *digits = [NSCharacterSet characterSetWithCharactersInString:@"0123456789"];
    for (NSUInteger i = 0; i < 3; i++) {
        NSString *part = i < parts.count ? parts[i] : @"0";
        if (!part.length || [part rangeOfCharacterFromSet:digits.invertedSet].location != NSNotFound ||
            part.length > 3 || part.integerValue > 255) return NO;
        result = (result << 8) | (unsigned)part.integerValue;
    }
    return result <= 0x030103u;
}

@interface VMIPAPackage ()
- (BOOL)loadURL:(NSURL *)url error:(NSError **)error;
@end

@implementation VMIPAPackage {
    vm_ipa_archive_t _plan;
    NSData *_packed;
    NSMutableArray<NSNumber *> *_offsets;
    NSURL *_stagingDirectory;
    NSString *_executable;
    NSString *_slug;
    uint8_t _identity[32];
}

- (instancetype)initWithURL:(NSURL *)url error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    if (!url.isFileURL) { IPAError(error, @"Choose an IPA file from Files."); return nil; }
    BOOL scoped = [url startAccessingSecurityScopedResource];
    __block BOOL loaded = NO;
    __block NSError *loadError = nil;
    NSError *coordinateError = nil;
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateReadingItemAtURL:url options:NSFileCoordinatorReadingWithoutChanges
        error:&coordinateError byAccessor:^(NSURL *readable) {
            loaded = [self loadURL:readable error:&loadError];
        }];
    if (scoped) [url stopAccessingSecurityScopedResource];
    if (!loaded || coordinateError) {
        if (error) *error = coordinateError ?: loadError;
        if (error && !*error) IPAError(error, @"The file provider did not make this file available. Download it in Files and choose it again.");
        return nil;
    }
    return self;
}

- (BOOL)loadURL:(NSURL *)url error:(NSError **)error {
    int fd = open(url.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) return IPAError(error, @"The selected file could not be opened. Download it in Files and choose it again.");
    BOOL success = NO;
    FILE *packed = NULL;
    @try {
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
            (uint64_t)st.st_size > 512u * 1024u * 1024u)
            return IPAError(error, @"Choose a regular IPA file no larger than 512 MB.");
        vmfw_zip_t zip;
        vmfw_zip_status_t status = vmfw_zip_open(&zip, IPAPread, &fd, (uint64_t)st.st_size);
        char detail[320] = {0};
        if (status != VMFW_ZIP_OK) return IPAError(error, [NSString stringWithUTF8String:vmfw_zip_strerror(status)]);
        if (!vm_ipa_archive_open(&zip, &_plan, detail, sizeof detail))
            return IPAError(error, [NSString stringWithUTF8String:detail]);
        if (_plan.files[_plan.info_index].uncompressed_size > 1024u * 1024u)
            return IPAError(error, @"This app's Info.plist is too large.");
        NSURL *cache = [[NSFileManager defaultManager] URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
        _stagingDirectory = [cache URLByAppendingPathComponent:
            [@"IPA-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
        if (![[NSFileManager defaultManager] createDirectoryAtURL:_stagingDirectory
            withIntermediateDirectories:NO attributes:nil error:error]) return NO;
        NSURL *packedURL = [_stagingDirectory URLByAppendingPathComponent:@"contents.bin"];
        packed = fopen(packedURL.fileSystemRepresentation, "wb");
        if (!packed) return IPAError(error, @"Not enough host storage to unpack this IPA.");
        _offsets = [NSMutableArray arrayWithCapacity:_plan.count];
        uint64_t offset = 0;
        for (unsigned i = 0; i < _plan.count; i++) {
            [_offsets addObject:@(offset)];
            if (_plan.files[i].is_directory) continue;
            status = vmfw_zip_extract(&zip, &_plan.files[i], IPASink, packed);
            if (status != VMFW_ZIP_OK) return IPAError(error, [NSString stringWithUTF8String:vmfw_zip_strerror(status)]);
            offset += _plan.files[i].uncompressed_size;
        }
        int closeStatus = fclose(packed);
        packed = NULL;
        if (closeStatus) return IPAError(error, @"The unpacked IPA could not be saved. Check host free space.");
        _packed = [NSData dataWithContentsOfURL:packedURL options:NSDataReadingMappedAlways error:error];
        if (!_packed || _packed.length != offset) return NO;
        const uint8_t *bytes = _packed.bytes;
        NSData *infoData = [NSData dataWithBytes:bytes + _offsets[_plan.info_index].unsignedLongLongValue
            length:(NSUInteger)_plan.files[_plan.info_index].uncompressed_size];
        id parsed = [NSPropertyListSerialization propertyListWithData:infoData
            options:NSPropertyListImmutable format:NULL error:error];
        if (![parsed isKindOfClass:[NSDictionary class]])
            return IPAError(error, @"The app does not contain a valid Info.plist dictionary.");
        NSDictionary *info = parsed;
        if (!IPAString(info[@"CFBundleIdentifier"]) || !IPAString(info[@"CFBundleExecutable"]) ||
            ![info[@"CFBundlePackageType"] isEqual:@"APPL"])
            return IPAError(error, @"The app is missing its identifier, executable, or APPL bundle type.");
        _bundleIdentifier = [info[@"CFBundleIdentifier"] copy];
        NSCharacterSet *identifierChars = [NSCharacterSet characterSetWithCharactersInString:
            @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"];
        if (_bundleIdentifier.length > 200 || [_bundleIdentifier hasPrefix:@"com.apple."] ||
            [_bundleIdentifier rangeOfCharacterFromSet:identifierChars.invertedSet].location != NSNotFound)
            return IPAError(error, @"This app has an unsupported or reserved bundle identifier.");
        _executable = [info[@"CFBundleExecutable"] copy];
        if (!vm_ipa_safe_path(_executable.UTF8String) || [_executable containsString:@"/"])
            return IPAError(error, @"This app has an unsafe executable name.");
        if (!IPAVersionSupported(info[@"MinimumOSVersion"]))
            return IPAError(error, @"This app requires a newer iOS version than 3.1.3.");
        id platforms = info[@"CFBundleSupportedPlatforms"];
        if (platforms && (![platforms isKindOfClass:[NSArray class]] || ![platforms containsObject:@"iPhoneOS"]))
            return IPAError(error, @"This app was not built for iPhone OS.");
        id capabilities = info[@"UIRequiredDeviceCapabilities"];
        id requiredARMv7 = [capabilities isKindOfClass:[NSDictionary class]] ? capabilities[@"armv7"] : nil;
        if (requiredARMv7 && ![requiredARMv7 isKindOfClass:[NSNumber class]])
            return IPAError(error, @"The app has an invalid device-capabilities list.");
        if (([capabilities isKindOfClass:[NSArray class]] && [capabilities containsObject:@"armv7"]) ||
            [requiredARMv7 boolValue])
            return IPAError(error, @"This app requires ARMv7. The guest emulates an ARMv6 iPhone 3G.");
        id families = info[@"UIDeviceFamily"];
        if (families && (![families isKindOfClass:[NSArray class]] || ![families containsObject:@1]))
            return IPAError(error, @"This app does not support iPhone.");
        NSString *mainPath = [[NSString stringWithUTF8String:_plan.app_root] stringByAppendingPathComponent:_executable];
        BOOL found = NO;
        for (unsigned i = 0; i < _plan.count; i++) {
            if (strcmp(_plan.files[i].name, mainPath.UTF8String)) continue;
            found = YES;
            if (!vm_ipa_check_executable(bytes + _offsets[i].unsignedLongLongValue,
                (size_t)_plan.files[i].uncompressed_size, detail, sizeof detail))
                return IPAError(error, [NSString stringWithUTF8String:detail]);
        }
        if (!found) return IPAError(error, @"The executable named by Info.plist is missing.");
        id name = info[@"CFBundleDisplayName"] ?: info[@"CFBundleName"];
        _displayName = IPAString(name) ? [name copy] : _bundleIdentifier;
        uint8_t identifierHash[32];
        NSData *identifier = [_bundleIdentifier dataUsingEncoding:NSUTF8StringEncoding];
        ios3_sha256(identifier.bytes, identifier.length, identifierHash);
        NSMutableString *slug = [NSMutableString stringWithString:@"s5lbox-"];
        for (unsigned i = 0; i < 16; i++) [slug appendFormat:@"%02x", identifierHash[i]];
        _slug = slug;
        ios3_sha256_context_t hash;
        ios3_sha256_init(&hash);
        ios3_sha256_update(&hash, _packed.bytes, _packed.length);
        for (unsigned i = 0; i < _plan.count; i++)
            ios3_sha256_update(&hash, _plan.files[i].name, strlen(_plan.files[i].name) + 1);
        ios3_sha256_final(&hash, _identity);
        success = YES;
    } @finally {
        if (packed) fclose(packed);
        close(fd);
    }
    return success;
}

- (void)dealloc {
    vm_ipa_archive_close(&_plan);
    if (_stagingDirectory) [[NSFileManager defaultManager] removeItemAtURL:_stagingDirectory error:NULL];
}

- (BOOL)installInDirectory:(NSString *)directory error:(NSError **)error {
    const char *work = directory.fileSystemRepresentation;
    char detail[320] = {0}, stage[VM_GUEST_INSTALL_PATH_CAPACITY], snapshots[VM_GUEST_INSTALL_PATH_CAPACITY];
    uint8_t jailbreakIdentity[32];
    if (vm_guest_install_probe(work, jailbreakIdentity, detail, sizeof detail) != VM_GUEST_INSTALL_PROBE_VALID)
        return IPAError(error, @"Jailbreak this guest from App Settings before installing an IPA.");
    if (vm_guest_maintenance_recover(work, NULL, NULL, NULL, detail, sizeof detail) != VM_GUEST_INSTALL_OK)
        return IPAError(error, [NSString stringWithUTF8String:detail]);
    NSString *live = [directory stringByAppendingPathComponent:@"rootfs-work.img"];
    uint64_t size = [[[NSFileManager defaultManager] attributesOfItemAtPath:live error:NULL] fileSize];
    if (!size || vm_resume_checkpoint_probe_state(work, size, VM_GUEST_RAM_BASE,
        VM_GUEST_RAM_SIZE, detail, sizeof detail) != VM_RESUME_CHECKPOINT_POWERED_OFF)
        return IPAError(error, @"The guest is not verified powered off. Shut it down and try again.");
    vm_snapshot_info_t snapshotItems[VM_SNAPSHOT_MAX];
    size_t snapshotCount = 0;
    if (vm_snapshot_dir(work, snapshots, sizeof snapshots) != VM_SNAPSHOT_OK ||
        vm_snapshot_list(snapshots, snapshotItems, VM_SNAPSHOT_MAX, &snapshotCount,
                         detail, sizeof detail) != VM_SNAPSHOT_OK || snapshotCount)
        return IPAError(error, @"Historical snapshots prevent changing this guest disk. Back up the machine and resolve its snapshots first.");

    NSString *app = [@"/Applications" stringByAppendingPathComponent:[_slug stringByAppendingString:@".app"]];
    NSMutableDictionary<NSString *, NSDictionary *> *objects = [NSMutableDictionary dictionary];
    objects[app] = @{ @"directory": @YES };
    NSString *root = [NSString stringWithUTF8String:_plan.app_root];
    for (unsigned i = 0; i < _plan.count; i++) {
        NSString *source = [NSString stringWithUTF8String:_plan.files[i].name];
        if ([source hasSuffix:@"/"]) source = [source substringToIndex:source.length - 1];
        if ([source isEqual:root]) continue;
        NSString *relative = [source substringFromIndex:root.length + 1];
        NSString *path = [app stringByAppendingPathComponent:relative];
        if (objects[path] && !_plan.files[i].is_directory)
            return IPAError(error, @"The IPA uses a file as a parent directory.");
        objects[path] = _plan.files[i].is_directory ? @{ @"directory": @YES } : @{ @"index": @(i) };
        NSString *parent = path.stringByDeletingLastPathComponent;
        while (![parent isEqual:app]) {
            if (objects[parent] && !objects[parent][@"directory"])
                return IPAError(error, @"The IPA uses a file as a parent directory.");
            objects[parent] = @{ @"directory": @YES };
            parent = parent.stringByDeletingLastPathComponent;
        }
    }
    /* A retryable guest-side refresh uses the uicache installed by the guest
     * jailbreak. The host never pretends a staged bundle is already registered. */
    NSString *scriptPath = [@"/usr/libexec" stringByAppendingPathComponent:_slug];
    NSString *stamp = [@"/private/var/mobile/Library" stringByAppendingPathComponent:[_slug stringByAppendingString:@".registered"]];
    NSString *script = [NSString stringWithFormat:
        @"#!/bin/sh\n[ -f '%@' ] && exit 0\n"
         "[ -f /var/mobile/Library/Caches/com.apple.mobile.installation.plist ] || exit 1\n"
         "[ -x /usr/bin/uicache ] || exit 1\n"
         "/bin/su --login --command /usr/bin/uicache mobile || exit 1\n"
         "touch '%@'\n", stamp, stamp];
    NSData *scriptData = [script dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *job = @{ @"Label": [@"com.s5lbox." stringByAppendingString:_slug],
        @"ProgramArguments": @[@"/bin/sh", scriptPath], @"RunAtLoad": @YES,
        @"StartInterval": @30, @"UserName": @"root" };
    NSData *jobData = [NSPropertyListSerialization dataWithPropertyList:job
        format:NSPropertyListXMLFormat_v1_0 options:0 error:error];
    if (!jobData) return NO;
    objects[scriptPath] = @{ @"data":scriptData };
    NSString *jobPath = [@"/Library/LaunchDaemons" stringByAppendingPathComponent:
        [[@"com.s5lbox." stringByAppendingString:_slug] stringByAppendingString:@".plist"]];
    objects[jobPath] = @{ @"data":jobData };
    NSArray<NSString *> *paths = [objects.allKeys sortedArrayUsingSelector:@selector(compare:)];
    if (paths.count > ROOTFS_WORK_MAX_ENTRIES)
        return IPAError(error, @"The app has too many files and directories for this installer.");
    rootfs_work_entry_t *entries = calloc(paths.count, sizeof *entries);
    if (!entries) return IPAError(error, @"Not enough memory to install this app.");
    BOOL success = NO;
    BOOL prepared = NO, publishAttempted = NO;
    @try {
        for (NSUInteger i = 0; i < paths.count; i++) {
            NSString *path = paths[i];
            NSDictionary *object = objects[path];
            entries[i].path = path.UTF8String;
            entries[i].kind = object[@"directory"] ? ROOTFS_WORK_ENTRY_DIRECTORY : ROOTFS_WORK_ENTRY_FILE;
            entries[i].permissions = entries[i].kind == ROOTFS_WORK_ENTRY_DIRECTORY ? 0755 : 0644;
            if (object[@"index"]) {
                unsigned index = [object[@"index"] unsignedIntValue];
                entries[i].content = (const uint8_t *)_packed.bytes + _offsets[index].unsignedLongLongValue;
                entries[i].content_size = (size_t)_plan.files[index].uncompressed_size;
                if ((_plan.files[index].unix_mode & 0111u) || [path isEqual:[app stringByAppendingPathComponent:_executable]])
                    entries[i].permissions = 0755;
            } else if (object[@"data"]) {
                NSData *data = object[@"data"];
                entries[i].content = data.bytes;
                entries[i].content_size = data.length;
            }
        }
        vm_guest_install_result_t transaction;
        if (vm_guest_app_prepare_stage(work, &transaction, detail, sizeof detail) != VM_GUEST_INSTALL_OK ||
            !vm_guest_app_stage_image_path(stage, sizeof stage, work))
            return IPAError(error, [NSString stringWithUTF8String:detail]);
        prepared = YES;
        if (vm_guest_app_clone_live_to_stage(work, detail, sizeof detail) != VM_GUEST_INSTALL_OK)
            return IPAError(error, [NSString stringWithUTF8String:detail]);
        rootfs_work_result_t result;
        if (rootfs_work_repair_powered_off_clone(stage, &result) != ROOTFS_WORK_OK ||
            rootfs_work_provision_clone(stage, entries, paths.count, true, &result) != ROOTFS_WORK_OK)
            return IPAError(error, [NSString stringWithFormat:
                @"The guest disk was not changed. %@", [NSString stringWithUTF8String:result.detail]]);
        publishAttempted = YES;
        vm_guest_install_status_t status = vm_guest_app_publish(work, _identity, &transaction, detail, sizeof detail);
        if (status != VM_GUEST_INSTALL_OK || !transaction.committed)
            return IPAError(error, [NSString stringWithFormat:
                @"Installation needs recovery before retrying: %@", [NSString stringWithUTF8String:detail]]);
        success = YES;
    } @finally {
        free(entries);
        if (prepared && !publishAttempted) vm_guest_app_discard_stage(work, NULL, 0);
    }
    return success;
}
@end
