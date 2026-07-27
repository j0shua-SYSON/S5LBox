//
//  iOS3-VM — the emulated machine's physical buttons.
//
//  An iPhone 3G has five inputs that are not the screen: Home, Sleep/Wake, the
//  two volume keys and the ringer switch. This is a row of them.
//
//  It is a row of DISABLED keys, and it says so underneath, because nothing
//  behind them exists: core/ models no PMU button line and no ringer GPIO, so
//  a press has nowhere to go. The bar asks VMEngine for the reason rather than
//  deciding it — +[VMEngine buttonUnavailableReason] returning nil is the single
//  edit that makes every key here live.
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
