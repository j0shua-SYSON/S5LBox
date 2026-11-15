// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <XCTest/XCTest.h>
#import "VMAdaptiveLayout.h"
#import "EmulatorViewController.h"
#import "VMFramebufferView.h"
#import "VMDeviceControlsViewController.h"
#import "VMSetupViewController.h"
#import "VMSettings.h"
#import "VMTouchMap.h"

// Exercise the real view/controller code without opening firmware or a disk.
@interface LayoutEmulator : EmulatorViewController
@end
@implementation LayoutEmulator
- (void)startEmulator {}
@end

@interface VMIPadLayoutTests : XCTestCase
@end
@implementation VMIPadLayoutTests
- (NSArray<NSValue *> *)sizes {
    return @[[NSValue valueWithCGSize:CGSizeMake(320,480)],
        [NSValue valueWithCGSize:CGSizeMake(375,667)],
        [NSValue valueWithCGSize:CGSizeMake(744,1133)],
        [NSValue valueWithCGSize:CGSizeMake(768,1024)],
        [NSValue valueWithCGSize:CGSizeMake(820,1180)],
        [NSValue valueWithCGSize:CGSizeMake(1024,768)],
        [NSValue valueWithCGSize:CGSizeMake(1366,1024)],
        [NSValue valueWithCGSize:CGSizeMake(320,1024)],
        [NSValue valueWithCGSize:CGSizeMake(507,768)],
        [NSValue valueWithCGSize:CGSizeMake(960,600)],
        [NSValue valueWithCGSize:CGSizeMake(500,300)]];
}
- (void)testGuestGeometryAcrossWindowSizes {
    for (NSValue *value in self.sizes) for (NSNumber *console in @[@NO,@YES]) {
        CGRect bounds = (CGRect){CGPointZero,value.CGSizeValue};
        UIEdgeInsets safe = UIEdgeInsetsMake(64,20,21,20);
        VMEmulatorLayout layout = VMLayoutEmulator(bounds,safe,console.boolValue);
        CGRect usable = UIEdgeInsetsInsetRect(bounds,safe);
        XCTAssertTrue(CGRectContainsRect(usable,layout.screen));
        XCTAssertTrue(CGRectContainsRect(usable,layout.dock));
        XCTAssertFalse(CGRectIntersectsRect(layout.screen,layout.dock));
        XCTAssertGreaterThan(layout.screen.size.height,0);
        XCTAssertEqualWithAccuracy(layout.screen.size.width / layout.screen.size.height,2.0/3.0,0.00001);
        vm_touch_point_t point = vm_touch_map(layout.screen.size.width,layout.screen.size.height,
            320,480,layout.screen.size.width*(160.5/320),layout.screen.size.height*(240.5/480));
        XCTAssertTrue(point.inside); XCTAssertEqual(point.x,160); XCTAssertEqual(point.y,240);
    }
}
- (void)testModernAndLegacyControllersResize {
    BOOL legacy = VMSettings.sharedSettings.legacyEmulatorUI;
    for (NSNumber *style in @[@NO,@YES]) {
        VMSettings.sharedSettings.legacyEmulatorUI = style.boolValue;
        LayoutEmulator *controller = [LayoutEmulator new];
        [controller loadViewIfNeeded];
        for (NSValue *value in self.sizes) {
            controller.view.frame = (CGRect){CGPointZero,value.CGSizeValue};
            [controller.view setNeedsLayout]; [controller.view layoutIfNeeded];
            VMFramebufferView *screen = nil;
            for (UIView *view in controller.view.subviews)
                if ([view isKindOfClass:VMFramebufferView.class]) screen = (VMFramebufferView *)view;
            XCTAssertNotNil(screen);
            XCTAssertTrue(CGRectContainsRect(controller.view.bounds,screen.frame));
            XCTAssertGreaterThan(screen.bounds.size.height,0);
            XCTAssertEqualWithAccuracy(screen.bounds.size.width/screen.bounds.size.height,2.0/3.0,0.02);
        }
    }
    VMSettings.sharedSettings.legacyEmulatorUI = legacy;
}
- (void)testReadableFormsResize {
    for (UIViewController *controller in @[[VMSetupViewController new],[VMDeviceControlsViewController new]]) {
        [controller loadViewIfNeeded];
        for (NSValue *value in self.sizes) {
            controller.view.frame = (CGRect){CGPointZero,value.CGSizeValue};
            [controller.view setNeedsLayout]; [controller.view layoutIfNeeded];
            UIScrollView *scroll = nil;
            for (UIView *view in controller.view.subviews) if ([view isKindOfClass:UIScrollView.class]) scroll = (UIScrollView *)view;
            XCTAssertNotNil(scroll);
            XCTAssertLessThanOrEqual(scroll.contentSize.width,scroll.bounds.size.width + 1);
            XCTAssertGreaterThan(scroll.bounds.size.height,0);
        }
    }
}
- (void)testSceneAndTransportConfiguration {
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    XCTAssertEqual([info[@"UISupportedInterfaceOrientations~ipad"] count],4u);
    XCTAssertFalse([info[@"UIApplicationSceneManifest"][@"UIApplicationSupportsMultipleScenes"] boolValue]);
    XCTAssertTrue([info[@"NSAppTransportSecurity"][@"NSExceptionDomains"][@"apt.thebigboss.org"][@"NSExceptionAllowsInsecureHTTPLoads"] boolValue]);
    XCTAssertNil(info[@"NSAppTransportSecurity"][@"NSAllowsArbitraryLoads"]);
    XCTAssertGreaterThan(UIApplication.sharedApplication.connectedScenes.count,0u);
}
@end
