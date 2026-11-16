// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <XCTest/XCTest.h>

@interface VMIPadInterfaceTests : XCTestCase
@end
@implementation VMIPadInterfaceTests
- (void)dismissMicrophonePrompt:(XCUIApplication *)system timeout:(NSTimeInterval)timeout {
    // Match this permission specifically: another system alert may also have
    // an Allow button. A synthesized tap can finish before dismissal does.
    XCUIElement *prompt = [system.alerts containingPredicate:[NSPredicate
        predicateWithFormat:@"label CONTAINS[c] %@ AND label CONTAINS[c] %@",
        @"S5LBox", @"microphone"]].firstMatch;
    if (![prompt waitForExistenceWithTimeout:timeout]) return;
    for (NSUInteger attempt = 0; attempt < 3 && prompt.exists; attempt++) {
        XCUIElement *allow = prompt.buttons[@"Allow"];
        XCTNSPredicateExpectation *ready = [[XCTNSPredicateExpectation alloc]
            initWithPredicate:[NSPredicate predicateWithFormat:@"hittable == YES"] object:allow];
        XCTAssertEqual([XCTWaiter waitForExpectations:@[ready] timeout:10], XCTWaiterResultCompleted);
        [allow tap];
        XCTNSPredicateExpectation *dismissed = [[XCTNSPredicateExpectation alloc]
            initWithPredicate:[NSPredicate predicateWithFormat:@"exists == NO"] object:prompt];
        if ([XCTWaiter waitForExpectations:@[dismissed] timeout:5] == XCTWaiterResultCompleted) return;
    }
    XCTAssertFalse(prompt.exists, @"Microphone permission must be resolved before testing the app layout.");
}
- (void)testSetupRotationImportAndSettings {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [XCUIApplication new];
    [app launch];
    // Exercise the real first-run permission prompt instead of trying to
    // override a typed NSNumber preference with an NSString launch argument.
    XCUIApplication *system = [[XCUIApplication alloc] initWithBundleIdentifier:@"com.apple.springboard"];
    [self dismissMicrophonePrompt:system timeout:10];
    for (NSNumber *orientation in @[@(UIDeviceOrientationPortrait),@(UIDeviceOrientationLandscapeLeft),
                                    @(UIDeviceOrientationPortraitUpsideDown),@(UIDeviceOrientationLandscapeRight)]) {
        XCUIDevice.sharedDevice.orientation = orientation.integerValue;
        // Permission presentation may arrive after the initial launch wait.
        [self dismissMicrophonePrompt:system timeout:1];
        XCUIElement *import = app.buttons[@"s5lbox.setup.import"];
        XCTAssertTrue([import waitForExistenceWithTimeout:15]);
        // Existence can precede the end of the rotation/presentation animation.
        XCTNSPredicateExpectation *ready = [[XCTNSPredicateExpectation alloc]
            initWithPredicate:[NSPredicate predicateWithFormat:@"hittable == YES"] object:import];
        XCTWaiterResult result = [XCTWaiter waitForExpectations:@[ready] timeout:10];
        XCTAttachment *picture = [XCTAttachment attachmentWithScreenshot:XCUIScreen.mainScreen.screenshot];
        picture.name = [NSString stringWithFormat:@"Setup-%@",orientation];
        picture.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:picture];
        XCTAssertEqual(result,XCTWaiterResultCompleted,@"%@",app.debugDescription);
        XCTAssertTrue(app.buttons[@"s5lbox.setup.primary"].hittable);
        CGRect window = app.windows.firstMatch.frame;
        BOOL landscape = orientation.integerValue == UIDeviceOrientationLandscapeLeft ||
                         orientation.integerValue == UIDeviceOrientationLandscapeRight;
        XCTAssertEqual(window.size.width > window.size.height,landscape);
        XCTAssertTrue(CGRectContainsRect(window,import.frame));
        XCTAssertTrue(CGRectContainsRect(window,app.buttons[@"s5lbox.setup.primary"].frame));
        XCTAssertTrue(app.buttons[@"Later"].hittable);
        XCTAssertTrue(app.buttons[@"Settings"].hittable);
    }
    [app.buttons[@"s5lbox.setup.import"] tap];
    XCUIElement *cancel = app.buttons[@"Cancel"].firstMatch;
    // Files starts in a separate process. On a cold simulator its remote
    // accessibility tree can arrive well after the picker is presented.
    XCTAssertTrue([cancel waitForExistenceWithTimeout:30], @"%@", app.debugDescription);
    XCTNSPredicateExpectation *pickerReady = [[XCTNSPredicateExpectation alloc]
        initWithPredicate:[NSPredicate predicateWithFormat:@"hittable == YES"] object:cancel];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[pickerReady] timeout:10], XCTWaiterResultCompleted);
    [cancel tap];
    XCTAssertTrue([app.buttons[@"Later"] waitForExistenceWithTimeout:10]);
    [app.buttons[@"Later"] tap];
    XCUIElement *settings = app.buttons[@"s5lbox.machines.settings"];
    XCTAssertTrue([settings waitForExistenceWithTimeout:10]); [settings tap];
    XCTAssertTrue([app.navigationBars[@"App Settings"] waitForExistenceWithTimeout:10]);
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationPortrait;
    XCUIElement *advanced = app.staticTexts[@"Advanced"];
    if (!advanced.hittable) [app.tables.firstMatch swipeUp];
    XCTAssertTrue(advanced.hittable); [advanced tap];
    XCTAssertTrue([app.navigationBars[@"Advanced"] waitForExistenceWithTimeout:10]);
    XCTAttachment *picture = [XCTAttachment attachmentWithScreenshot:XCUIScreen.mainScreen.screenshot];
    picture.name = @"iPad-advanced-settings";
    picture.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:picture];
    // Returning from the background must preserve the current navigation stack.
    [XCUIDevice.sharedDevice pressButton:XCUIDeviceButtonHome]; [app activate];
    XCTAssertTrue([app.navigationBars[@"Advanced"] waitForExistenceWithTimeout:10]);
}
@end
