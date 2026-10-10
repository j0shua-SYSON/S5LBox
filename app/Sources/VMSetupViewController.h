#import <UIKit/UIKit.h>
NS_ASSUME_NONNULL_BEGIN
@interface VMSetupViewController : UIViewController
@property (nonatomic, copy, nullable) void (^completion)(BOOL startMachine);
@end
NS_ASSUME_NONNULL_END
