//
//  S5LBox — application delegate.
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "AppDelegate.h"
#import "VMSettings.h"
#import "VMInstanceListViewController.h"

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
    UINavigationController *nav = [[UINavigationController alloc]
        initWithRootViewController:[[VMInstanceListViewController alloc] init]];
    self.window.rootViewController = nav;
    [self.window makeKeyAndVisible];
    return YES;
}

@end
