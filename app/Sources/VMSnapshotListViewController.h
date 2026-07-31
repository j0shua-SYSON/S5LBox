//
//  VMSnapshotListViewController.h
//  S5LBox
//
//  The snapshots of one machine: a button that takes a new one, and the
//  existing ones newest first. Selecting one offers Open or Delete.
//
//  THIS SCREEN KNOWS NOTHING ABOUT THE EMULATOR. It reads the snapshots
//  directory through VMSnapshotStore and asks a delegate to do the two things
//  that need a running machine. That split is deliberate: taking a snapshot has
//  preconditions this screen cannot evaluate -- the guest must not be part-way
//  through a native uiomove continuation, and the block layer must be flushed
//  first -- and a view controller that thinks it can save state is a view
//  controller that will eventually do it at the wrong moment.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//

#import <UIKit/UIKit.h>

@class VMSnapshotListViewController;

/*
 * Both operations are asynchronous and report progress, because both move
 * roughly a hundred megabytes and iOS will not let the main thread stop for
 * that long. `fraction` is 0..1, or negative when the total is not yet known;
 * `stage` is a short human phrase for the label above the bar.
 *
 * Progress and completion are always delivered on the main queue -- the screen
 * touches UIKit in both, and a delegate that forgot would fail intermittently
 * rather than immediately, which is the worst way to find out.
 */
@protocol VMSnapshotListDelegate <NSObject>

@optional

/*
 * Optional because a delegate that cannot yet do these should say so rather
 * than stub them. The list checks -respondsToSelector: and reports "not
 * available" with a reason; a stub that silently succeeds would be worse than
 * an absent method, and one that silently fails would be worse still.
 */
- (void)snapshotList:(VMSnapshotListViewController *)list
    takeSnapshotWithProgress:(void (^)(double fraction, NSString *stage))progress
                  completion:(void (^)(BOOL ok, NSString *message))completion;

- (void)snapshotList:(VMSnapshotListViewController *)list
      openSnapshotId:(NSString *)snapshotId
            progress:(void (^)(double fraction, NSString *stage))progress
          completion:(void (^)(BOOL ok, NSString *message))completion;

/*
 * Whether opening is available at all, and if not, why. Answered by the
 * delegate rather than assumed here: restoring an older snapshot needs the
 * copy-on-write overlay layer, and until that exists the honest thing is a
 * disabled row with a reason rather than a button that corrupts a filesystem.
 */
- (NSString *)snapshotListOpenUnavailableReason:(VMSnapshotListViewController *)list;

/*
 * The same question for taking, and it exists because the answer that was
 * inferred here was WRONG. This class used to decide for itself: if the
 * delegate did not implement the take selector it reported "This machine is
 * not running, so there is nothing to save." That tests whether a METHOD
 * EXISTS and then announces a fact about the MACHINE, which are unrelated --
 * so a perfectly healthy running machine was told it was stopped, and the one
 * person who could act on the real reason was told a fiction instead.
 *
 * A view controller cannot know why saving is unavailable. The delegate can.
 */
- (NSString *)snapshotListTakeUnavailableReason:(VMSnapshotListViewController *)list;

@end

@interface VMSnapshotListViewController : UITableViewController

/* The machine's snapshots directory. Created on demand when one is taken. */
@property (nonatomic, copy) NSString *snapshotsDirectory;

@property (nonatomic, weak) id<VMSnapshotListDelegate> delegate;

@end
