// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <XCTest/XCTest.h>

@interface VMIPadInterfaceTests : XCTestCase
@end
@implementation VMIPadInterfaceTests
- (void)testSetupRotationImportAndSettings {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [XCUIApplication new];
    app.launchArguments = @[@"-VMGuestMicrophoneEnabled",@"NO"];
    [app launch];
    for (NSNumber *orientation in @[@(UIDeviceOrientationPortrait),@(UIDeviceOrientationLandscapeLeft),
                                    @(UIDeviceOrientationPortraitUpsideDown),@(UIDeviceOrientationLandscapeRight)]) {
        XCUIDevice.sharedDevice.orientation = orientation.integerValue;
        XCUIElement *import = app.buttons[@"s5lbox.setup.import"];
        XCTAssertTrue([import waitForExistenceWithTimeout:15]);
        // Existence can precede the end of the rotation/presentation animation.
        XCTNSPredicateExpectation *ready = [[XCTNSPredicateExpectation alloc]
            initWithPredicate:[NSPredicate predicateWithFormat:@"hittable == YES"] object:import];
        XCTWaiterResult result = [XCTWaiter waitForExpectations:@[ready] timeout:10];
        XCTAttachment *picture = [XCTAttachment attachmentWithScreenshot:app.screenshot];
        picture.name = [NSString stringWithFormat:@"Setup-%@",orientation];
        picture.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:picture];
        XCTAssertEqual(result,XCTWaiterResultCompleted,@"%@",app.debugDescription);
        XCTAssertTrue(app.buttons[@"s5lbox.setup.primary"].hittable);
    }
    [app.buttons[@"s5lbox.setup.import"] tap];
    XCUIElement *cancel = app.buttons[@"Cancel"].firstMatch;
    XCTAssertTrue([cancel waitForExistenceWithTimeout:10]); [cancel tap];
    [app.buttons[@"Later"] tap];
    XCUIElement *settings = app.buttons[@"s5lbox.machines.settings"];
    XCTAssertTrue([settings waitForExistenceWithTimeout:10]); [settings tap];
    XCTAssertTrue([app.navigationBars[@"App Settings"] waitForExistenceWithTimeout:10]);
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationPortrait;
    XCUIElement *advanced = app.staticTexts[@"Advanced"];
    if (!advanced.hittable) [app.tables.firstMatch swipeUp];
    XCTAssertTrue(advanced.hittable); [advanced tap];
    XCTAssertTrue([app.navigationBars[@"Advanced"] waitForExistenceWithTimeout:10]);
    XCTAttachment *picture = [XCTAttachment attachmentWithScreenshot:app.screenshot];
    picture.name = @"iPad-advanced-settings";
    picture.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:picture];
    // Returning from the background must preserve the current navigation stack.
    [XCUIDevice.sharedDevice pressButton:XCUIDeviceButtonHome]; [app activate];
    XCTAssertTrue([app.navigationBars[@"Advanced"] waitForExistenceWithTimeout:10]);
}
@end
