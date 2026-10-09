//
//  S5LBox — app settings, presented from Machines only.
//  Owns app-wide defaults, new-machine graphics, firmware import and the
//  stopped-machine jailbreak entry. VMRuntimeSettingsViewController owns
//  controls for an open session. Changes here persist immediately.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

@interface VMSettingsViewController : UITableViewController

/*
 * Supplied only by the Machines screen, where every emulator is stopped.
 * Settings dismisses itself before invoking the request; disk replacement is
 * never attempted from a live emulator screen.
 */
@property (nonatomic, copy, nullable) void (^guestInstallRequest)(
    NSString *instanceID, NSString *machineName);
@property (nonatomic, copy, nullable) void (^guestIPARequest)(
    NSString *instanceID, NSString *machineName);

@end
