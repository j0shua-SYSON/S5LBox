// Guest-managed installation through virtual USB. Worker-thread only.
#import <Foundation/Foundation.h>
@class VMUSBTransport;
@interface VMUSBInstaller : NSObject
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
@property (atomic) BOOL canceled;
@property (nonatomic, copy) void (^progress)(NSString *message, double fraction);
// Returns YES only for installation_proxy Status=Complete. No disk-image writes.
- (BOOL)installURL:(NSURL *)url error:(NSError **)error;
@end
