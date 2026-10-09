// S5LBox -- install an app in a stopped guest. MIT licensed.
#import <UIKit/UIKit.h>
@class VMUSBTransport;

@interface VMIPAInstallViewController : UITableViewController
- (instancetype)initWithInstanceID:(NSString *)identifier machineName:(NSString *)name;
- (void)inspectURL:(NSURL *)url;
/* Set before inspection to use the running guest's installer, never HFS edits. */
@property (nonatomic, strong) VMUSBTransport *liveTransport;
/* Machines owns shutdown/navigation. The package is inspected before this is
 * called. Re-present this controller only after a verified guest shutdown. */
@property (nonatomic, copy) void (^prepareHandler)(void);
@property (nonatomic, copy) void (^readyHandler)(void);
@end
