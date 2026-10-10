// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <UIKit/UIKit.h>
#import "VMRuntimeSettingsViewController.h"
#import "VMEngine.h"

@protocol VMDeviceControlsDelegate <VMRuntimeSettingsDelegate>
- (NSString *)deviceButtonUnavailableReason;
- (BOOL)deviceSilent;
- (void)setDeviceButton:(VMButton)button pressed:(BOOL)pressed;
@end

@interface VMDeviceControlsViewController : UIViewController
@property (nonatomic, weak) id<VMDeviceControlsDelegate> controlDelegate;
@end
