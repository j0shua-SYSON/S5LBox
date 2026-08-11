//
//  S5LBox — the settings screen.
//
//  A grouped table of every option the emulator has, in the same order and
//  under the same headings the desktop harness prints them, driven by the C
//  table in VMOptions.c so the two cannot drift.
//
//  The screen leads with what it cannot do. The app boots no firmware, so none
//  of the switches change the machine on the previous screen; the banner above
//  the first section says exactly that, the switches are tinted grey rather
//  than the green that means "on and working", and the two settings that ARE
//  applied are grouped separately and labelled as applied. The firmware rows
//  are read-only and say "not supplied", because the app ships none, bundles
//  none, and downloads none.
//
//  Present it modally inside a UINavigationController; it installs its own Done
//  button. Everything it changes is written through to NSUserDefaults
//  immediately and announced with VMSettingsDidChangeNotification, so there is
//  nothing to save and no dismissal to intercept.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

@class VMSnapshotListViewController;
@protocol VMSnapshotListDelegate;

@interface VMSettingsViewController : UITableViewController

/*
 * Supplied by whoever presents this screen, because Settings is built with a
 * plain -init and owns no machine. Both are passed straight through to the
 * snapshots list; a nil directory means nobody said which machine, and the
 * list then shows nothing rather than guessing at one.
 */
@property (nonatomic, copy) NSString *snapshotsDirectory;
@property (nonatomic, weak) id<VMSnapshotListDelegate> snapshotDelegate;

/*
 * Supplied only by the Machines screen, where every emulator is stopped.
 * Settings dismisses itself before invoking the request; disk replacement is
 * never attempted from a live emulator screen.
 */
@property (nonatomic, copy, nullable) void (^guestInstallRequest)(
    NSString *instanceID, NSString *machineName);

@end
