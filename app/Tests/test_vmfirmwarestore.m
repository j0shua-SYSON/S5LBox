// Foundation integration: atomic activation, legacy compatibility and importer state.
// Fixtures are sparse zero files and a malformed archive, never Apple firmware.
#import "VMFirmwareStore.h"
#import "VMFirmwareImporter.h"
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

static NSString *testRoot;
@interface TestImporter : VMFirmwareImporter <VMFirmwareImporterDelegate>
@property unsigned completions;
@end
@implementation TestImporter
- (NSString *)firmwareRootDirectory { return testRoot; }
- (void)importer:(VMFirmwareImporter *)importer didFinishWithStatus:(vm_fw_status_t)status report:(const vm_fw_report_t *)report {
    (void)importer;
    assert(NSThread.isMainThread && report && status != VM_FW_OK);
    self.completions++;
}
@end

static void makeSet(NSString *path) {
    NSDictionary *sizes = @{@"kernel.macho": @7942144, @"devicetree.bin": @40544, @"rootfs.img": @433274880};
    for (NSString *name in sizes) {
        NSString *file = [path stringByAppendingPathComponent:name];
        int fd = open(file.fileSystemRepresentation, O_RDWR | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && ftruncate(fd, [sizes[name] longLongValue]) == 0);
        assert(close(fd) == 0);
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        assert(argc == 2);
        testRoot = [[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSFileManager *fm = NSFileManager.defaultManager;
        assert([fm createDirectoryAtPath:testRoot withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert(![VMFirmwareStore hasPreparedFilesInDirectory:testRoot]);
        makeSet(testRoot);
        assert([VMFirmwareStore hasPreparedFilesInDirectory:testRoot]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:testRoot]);
        NSString *first = [VMFirmwareStore createStagingDirectoryInRoot:testRoot error:NULL];
        assert(first && ![VMFirmwareStore publishDirectory:first inRoot:testRoot error:NULL]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:testRoot]);
        makeSet(first);
        assert([VMFirmwareStore publishDirectory:first inRoot:testRoot error:NULL]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:first]);
        NSString *pointer = [testRoot stringByAppendingPathComponent:@"current-firmware.txt"];
        NSData *before = [NSData dataWithContentsOfFile:pointer];
        NSString *second = [VMFirmwareStore createStagingDirectoryInRoot:testRoot error:NULL];
        assert(second && ![VMFirmwareStore publishDirectory:second inRoot:testRoot error:NULL]);
        assert([[NSData dataWithContentsOfFile:pointer] isEqual:before]);
        makeSet(second);
        assert([VMFirmwareStore publishDirectory:second inRoot:testRoot error:NULL]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:second]);
        assert(![fm fileExistsAtPath:first] && [VMFirmwareStore hasPreparedFilesInDirectory:testRoot]);
        // Pointer traversal cannot redirect the boot source or cleanup target.
        assert([@"../outside" writeToFile:pointer atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:testRoot]);
        assert([fm removeItemAtPath:pointer error:NULL]);
        assert([fm createDirectoryAtPath:pointer withIntermediateDirectories:NO attributes:nil error:NULL]);
        NSError *error = nil;
        assert(![VMFirmwareStore publishDirectory:second inRoot:testRoot error:&error] && error);
        assert([VMFirmwareStore hasPreparedFilesInDirectory:second] && [VMFirmwareStore hasPreparedFilesInDirectory:testRoot]);
        assert([fm removeItemAtPath:pointer error:NULL]);
        assert([VMFirmwareStore publishDirectory:second inRoot:testRoot error:NULL]);

        NSString *archive = [testRoot stringByAppendingPathComponent:@"renamed-iPhone1,2_3.1.3_7E18.ipsw"];
        assert([@"not a zip" writeToFile:archive atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
        NSSet *filesBefore = [NSSet setWithArray:[fm contentsOfDirectoryAtPath:testRoot error:NULL]];
        TestImporter *importer = [TestImporter new];
        importer.delegate = importer;
        NSURL *url = [NSURL fileURLWithPath:archive];
        [importer importIPSWAtURL:url];
        [importer importIPSWAtURL:url]; // Duplicate delivery is a no-op.
        assert(importer.isRunning && [importer.selectedURL isEqual:url]);
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:15];
        while (importer.isRunning && deadline.timeIntervalSinceNow > 0)
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        assert(!importer.isRunning && importer.completions == 1);
        vm_fw_report_t report;
        assert([importer getLastReport:&report] && report.status == VM_FW_ERR_NOT_AN_ARCHIVE);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:second]);
        assert([[NSSet setWithArray:[fm contentsOfDirectoryAtPath:testRoot error:NULL]] isEqual:filesBefore]);
        [importer replayStateToDelegate];
        assert(importer.completions == 2); // Reopened screen gets the result without importing again.
        [importer importIPSWAtURL:url];
        [importer cancelImport];
        deadline = [NSDate dateWithTimeIntervalSinceNow:15];
        while (importer.isRunning && deadline.timeIntervalSinceNow > 0)
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        assert(!importer.isRunning && importer.completions == 3);
        assert([importer getLastReport:&report] && report.status == VM_FW_ERR_CANCELLED);
        assert([[NSSet setWithArray:[fm contentsOfDirectoryAtPath:testRoot error:NULL]] isEqual:filesBefore]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:second]);
        assert([fm removeItemAtPath:testRoot error:NULL]); // Only this test's UUID root.
        puts("firmware store: publication, rollback, legacy paths, cancellation and retained importer state passed");
    }
    return 0;
}
