// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMFirmwareStore.h"

static NSString *const pointerName = @"current-firmware.txt";
static NSString *const generationPrefix = @"prepared-";

static BOOL validGeneration(NSString *name) {
    return name.length == generationPrefix.length + 36 &&
        [name hasPrefix:generationPrefix] &&
        [[NSUUID alloc] initWithUUIDString:[name substringFromIndex:generationPrefix.length]] != nil;
}

static NSString *selectedGeneration(NSString *root) {
    NSString *path = [root stringByAppendingPathComponent:pointerName];
    NSDictionary *info = [NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL];
    if (![info[NSFileType] isEqual:NSFileTypeRegular] || [info[NSFileSize] unsignedLongLongValue] > 128)
        return nil;
    NSString *name = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
    return validGeneration(name) ? name : nil;
}

@implementation VMFirmwareStore
+ (BOOL)hasPreparedFilesInDirectory:(NSString *)directory {
    if (!directory.length) return NO;
    // Availability only; boot independently authenticates contents. Imports may
    // publish only after the C pipeline's complete SHA-256 verification.
    NSDictionary<NSString *, NSNumber *> *sizes = @{
        @"kernel.macho": @7942144, @"devicetree.bin": @40544, @"rootfs.img": @433274880
    };
    for (NSString *name in sizes) {
        NSDictionary *info = [NSFileManager.defaultManager attributesOfItemAtPath:
            [directory stringByAppendingPathComponent:name] error:NULL];
        if (![info[NSFileType] isEqual:NSFileTypeRegular] ||
            ![info[NSFileSize] isEqual:sizes[name]]) return NO;
    }
    return YES;
}

+ (NSString *)activeDirectoryInRoot:(NSString *)root {
    NSString *selected = selectedGeneration(root);
    if (selected) {
        NSString *directory = [root stringByAppendingPathComponent:selected];
        NSDictionary *info = [NSFileManager.defaultManager attributesOfItemAtPath:directory error:NULL];
        if ([info[NSFileType] isEqual:NSFileTypeDirectory] &&
            [self hasPreparedFilesInDirectory:directory]) return directory;
    }
    return root; // Existing manually prepared installations need no migration.
}

+ (NSString *)createStagingDirectoryInRoot:(NSString *)root error:(NSError **)error {
    NSString *name = [generationPrefix stringByAppendingString:NSUUID.UUID.UUIDString];
    NSString *path = [root stringByAppendingPathComponent:name];
    if (![NSFileManager.defaultManager createDirectoryAtPath:path
        withIntermediateDirectories:YES attributes:nil error:error]) return nil;
    return path;
}

+ (BOOL)publishDirectory:(NSString *)directory inRoot:(NSString *)root error:(NSError **)error {
    NSDictionary *info = [NSFileManager.defaultManager attributesOfItemAtPath:directory error:NULL];
    if (![[directory stringByDeletingLastPathComponent] isEqual:root] ||
        !validGeneration(directory.lastPathComponent) ||
        ![info[NSFileType] isEqual:NSFileTypeDirectory] ||
        ![self hasPreparedFilesInDirectory:directory]) {
        if (error) *error = [NSError errorWithDomain:@"S5LBox.Firmware" code:1 userInfo:@{
            NSLocalizedDescriptionKey: @"The prepared firmware set is incomplete. Import the IPSW again."}];
        return NO;
    }
    NSString *previous = selectedGeneration(root);
    // No active file changes until this single atomic publication succeeds.
    if (![directory.lastPathComponent writeToFile:[root stringByAppendingPathComponent:pointerName]
        atomically:YES encoding:NSUTF8StringEncoding error:error]) return NO;
    // Reclaim only the previously selected, app-owned generation. Never remove
    // loose legacy files, the user's archive, or arbitrary sibling directories.
    if (previous && ![previous isEqual:directory.lastPathComponent])
        [NSFileManager.defaultManager removeItemAtPath:[root stringByAppendingPathComponent:previous] error:NULL];
    return YES;
}
@end
