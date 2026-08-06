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
