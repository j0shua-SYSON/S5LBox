// S5LBox -- controls for one open machine, never app/new-machine defaults.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <UIKit/UIKit.h>
#import "VMSnapshotListViewController.h"
@class VMUSBTransport;

typedef NS_ENUM(NSInteger, VMRuntimeAction) {
    VMRuntimeActionSaveAndClose,
    VMRuntimeActionShutDown,
    VMRuntimeActionRestart,
    VMRuntimeActionForcePowerOff,
};

@protocol VMRuntimeSettingsDelegate <VMSnapshotListDelegate>
- (BOOL)runtimeCanControlGuest;
- (BOOL)runtimeCanShutDown;
- (BOOL)runtimeCanForcePowerOff;
- (BOOL)runtimePaused;
- (void)setRuntimePaused:(BOOL)paused;
- (BOOL)runtimePausesInBackground;
- (void)setRuntimePausesInBackground:(BOOL)pauses;
- (BOOL)runtimeInlineConsole;
- (void)setRuntimeInlineConsole:(BOOL)enabled;
- (uint64_t)runtimeInstructionCap;
- (void)setRuntimeInstructionCap:(uint64_t)cap;
- (void)performRuntimeAction:(VMRuntimeAction)action;
@end

@interface VMRuntimeSettingsViewController : UITableViewController
@property (nonatomic, weak) id<VMRuntimeSettingsDelegate> runtimeDelegate;
@property (nonatomic, copy) NSString *machineName;
@property (nonatomic, copy) NSString *snapshotsDirectory;
@property (nonatomic) BOOL showsDeveloperControls;
@property (nonatomic, strong) VMUSBTransport *usbTransport;
@property (nonatomic, copy) NSString *instanceID;
@end
