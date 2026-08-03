//
//  S5LBox — the manual.
//
//  The settings screen is a faithful mirror of the desktop harness's option
//  table: sixteen rows, each titled with a device-tree path, each carrying a
//  paragraph about what it does to a kernel. That is the right screen for
//  somebody bisecting a boot and the wrong one to meet first, and there was
//  nothing else to read.
//
//  This is the something else. It answers, in order, the questions a person
//  actually arrives with: what is this, what does it do right now, why is it
//  showing me a synthetic guest, what are all those switches, and what would I
//  need to run the real thing.
//
//  It states limits plainly rather than in the margins. Somebody who installs
//  an emulator and finds a test pattern instead of iPhone OS should learn why
//  from the app, not from a GitHub README they have not opened.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

@interface VMManualViewController : UITableViewController
@end
