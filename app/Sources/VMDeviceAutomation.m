//
//  S5LBox — narrow, opt-in physical-device observation and automation.
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMDeviceAutomation.h"

#import "EmulatorViewController.h"
#import "VMEngine.h"
#import "VMFrameTelemetry.h"
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
 * the controller changes. Ordinary launches take this path only when the user
 * has enabled Developer Mode, and then use the non-mutating observation mode.
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

static double VMDeviceAutomationSeconds(uint64_t firstNS, uint64_t lastNS) {
    if (firstNS == 0 || lastNS <= firstNS) return 0.0;
    return (double)(lastNS - firstNS) / 1.0e9;
}

@interface VMDeviceAutomation ()
- (void)updateFrameTelemetry;
@end

@implementation VMDeviceAutomation {
    __weak UINavigationController *_navigationController;
    __weak VMInstanceListViewController *_machineList;
    __weak EmulatorViewController *_emulator;
    __weak UIView *_telemetryScreen;
    __weak UILabel *_telemetryLabel;
    VMEngine *_stoppingEngine;
    NSTimer *_timer;
    VMDeviceAutomationState _state;
    VMDeviceAutomationMode _mode;
    BOOL _sawPreparation;
    BOOL _endpointLogged;
    NSUInteger _ticks;
}

- (instancetype)initWithNavigationController:
        (UINavigationController *)navigationController
                              machineList:
        (VMInstanceListViewController *)machineList
                                    mode:(VMDeviceAutomationMode)mode {
    self = [super init];
    if (!self) return nil;
    _navigationController = navigationController;
    _machineList = machineList;
    _state = VMDeviceAutomationStateObserving;
    _mode = mode;
    return self;
}

- (void)dealloc {
    [_timer invalidate];
}

- (void)start {
    NSAssert([NSThread isMainThread], @"device automation is UIKit work");
    [self attachToCurrentEmulator];
    [self updateFrameTelemetry];
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
        _telemetryScreen = nil;
        _telemetryLabel = nil;
        return;
    }

    EmulatorViewController *emulator = (EmulatorViewController *)top;
    [emulator loadViewIfNeeded];
    _emulator = emulator;
    emulator.view.accessibilityIdentifier = @"s5lbox.emulator.root";

    UIView *screen = VMDeviceAutomationValue(emulator, @"screen");
    if ([screen isKindOfClass:[UIView class]]) {
        _telemetryScreen = screen;
        screen.isAccessibilityElement = YES;
        screen.accessibilityIdentifier = @"s5lbox.emulator.screen";
        screen.accessibilityLabel = @"iPhone OS guest display";
    }

    /* ios-mcp's compact accessibility serializer can omit a custom view's
     * value even though Apple's full AX snapshot retains it. A real UILabel is
     * exposed consistently by both. Its one-point clear frame is visible to AX
     * but not to the user, cannot receive touches, and exists only in the two
     * opt-in diagnostic modes that construct this object. */
    UILabel *telemetryLabel = _telemetryLabel;
    if (telemetryLabel.superview != emulator.view) {
        telemetryLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, 1, 1)];
        telemetryLabel.userInteractionEnabled = NO;
        telemetryLabel.textColor = UIColor.clearColor;
        telemetryLabel.backgroundColor = UIColor.clearColor;
        telemetryLabel.font = [UIFont systemFontOfSize:1.0];
        telemetryLabel.isAccessibilityElement = YES;
        telemetryLabel.accessibilityTraits = UIAccessibilityTraitStaticText;
        [emulator.view addSubview:telemetryLabel];
        _telemetryLabel = telemetryLabel;
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

