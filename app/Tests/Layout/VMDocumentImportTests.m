// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <XCTest/XCTest.h>
#import "VMDocumentPicker.h"
#import "VMImportedDocument.h"

@interface VMDocumentImportTests : XCTestCase
@end
@implementation VMDocumentImportTests
- (void)testPickerRequestsAnImportedCopy {
    UIDocumentPickerViewController *picker = VMCreateImportPicker();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    XCTAssertEqual(picker.documentPickerMode, UIDocumentPickerModeImport);
#pragma clang diagnostic pop
    XCTAssertFalse(picker.allowsMultipleSelection);
}
- (void)testCleanupOnlyRemovesTheSelectedTemporaryFile {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *temporary = [[NSURL fileURLWithPath:NSTemporaryDirectory()]
        URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    NSURL *documents = [fm URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *original = [documents URLByAppendingPathComponent:
        [NSUUID.UUID.UUIDString stringByAppendingPathExtension:@"ipa"]];
    XCTAssertTrue([fm createDirectoryAtURL:temporary withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSData *bytes = [@"original" dataUsingEncoding:NSUTF8StringEncoding];
    XCTAssertTrue([bytes writeToURL:original atomically:NO]);
    NSURL *copy = [temporary URLByAppendingPathComponent:@"copy.ipa"];
    XCTAssertTrue([bytes writeToURL:copy atomically:NO]);
    @autoreleasepool {
        __attribute__((objc_precise_lifetime)) VMImportedDocument *owned =
            [[VMImportedDocument alloc] initWithPickerURL:copy];
        XCTAssertEqualObjects(owned.url, copy);
        XCTAssertTrue([fm fileExistsAtPath:copy.path]);
    }
    XCTAssertFalse([fm fileExistsAtPath:copy.path]);
    // Originals and directories must survive even if passed accidentally.
    @autoreleasepool {
        __attribute__((objc_precise_lifetime)) VMImportedDocument *external =
            [[VMImportedDocument alloc] initWithPickerURL:original];
        __attribute__((objc_precise_lifetime)) VMImportedDocument *directory =
            [[VMImportedDocument alloc] initWithPickerURL:temporary];
        XCTAssertNotNil(external); XCTAssertNotNil(directory);
    }
    XCTAssertTrue([fm fileExistsAtPath:temporary.path]);
    XCTAssertEqualObjects([NSData dataWithContentsOfURL:original], bytes);
    // A symlink must never cause deletion of its target.
    XCTAssertTrue([fm createSymbolicLinkAtURL:copy withDestinationURL:original error:NULL]);
    @autoreleasepool {
        __attribute__((objc_precise_lifetime)) VMImportedDocument *link =
            [[VMImportedDocument alloc] initWithPickerURL:copy];
        XCTAssertNotNil(link);
    }
    XCTAssertTrue([fm fileExistsAtPath:copy.path]);
    XCTAssertEqualObjects([NSData dataWithContentsOfURL:original], bytes);
    XCTAssertTrue([fm removeItemAtURL:copy error:NULL]);
    XCTAssertTrue([bytes writeToURL:copy atomically:NO]);
    @autoreleasepool {
        __attribute__((objc_precise_lifetime)) VMImportedDocument *replaced =
            [[VMImportedDocument alloc] initWithPickerURL:copy];
        XCTAssertNotNil(replaced);
        XCTAssertTrue([fm moveItemAtURL:copy toURL:[temporary URLByAppendingPathComponent:@"old.ipa"] error:NULL]);
        XCTAssertTrue([bytes writeToURL:copy atomically:NO]);
    }
    XCTAssertTrue([fm fileExistsAtPath:copy.path]);
    XCTAssertTrue([fm removeItemAtURL:original error:NULL]);
    XCTAssertTrue([fm removeItemAtURL:temporary error:NULL]);
}
@end
