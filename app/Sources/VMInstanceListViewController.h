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
@end
