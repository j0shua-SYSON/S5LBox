// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMStorageLease.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

@implementation VMStorageLease {
    int _descriptor;
    NSString *_directory;
}
- (instancetype)init { self=[super init]; if (self) _descriptor=-1; return self; }
+ (instancetype)acquireDirectory:(NSString *)directory error:(NSError **)error {
    VMStorageLease *lease=[self new]; lease->_directory=[directory copy];
    NSString *path=[directory stringByAppendingPathComponent:@"machine-storage.lock"];
    int fd=open(path.fileSystemRepresentation,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    struct stat st;
    BOOL valid=fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_nlink==1;
    if (!valid || flock(fd,LOCK_EX|LOCK_NB)) {
        if (fd>=0) close(fd);
        if (error) *error=[NSError errorWithDomain:@"S5LBox.Storage" code:EBUSY
            userInfo:@{NSLocalizedDescriptionKey:@"This machine is in use. Close its running app or shut down iPhone OS first."}];
        return nil;
    }
    lease->_descriptor=fd; return lease;
}
- (BOOL)beginGuestRun:(NSError **)error {
    if (_descriptor<0) return NO;
    NSString *path=[_directory stringByAppendingPathComponent:@"offline-storage.plist"];
    if (unlink(path.fileSystemRepresentation) && errno!=ENOENT) {
        if (error) *error=[NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil]; return NO;
    }
    int directory=open(_directory.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    BOOL ok=directory>=0 && !fsync(directory); int saved=errno;
    if (directory>=0) close(directory);
    if (!ok && error) *error=[NSError errorWithDomain:NSPOSIXErrorDomain code:saved userInfo:nil];
    return ok;
}
- (void)close { if (_descriptor>=0) { close(_descriptor); _descriptor=-1; } }
- (void)dealloc { [self close]; }
@end
