// Foundation integration: atomic activation, legacy compatibility and importer state.
// Fixtures are sparse zero files and a malformed archive, never Apple firmware.
#import "VMFirmwareStore.h"
#import "VMFirmwareImporter.h"
#import "VMFirmwarePolicy.h"
#import "VMSettings.h"
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

static NSString *testRoot;
@interface TestImporter : VMFirmwareImporter <VMFirmwareImporterDelegate>
@property unsigned completions;
@property (copy) NSArray<NSString *> *archives;
@end
@implementation TestImporter
- (NSString *)firmwareRootDirectory { return testRoot; }
- (NSArray<NSString *> *)detectedArchivePaths { return self.archives ?: @[]; }
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

// Isolate the native publication layer from extraction (covered by C tests).
@interface PreparedImporter : VMFirmwareImporter <VMFirmwareImporterDelegate>
@property unsigned completions;
@property BOOL usedSkip;
@end
@implementation PreparedImporter
- (NSString *)firmwareRootDirectory { return testRoot; }
- (vm_fw_status_t)extractURL:(NSURL *)url intoReport:(vm_fw_report_t *)report outputDirectory:(NSString *)directory {
    (void)url;
    memset(report, 0, sizeof *report);
    self.usedSkip = self.skipStockChecksums;
    report->manifest_read = report->reference_build = true;
    snprintf(report->product_type, sizeof report->product_type, "iPhone1,2");
    snprintf(report->product_version, sizeof report->product_version, "3.1.3");
    snprintf(report->build, sizeof report->build, "7E18");
    if (!self.usedSkip) snprintf(report->firmware_name, sizeof report->firmware_name, "iPhone OS 3.1.3");
    report->stock_checksums_skipped = self.usedSkip;
    const uint64_t stock[] = {7942144, 40544, 433274880};
    if (!self.usedSkip) makeSet(directory);
    for (unsigned i = 0; i < VM_FW_ARTEFACT_COUNT; i++) {
        NSString *name = [NSString stringWithUTF8String:vm_fw_artefact_filename((vm_fw_artefact_t)i)];
        if (self.usedSkip)
            assert([[NSMutableData dataWithLength:512] writeToFile:[directory stringByAppendingPathComponent:name] atomically:NO]);
        report->artefacts[i].state = self.usedSkip ? VM_FW_STATE_EXTRACTED : VM_FW_STATE_VERIFIED;
        report->artefacts[i].sha256_valid = true;
        report->artefacts[i].reference_known = !self.usedSkip;
        report->artefacts[i].matches_reference = !self.usedSkip;
        report->artefacts[i].produced = self.usedSkip ? 512 : stock[i];
    }
    return VM_FW_OK;
}
- (void)importer:(VMFirmwareImporter *)importer didFinishWithStatus:(vm_fw_status_t)status report:(const vm_fw_report_t *)report {
    (void)importer;
    assert(NSThread.isMainThread && status == VM_FW_OK && vm_fw_report_is_prepared(report));
    self.completions++;
}
@end

