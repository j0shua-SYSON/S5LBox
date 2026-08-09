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

typedef NS_ENUM(NSUInteger, VMDeviceAutomationTouchState) {
    VMDeviceAutomationTouchStateWaitingForFirmware = 0,
    VMDeviceAutomationTouchStateDownQueued,
    VMDeviceAutomationTouchStateUpQueued,
    VMDeviceAutomationTouchStateComplete,
};

typedef struct {
    NSInteger x;
    NSInteger y;
    uint64_t instructionCap;
} VMDeviceAutomationTouchPlan;

static NSString *VMDeviceAutomationTouchPlanPath(void) {
    NSURL *documents = [[[NSFileManager defaultManager]
        URLsForDirectory:NSDocumentDirectory
               inDomains:NSUserDomainMask] firstObject];
    if (!documents) return nil;
    return [[[documents URLByAppendingPathComponent:@"Automation"
                                        isDirectory:YES]
        URLByAppendingPathComponent:@"touch-once.txt"
                         isDirectory:NO] path];
}

/*
 * One deliberately boring line:
 *
 *     v1 <guest-x> <guest-y> <session-instruction-cap>
 *
 * The small size bound prevents an accidentally selected log or firmware file
 * from being read as a command. A cap is mandatory: a diagnostic that can
 * silently become an unbounded boot is not deterministic automation.
 */
static BOOL VMDeviceAutomationLoadTouchPlan(
        VMDeviceAutomationTouchPlan *outPlan, NSString **outReason) {
    NSString *path = VMDeviceAutomationTouchPlanPath();
    if (!path.length) {
        if (outReason) *outReason = @"the Documents directory is unavailable";
        return NO;
    }

    NSError *error = nil;
    NSDictionary<NSFileAttributeKey, id> *attributes =
        [[NSFileManager defaultManager] attributesOfItemAtPath:path
                                                        error:&error];
    if (!attributes) {
        if (outReason) *outReason = error.localizedDescription ?: @"not found";
        return NO;
    }
    unsigned long long size = [attributes[NSFileSize] unsignedLongLongValue];
    if (size == 0u || size > 128u) {
        if (outReason) *outReason = @"the plan must be 1 through 128 bytes";
        return NO;
    }

    NSString *text = [NSString stringWithContentsOfFile:path
                                                encoding:NSUTF8StringEncoding
                                                   error:&error];
    if (!text) {
        if (outReason) *outReason = error.localizedDescription ?: @"not UTF-8";
        return NO;
    }

    NSScanner *scanner = [NSScanner scannerWithString:text];
    scanner.charactersToBeSkipped =
        [NSCharacterSet whitespaceAndNewlineCharacterSet];
    NSInteger x = 0, y = 0;
    long long cap = 0;
    BOOL valid = [scanner scanString:@"v1" intoString:NULL] &&
                 [scanner scanInteger:&x] &&
                 [scanner scanInteger:&y] &&
                 [scanner scanLongLong:&cap] && scanner.isAtEnd &&
                 x >= 0 && x < (NSInteger)VM_FB_WIDTH &&
                 y >= 0 && y < (NSInteger)VM_FB_HEIGHT &&
                 cap >= 1000000ll && cap <= 1000000000ll;
    if (!valid) {
        if (outReason) {
            *outReason = [NSString stringWithFormat:
                @"expected 'v1 x y cap', panel %ux%u, cap 1000000..1000000000",
                VM_FB_WIDTH, VM_FB_HEIGHT];
        }
        return NO;
    }

    if (outPlan) {
        outPlan->x = x;
        outPlan->y = y;
        outPlan->instructionCap = (uint64_t)cap;
    }
    if (outReason) *outReason = nil;
    return YES;
}

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

@interface VMDeviceAutomation () <UINavigationControllerDelegate>
- (void)attachToEmulator:(EmulatorViewController *)emulator;
- (void)driveTouchPlan;
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
    NSUInteger _fastTicks;
    VMDeviceAutomationTouchState _touchState;
    VMDeviceAutomationTouchPlan _touchPlan;
    uint64_t _touchDeliveredBaseline;
    NSTimeInterval _touchLiftNotBefore;
    NSTimeInterval _touchDeadline;
}

