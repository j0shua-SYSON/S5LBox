//
//  S5LBox — narrow, opt-in physical-device automation. See the header.
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMDeviceAutomation.h"

#import "EmulatorViewController.h"
#import "VMEngine.h"
#import "VMInstanceListViewController.h"

#import <UIKit/UIKit.h>

typedef NS_ENUM(NSUInteger, VMDeviceAutomationState) {
    VMDeviceAutomationStateObserving = 0,
    VMDeviceAutomationStateWaitingForStop,
    VMDeviceAutomationStateFinished,
};

/*
 * The user-owned emulator controller intentionally exposes no diagnostic
 * internals. Automation still needs the exact engine status that controller
 * already renders and the views to which stable identifiers belong. KVC is
 * confined to this opt-in file, names only our own ivars, and fails closed if
 * the controller changes. Normal launches never take this path.
 */
static id VMDeviceAutomationValue(id object, NSString *key) {
    if (!object || !key.length) return nil;
    @try {
        return [object valueForKey:key];
    } @catch (NSException *exception) {
        NSLog(@"[automation] %@ is unavailable on %@: %@",
              key, NSStringFromClass([object class]), exception.reason);
        return nil;
    }
}

@implementation VMDeviceAutomation {
    __weak UINavigationController *_navigationController;
    __weak VMInstanceListViewController *_machineList;
    __weak EmulatorViewController *_emulator;
    VMEngine *_stoppingEngine;
    NSTimer *_timer;
    VMDeviceAutomationState _state;
    BOOL _sawPreparation;
    NSUInteger _ticks;
}

- (instancetype)initWithNavigationController:
        (UINavigationController *)navigationController
                              machineList:
        (VMInstanceListViewController *)machineList {
    self = [super init];
    if (!self) return nil;
    _navigationController = navigationController;
    _machineList = machineList;
    _state = VMDeviceAutomationStateObserving;
    return self;
}

- (void)dealloc {
    [_timer invalidate];
}

- (void)start {
    NSAssert([NSThread isMainThread], @"device automation is UIKit work");
    [self attachToCurrentEmulator];
    _timer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                              target:self
                                            selector:@selector(tick:)
                                            userInfo:nil
                                             repeats:YES];
    _timer.tolerance = 0.1;
    [self tick:_timer];
}

- (void)finishWithMessage:(NSString *)message {
    if (message.length) NSLog(@"[automation] %@", message);
    _state = VMDeviceAutomationStateFinished;
    [_timer invalidate];
    _timer = nil;
}

- (void)attachToCurrentEmulator {
    UIViewController *top = _navigationController.topViewController;
    if (![top isKindOfClass:[EmulatorViewController class]]) {
        _emulator = nil;
        return;
    }

    EmulatorViewController *emulator = (EmulatorViewController *)top;
    [emulator loadViewIfNeeded];
    _emulator = emulator;
    emulator.view.accessibilityIdentifier = @"s5lbox.emulator.root";

    UIView *screen = VMDeviceAutomationValue(emulator, @"screen");
    if ([screen isKindOfClass:[UIView class]]) {
        screen.isAccessibilityElement = YES;
        screen.accessibilityIdentifier = @"s5lbox.emulator.screen";
        screen.accessibilityLabel = @"iPhone OS guest display";
    }

    UILabel *status = VMDeviceAutomationValue(emulator, @"stats");
    if ([status isKindOfClass:[UILabel class]])
        status.accessibilityIdentifier = @"s5lbox.emulator.status";

    UITextView *console = VMDeviceAutomationValue(emulator, @"console");
    if ([console isKindOfClass:[UITextView class]])
        console.accessibilityIdentifier = @"s5lbox.emulator.console";

    UIView *keys = VMDeviceAutomationValue(emulator, @"keys");
    if ([keys isKindOfClass:[UIView class]])
        keys.accessibilityIdentifier = @"s5lbox.emulator.keys";

    UIToolbar *toolbar = VMDeviceAutomationValue(emulator, @"toolbar");
    if ([toolbar isKindOfClass:[UIToolbar class]])
        toolbar.accessibilityIdentifier = @"s5lbox.emulator.toolbar";
}

- (VMEngine *)currentEngine {
    id engine = VMDeviceAutomationValue(_emulator, @"engine");
    return [engine isKindOfClass:[VMEngine class]] ? engine : nil;
}

- (void)beginReopenAfterPreparation:(VMEngine *)engine {
    if (_state != VMDeviceAutomationStateObserving || !engine) return;

    _state = VMDeviceAutomationStateWaitingForStop;
    _stoppingEngine = engine;
    [engine stop];

    EmulatorViewController *emulator = _emulator;
    UINavigationController *navigation = _navigationController;
    void (^popToList)(void) = ^{
        [navigation popToRootViewControllerAnimated:NO];
    };
    if (emulator.presentedViewController)
        [emulator dismissViewControllerAnimated:NO completion:popToList];
    else
        popToList();

    NSLog(@"[automation] root filesystem is ready; waiting for the test guest "
           "to stop before reopening the machine");
}

- (void)reopenWhenStopped {
    if (_stoppingEngine.isRunning) return;

    _stoppingEngine = nil;
    VMInstanceListViewController *machineList = _machineList;
    UINavigationController *navigation = _navigationController;
    if (!machineList || navigation.topViewController != machineList ||
        ![machineList openFirstMachineForAutomation]) {
        [self finishWithMessage:
            @"could not reopen the prepared machine; no firmware run started"];
        return;
    }

    _state = VMDeviceAutomationStateObserving;
    _sawPreparation = NO;
    [self attachToCurrentEmulator];
    NSLog(@"[automation] reopened the prepared machine through the normal path");
}

- (void)tick:(NSTimer *)timer {
    (void)timer;
    if (_state == VMDeviceAutomationStateFinished) return;
    if (++_ticks >= 3600u) {
        [self finishWithMessage:
            @"setup observer timed out after 30 minutes; no state was forced"];
        return;
    }
    if (_state == VMDeviceAutomationStateWaitingForStop) {
        [self reopenWhenStopped];
        return;
    }

    if (!_emulator || _navigationController.topViewController != _emulator)
        [self attachToCurrentEmulator];
    VMEngine *engine = [self currentEngine];
    if (!engine) return;

    BOOL preparing = engine.isPreparingRootFilesystem;
    if (preparing) {
        _sawPreparation = YES;
        return;
    }

    if (engine.isRunningFirmware) {
        [self finishWithMessage:
            @"firmware engine is running; setup observer removed"];
        return;
    }

    NSString *note = engine.bringUpNote;
    BOOL ready = [note hasPrefix:
        @"This machine's root filesystem is ready. Reopen it to boot iPhone OS."];
    if (ready && (_sawPreparation || note.length)) {
        [self beginReopenAfterPreparation:engine];
        return;
    }

    if (_sawPreparation && note.length) {
        [self finishWithMessage:[NSString stringWithFormat:
            @"root-filesystem preparation did not become bootable: %@", note]];
        return;
    }

    /* No firmware means the synthetic guest is the intended endpoint. The
     * identifiers are installed already; leaving a polling timer alive would
     * only perturb the profile it was meant to observe. */
    if ([engine.modeDescription isEqualToString:@"built-in test guest"] &&
        note.length == 0)
        [self finishWithMessage:
            @"built-in test guest is running; setup observer removed"];
}

@end
