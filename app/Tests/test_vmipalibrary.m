// Foundation integration tests; no guest or user Documents directory is touched.
#import "VMIPALibrary.h"
#include <assert.h>

static NSURL *testDirectory;
@interface TestIPALibrary : VMIPALibrary
@end
@implementation TestIPALibrary
+ (NSURL *)directoryWithError:(NSError **)error {
    if (![NSFileManager.defaultManager createDirectoryAtURL:testDirectory
        withIntermediateDirectories:YES attributes:nil error:error]) return nil;
    return testDirectory;
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        assert(argc == 2);
        NSURL *root = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES];
        root = [root URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
        testDirectory = [root URLByAppendingPathComponent:@"IPAs" isDirectory:YES];
        NSError *error = nil;
        assert([TestIPALibrary directoryWithError:&error] && !error);
        assert([TestIPALibrary filesWithError:&error].count == 0);
        NSData *bytes = [@"fixture bytes; package validation is a separate layer" dataUsingEncoding:NSUTF8StringEncoding];
        NSURL *source = [root URLByAppendingPathComponent:@"Test.ipa"];
        assert([bytes writeToURL:source options:NSDataWritingAtomic error:&error]);
        NSURL *first = [TestIPALibrary importURL:source error:&error];
        assert(first && !error && [first.lastPathComponent isEqual:@"Test.ipa"]);
        assert([[NSData dataWithContentsOfURL:first] isEqual:bytes]);
        assert([[NSData dataWithContentsOfURL:source] isEqual:bytes]);
        assert([TestIPALibrary filesWithError:&error].count == 1);
        NSURL *second = [TestIPALibrary importURL:source error:&error];
        assert(second && ![second isEqual:first]);
        assert([TestIPALibrary filesWithError:&error].count == 2);
        assert([[NSData dataWithContentsOfURL:first] isEqual:bytes]);
        assert([[TestIPALibrary importURL:first error:&error] isEqual:first]);
        assert([TestIPALibrary filesWithError:&error].count == 2);
        NSURL *link = [testDirectory URLByAppendingPathComponent:@"link.ipa"];
        assert([NSFileManager.defaultManager createSymbolicLinkAtURL:link withDestinationURL:source error:&error]);
        assert([TestIPALibrary filesWithError:&error].count == 2);
        error = nil;
        assert(![TestIPALibrary importURL:link error:&error] && error);
        error = nil;
        NSURL *directory = [testDirectory URLByAppendingPathComponent:@"directory.ipa" isDirectory:YES];
        assert([NSFileManager.defaultManager createDirectoryAtURL:directory withIntermediateDirectories:NO attributes:nil error:&error]);
        assert([TestIPALibrary filesWithError:&error].count == 2);
        assert(![TestIPALibrary importURL:directory error:&error] && error);
        error = nil;
        assert(![TestIPALibrary importURL:[root URLByAppendingPathComponent:@"wrong.zip"] error:&error] && error);
        error = nil;
        NSURL *empty = [root URLByAppendingPathComponent:@"empty.ipa"];
        assert([NSData.data writeToURL:empty options:0 error:&error]);
        assert(![TestIPALibrary importURL:empty error:&error] && error);
        error = nil;
        // Test root is a unique directory created by this process, never Documents.
        assert([NSFileManager.defaultManager removeItemAtURL:root error:&error]);
        puts("IPA library: coordinated copy, source preservation, duplicate names, same-folder import, listing and refusals passed");
    }
    return 0;
}
