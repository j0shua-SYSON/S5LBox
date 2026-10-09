// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMIPALibrary.h"

static void LibraryError(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"com.j0shua.S5LBox.IPALibrary"
        code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
}

@implementation VMIPALibrary
+ (NSURL *)directoryWithError:(NSError **)error {
    NSFileManager *manager = NSFileManager.defaultManager;
    NSURL *documents = [manager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    if (!documents) { LibraryError(error, @"The app's Documents folder is unavailable."); return nil; }
    NSURL *folder = [documents URLByAppendingPathComponent:@"IPAs" isDirectory:YES];
    NSDictionary *attributes = [manager attributesOfItemAtPath:folder.path error:NULL];
    if (attributes) {
        if (![attributes[NSFileType] isEqual:NSFileTypeDirectory]) {
            LibraryError(error, @"IPAs must be a folder, not a file or link. Rename the conflicting item in Files.");
            return nil;
        }
    } else if (![manager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:error]) return nil;
    return folder;
}
+ (NSArray<NSURL *> *)filesWithError:(NSError **)error {
    NSURL *folder = [self directoryWithError:error];
    if (!folder) return nil;
    NSArray<NSURL *> *files = [NSFileManager.defaultManager contentsOfDirectoryAtURL:folder
        includingPropertiesForKeys:@[NSURLIsRegularFileKey, NSURLFileSizeKey]
        options:NSDirectoryEnumerationSkipsHiddenFiles error:error];
    if (!files) return nil;
    NSMutableArray<NSURL *> *ipas = [NSMutableArray array];
    for (NSURL *file in files) {
        if ([file.pathExtension caseInsensitiveCompare:@"ipa"] != NSOrderedSame) continue;
        NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:file.path error:NULL];
        if ([attributes[NSFileType] isEqual:NSFileTypeRegular]) [ipas addObject:file];
    }
    return [ipas sortedArrayUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        return [a.lastPathComponent localizedStandardCompare:b.lastPathComponent];
    }];
}
+ (NSURL *)importURL:(NSURL *)url error:(NSError **)error {
    if (!url.isFileURL || [url.pathExtension caseInsensitiveCompare:@"ipa"] != NSOrderedSame) {
        LibraryError(error, @"Choose a file ending in .ipa."); return nil;
    }
    NSURL *folder = [self directoryWithError:error];
    if (!folder) return nil;
    BOOL scoped = [url startAccessingSecurityScopedResource];
    __block NSURL *destination = nil;
    __block NSError *copyError = nil;
    NSError *accessError = nil;
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateReadingItemAtURL:url options:NSFileCoordinatorReadingWithoutChanges
        error:&accessError byAccessor:^(NSURL *readable) {
        NSFileManager *manager = NSFileManager.defaultManager;
        NSDictionary *attributes = [manager attributesOfItemAtPath:readable.path error:&copyError];
        if (![attributes[NSFileType] isEqual:NSFileTypeRegular] || ![attributes fileSize] ||
            [attributes fileSize] > 512ull * 1024ull * 1024ull) {
            LibraryError(&copyError, @"Choose a regular IPA file no larger than 512 MB."); return;
        }
        if ([readable.URLByDeletingLastPathComponent.URLByStandardizingPath isEqual:folder.URLByStandardizingPath]) {
            destination = readable; return;
        }
        NSString *name = url.lastPathComponent;
        if (name.length > 180) name = [[name substringToIndex:176] stringByAppendingString:@".ipa"];
        NSURL *target = [folder URLByAppendingPathComponent:name];
        if ([manager fileExistsAtPath:target.path]) {
            name = [NSString stringWithFormat:@"%@-%@.ipa", name.stringByDeletingPathExtension, NSUUID.UUID.UUIDString];
            target = [folder URLByAppendingPathComponent:name];
        }
        NSURL *partial = [folder URLByAppendingPathComponent:[@".import-" stringByAppendingString:NSUUID.UUID.UUIDString]];
        if ([manager copyItemAtURL:readable toURL:partial error:&copyError]) {
            NSDictionary *copied = [manager attributesOfItemAtPath:partial.path error:&copyError];
            if ([copied fileSize] != [attributes fileSize]) LibraryError(&copyError, @"The file changed while it was being imported. Try again.");
            else if ([manager moveItemAtURL:partial toURL:target error:&copyError]) destination = target;
        }
        // Only our private partial name is removed. Existing library files stay.
        if (!destination) [manager removeItemAtURL:partial error:NULL];
    }];
    if (scoped) [url stopAccessingSecurityScopedResource];
    if (!destination && error) {
        *error = accessError ?: copyError;
        if (!*error) LibraryError(error, @"Files could not provide this IPA. Download it locally and try again.");
    }
    return destination;
}
@end
