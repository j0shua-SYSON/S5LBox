// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>

// One open-file-description lock per machine. Unlike process-owned fcntl
// locks, flock also excludes two engines/backends in the same app process.
@interface VMStorageLease : NSObject
+ (instancetype)acquireDirectory:(NSString *)directory error:(NSError **)error;
- (void)close;
// The offline witness is revoked before a guest can mount the disk. A stopped
// guest must then supply a new, fully validated powered-off checkpoint.
- (BOOL)beginGuestRun:(NSError **)error;
@end
