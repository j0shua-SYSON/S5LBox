// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <UIKit/UIKit.h>
@class VMUSBTransport;
@interface VMTouchHLECoordinator : NSObject
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier presenter:(UIViewController *)presenter;
// Main thread; safe to call every display tick. Internally rate limited.
- (void)tick:(BOOL)canRoute guestAwake:(BOOL)guestAwake;
- (void)invalidate;
@end
@interface VMTouchHLEAppsViewController : UITableViewController
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
@end
#if S5LBOX_TOUCHHLE
// Separate Apps tab. Its run controllers do not construct a VMEngine.
@interface VMTouchHLEOfflineAppsViewController : UITableViewController
@property(nonatomic,copy) void (^openMachine)(NSString *identifier);
@end
#endif
