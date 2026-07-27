//
//  S5LBox — the emulated machine's physical buttons.
//
//  An iPhone 3G has five inputs that are not the screen: Home, Sleep/Wake, the
//  two volume keys and the ringer switch. This is a row of them.
//
//  The keys are LIVE now. core/src/soc/buttons.c models all five switches on
//  the GPIO pins and interrupt lines /device-tree/buttons names, so a press has
//  somewhere to go: VMEngine queues it and the emulator thread hands it to the
//  board. The bar still asks VMEngine for a reason rather than deciding for
//  itself, and +[VMEngine buttonUnavailableReason] returning nil is what lights
//  it up.
//
//  "Live" is a claim about the emulator, not about the guest. Whether anything
//  in the guest is listening is a separate question with its own answer in
//  -[VMEngine buttonUnavailableReason]; while this app runs the synthetic guest
//  in VMGuest.c the honest answer is that no driver has armed the button
//  interrupt lines, and the board refuses every press for that reason.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

#import "VMEngine.h"

@class VMButtonBar;

@protocol VMButtonBarDelegate <NSObject>
- (void)buttonBar:(VMButtonBar *)bar
  didChangeButton:(VMButton)button
          pressed:(BOOL)pressed;
@end

@interface VMButtonBar : UIView

/* Weak, as a delegate must be: the view controller owns the bar. */
@property (nonatomic, weak) id<VMButtonBarDelegate> delegate;

/*
 * nil enables every key. Any other value disables all of them and prints that
 * exact sentence under the row, so a dead control is never merely dim — it is
 * dim next to the reason it is dim.
 */
@property (nonatomic, copy) NSString *unavailableReason;

/* The height the bar needs: one row of keys plus the caption under them. The
 * view controller lays everything out with explicit frames, so it has to ask. */
+ (CGFloat)preferredHeight;

@end
