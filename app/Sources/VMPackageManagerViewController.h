#import <UIKit/UIKit.h>
@class VMUSBTransport;
@interface VMPackageManagerViewController : UITableViewController
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
@end
