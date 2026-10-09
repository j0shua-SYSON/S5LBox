#import <UIKit/UIKit.h>
@class VMUSBTransport;
@interface VMPackageManagerViewController : UITabBarController
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
@end
