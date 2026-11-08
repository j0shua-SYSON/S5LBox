// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>
#import "VMGuestApplications.h"
#import "VMTouchHLEFiles.h"
#include "../../integrations/touchhle-cpu/s5lbox_hle.h"
#if S5LBOX_TOUCHHLE
@class VMIPAPackage;
@interface VMTouchHLEOfflineFiles : NSObject <VMTouchHLEFileChannel>
// Worker-thread operations. No VMEngine, virtual USB or guest boot is used.
- (instancetype)initWithInstanceID:(NSString *)identifier error:(NSError **)error;
- (NSArray<VMGuestApplication *> *)installedApplications:(NSError **)error;
- (BOOL)selectApplication:(VMGuestApplication *)application error:(NSError **)error;
- (NSData *)iconForApplication:(VMGuestApplication *)application;
- (BOOL)installPackage:(VMIPAPackage *)package error:(NSError **)error;
- (s5lbox_hle_live_config)configuration;
// Flushes changes, then releases exclusive disk ownership. Inspect error even
// when the interpreted app exited successfully: failed saves must stay visible.
- (void)close;
@property(nonatomic,readonly) NSError *error;
@end
#endif
