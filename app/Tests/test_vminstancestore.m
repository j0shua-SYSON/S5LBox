// Exercise the real Foundation persistence owner in an isolated test directory.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMInstanceStore.h"
#import "VMSettings.h"
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
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    [defaults removePersistentDomainForName:suite];
    puts("interface settings: Modern default, Legacy persistence, notifications, reset and unrelated values passed");
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

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        testInterfaceChoice();
        assert(argc == 2);
        testRoot = [[NSString stringWithUTF8String:argv[1]]
            stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSFileManager *fm = NSFileManager.defaultManager;
        assert([fm createDirectoryAtPath:testRoot withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert([NSData.data writeToFile:[testRoot stringByAppendingPathComponent:@"not-a-directory"] atomically:YES]);
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
        // Only this process's UUID fixture, never the user's machine directory.
        assert([fm removeItemAtPath:testRoot error:NULL]);
        puts("instance store: 2/4/8 GiB persistence, duplication, write-failure rollback and deletion ordering passed");
    }
    return 0;
}
