// Exercise the real Foundation persistence owner in an isolated test directory.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMInstanceStore.h"
#import "VMSettings.h"
#import "VMFirmwareStore.h"
#import "VMFirmwarePolicy.h"
#include "VMDiskSize.h"
#include <assert.h>

static NSString *testRoot;
@interface VMSettings (InterfaceTest)
- (NSUserDefaults *)defaults;
@end
@interface TestInterfaceSettings : VMSettings
@property(nonatomic, strong) NSUserDefaults *testDefaults;
@end
@implementation TestInterfaceSettings
- (NSUserDefaults *)defaults { return self.testDefaults; }
@end

static void testRingerChoice(void) {
    NSString *suite = [@"S5LBox.RingerTests." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    TestInterfaceSettings *settings = [TestInterfaceSettings new];
    settings.testDefaults = defaults;
    NSString *first = @"1111111111111111", *second = @"2222222222222222";
    assert(![settings ringerSilentForInstanceID:first]);
    [settings setRingerSilent:YES forInstanceID:first];
    TestInterfaceSettings *reopened = [TestInterfaceSettings new];
    reopened.testDefaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    assert([reopened ringerSilentForInstanceID:first]);
    assert(![reopened ringerSilentForInstanceID:second]);
    [settings resetToDefaults];
    assert([reopened ringerSilentForInstanceID:first]);
    [settings setRingerSilent:NO forInstanceID:first];
    assert(![reopened ringerSilentForInstanceID:first]);
    for (id invalid in @[@"YES", @2, @[], @{}]) {
        [defaults setObject:invalid forKey:[@"vm.machine.ringerSilent." stringByAppendingString:first]];
        assert(![reopened ringerSilentForInstanceID:first]);
    }
    [settings setRingerSilent:YES forInstanceID:nil];
    assert(![reopened ringerSilentForInstanceID:nil]);
    assert(![reopened ringerSilentForInstanceID:@""]);
    [defaults removePersistentDomainForName:suite];
    puts("ringer: ring default, per-machine persistence, app reset isolation and invalid choices passed");
}

static void testInterfaceChoice(void) {
    NSString *suite = [@"S5LBox.InterfaceTests." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    TestInterfaceSettings *settings = [TestInterfaceSettings new];
    settings.testDefaults = defaults;
    assert(!settings.legacyEmulatorUI);
    [settings setInstructionCap:10000000];
    [settings setPausesInBackground:NO];
    [settings setGraphicsModeForNewMachines:VMGraphicsModeSoftware];
    NSDictionary *before = [defaults persistentDomainForName:suite];
    __block NSUInteger changes = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:VMSettingsDidChangeNotification
        object:settings queue:nil usingBlock:^(__unused NSNotification *note) { ++changes; }];
    settings.legacyEmulatorUI = YES;
    assert(settings.legacyEmulatorUI && changes == 1);
    TestInterfaceSettings *reopened = [TestInterfaceSettings new];
    reopened.testDefaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    assert(reopened.legacyEmulatorUI);
    assert(settings.instructionCap == 10000000 && !settings.pausesInBackground);
    assert(settings.graphicsModeForNewMachines == VMGraphicsModeSoftware);
    NSMutableDictionary *after = [[defaults persistentDomainForName:suite] mutableCopy];
    [after removeObjectForKey:@"VMLegacyEmulatorUI"];
    assert([before isEqual:after]); // The UI choice does not rewrite emulator defaults.
    settings.legacyEmulatorUI = NO;
    assert(!reopened.legacyEmulatorUI && changes == 2);
    settings.legacyEmulatorUI = YES;
    [settings resetToDefaults];
    assert(!settings.legacyEmulatorUI && changes == 4);
    assert(!settings.homeOnlyEmulatorUI);
    settings.homeOnlyEmulatorUI = YES;
    assert(reopened.homeOnlyEmulatorUI && !reopened.legacyEmulatorUI && changes == 5);
    [settings resetToDefaults];
    assert(!reopened.homeOnlyEmulatorUI && changes == 6);
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    [defaults removePersistentDomainForName:suite];
    puts("interface settings: Modern default, Legacy persistence, notifications, reset and unrelated values passed");
}
static void testPackagesWithJailbreakChoice(void) {
    NSString *suite = [@"S5LBox.PackageSettingsTests." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    TestInterfaceSettings *settings = [TestInterfaceSettings new];
    settings.testDefaults = defaults;
    assert(settings.packagesWithJailbreak); // No saved choice means enabled.
    [defaults setObject:@"invalid" forKey:@"VMPackagesWithJailbreak"];
    assert(settings.packagesWithJailbreak);
    [defaults removeObjectForKey:@"VMPackagesWithJailbreak"];
    settings.legacyEmulatorUI = YES;
    NSDictionary *before = [defaults persistentDomainForName:suite];
    __block NSUInteger changes = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:VMSettingsDidChangeNotification
        object:settings queue:nil usingBlock:^(__unused NSNotification *note) { ++changes; }];
    settings.packagesWithJailbreak = NO;
    assert(!settings.packagesWithJailbreak && changes == 1);
    TestInterfaceSettings *reopened = [TestInterfaceSettings new];
    reopened.testDefaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    assert(!reopened.packagesWithJailbreak);
    NSMutableDictionary *after = [[defaults persistentDomainForName:suite] mutableCopy];
    [after removeObjectForKey:@"VMPackagesWithJailbreak"];
    assert([before isEqual:after]);
    settings.packagesWithJailbreak = YES;
    assert(reopened.packagesWithJailbreak && changes == 2);
    settings.packagesWithJailbreak = NO;
    [settings resetToDefaults];
    assert(reopened.packagesWithJailbreak && changes == 4);
    assert(![defaults objectForKey:@"VMPackagesWithJailbreak"]);
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    [defaults removePersistentDomainForName:suite];
    puts("Packages with jailbreak: default on, opt-out persistence, notifications and reset passed");
}

static void testBackgroundPauseChoice(void) {
    NSString *suite = [@"S5LBox.BackgroundSettingsTests." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    TestInterfaceSettings *settings = [TestInterfaceSettings new];
    settings.testDefaults = defaults;
    assert(settings.pausesInBackground);
    [defaults setObject:@"invalid" forKey:@"vm.diag.pauseInBackground"];
    assert(settings.pausesInBackground);
    settings.pausesInBackground = NO;
    TestInterfaceSettings *reopened = [TestInterfaceSettings new];
    reopened.testDefaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    assert(!reopened.pausesInBackground);
    assert(reopened.guestMicrophoneEnabled); // Session defaults are independent.
    reopened.pausesInBackground = YES;
    assert(settings.pausesInBackground);
    settings.pausesInBackground = NO;
    [settings resetToDefaults];
    assert(reopened.pausesInBackground && reopened.guestMicrophoneEnabled);
    [defaults removePersistentDomainForName:suite];
    puts("Background pause: default on, invalid preference, opt-out persistence and reset passed");
}

static void testAppSyncChoice(void) {
    NSString *suite = [@"S5LBox.AppSyncSettingsTests." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    TestInterfaceSettings *settings = [TestInterfaceSettings new];
    settings.testDefaults = defaults;
    assert(settings.appSyncWithJailbreak);
    [defaults setObject:@"invalid" forKey:@"VMAppSyncWithJailbreak"];
    assert(settings.appSyncWithJailbreak);
    [defaults removeObjectForKey:@"VMAppSyncWithJailbreak"];
    settings.packagesWithJailbreak = NO;
    assert(settings.appSyncWithJailbreak); // Independent of the helper.
    __block NSUInteger changes = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:VMSettingsDidChangeNotification
        object:settings queue:nil usingBlock:^(__unused NSNotification *note) { ++changes; }];
    settings.appSyncWithJailbreak = NO;
    TestInterfaceSettings *reopened = [TestInterfaceSettings new];
    reopened.testDefaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    assert(!reopened.appSyncWithJailbreak && changes == 1);
    settings.packagesWithJailbreak = YES;
    assert(!reopened.appSyncWithJailbreak && changes == 2);
    settings.appSyncWithJailbreak = YES;
    assert(reopened.appSyncWithJailbreak && changes == 3);
    settings.appSyncWithJailbreak = NO;
    [settings resetToDefaults];
    assert(reopened.appSyncWithJailbreak && changes == 5);
    assert(![defaults objectForKey:@"VMAppSyncWithJailbreak"]);
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    [defaults removePersistentDomainForName:suite];
    puts("AppSync: default on, independent helper opt-out, persistence, notifications and reset passed");
}

static void testGuestMicrophoneChoice(void) {
    NSString *suite = [@"S5LBox.MicrophoneSettingsTests." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    TestInterfaceSettings *settings = [TestInterfaceSettings new];
    settings.testDefaults = defaults;
    assert(settings.guestMicrophoneEnabled);
    [defaults setObject:@"invalid" forKey:@"VMGuestMicrophoneEnabled"];
    assert(settings.guestMicrophoneEnabled);
    [defaults removeObjectForKey:@"VMGuestMicrophoneEnabled"];
    settings.packagesWithJailbreak = NO;
    NSDictionary *before = [defaults persistentDomainForName:suite];
    __block NSUInteger changes = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:VMSettingsDidChangeNotification
        object:settings queue:nil usingBlock:^(__unused NSNotification *note) { ++changes; }];
    settings.guestMicrophoneEnabled = NO;
    assert(!settings.guestMicrophoneEnabled && changes == 1);
    TestInterfaceSettings *reopened = [TestInterfaceSettings new];
    reopened.testDefaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    assert(!reopened.guestMicrophoneEnabled);
    NSMutableDictionary *after = [[defaults persistentDomainForName:suite] mutableCopy];
    [after removeObjectForKey:@"VMGuestMicrophoneEnabled"];
    assert([before isEqual:after]);
    settings.guestMicrophoneEnabled = YES;
    assert(reopened.guestMicrophoneEnabled && changes == 2);
    settings.guestMicrophoneEnabled = NO;
    [settings resetToDefaults];
    assert(reopened.guestMicrophoneEnabled && changes == 4);
    assert(![defaults objectForKey:@"VMGuestMicrophoneEnabled"]);
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    [defaults removePersistentDomainForName:suite];
    puts("Guest microphone: default on, opt-out persistence, notifications and reset passed");
}

static void testDiskSizeChoice(void) {
    NSString *suite = [@"S5LBox.DiskSettingsTests." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    TestInterfaceSettings *settings = [TestInterfaceSettings new];
    settings.testDefaults = defaults;
    assert(settings.newMachineDiskSizeGiB == 8);
    for (id invalid in @[@"4", @0, @3, @16, @(-2), @2.5, @YES]) {
        [defaults setObject:invalid forKey:@"VMNewMachineDiskSizeGiB"];
        assert(settings.newMachineDiskSizeGiB == 8);
    }
    [defaults removeObjectForKey:@"VMNewMachineDiskSizeGiB"];
    settings.guestMicrophoneEnabled = NO;
    NSDictionary *before = [defaults persistentDomainForName:suite];
    TestInterfaceSettings *reopened = [TestInterfaceSettings new];
    reopened.testDefaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    __block NSUInteger changes = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:VMSettingsDidChangeNotification
        object:settings queue:nil usingBlock:^(__unused NSNotification *note) { ++changes; }];
    for (NSNumber *choice in @[@4, @2, @8]) {
        settings.newMachineDiskSizeGiB = choice.unsignedIntegerValue;
        assert(settings.newMachineDiskSizeGiB == choice.unsignedIntegerValue);
        assert(reopened.newMachineDiskSizeGiB == choice.unsignedIntegerValue);
    }
    assert(changes == 3);
    settings.newMachineDiskSizeGiB = 3;
    settings.newMachineDiskSizeGiB = NSUIntegerMax;
    assert(settings.newMachineDiskSizeGiB == 8 && changes == 3);
    NSMutableDictionary *after = [[defaults persistentDomainForName:suite] mutableCopy];
    [after removeObjectForKey:@"VMNewMachineDiskSizeGiB"];
    assert([before isEqual:after]);
    settings.newMachineDiskSizeGiB = 2;
    [settings resetToDefaults];
    assert(reopened.newMachineDiskSizeGiB == 8 && changes == 5);
    assert(![defaults objectForKey:@"VMNewMachineDiskSizeGiB"]);
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    [defaults removePersistentDomainForName:suite];
    puts("new-machine storage: 8 GiB default, strict choices, persistence, notifications and reset passed");
}

@interface VMInstanceStore (PersistenceTest)
- (NSString *)containerDirectory;
- (NSString *)storePath;
- (BOOL)save;
@end

@interface TestInstanceStore : VMInstanceStore
@property BOOL failSave;
@property BOOL blockedPath;
@end
@implementation TestInstanceStore
- (NSString *)containerDirectory { return testRoot; }
- (NSString *)storePath {
    // A regular file as the parent makes Foundation's atomic write fail,
    // without chmod/root privileges or filling the host's storage.
    if (self.blockedPath)
        return [testRoot stringByAppendingPathComponent:@"not-a-directory/machines.txt"];
    return [super storePath];
}
- (BOOL)save { return self.failSave ? NO : [super save]; }
@end

static NSArray *rows(VMInstanceStore *store) {
    NSMutableArray *result = [NSMutableArray array];
    for (NSUInteger i = 0; i < store.count; ++i)
        [result addObject:[store instanceAtIndex:i]];
    return result;
}
static NSSet *files(void) {
    NSArray *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:testRoot error:NULL];
    assert(names);
    return [NSSet setWithArray:names];
}

static void testDiskCreationDefaults(void) {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    id previous = [defaults objectForKey:@"VMNewMachineDiskSizeGiB"];
    [defaults removeObjectForKey:@"VMNewMachineDiskSizeGiB"];
    VMSettings *settings = VMSettings.sharedSettings;
    TestInstanceStore *store = [TestInstanceStore new];
    assert(store.count == 0);
    NSError *error = nil;
    // The same convenience method serves onboarding and the New Machine action.
    assert([store createInstanceNamed:@"Automatic" error:&error] && !error);
    for (NSNumber *choice in @[@4, @2]) {
        settings.newMachineDiskSizeGiB = choice.unsignedIntegerValue;
        assert([store createInstanceNamed:@"Advanced choice" error:&error] && !error);
    }
    settings.newMachineDiskSizeGiB = 8;
    NSArray *expected = @[@8, @4, @2];
    for (NSUInteger i = 0; i < expected.count; ++i)
        assert([[store instanceAtIndex:i][@"diskSizeGiB"] isEqual:expected[i]]);
    assert([rows(store) isEqual:rows([TestInstanceStore new])]);
    assert([store duplicateInstanceAtIndex:2 error:&error] && !error);
    assert([[store instanceAtIndex:3][@"diskSizeGiB"] isEqual:@2]);

    // Simulate a legacy machine. Neither the 8 GiB preference nor reopening
    // the store can materialize a record and resize that existing machine.
    NSString *legacyID = [store instanceAtIndex:1][@"id"];
    NSString *directory = [store directoryForInstanceWithID:legacyID];
    NSString *record = [directory stringByAppendingPathComponent:@VM_DISK_SIZE_FILE];
    assert([NSFileManager.defaultManager removeItemAtPath:record error:NULL]);
    [store noteOpenedInstanceWithID:legacyID];
    assert([[store instanceAtIndex:1][@"diskSizeGiB"] isEqual:@0]);
    assert([rows(store) isEqual:rows([TestInstanceStore new])]);
    uint64_t capacity = 123;
    assert(vm_disk_size_read(directory.fileSystemRepresentation, &capacity) && capacity == 0);
    assert(![NSFileManager.defaultManager fileExistsAtPath:record]);
    assert([store duplicateInstanceAtIndex:1 error:&error] && !error);
    assert([[store instanceAtIndex:4][@"diskSizeGiB"] isEqual:@2]);
    assert([[store instanceAtIndex:1][@"diskSizeGiB"] isEqual:@0]);
    while (store.count) assert([store deleteInstanceAtIndex:0 error:&error] && !error);
    if (previous) [defaults setObject:previous forKey:@"VMNewMachineDiskSizeGiB"];
    else [defaults removeObjectForKey:@"VMNewMachineDiskSizeGiB"];
    puts("machine creation: automatic 8 GiB, Advanced choices, immutable records and legacy fallback passed");
}

static void testFirmwareSelection(void) {
    NSString *previousRoot = testRoot;
    NSString *library = [testRoot stringByAppendingPathComponent:@"firmware-library/firmware"];
    testRoot = [testRoot stringByAppendingPathComponent:@"firmware-library/Machines"];
    NSFileManager *fm = NSFileManager.defaultManager;
    assert([fm createDirectoryAtPath:testRoot withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *first = [VMFirmwareStore createStagingDirectoryInRoot:library error:NULL];
    NSString *second = [VMFirmwareStore createStagingDirectoryInRoot:library error:NULL];
    vm_fw_policy_t policy = {true, {512, 512, 512}};
    for (NSString *directory in @[first, second]) {
        for (NSString *name in @[@"kernel.macho", @"devicetree.bin", @"rootfs.img"])
            assert([[NSMutableData dataWithLength:512] writeToFile:[directory stringByAppendingPathComponent:name] atomically:NO]);
        assert(vm_fw_policy_write(directory.fileSystemRepresentation, &policy));
    }
    assert([VMFirmwareStore publishDirectory:first inRoot:library error:NULL]);
    TestInstanceStore *store = [TestInstanceStore new];
    NSString *original = [store createInstanceNamed:@"First" error:NULL];
    assert(original);
    assert([VMFirmwareStore publishDirectory:second inRoot:library error:NULL]);
    NSString *duplicate = [store duplicateInstanceAtIndex:0 error:NULL];
    assert(duplicate);
    for (NSString *identifier in @[original, duplicate])
        assert([[VMFirmwareStore firmwareForMachine:[store directoryForInstanceWithID:identifier]
            inRoot:library error:NULL] isEqual:first]);
    NSString *other = [store createInstanceNamed:@"Second" firmwareIdentifier:second.lastPathComponent diskSizeGiB:8 error:NULL];
    assert(other);
    assert([VMFirmwareStore selectFirmware:first.lastPathComponent inRoot:library error:NULL]);
    assert([[VMFirmwareStore firmwareForMachine:[store directoryForInstanceWithID:other]
        inRoot:library error:NULL] isEqual:second]);
    assert(![VMFirmwareStore removeFirmware:second.lastPathComponent inRoot:library error:NULL]);
    NSString *record = [[store directoryForInstanceWithID:original] stringByAppendingPathComponent:@"firmware-source.txt"];
    assert([@"../outside" writeToFile:record atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
    assert(![store duplicateInstanceAtIndex:0 error:NULL] && store.count == 3);
    testRoot = previousRoot;
    puts("machine firmware: explicit selection, default changes, duplication and damaged-source refusal passed");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        testInterfaceChoice();
        testRingerChoice();
        testPackagesWithJailbreakChoice();
        testAppSyncChoice();
        testGuestMicrophoneChoice();
        testBackgroundPauseChoice();
        testDiskSizeChoice();
        assert(argc == 2);
        testRoot = [[NSString stringWithUTF8String:argv[1]]
            stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSFileManager *fm = NSFileManager.defaultManager;
        assert([fm createDirectoryAtPath:testRoot withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert([NSData.data writeToFile:[testRoot stringByAppendingPathComponent:@"not-a-directory"] atomically:YES]);
        testDiskCreationDefaults();
        TestInstanceStore *store = [TestInstanceStore new];
        __block unsigned notifications = 0;
        id observer = [NSNotificationCenter.defaultCenter addObserverForName:VMInstanceStoreDidChangeNotification
            object:store queue:nil usingBlock:^(NSNotification *note) { (void)note; notifications++; }];
        for (NSNumber *gib in @[@2, @4, @8]) {
            NSError *error = nil;
            NSString *name = [NSString stringWithFormat:@"Disk %@", gib];
            assert([store createInstanceNamed:name
                diskSizeGiB:gib.unsignedIntegerValue error:&error] && !error);
        }
        for (NSUInteger i = 0; i < 3; ++i) {
            NSError *error = nil;
            assert([store duplicateInstanceAtIndex:i error:&error] && !error);
            assert([[store instanceAtIndex:i][@"diskSizeGiB"] isEqual:[store instanceAtIndex:i + 3][@"diskSizeGiB"]]);
        }
        assert(store.count == 6 && notifications == 6);
        assert([rows(store) isEqual:rows([TestInstanceStore new])]);

        NSString *disk = [[store directoryForInstanceWithID:[store instanceAtIndex:1][@"id"]]
            stringByAppendingPathComponent:@"rootfs-work.img"];
        NSData *sentinel = [@"must survive a refused delete" dataUsingEncoding:NSUTF8StringEncoding];
        assert([sentinel writeToFile:disk atomically:YES]);
        NSString *listPath = [testRoot stringByAppendingPathComponent:@"machines.txt"];
        NSData *beforeDisk = [NSData dataWithContentsOfFile:listPath];
        NSArray *beforeRows = rows(store);
        NSSet *beforeFiles = files();
        for (unsigned mode = 0; mode < 2; ++mode) {
            store.failSave = mode == 0;
            store.blockedPath = mode == 1;
            for (NSUInteger i = 0; i < 3; ++i) {
                NSError *error = nil;
                NSUInteger gib = [[store instanceAtIndex:i][@"diskSizeGiB"] unsignedIntegerValue];
                assert(![store createInstanceNamed:@"Refused" diskSizeGiB:gib error:&error] && error.code == 1002);
                error = nil;
                assert(![store duplicateInstanceAtIndex:i error:&error] && error.code == 1002);
                assert([rows(store) isEqual:beforeRows] && [files() isEqual:beforeFiles]);
            }
            NSError *error = nil;
            assert(![store renameInstanceAtIndex:1 to:@"Not persisted" error:&error] && error.code == 1002);
            error = nil;
            assert(![store deleteInstanceAtIndex:1 error:&error] && error.code == 1002);
            assert([rows(store) isEqual:beforeRows] && [files() isEqual:beforeFiles]);
            assert([[NSData dataWithContentsOfFile:listPath] isEqual:beforeDisk]);
            assert([[NSData dataWithContentsOfFile:disk] isEqual:sentinel]);
            assert(notifications == 6);
            assert([rows([TestInstanceStore new]) isEqual:beforeRows]);
        }
        store.failSave = NO;
        store.blockedPath = NO;
        NSError *error = nil;
        assert([store renameInstanceAtIndex:1 to:@"Persisted" error:&error] && !error);
        assert([rows(store) isEqual:rows([TestInstanceStore new])]);
        assert([store deleteInstanceAtIndex:1 error:&error] && !error);
        assert(![fm fileExistsAtPath:disk] && store.count == 5 && notifications == 8);
        assert([rows(store) isEqual:rows([TestInstanceStore new])]);
        [NSNotificationCenter.defaultCenter removeObserver:observer];
        testFirmwareSelection();
        // Only this process's UUID fixture, never the user's machine directory.
        assert([fm removeItemAtPath:testRoot error:NULL]);
        puts("instance store: 2/4/8 GiB persistence, duplication, write-failure rollback and deletion ordering passed");
    }
    return 0;
}