+ (BOOL)hasPendingTouchPlan {
    NSString *path = VMDeviceAutomationTouchPlanPath();
    if (!path.length ||
        ![[NSFileManager defaultManager] fileExistsAtPath:path]) return NO;

    NSString *reason = nil;
    if (VMDeviceAutomationLoadTouchPlan(NULL, &reason)) return YES;
    NSLog(@"[automation] ignored invalid touch-once plan: %@",
          reason ?: @"unknown error");
    return NO;
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
    if (_navigationController.delegate == self)
        _navigationController.delegate = nil;
}

- (void)start {
    NSAssert([NSThread isMainThread], @"device automation is UIKit work");
    if (_mode == VMDeviceAutomationModeInjectTouchOnce) {
        NSString *reason = nil;
        if (!VMDeviceAutomationLoadTouchPlan(&_touchPlan, &reason)) {
            [self finishWithMessage:[NSString stringWithFormat:
                @"touch-once plan became unavailable: %@",
                reason ?: @"unknown error"]];
            return;
        }
        _touchState = VMDeviceAutomationTouchStateWaitingForFirmware;
        _touchDeadline = [NSDate timeIntervalSinceReferenceDate] + 30.0;
        UINavigationController *navigation = _navigationController;
        if (navigation.delegate && navigation.delegate != self) {
            [self finishWithMessage:
                @"touch-once refused because navigation already has a delegate"];
            return;
        }
        navigation.delegate = self;
    }
    [self attachToCurrentEmulator];
    [self updateFrameTelemetry];
    NSTimeInterval interval =
        _mode == VMDeviceAutomationModeInjectTouchOnce ? 0.01 : 0.5;
    _timer = [NSTimer scheduledTimerWithTimeInterval:interval
                                              target:self
                                            selector:@selector(tick:)
                                            userInfo:nil
                                             repeats:YES];
    _timer.tolerance = interval >= 0.5 ? 0.1 : 0.0;
    [self tick:_timer];
}

- (void)finishWithMessage:(NSString *)message {
    if (message.length) NSLog(@"[automation] %@", message);
    _state = VMDeviceAutomationStateFinished;
    [_timer invalidate];
    _timer = nil;
    if (_navigationController.delegate == self)
        _navigationController.delegate = nil;
}

