//
//  S5LBox — application delegate.
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "AppDelegate.h"
#import "VMInstanceListViewController.h"

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
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
