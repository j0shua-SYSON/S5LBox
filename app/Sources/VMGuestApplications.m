// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMGuestApplications.h"
#import "VMUSBLockdown.h"
#import "VMUSBService.h"
#include <math.h>
static NSError *AppsError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.GuestApps" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
}
static NSString *String(id value,NSUInteger limit) {
    if (![value isKindOfClass:NSString.class] || ![value length] || [value length]>limit ||
        [value rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location!=NSNotFound) return nil;
    return value;
}
@implementation VMGuestApplication
+ (instancetype)fromInstallationRecord:(NSDictionary *)record {
    if (![record isKindOfClass:NSDictionary.class]) return nil;
    NSString *identifier=String(record[@"CFBundleIdentifier"],255), *path=String(record[@"Path"],1023);
    NSCharacterSet *identifiers=[NSCharacterSet characterSetWithCharactersInString:
        @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-"];
    if (!identifier || [identifier rangeOfCharacterFromSet:identifiers.invertedSet].location!=NSNotFound || !path) return nil;
    if ([path hasPrefix:@"/private/"]) path=[path substringFromIndex:8];
    NSString *prefix=@"/var/mobile/Applications/";
    if (![path hasPrefix:prefix]) return nil;
    NSArray<NSString *> *parts=[[path substringFromIndex:prefix.length] componentsSeparatedByString:@"/"];
    if (parts.count!=2) return nil;
    NSString *uuid=parts[0],*bundle=parts[1];
    NSUUID *identity=[[NSUUID alloc] initWithUUIDString:uuid];
    if (uuid.length!=36 || !identity || [identity.UUIDString caseInsensitiveCompare:uuid]!=NSOrderedSame ||
        bundle.length<5 || bundle.length>255 || ![bundle hasSuffix:@".app"] || [bundle isEqual:@"..app"]) return nil;
    VMGuestApplication *app=[self new]; app->_identifier=[identifier copy];
    app->_name=[(String(record[@"CFBundleDisplayName"],256) ?: String(record[@"CFBundleName"],256) ?: identifier) copy];
    app->_version=[(String(record[@"CFBundleShortVersionString"],128) ?: String(record[@"CFBundleVersion"],128) ?: @"") copy];
    app->_containerID=[uuid copy]; app->_bundleDirectory=[bundle copy];
    app->_homeDirectory=[prefix stringByAppendingString:uuid];
    return app;
}
@end
@implementation VMGuestApplications {
    VMUSBTransport *_transport;
    NSString *_identifier;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier {
    self=[super init]; if (self) { _transport=transport; _identifier=[identifier copy]; } return self;
}
- (VMUSBService *)connect:(NSError **)error {
    if (NSThread.isMainThread || self.canceled) { if (error) *error=AppsError(@"App discovery requires an active worker."); return nil; }
    __weak VMGuestApplications *weakSelf=self;
    VMUSBLockdown *lockdown=[VMUSBLockdown openWithTransport:_transport instanceID:_identifier
        canceled:^BOOL{ return !weakSelf || weakSelf.canceled; }
        deadline:NSProcessInfo.processInfo.systemUptime+120 error:error];
    VMUSBService *service=[lockdown openService:@"com.apple.mobile.installation_proxy" error:error];
    [lockdown close];
    [service setOperationDeadline:NSProcessInfo.processInfo.systemUptime+120];
    return service;
}
- (NSArray<VMGuestApplication *> *)installedApplications:(NSError **)error {
    if (self.progress) self.progress(VMGuestApplicationsConnecting);
    VMUSBService *service=[self connect:error]; if (!service) return nil;
    @try {
        if (self.progress) self.progress(VMGuestApplicationsReading);
        if (![service sendPlist:@{@"Command":@"Browse",@"ClientOptions":@{@"ApplicationType":@"User"}} error:error]) return nil;
        NSMutableArray *apps=[NSMutableArray array]; NSMutableSet *identifiers=[NSMutableSet set];
        NSUInteger records=0;
        for (unsigned page=0;page<256;page++) {
            if (self.canceled) { if (error) *error=AppsError(@"App discovery canceled."); return nil; }
            NSDictionary *reply=[service receivePlist:error]; if (!reply) return nil;
            if (reply[@"Error"]) { if (error) *error=AppsError(@"The guest could not list its installed apps."); return nil; }
            id list=reply[@"CurrentList"];
            if (list && ![list isKindOfClass:NSArray.class]) { if (error) *error=AppsError(@"Invalid installed-app response."); return nil; }
            NSUInteger count=[list count];
            if (count>4096-records) { if (error) *error=AppsError(@"The installed-app list exceeded its safety limit."); return nil; }
            id amount=reply[@"CurrentAmount"];
            if (amount && (![amount isKindOfClass:NSNumber.class] || !isfinite([amount doubleValue]) ||
                [amount doubleValue]!=(double)count)) { if (error) *error=AppsError(@"Incomplete installed-app list."); return nil; }
            records+=count;
            for (id record in list) {
                VMGuestApplication *app=[VMGuestApplication fromInstallationRecord:record];
                if (!app) continue; // System/non-container apps are not launch targets.
                if ([identifiers containsObject:app.identifier]) { if (error) *error=AppsError(@"The guest returned duplicate app identities. Refresh and retry."); return nil; }
                [identifiers addObject:app.identifier]; [apps addObject:app];
            }
            if ([reply[@"Status"] isEqual:@"Complete"]) return [apps sortedArrayUsingComparator:^NSComparisonResult(VMGuestApplication *a,VMGuestApplication *b) {
                NSComparisonResult order=[a.name localizedCaseInsensitiveCompare:b.name];
                return order==NSOrderedSame?[a.identifier compare:b.identifier]:order;
            }];
        }
        if (error) *error=AppsError(@"The guest did not complete its installed-app list."); return nil;
    } @finally { [service close]; }
}
@end
