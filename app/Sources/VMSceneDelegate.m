// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "AppDelegate.h"

@interface VMSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation VMSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
    options:(UISceneConnectionOptions *)connectionOptions {
    (void)session; (void)connectionOptions;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    AppDelegate *app = (AppDelegate *)UIApplication.sharedApplication.delegate;
    [app connectWindowScene:(UIWindowScene *)scene];
    self.window = app.window;
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    AppDelegate *app = (AppDelegate *)UIApplication.sharedApplication.delegate;
    [app applicationDidBecomeActive:UIApplication.sharedApplication];
}
// UIKit still posts the aggregate UIApplication lifecycle notifications used
// by the engine, audio and importers. Do not post duplicates from this scene.
@end
