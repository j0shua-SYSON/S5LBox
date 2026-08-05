//
//  S5LBox — the machine list screen. The app's root.
//
//  A table of configured machines: tap one to open it, swipe for rename,
//  duplicate and delete, plus at the top of the screen the one sentence that
//  matters most about this app — whether these machines can boot Apple's
//  firmware yet. The list is a real list; what it lists is a synthetic guest.
//  Saying so on the first screen is cheaper than letting somebody create six
//  machines before finding out.
//
//  It owns no state. VMInstanceStore holds the list, VMInstances.c holds every
//  rule about it, and this class turns rows into cells and refusals into
//  alerts.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

@interface VMInstanceListViewController : UITableViewController

/*
 * A deliberately narrow device-automation hook. Normal launches and taps do
 * not call it. It opens the first configured machine through the same path as
 * -tableView:didSelectRowAtIndexPath:, without inventing a second boot path.
 *
 * Returns NO when the list is not the visible navigation root or no machine is
 * available. The caller can then report a failed automation setup rather than
 * claiming that a guest was started.
 */
- (BOOL)openFirstMachineForAutomation;

@end
