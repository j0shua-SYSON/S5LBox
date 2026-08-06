//
//  S5LBox — application delegate.
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "AppDelegate.h"
#import "EmulatorViewController.h"
#import "VMDeviceAutomation.h"
#import "VMSettings.h"
#import "VMInstanceListViewController.h"

static NSString *const kAutomationOpenFirstMachineArgument =
    @"--s5lbox-automation-open-first-machine";

static BOOL VMLaunchRequestsFirstMachine(void) {
    return [[[NSProcessInfo processInfo] arguments]
        containsObject:kAutomationOpenFirstMachineArgument];
}

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

@interface AppDelegate ()
@property (strong, nonatomic) VMDeviceAutomation *deviceAutomation;
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

    (void)launchOptions;
    BOOL automationRequested = VMLaunchRequestsFirstMachine();
    /* A long phone profile must not turn into a lock-screen measurement half
     * way through. This lasts only for the explicitly automated app process;
     * ordinary launches keep the user's normal Auto-Lock setting. */
    if (automationRequested) application.idleTimerDisabled = YES;

    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    /* Keep the shell native rather than painting a second design system over
     * UIKit.  One explicit accent and large navigation titles are enough to
     * make the app read like a current iOS utility, while semantic system
     * colours continue to follow light mode, dark mode and increased contrast. */
    self.window.tintColor = [UIColor systemBlueColor];
    /* The machine list is the root now, with the emulator pushed on top of it.
     * A navigation stack rather than a modal presentation because going back
     * to the list is the common action once there is more than one machine. */
    VMInstanceListViewController *machines =
        [[VMInstanceListViewController alloc] init];
    UINavigationController *nav = [[VMNavigationController alloc]
        initWithRootViewController:machines];
    nav.navigationBar.prefersLargeTitles = YES;
    self.window.rootViewController = nav;
    [self.window makeKeyAndVisible];

    /*
     * Device profiling needs a deterministic way to reach the running guest.
     * This exact argument is opt-in and uses the same list-controller method as
     * a real row tap. Dispatching once lets the root finish appearing before a
     * no-animation push; normal launches remain on the machine list.
     */
    if (automationRequested) {
        dispatch_async(dispatch_get_main_queue(), ^{
            BOOL opened = [machines openFirstMachineForAutomation];
            NSLog(@"[automation] open first machine: %@",
                  opened ? @"started" : @"refused");
            if (opened) {
                self.deviceAutomation = [[VMDeviceAutomation alloc]
                    initWithNavigationController:nav machineList:machines];
                [self.deviceAutomation start];
            }
        });
    }
    return YES;
}

@end