- (void)attachToEmulator:(EmulatorViewController *)emulator {
    if (![emulator isKindOfClass:[EmulatorViewController class]]) {
        _emulator = nil;
        _telemetryScreen = nil;
        _telemetryLabel = nil;
        return;
    }

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

    /* Some compact accessibility serializers can omit a custom view's value
     * even though Apple's full AX snapshot retains it. A real UILabel is
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

- (void)attachToCurrentEmulator {
    UIViewController *top = _navigationController.topViewController;
    [self attachToEmulator:
        [top isKindOfClass:[EmulatorViewController class]]
            ? (EmulatorViewController *)top : nil];
}

- (void)          navigationController:(UINavigationController *)navigation
        willShowViewController:(UIViewController *)viewController
                       animated:(BOOL)animated {
    (void)navigation;
    (void)animated;
    if (_mode != VMDeviceAutomationModeInjectTouchOnce ||
        _state == VMDeviceAutomationStateFinished ||
        ![viewController isKindOfClass:[EmulatorViewController class]]) return;

    /* This callback runs inside the push, before AppDelegate's open call
     * returns. Loading the controller here starts the ordinary VMEngine path;
     * queuing immediately afterwards closes the multi-second race in which a
     * restored guest could reach its cap before the old post-push observer
     * ever saw the engine. */
    [self attachToEmulator:(EmulatorViewController *)viewController];
    [self updateFrameTelemetry];
    [self driveTouchPlan];
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
    const uint64_t telemetryNowNS = vm_frame_telemetry_now_ns();
    const uint64_t telemetryBoundaryNS = telemetryNowNS != 0u
        ? telemetryNowNS : state.scanout_last_host_ns;
    const double lastAttemptAgoSeconds =
        state.scanout_last_host_ns != 0u &&
        telemetryBoundaryNS >= state.scanout_last_host_ns
        ? (double)(telemetryBoundaryNS - state.scanout_last_host_ns) / 1.0e9
        : 0.0;

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
    const uint64_t scanoutInvalid = state.scanout_attempts >= state.scanout_valid
        ? state.scanout_attempts - state.scanout_valid : 0u;
    const vm_frame_scanout_reason_t lastReason = state.scanout_last.reason;
    const uint32_t lastReasonIndex = (uint32_t)lastReason;
    const uint64_t lastReasonCount =
        lastReasonIndex < (uint32_t)VM_FRAME_SCANOUT_REASON_COUNT
        ? state.scanout_reason_counts[lastReasonIndex] : 0u;
    const double lastValidAgoSeconds =
        state.scanout_last_valid_host_ns != 0u &&
        telemetryBoundaryNS >= state.scanout_last_valid_host_ns
        ? (double)(telemetryBoundaryNS -
                   state.scanout_last_valid_host_ns) / 1.0e9
        : 0.0;
    const double lastChangeAgoSeconds =
        state.scanout_last_change_host_ns != 0u &&
        telemetryBoundaryNS >= state.scanout_last_change_host_ns
        ? (double)(telemetryBoundaryNS -
                   state.scanout_last_change_host_ns) / 1.0e9
        : 0.0;

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
    const double layerLastAttemptAgoSeconds =
        state.layer_last_host_ns != 0u &&
        telemetryBoundaryNS >= state.layer_last_host_ns
        ? (double)(telemetryBoundaryNS - state.layer_last_host_ns) / 1.0e9
        : 0.0;
    const double layerLastChangeAgoSeconds =
        state.layer_last_change_host_ns != 0u &&
        telemetryBoundaryNS >= state.layer_last_change_host_ns
        ? (double)(telemetryBoundaryNS - state.layer_last_change_host_ns) /
              1.0e9
        : 0.0;

    NSString *execution = @"execution_captured=0";
    if (state.execution_captured) {
        const vm_execution_telemetry_observation_t *first =
            &state.execution_first;
        const vm_execution_telemetry_observation_t *last =
            &state.execution_last;
#define VM_EXEC_DELTA(field_) \
    (last->field_ >= first->field_ ? last->field_ - first->field_ : 0u)
        execution = [NSString stringWithFormat:
            @"execution_captured=1,execution_consistent=%u,"
             "execution_observations=%llu,cpu_retired=%llu,"
             "interpreter_tick_batches=%llu,"
             "interpreter_tick_batched_retired=%llu,"
             "static_native_retired=%llu,compact_attempts=%llu,"
             "compact_calls=%llu,compact_native_retired=%llu,"
             "compact_fallback_retired=%llu,"
             "compact_privileged_attempts=%llu,"
             "compact_privileged_calls=%llu,"
             "compact_privileged_retired=%llu,"
             "compact_privileged_window_refills=%llu,"
             "compact_privileged_boundary_retired=%llu,"
             "compact_window_crossings=%llu,compact_window_reloads=%llu,"
             "compact_window_fast_refills=%llu,compact_window_stops=%llu,"
             "compact_refused_guard=%llu,"
             "compact_refused_privileged=%llu,"
             "compact_refused_alignment=%llu,"
             "compact_refused_fetch_witness=%llu,"
             "compact_refused_runner=%llu,compact_zero_retired=%llu,"
             "fetch_refill_attempts=%llu,fetch_refill_hits=%llu,"
             "fetch_refill_skips=%llu,known_negative_bypasses=%llu,"
             "mbx_2d_candidates=%llu,mbx_2d_completed=%llu,"
             "mbx_2d_rejected=%llu,mbx_2d_bytes=%llu,"
             "mbx_3d_candidates=%llu,mbx_3d_completed=%llu,"
             "mbx_3d_rejected=%llu,mbx_3d_pixels=%llu,"
             "active_clock_updates=%llu,active_clock_added_ticks=%llu,"
             "active_clock_clamps=%llu,active_clock_failures=%llu",
            state.execution_consistent ? 1u : 0u,
            (unsigned long long)state.execution_observations,
            (unsigned long long)VM_EXEC_DELTA(cpu_retired),
            (unsigned long long)VM_EXEC_DELTA(interpreter_tick_batches),
            (unsigned long long)VM_EXEC_DELTA(
                interpreter_tick_batched_retired),
            (unsigned long long)VM_EXEC_DELTA(static_native_retired),
            (unsigned long long)VM_EXEC_DELTA(compact_attempts),
            (unsigned long long)VM_EXEC_DELTA(compact_calls),
            (unsigned long long)VM_EXEC_DELTA(compact_native_retired),
            (unsigned long long)VM_EXEC_DELTA(compact_fallback_retired),
            (unsigned long long)VM_EXEC_DELTA(compact_privileged_attempts),
            (unsigned long long)VM_EXEC_DELTA(compact_privileged_calls),
            (unsigned long long)VM_EXEC_DELTA(compact_privileged_retired),
            (unsigned long long)VM_EXEC_DELTA(
                compact_privileged_window_refills),
            (unsigned long long)VM_EXEC_DELTA(
                compact_privileged_boundary_retired),
            (unsigned long long)VM_EXEC_DELTA(compact_window_crossings),
            (unsigned long long)VM_EXEC_DELTA(compact_window_reloads),
            (unsigned long long)VM_EXEC_DELTA(compact_window_fast_refills),
            (unsigned long long)VM_EXEC_DELTA(compact_window_stops),
            (unsigned long long)VM_EXEC_DELTA(compact_refused_guard),
            (unsigned long long)VM_EXEC_DELTA(compact_refused_privileged),
            (unsigned long long)VM_EXEC_DELTA(compact_refused_alignment),
            (unsigned long long)VM_EXEC_DELTA(
                compact_refused_fetch_witness),
            (unsigned long long)VM_EXEC_DELTA(compact_refused_runner),
            (unsigned long long)VM_EXEC_DELTA(compact_zero_retired),
            (unsigned long long)VM_EXEC_DELTA(fetch_refill_attempts),
            (unsigned long long)VM_EXEC_DELTA(fetch_refill_hits),
            (unsigned long long)VM_EXEC_DELTA(fetch_refill_skips),
            (unsigned long long)VM_EXEC_DELTA(known_negative_bypasses),
            (unsigned long long)VM_EXEC_DELTA(mbx_2d_candidates),
            (unsigned long long)VM_EXEC_DELTA(mbx_2d_completed),
            (unsigned long long)VM_EXEC_DELTA(mbx_2d_rejected),
            (unsigned long long)VM_EXEC_DELTA(mbx_2d_bytes),
            (unsigned long long)VM_EXEC_DELTA(mbx_3d_candidates),
            (unsigned long long)VM_EXEC_DELTA(mbx_3d_completed),
            (unsigned long long)VM_EXEC_DELTA(mbx_3d_rejected),
            (unsigned long long)VM_EXEC_DELTA(mbx_3d_pixels),
            (unsigned long long)VM_EXEC_DELTA(active_clock_updates),
            (unsigned long long)VM_EXEC_DELTA(active_clock_added_ticks),
            (unsigned long long)VM_EXEC_DELTA(active_clock_clamps),
            (unsigned long long)VM_EXEC_DELTA(active_clock_failures)];
#undef VM_EXEC_DELTA
    }

    NSString *stallWitness = state.scanout_max_gap_execution_captured
        ? [NSString stringWithFormat:
            @"scanout_stall_execution_captured=1,"
             "scanout_stall_cpu_retired=%llu,"
             "scanout_stall_mbx_2d_candidates=%llu,"
             "scanout_stall_mbx_2d_completed=%llu,"
             "scanout_stall_mbx_2d_rejected=%llu,"
             "scanout_stall_mbx_2d_bytes=%llu,"
             "scanout_stall_mbx_3d_candidates=%llu,"
             "scanout_stall_mbx_3d_completed=%llu,"
             "scanout_stall_mbx_3d_rejected=%llu,"
             "scanout_stall_mbx_3d_pixels=%llu",
            (unsigned long long)state.scanout_max_gap_cpu_retired,
            (unsigned long long)state.scanout_max_gap_mbx_2d_candidates,
            (unsigned long long)state.scanout_max_gap_mbx_2d_completed,
            (unsigned long long)state.scanout_max_gap_mbx_2d_rejected,
            (unsigned long long)state.scanout_max_gap_mbx_2d_bytes,
            (unsigned long long)state.scanout_max_gap_mbx_3d_candidates,
            (unsigned long long)state.scanout_max_gap_mbx_3d_completed,
            (unsigned long long)state.scanout_max_gap_mbx_3d_rejected,
            (unsigned long long)state.scanout_max_gap_mbx_3d_pixels]
        : @"scanout_stall_execution_captured=0";

    NSString *guest = guestClockValid
        ? [NSString stringWithFormat:
            @"guest_s=%.6f,vblanks=%llu,scanout_changed_per_guest_s=%.3f",
            guestSeconds, (unsigned long long)vblanks,
            scanoutChangedPerGuestSecond]
        : @"guest_clock=invalid";
    NSString *value = [NSString stringWithFormat:
        @"frame_pipeline_v2,generation=%llu,"
         "scanout_attempts=%llu,scanout_valid=%llu,scanout_invalid=%llu,"
         "scanout_signatures=%llu,scanout_host_s=%.6f,"
         "scanout_changed_per_host_s=%.3f,%@,"
         "scanout_attempt_gap_max_ms=%.3f,"
         "scanout_attempt_gaps_over_100ms=%llu,"
         "scanout_attempt_gaps_over_500ms=%llu,"
         "scanout_change_gap_max_ms=%.3f,"
         "scanout_last_attempt_ago_host_s=%.6f,"
         "scanout_last_change_ago_host_s=%.6f,%@,"
         "scanout_last_reason=%s,scanout_last_reason_count=%llu,"
         "scanout_last_reason_streak=%llu,scanout_last_valid_ago_host_s=%.6f,"
         "scanout_last_scanning=%u,scanout_last_ctrl=%08x,"
         "scanout_last_gate=%08x,scanout_last_active=%u,"
         "scanout_last_fb=%08x,scanout_last_width=%u,"
         "scanout_last_height=%u,scanout_last_stride=%u,"
         "scanout_last_format=%u,"
         "layer_attempts=%llu,layer_accepted=%llu,layer_rejected=%llu,"
         "layer_signatures=%llu,layer_host_s=%.6f,"
         "layer_changed_per_host_s=%.3f,layer_work_mean_ms=%.3f,"
         "layer_work_max_ms=%.3f,layer_attempt_gap_max_ms=%.3f,"
         "layer_attempt_gaps_over_100ms=%llu,"
         "layer_attempt_gaps_over_500ms=%llu,"
         "layer_change_gap_max_ms=%.3f,"
         "layer_last_attempt_ago_host_s=%.6f,"
         "layer_last_change_ago_host_s=%.6f,"
         "layer_is_submission_not_display=1,%@",
        (unsigned long long)state.generation,
        (unsigned long long)state.scanout_attempts,
        (unsigned long long)state.scanout_valid,
        (unsigned long long)scanoutInvalid,
        (unsigned long long)state.scanout_changes,
        scanoutSeconds, scanoutChangedPerHostSecond, guest,
        (double)state.scanout_max_attempt_gap_ns / 1.0e6,
        (unsigned long long)state.scanout_attempt_gaps_over_100ms,
        (unsigned long long)state.scanout_attempt_gaps_over_500ms,
        (double)state.scanout_max_change_gap_ns / 1.0e6,
        lastAttemptAgoSeconds, lastChangeAgoSeconds, stallWitness,
        vm_frame_scanout_reason_name(lastReason),
        (unsigned long long)lastReasonCount,
        (unsigned long long)state.scanout_last_reason_streak,
        lastValidAgoSeconds,
        state.scanout_last.scanning,
        state.scanout_last.ctrl,
        state.scanout_last.gate,
        state.scanout_last.active_window,
        state.scanout_last.framebuffer_phys,
        state.scanout_last.width,
        state.scanout_last.height,
        state.scanout_last.stride,
        state.scanout_last.format,
        (unsigned long long)state.layer_attempts,
        (unsigned long long)state.layer_accepted,
        (unsigned long long)state.layer_rejected,
        (unsigned long long)state.layer_changes,
        layerSeconds, layerChangedPerHostSecond, layerMeanMS, layerMaxMS,
        (double)state.layer_max_attempt_gap_ns / 1.0e6,
        (unsigned long long)state.layer_attempt_gaps_over_100ms,
        (unsigned long long)state.layer_attempt_gaps_over_500ms,
        (double)state.layer_max_change_gap_ns / 1.0e6,
        layerLastAttemptAgoSeconds, layerLastChangeAgoSeconds,
        execution];
    if ([screen isKindOfClass:[UIView class]])
        screen.accessibilityValue = value;
    telemetryLabel.text = value;
    telemetryLabel.accessibilityLabel = value;
}

