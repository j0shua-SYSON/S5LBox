// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMImportedDocument.h"
#include <sys/stat.h>
#include <unistd.h>

@implementation VMImportedDocument {
    NSString *_cleanupPath;
    dev_t _device;
    ino_t _inode;
}
- (instancetype)initWithPickerURL:(NSURL *)url {
    if (!(self = [super init])) return nil;
    _url = url;
    if (!url.isFileURL) return self;
    NSString *path = url.path.stringByStandardizingPath;
    struct stat info;
    if (lstat(path.fileSystemRepresentation, &info) || !S_ISREG(info.st_mode)) return self;
    path = path.stringByResolvingSymlinksInPath;
    NSString *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
        inDomains:NSUserDomainMask].firstObject.path;
    NSArray<NSString *> *roots = @[NSTemporaryDirectory(),
        [(documents ?: @"") stringByAppendingPathComponent:@"Inbox"]];
    for (NSString *root in roots) {
        if (![root isAbsolutePath]) continue;
        NSString *prefix = [root.stringByStandardizingPath.stringByResolvingSymlinksInPath
            stringByAppendingString:@"/"];
        if ([path hasPrefix:prefix]) {
            _cleanupPath = path;
            _device = info.st_dev;
            _inode = info.st_ino;
            break;
        }
    }
    return self;
}
- (void)dealloc {
    if (!_cleanupPath) return;
    struct stat info;
    // A replaced path, link or directory is not the selected temporary file.
    if (!lstat(_cleanupPath.fileSystemRepresentation, &info) && S_ISREG(info.st_mode) &&
        info.st_dev == _device && info.st_ino == _inode)
        unlink(_cleanupPath.fileSystemRepresentation);
}
@end