- (void)updateFrameTelemetry {
    vm_frame_telemetry_snapshot_t state;
    vm_frame_telemetry_snapshot(&state);
    if (!state.enabled) return;

    UIView *screen = _telemetryScreen;
    UILabel *telemetryLabel = _telemetryLabel;
    if (![screen isKindOfClass:[UIView class]] &&
        ![telemetryLabel isKindOfClass:[UILabel class]]) return;

    const double scanoutSeconds = VMDeviceAutomationSeconds(
        state.scanout_first_host_ns, state.scanout_last_host_ns);
    const uint64_t scanoutPostBaseline = state.scanout_changes > 0
        ? state.scanout_changes - 1u : 0u;
    const double scanoutChangedPerHostSecond = scanoutSeconds > 0.0
        ? (double)scanoutPostBaseline / scanoutSeconds : 0.0;

    const BOOL guestClockValid =
        state.scanout_guest_clock_captured &&
        state.scanout_guest_clock_consistent &&
        state.scanout_timebase_hz != 0u &&
        state.scanout_last_timer_ticks >= state.scanout_first_timer_ticks &&
        state.scanout_last_clcd_frames >= state.scanout_first_clcd_frames;
    const double guestSeconds = guestClockValid
        ? (double)(state.scanout_last_timer_ticks -
                   state.scanout_first_timer_ticks) /
              (double)state.scanout_timebase_hz
        : 0.0;
    const uint64_t vblanks = guestClockValid
        ? state.scanout_last_clcd_frames - state.scanout_first_clcd_frames
        : 0u;
    const double scanoutChangedPerGuestSecond = guestSeconds > 0.0
        ? (double)scanoutPostBaseline / guestSeconds : 0.0;

    const double layerSeconds = VMDeviceAutomationSeconds(
        state.layer_first_host_ns, state.layer_last_host_ns);
    const uint64_t layerPostBaseline = state.layer_changes > 0
        ? state.layer_changes - 1u : 0u;
    const double layerChangedPerHostSecond = layerSeconds > 0.0
        ? (double)layerPostBaseline / layerSeconds : 0.0;
    const double layerMeanMS = state.layer_attempts > 0
        ? (double)state.layer_total_work_ns /
              (double)state.layer_attempts / 1.0e6
        : 0.0;
    const double layerMaxMS = (double)state.layer_max_work_ns / 1.0e6;

    NSString *guest = guestClockValid
        ? [NSString stringWithFormat:
            @"guest_s=%.6f,vblanks=%llu,scanout_changed_per_guest_s=%.3f",
            guestSeconds, (unsigned long long)vblanks,
            scanoutChangedPerGuestSecond]
        : @"guest_clock=invalid";
    NSString *value = [NSString stringWithFormat:
        @"frame_pipeline_v1,generation=%llu,"
         "scanout_attempts=%llu,scanout_valid=%llu,"
         "scanout_signatures=%llu,scanout_host_s=%.6f,"
         "scanout_changed_per_host_s=%.3f,%@,"
         "layer_attempts=%llu,layer_accepted=%llu,layer_rejected=%llu,"
         "layer_signatures=%llu,layer_host_s=%.6f,"
         "layer_changed_per_host_s=%.3f,layer_work_mean_ms=%.3f,"
         "layer_work_max_ms=%.3f,layer_is_submission_not_display=1",
        (unsigned long long)state.generation,
        (unsigned long long)state.scanout_attempts,
        (unsigned long long)state.scanout_valid,
        (unsigned long long)state.scanout_changes,
        scanoutSeconds, scanoutChangedPerHostSecond, guest,
        (unsigned long long)state.layer_attempts,
        (unsigned long long)state.layer_accepted,
        (unsigned long long)state.layer_rejected,
        (unsigned long long)state.layer_changes,
        layerSeconds, layerChangedPerHostSecond, layerMeanMS, layerMaxMS];
    if ([screen isKindOfClass:[UIView class]])
        screen.accessibilityValue = value;
    telemetryLabel.text = value;
    telemetryLabel.accessibilityLabel = value;
}

- (VMEngine *)currentEngine {
    id engine = VMDeviceAutomationValue(_emulator, @"engine");
    return [engine isKindOfClass:[VMEngine class]] ? engine : nil;
}

- (void)beginReopenAfterPreparation:(VMEngine *)engine {
    if (_mode != VMDeviceAutomationModePrepareAndReopen ||
        _state != VMDeviceAutomationStateObserving || !engine) return;

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
    vm_frame_telemetry_reset(true);
    if (!machineList || navigation.topViewController != machineList ||
        ![machineList openFirstMachineForAutomation]) {
        [self finishWithMessage:
            @"could not reopen the prepared machine; no firmware run started"];
        return;
    }

    _state = VMDeviceAutomationStateObserving;
    _sawPreparation = NO;
    _endpointLogged = NO;
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
    [self updateFrameTelemetry];
    VMEngine *engine = [self currentEngine];
    if (!engine) return;

    /* Developer Mode deliberately stops here. Accessibility metadata and the
     * frame-pipeline snapshot above are observations; every lifecycle action
     * below belongs exclusively to the explicit launch-argument workflow. */
    if (_mode == VMDeviceAutomationModeObserveOnly) {
        if (engine.isRunningFirmware && !_endpointLogged) {
            NSLog(@"[automation] firmware engine is running; observation-only "
                   "frame telemetry remains active");
            _endpointLogged = YES;
        } else if ([engine.modeDescription
                       isEqualToString:@"built-in test guest"] &&
                   !_endpointLogged) {
            NSLog(@"[automation] built-in test guest is running; "
                   "observation-only frame telemetry remains active");
            _endpointLogged = YES;
        }
        return;
    }

    BOOL preparing = engine.isPreparingRootFilesystem;
    if (preparing) {
        _sawPreparation = YES;
        return;
    }

    if (engine.isRunningFirmware) {
        if (!_endpointLogged) {
            NSLog(@"[automation] firmware engine is running; frame telemetry "
                   "observer remains active");
            _endpointLogged = YES;
        }
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

    /* No firmware means the synthetic guest is the intended endpoint. Keep
     * the low-rate observer because it now carries the before/after pipeline
     * counters used by the physical-device profile. It does no per-frame
     * Objective-C work; the two boundary hooks are the measured overhead. */
    if ([engine.modeDescription isEqualToString:@"built-in test guest"] &&
        note.length == 0 && !_endpointLogged) {
        NSLog(@"[automation] built-in test guest is running; frame telemetry "
               "observer remains active");
        _endpointLogged = YES;
    }
}

@end