static void waitForImport(VMFirmwareImporter *importer) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:15];
    while (importer.isRunning && deadline.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    assert(!importer.isRunning);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        assert(argc == 2);
        testRoot = [[NSString stringWithUTF8String:argv[1]] stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSFileManager *fm = NSFileManager.defaultManager;
        assert([fm createDirectoryAtPath:testRoot withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert(![VMFirmwareStore hasPreparedFilesInDirectory:testRoot]);
        [VMSettings.sharedSettings resetToDefaults];
        TestImporter *choice = [TestImporter new];
        assert(choice.canChangeChecksumVerification && !choice.skipStockChecksums);
        choice.skipStockChecksums = YES;
        assert([TestImporter new].skipStockChecksums);
        choice.skipStockChecksums = NO;
        makeSet(testRoot);
        assert(choice.canChangeChecksumVerification && !choice.skipStockChecksums);
        choice.skipStockChecksums = YES;
        assert(choice.skipStockChecksums && [TestImporter new].skipStockChecksums);
        assert([VMFirmwareStore hasPreparedFilesInDirectory:testRoot]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:testRoot]);
        NSString *first = [VMFirmwareStore createStagingDirectoryInRoot:testRoot error:NULL];
        assert(first && ![VMFirmwareStore publishDirectory:first inRoot:testRoot error:NULL]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:testRoot]);
        makeSet(first);
        vm_fw_policy_t custom = {true, {7942144, 40544, 433274880}};
        assert(vm_fw_policy_write(first.fileSystemRepresentation, &custom));
        assert([VMFirmwareStore publishDirectory:first inRoot:testRoot error:NULL]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:first]);
        assert(choice.canChangeChecksumVerification && choice.skipStockChecksums);
        choice.skipStockChecksums = NO;
        assert(!choice.skipStockChecksums && ![TestImporter new].skipStockChecksums);
        [VMSettings.sharedSettings resetToDefaults];
        assert(!choice.skipStockChecksums && ![TestImporter new].skipStockChecksums);
        assert(vm_fw_policy_read(first.fileSystemRepresentation, &custom) == 1 && custom.skip_stock_checksums);
        NSString *pointer = [testRoot stringByAppendingPathComponent:@"current-firmware.txt"];
        NSData *before = [NSData dataWithContentsOfFile:pointer];
        NSString *second = [VMFirmwareStore createStagingDirectoryInRoot:testRoot error:NULL];
        assert(second && ![VMFirmwareStore publishDirectory:second inRoot:testRoot error:NULL]);
        assert([[NSData dataWithContentsOfFile:pointer] isEqual:before]);
        makeSet(second);
        assert([VMFirmwareStore publishDirectory:second inRoot:testRoot error:NULL]);
        assert([[VMFirmwareStore activeDirectoryInRoot:testRoot] isEqual:second]);
        assert([fm fileExistsAtPath:first] && [VMFirmwareStore hasPreparedFilesInDirectory:testRoot]);
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
        [importer forgetSelectedURL:url]; // A running import keeps its source.
        assert([importer.selectedURL isEqual:url]);
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
        // A picker cancellation is not undone by Files auto-detection on return.
        NSString *fixtureRoot = testRoot;
        testRoot = [fixtureRoot stringByAppendingPathComponent:@"auto-detection"];
        assert([fm createDirectoryAtPath:testRoot withIntermediateDirectories:NO attributes:nil error:NULL]);
        importer.archives = @[archive];
        assert(importer.canChangeChecksumVerification);
        importer.skipStockChecksums = YES;
        assert(![importer importDetectedIPSWIfNeeded]);
        // Replacing the archive is a new import opportunity, exactly once.
        assert([@"a replacement archive fixture" writeToFile:archive atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
        assert([importer importDetectedIPSWIfNeeded]);
        assert(!importer.canChangeChecksumVerification);
        importer.skipStockChecksums = NO;
        assert(importer.skipStockChecksums);
        deadline = [NSDate dateWithTimeIntervalSinceNow:15];
        while (importer.isRunning && deadline.timeIntervalSinceNow > 0)
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        assert(!importer.isRunning && importer.completions == 4);
        assert(importer.canChangeChecksumVerification && importer.skipStockChecksums);
        assert(![importer importDetectedIPSWIfNeeded]);
        assert([fm contentsOfDirectoryAtPath:testRoot error:NULL].count == 0);
        [importer forgetSelectedURL:[NSURL fileURLWithPath:@"/unrelated.ipsw"]];
        assert([importer.selectedURL isEqual:url]);
        [importer forgetSelectedURL:url];
        assert(!importer.selectedURL);
        assert([fm fileExistsAtPath:url.path]); // Forgetting never deletes a manual archive.
        [VMSettings.sharedSettings resetToDefaults];
        PreparedImporter *prepared = [PreparedImporter new];
        prepared.delegate = prepared;
        prepared.skipStockChecksums = YES;
        [prepared importIPSWAtURL:url];
        waitForImport(prepared);
        assert(prepared.completions == 1 && prepared.usedSkip);
        assert(prepared.canChangeChecksumVerification && [TestImporter new].skipStockChecksums);
        NSString *active = [VMFirmwareStore activeDirectoryInRoot:testRoot];
        vm_fw_policy_t policy;
        assert(vm_fw_policy_read(active.fileSystemRepresentation, &policy) == 1 && policy.skip_stock_checksums);
        assert(policy.sizes[0] == 512 && [VMFirmwareStore hasPreparedFilesInDirectory:active]);
        [VMSettings.sharedSettings resetToDefaults];
        prepared.skipStockChecksums = NO;
        assert(!prepared.skipStockChecksums && ![TestImporter new].skipStockChecksums);
        // Changing the next import does not invalidate the current custom set.
        assert(vm_fw_policy_read(active.fileSystemRepresentation, &policy) == 1 && policy.skip_stock_checksums);
        assert([VMFirmwareStore hasPreparedFilesInDirectory:active]);
        [prepared importIPSWAtURL:url];
        waitForImport(prepared);
        assert(prepared.completions == 2 && !prepared.usedSkip && !prepared.skipStockChecksums);
        assert([fm fileExistsAtPath:active]); // Imported generations remain in the library.
        active = [VMFirmwareStore activeDirectoryInRoot:testRoot];
        assert(vm_fw_policy_read(active.fileSystemRepresentation, &policy) == 1 && !policy.skip_stock_checksums);
        assert(policy.sizes[0] == 7942144 && [VMFirmwareStore hasPreparedFilesInDirectory:active]);
        prepared.skipStockChecksums = YES;
        [prepared importIPSWAtURL:url];
        waitForImport(prepared);
        assert(prepared.completions == 3 && prepared.usedSkip && prepared.skipStockChecksums);
        assert([fm fileExistsAtPath:active]);
        active = [VMFirmwareStore activeDirectoryInRoot:testRoot];
        assert(vm_fw_policy_read(active.fileSystemRepresentation, &policy) == 1 && policy.skip_stock_checksums);
        policy.skip_stock_checksums = false;
        assert(vm_fw_policy_write(active.fileSystemRepresentation, &policy));
        assert(![VMFirmwareStore hasPreparedFilesInDirectory:active]); // Non-stock sizes require explicit policy.
        assert(prepared.canChangeChecksumVerification); // Damaged firmware can be replaced with either policy.
        prepared.skipStockChecksums = NO;
        assert(![VMFirmwareStore hasPreparedFilesInDirectory:active]);
        // A machine must retain its own firmware when the library default changes.
        NSString *library = [fixtureRoot stringByAppendingPathComponent:@"library/firmware"];
        NSString *machines = [fixtureRoot stringByAppendingPathComponent:@"library/Machines"];
        assert([fm createDirectoryAtPath:library withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert([fm createDirectoryAtPath:machines withIntermediateDirectories:YES attributes:nil error:NULL]);
        NSString *stock = [VMFirmwareStore createStagingDirectoryInRoot:library error:NULL];
        makeSet(stock);
        assert([VMFirmwareStore publishDirectory:stock inRoot:library error:NULL]);
        NSString *machineA = [machines stringByAppendingPathComponent:@"1111111111111111"];
        assert([fm createDirectoryAtPath:machineA withIntermediateDirectories:NO attributes:nil error:NULL]);
        // Leave this legacy machine unpinned until the next import migrates it.
        NSString *whited = [VMFirmwareStore createStagingDirectoryInRoot:library error:NULL];
        makeSet(whited);
        vm_fw_policy_t whitedPolicy = {false, {7942144, 40544, 567279616}};
        int rootFD = open([whited stringByAppendingPathComponent:@"rootfs.img"].fileSystemRepresentation, O_RDWR);
        assert(rootFD >= 0 && ftruncate(rootFD, 567279616) == 0 && close(rootFD) == 0);
        assert(vm_fw_policy_write(whited.fileSystemRepresentation, &whitedPolicy));
        assert([VMFirmwareStore firmwaresInRoot:library].count == 1); // Complete but unpublished.
        assert(![VMFirmwareStore selectFirmware:whited.lastPathComponent inRoot:library error:NULL]);
        assert([VMFirmwareStore publishDirectory:whited inRoot:library error:NULL]);
        assert([[VMFirmwareStore firmwareForMachine:machineA inRoot:library error:NULL] isEqual:stock]);
        assert([[VMFirmwareStore activeDirectoryInRoot:library] isEqual:whited]);
        assert([VMFirmwareStore firmwaresInRoot:library].count == 2);
        assert([[VMFirmwareStore displayNameInDirectory:whited] isEqual:@"Whited00r 7.1"]);
        NSString *machineB = [machines stringByAppendingPathComponent:@"2222222222222222"];
        assert([fm createDirectoryAtPath:machineB withIntermediateDirectories:NO attributes:nil error:NULL]);
        assert([VMFirmwareStore pinFirmware:whited.lastPathComponent forMachine:machineB inRoot:library error:NULL]);
        assert(![VMFirmwareStore pinFirmware:stock.lastPathComponent forMachine:machineB inRoot:library error:NULL]);
        assert(![VMFirmwareStore removeFirmware:stock.lastPathComponent inRoot:library error:NULL]);
        assert([VMFirmwareStore selectFirmware:stock.lastPathComponent inRoot:library error:NULL]);
        assert([[VMFirmwareStore firmwareForMachine:machineB inRoot:library error:NULL] isEqual:whited]);
        assert(![VMFirmwareStore removeFirmware:whited.lastPathComponent inRoot:library error:NULL]);
        // A malformed source must refuse rather than silently boot the default.
        NSString *record = [machineB stringByAppendingPathComponent:@"firmware-source.txt"];
        assert([@"../outside" writeToFile:record atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
        assert(![VMFirmwareStore firmwareForMachine:machineB inRoot:library error:NULL]);
        assert(![VMFirmwareStore removeFirmware:whited.lastPathComponent inRoot:library error:NULL]);
        // One damaged machine does not prevent selecting firmware for others.
        assert([VMFirmwareStore selectFirmware:whited.lastPathComponent inRoot:library error:NULL]);
        assert([[NSString stringWithContentsOfFile:record encoding:NSUTF8StringEncoding error:NULL] isEqual:@"../outside"]);
        assert([VMFirmwareStore selectFirmware:stock.lastPathComponent inRoot:library error:NULL]);
        assert([whited.lastPathComponent writeToFile:record atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
        assert([fm removeItemAtPath:machineB error:NULL]);
        assert([VMFirmwareStore removeFirmware:whited.lastPathComponent inRoot:library error:NULL]);
        assert([VMFirmwareStore firmwaresInRoot:library].count == 1 && [fm fileExistsAtPath:stock]);
        assert(![VMFirmwareStore removeFirmware:@"../outside" inRoot:library error:NULL]);
        testRoot = fixtureRoot;
        assert([fm removeItemAtPath:testRoot error:NULL]); // Only this test's UUID root.
        puts("firmware store: publication, rollback, legacy paths, cancellation and retained importer state passed");
    }
    return 0;
}
