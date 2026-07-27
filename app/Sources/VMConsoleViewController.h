//
//  S5LBox — the guest console, on its own screen.
//
//  The emulator screen used to carry the guest's serial output underneath it,
//  which meant the first thing anyone saw was a wall of kernel logging beneath
//  a small picture. The logs are the most valuable thing in the app to about
//  one person in fifty, and the least useful thing to everybody else, so they
//  now live here and the emulator screen is a screen.
//
//  Nothing is lost by moving them: this view is fed the same accumulated text
//  the emulator screen already keeps, and appears in developer mode.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface VMConsoleViewController : UIViewController

/* The text to show. Set before presenting; -appendText: adds to it while the
 * screen is up, so a console left open keeps filling. */
@property (nonatomic, copy) NSString *text;
- (void)appendText:(NSString *)more;

@end

NS_ASSUME_NONNULL_END
