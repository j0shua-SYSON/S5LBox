// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMFirmwareStore.h"
#import "VMFirmwarePolicy.h"
#include "ios3_firmware_profiles.h"

static NSString *const pointerName = @"current-firmware.txt";
static NSString *const generationPrefix = @"prepared-";
static NSString *const machineSourceName = @"firmware-source.txt";

static BOOL firmwareError(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"S5LBox.Firmware" code:2
        userInfo:@{NSLocalizedDescriptionKey:message}];
    return NO;
}

static BOOL validMachine(NSString *name) {
    return name.length == 16 &&
        [name rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:
            @"0123456789abcdef"] invertedSet]].location == NSNotFound;
}

static BOOL regularDirectory(NSString *path) {
    return [[NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL][NSFileType]
        isEqual:NSFileTypeDirectory];
}

static NSString *machineSource(NSString *machine, NSError **error) {
    NSString *path = [machine stringByAppendingPathComponent:machineSourceName];
    NSDictionary *info = [NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL];
    if (!info) return nil;
    if (![info[NSFileType] isEqual:NSFileTypeRegular] || [info[NSFileSize] unsignedLongLongValue] > 128) {
        firmwareError(error, @"This machine's firmware record is damaged.");
        return @"";
    }
    NSString *value = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:error];
    return value ?: @"";
}

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
    // The policy belongs to this generation, never to mutable app settings.
    vm_fw_policy_t policy;
    int recorded = vm_fw_policy_read(directory.fileSystemRepresentation, &policy);
    if (recorded < 0) return NO;
    NSArray<NSString *> *names = @[@"kernel.macho", @"devicetree.bin", @"rootfs.img"];
    const uint64_t stock[] = {7942144, 40544, 433274880};
    if (recorded && !policy.skip_stock_checksums &&
        !ios3_firmware_profile_for_sizes(policy.sizes)) return NO;
    for (NSUInteger i = 0; i < names.count; i++) {
        uint64_t size = recorded ? policy.sizes[i] : stock[i];
        NSDictionary *info = [NSFileManager.defaultManager attributesOfItemAtPath:
            [directory stringByAppendingPathComponent:names[i]] error:NULL];
        if (![info[NSFileType] isEqual:NSFileTypeRegular] ||
            [info[NSFileSize] unsignedLongLongValue] != size) return NO;
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

/* All selection/pin/removal operations use the same lock. An import finishing
 * on its worker queue cannot change the default between creation and pinning. */
+ (NSString *)sourceDirectory:(NSString *)identifier inRoot:(NSString *)root {
    if ([identifier isEqual:@"legacy"]) return root;
    if (!validGeneration(identifier)) return nil;
    NSString *directory = [root stringByAppendingPathComponent:identifier];
    return regularDirectory(directory) ? directory : nil;
}

+ (BOOL)pinFirmware:(NSString *)identifier forMachine:(NSString *)machine
             inRoot:(NSString *)root error:(NSError **)error {
    @synchronized(self) {
        NSString *machines = [[root stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"Machines"];
        if (!validMachine(machine.lastPathComponent) || !regularDirectory(machine) ||
            ![machine.stringByDeletingLastPathComponent.stringByResolvingSymlinksInPath
                isEqual:machines.stringByResolvingSymlinksInPath])
            return firmwareError(error, @"This machine's storage is unavailable.");
        NSString *existing = machineSource(machine, error);
        if (existing) {
            NSString *directory = [self sourceDirectory:existing inRoot:root];
            if (!directory || ![self hasPreparedFilesInDirectory:directory])
                return firmwareError(error, @"This machine's firmware is missing or damaged. Import it again.");
            if (identifier && ![identifier isEqual:existing])
                return firmwareError(error, @"An existing machine keeps its original firmware.");
            return YES;
        }
        NSString *directory = identifier ? [self sourceDirectory:identifier inRoot:root]
                                        : [self activeDirectoryInRoot:root];
        if (!directory || ![self hasPreparedFilesInDirectory:directory]) {
            if (!identifier) return YES; // The initial placeholder precedes setup.
            return firmwareError(error, @"The selected firmware is unavailable.");
        }
        NSString *source = [directory isEqual:root] ? @"legacy" : directory.lastPathComponent;
        return [source writeToFile:[machine stringByAppendingPathComponent:machineSourceName]
            atomically:YES encoding:NSUTF8StringEncoding error:error];
    }
}

+ (NSString *)firmwareForMachine:(NSString *)machine inRoot:(NSString *)root error:(NSError **)error {
    @synchronized(self) {
        if (![self pinFirmware:nil forMachine:machine inRoot:root error:error]) return nil;
        NSString *source = machineSource(machine, error);
        return source ? [self sourceDirectory:source inRoot:root] : [self activeDirectoryInRoot:root];
    }
}

+ (NSArray<NSString *> *)machineDirectoriesInRoot:(NSString *)root error:(NSError **)error {
    NSString *machines = [[root stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"Machines"];
    if (![NSFileManager.defaultManager fileExistsAtPath:machines]) return @[];
    if (!regularDirectory(machines)) {
        firmwareError(error, @"The machine library is unavailable.");
        return nil;
    }
    NSArray *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:machines error:error];
    if (!names) return nil;
    NSMutableArray *result = [NSMutableArray new];
    for (NSString *name in names) {
        NSString *path = [machines stringByAppendingPathComponent:name];
        if (validMachine(name) && regularDirectory(path)) [result addObject:path];
    }
    return result;
}

+ (BOOL)selectFirmware:(NSString *)identifier inRoot:(NSString *)root error:(NSError **)error {
    @synchronized(self) {
        NSString *directory = [self sourceDirectory:identifier inRoot:root];
        if (!directory || ![self hasPreparedFilesInDirectory:directory])
            return firmwareError(error, @"The selected firmware is incomplete. Import it again.");
        // Pin every older machine before changing the default. Partial pinning
        // is harmless: the old default stays active if any write fails.
        NSArray *machines = [self machineDirectoriesInRoot:root error:error];
        if (!machines) return NO;
        for (NSString *machine in machines)
            if (![self pinFirmware:nil forMachine:machine inRoot:root error:error]) return NO;
        return [identifier writeToFile:[root stringByAppendingPathComponent:pointerName]
            atomically:YES encoding:NSUTF8StringEncoding error:error];
    }
}

+ (BOOL)publishDirectory:(NSString *)directory inRoot:(NSString *)root error:(NSError **)error {
    @synchronized(self) {
        if (![directory.stringByDeletingLastPathComponent isEqual:root] ||
            !validGeneration(directory.lastPathComponent) || !regularDirectory(directory) ||
            ![self hasPreparedFilesInDirectory:directory])
            return firmwareError(error, @"The prepared firmware set is incomplete. Import the IPSW again.");
        return [self selectFirmware:directory.lastPathComponent inRoot:root error:error];
    }
}

+ (NSString *)displayNameInDirectory:(NSString *)directory {
    vm_fw_policy_t policy;
    int recorded = vm_fw_policy_read(directory.fileSystemRepresentation, &policy);
    if (recorded == 0) return @"iPhone OS 3.1.3";
    if (recorded == 1 && !policy.skip_stock_checksums) {
        const ios3_firmware_profile_t *profile = ios3_firmware_profile_for_sizes(policy.sizes);
        if (profile) return [NSString stringWithUTF8String:profile->name];
    }
    return @"Custom firmware";
}

+ (NSArray<NSDictionary<NSString *, id> *> *)firmwaresInRoot:(NSString *)root {
    @synchronized(self) {
        NSString *active = [self activeDirectoryInRoot:root];
        NSArray *machines = [self machineDirectoriesInRoot:root error:NULL] ?: @[];
        NSMutableArray *entries = [NSMutableArray new];
        NSMutableArray *names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:NULL] mutableCopy];
        if (!names) names = [NSMutableArray new];
        if ([self hasPreparedFilesInDirectory:root]) [names addObject:@"legacy"];
        for (NSString *name in names) {
            NSString *directory = [self sourceDirectory:name inRoot:root];
            if (!directory || ![self hasPreparedFilesInDirectory:directory]) continue;
            NSUInteger used = 0;
            for (NSString *machine in machines) {
                NSString *source = machineSource(machine, NULL);
                if ([source isEqual:name] || (!source && [directory isEqual:active])) used++;
            }
            [entries addObject:@{@"id":name, @"name":[self displayNameInDirectory:directory],
                @"device":@"iPhone 3G", @"version":@"3.1.3", @"build":@"7E18",
                @"default":@([directory isEqual:active]), @"machines":@(used),
                @"removable":@(!used && ![directory isEqual:active] && validGeneration(name))}];
        }
        [entries sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
            NSComparisonResult order = [a[@"name"] localizedStandardCompare:b[@"name"]];
            return order == NSOrderedSame ? [a[@"id"] compare:b[@"id"]] : order;
        }];
        return entries;
    }
}

+ (BOOL)removeFirmware:(NSString *)identifier inRoot:(NSString *)root error:(NSError **)error {
    @synchronized(self) {
        if (!validGeneration(identifier))
            return firmwareError(error, @"Legacy firmware files are managed in Files.");
        NSString *directory = [self sourceDirectory:identifier inRoot:root];
        if (!directory || [directory isEqual:[self activeDirectoryInRoot:root]])
            return firmwareError(error, @"Choose another default firmware before removing this one.");
        NSArray *machines = [self machineDirectoriesInRoot:root error:error];
        if (!machines) return NO;
        for (NSString *machine in machines) {
            NSString *source = machineSource(machine, error);
            if (source && ![self sourceDirectory:source inRoot:root])
                return firmwareError(error, @"A machine's firmware record is damaged. Nothing was removed.");
            if ([source isEqual:identifier])
                return firmwareError(error, @"This firmware is used by a machine.");
        }
        return [NSFileManager.defaultManager removeItemAtPath:directory error:error];
    }
}
@end