- (VMEngine *)currentEngine {
    id engine = VMDeviceAutomationValue(_emulator, @"engine");
    return [engine isKindOfClass:[VMEngine class]] ? engine : nil;
}

- (void)finishTouchPlan {
    NSString *path = VMDeviceAutomationTouchPlanPath();
    NSError *error = nil;
    if (!path.length ||
        ![[NSFileManager defaultManager] removeItemAtPath:path error:&error]) {
        [self finishWithMessage:[NSString stringWithFormat:
            @"touch reached the guest but the one-shot plan could not be "
             "consumed: %@", error.localizedDescription ?: @"path unavailable"]];
        return;
    }

    _touchState = VMDeviceAutomationTouchStateComplete;
    _mode = VMDeviceAutomationModeObserveOnly;
    if (_navigationController.delegate == self)
        _navigationController.delegate = nil;
    _ticks = 0u;
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                              target:self
                                            selector:@selector(tick:)
                                            userInfo:nil
                                             repeats:YES];
    _timer.tolerance = 0.1;
    NSLog(@"[automation] touch-once delivered at %ld,%ld; instruction cap %llu",
          (long)_touchPlan.x, (long)_touchPlan.y,
          (unsigned long long)_touchPlan.instructionCap);
}

- (void)driveTouchPlan {
    VMEngine *engine = [self currentEngine];
    if (!engine || !engine.isRunningFirmware) return;

    uint64_t delivered = 0u;
    [engine touchCountersQueued:NULL delivered:&delivered
                       coalesced:NULL dropped:NULL];

    if (_touchState == VMDeviceAutomationTouchStateWaitingForFirmware) {
        [engine setInstructionCap:_touchPlan.instructionCap];
        _touchDeliveredBaseline = delivered;
        if (![engine sendTouchAtGuestX:(int)_touchPlan.x
                                     y:(int)_touchPlan.y
                                 phase:VM_TOUCH_BEGAN]) return;
        NSLog(@"[automation] touch-down queued at %ld,%ld",
              (long)_touchPlan.x, (long)_touchPlan.y);
        _touchLiftNotBefore = [NSDate timeIntervalSinceReferenceDate] + 0.05;
        _touchState = VMDeviceAutomationTouchStateDownQueued;
        return;
    }

    if (_touchState == VMDeviceAutomationTouchStateDownQueued) {
        if (delivered <= _touchDeliveredBaseline ||
            [NSDate timeIntervalSinceReferenceDate] < _touchLiftNotBefore)
            return;
        if (![engine sendTouchAtGuestX:(int)_touchPlan.x
                                     y:(int)_touchPlan.y
                                 phase:VM_TOUCH_ENDED]) return;
        NSLog(@"[automation] touch-up queued after controller delivery");
        _touchState = VMDeviceAutomationTouchStateUpQueued;
        return;
    }

    if (_touchState == VMDeviceAutomationTouchStateUpQueued &&
        delivered >= _touchDeliveredBaseline + 2u)
        [self finishTouchPlan];
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
    if (_mode == VMDeviceAutomationModeInjectTouchOnce &&
        _touchState != VMDeviceAutomationTouchStateComplete) {
        ++_fastTicks;
        if ([NSDate timeIntervalSinceReferenceDate] >= _touchDeadline) {
            [self finishWithMessage:
                @"touch-once timed out after 30 seconds; its plan was retained"];
            return;
        }
        if (!_emulator || _navigationController.topViewController != _emulator)
            [self attachToCurrentEmulator];
        if ((_fastTicks % 50u) == 0u) [self updateFrameTelemetry];
        [self driveTouchPlan];
        return;
    }
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
