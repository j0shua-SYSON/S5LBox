#import <UIKit/UIKit.h>
@class VMUSBTransport;
@interface VMPackageManagerViewController : UITabBarController
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
@property (nonatomic, copy) void (^setupRequest)(void);
@end
