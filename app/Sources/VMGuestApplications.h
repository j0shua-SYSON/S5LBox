// Installed-app discovery through the guest's installation_proxy. MIT licensed.
#import <Foundation/Foundation.h>
@class VMUSBTransport;
typedef NS_ENUM(NSUInteger, VMGuestApplicationsStage) {
    VMGuestApplicationsConnecting,
    VMGuestApplicationsReading,
};
@interface VMGuestApplication : NSObject
@property(nonatomic,readonly,copy) NSString *identifier,*name,*version;
@property(nonatomic,readonly,copy) NSString *containerID,*homeDirectory,*bundleDirectory;
// Rejects records outside a UUID-based installed user-app container.
+ (instancetype)fromInstallationRecord:(NSDictionary *)record;
@end
@interface VMGuestApplications : NSObject
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
@property(atomic) BOOL canceled;
// Configure before starting. Called on the discovery worker at real stage changes.
@property(nonatomic,copy) void (^progress)(VMGuestApplicationsStage stage);
// Worker thread. A fresh authoritative browse; never a host-side app mirror.
- (NSArray<VMGuestApplication *> *)installedApplications:(NSError **)error;
@end
