//
//  S5LBox — application delegate.
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "AppDelegate.h"
#import "EmulatorViewController.h"
#import "VMSettings.h"
#import "VMInstanceListViewController.h"

/*
 * iOS 26's content-wide interactive pop gesture is not an edge gesture.  A
 * rightward drag anywhere over the guest can therefore pop the emulator and
 * tear its machine down.  EmulatorViewController disables both public pop
 * recognizers while it is visible, but UIKit owns those recognizers and may
 * replace or re-enable one during a navigation transition.
 *
 * This navigation-level guard is independent of the enabled flags.  Both
 * system pop recognizers must wait for this non-cancelling pan recognizer to
 * fail.  It recognizes whenever the emulator is the top controller, so a
 * guest pan wins and the system pop fails.  cancelsTouchesInView=NO and the
 * simultaneous-recognition delegate preserve the raw touches delivered to the
 * framebuffer.  On every other screen the guard refuses to begin, preserving
 * normal interactive navigation there.
 */
static UIGestureRecognizer *VMNavigationContentPopGestureRecognizer(
        UINavigationController *navigationController) {
    SEL selector = NSSelectorFromString(@"interactiveContentPopGestureRecognizer");
    if (![navigationController respondsToSelector:selector]) return nil;
    return [navigationController valueForKey:NSStringFromSelector(selector)];
}

@interface VMNavigationController : UINavigationController
    <UIGestureRecognizerDelegate>
- (void)configureGuestPanGuard;
- (void)guestPanGuardChanged:(UIPanGestureRecognizer *)gesture;
@end

@implementation VMNavigationController {
    UIPanGestureRecognizer *_guestPanGuard;
    __weak UIGestureRecognizer *_configuredEdgePopGesture;
    __weak UIGestureRecognizer *_configuredContentPopGesture;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    _guestPanGuard = [[UIPanGestureRecognizer alloc]
        initWithTarget:self action:@selector(guestPanGuardChanged:)];
    _guestPanGuard.delegate = self;
    _guestPanGuard.cancelsTouchesInView = NO;
    _guestPanGuard.delaysTouchesBegan = NO;
    _guestPanGuard.delaysTouchesEnded = NO;
    [self.view addGestureRecognizer:_guestPanGuard];
    [self configureGuestPanGuard];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self configureGuestPanGuard];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    /* Catch a recognizer that UIKit installs or replaces after a transition. */
    [self configureGuestPanGuard];
}

- (void)configureGuestPanGuard {
    if (!_guestPanGuard) return;

    UIGestureRecognizer *edge = self.interactivePopGestureRecognizer;
    UIGestureRecognizer *content =
        VMNavigationContentPopGestureRecognizer(self);

    if (edge && edge != _configuredEdgePopGesture) {
        [edge requireGestureRecognizerToFail:_guestPanGuard];
        _configuredEdgePopGesture = edge;
    }
    if (content && content != edge &&
        content != _configuredContentPopGesture) {
        [content requireGestureRecognizerToFail:_guestPanGuard];
        _configuredContentPopGesture = content;
    }
}

- (void)guestPanGuardChanged:(UIPanGestureRecognizer *)gesture {
    (void)gesture;
}

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gestureRecognizer {
    if (gestureRecognizer != _guestPanGuard) return YES;
    return [self.topViewController
        isKindOfClass:[EmulatorViewController class]];
}

- (BOOL)       gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
 shouldRecognizeSimultaneouslyWithGestureRecognizer:
                 (UIGestureRecognizer *)otherGestureRecognizer {
    return gestureRecognizer == _guestPanGuard ||
           otherGestureRecognizer == _guestPanGuard;
}

@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    /*
     * Before anything else, and specifically before the window exists: Files
     * shows Documents the moment the app is installed, and a Documents with
     * nothing in it is an empty folder. A user told to drop an IPSW in the
     * firmware folder must be able to FIND a firmware folder.
     */
    [[VMSettings sharedSettings] ensureUserVisibleDirectories];

    (void)application; (void)launchOptions;
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    /* The machine list is the root now, with the emulator pushed on top of it.
     * A navigation stack rather than a modal presentation because going back
     * to the list is the common action once there is more than one machine. */
    UINavigationController *nav = [[VMNavigationController alloc]
        initWithRootViewController:[[VMInstanceListViewController alloc] init]];
    self.window.rootViewController = nav;
    [self.window makeKeyAndVisible];
    return YES;
}

@end
