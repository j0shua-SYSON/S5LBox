//
//  S5LBox — narrow, opt-in physical-device observation and automation.
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>

@class UINavigationController;
@class VMInstanceListViewController;

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSUInteger, VMDeviceAutomationMode) {
    /* Publish accessibility IDs and frame-pipeline counters only. This mode
     * must never start, stop, reopen or otherwise steer a machine. */
    VMDeviceAutomationModeObserveOnly = 0,

    /* In addition to observing, handle the documented first-firmware-launch
     * transition for an explicitly automated process. */
    VMDeviceAutomationModePrepareAndReopen,

    /* Consume one validated Documents/Automation/touch-once.txt plan. The
     * existing first machine is opened through its normal path, capped at the
     * requested retired-instruction count, and receives one ordinary app-path
     * touch. Developer Mode and the marker are both required. */
    VMDeviceAutomationModeInjectTouchOnce,
};

/*
 * Adds stable accessibility IDs and publishes the existing frame-pipeline
 * telemetry at a low rate. Developer Mode uses ObserveOnly during an ordinary
 * launch, so an external accessibility client can inspect the real pipeline
 * without changing guest behaviour.
 *
 * PrepareAndReopen is reserved for the documented automation launch argument.
 * The initial machine is still opened by VMInstanceListViewController through
 * its normal row-opening path. After the normal root-filesystem provisioner
 * finishes, that mode stops the synthetic guest and reopens the same machine
 * so the verified firmware can boot unattended.
 */
@interface VMDeviceAutomation : NSObject

/* Whether a complete, bounded one-shot touch plan is waiting. Invalid plans
 * are reported and ignored; this method never creates, changes or deletes the
 * file. It exists so AppDelegate can leave an ordinary launch on the machine
 * list unless the user deliberately armed the diagnostic. */
+ (BOOL)hasPendingTouchPlan;

- (instancetype)initWithNavigationController:
        (UINavigationController *)navigationController
                              machineList:
        (VMInstanceListViewController *)machineList
                                    mode:(VMDeviceAutomationMode)mode
    NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

- (void)start;

@end

NS_ASSUME_NONNULL_END
