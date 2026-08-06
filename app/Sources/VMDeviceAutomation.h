//
//  S5LBox — narrow, opt-in physical-device automation.
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>

@class UINavigationController;
@class VMInstanceListViewController;

NS_ASSUME_NONNULL_BEGIN

/*
 * Coordinates only a process launched with the documented automation
 * argument. It never runs during an ordinary app launch.
 *
 * The initial machine is still opened by VMInstanceListViewController through
 * its normal row-opening path. This object adds stable accessibility IDs and
 * handles the one unavoidable first-firmware-launch transition: after the
 * normal root-filesystem provisioner finishes, stop the synthetic guest and
 * reopen the same machine so the verified firmware can boot unattended.
 */
@interface VMDeviceAutomation : NSObject

- (instancetype)initWithNavigationController:
        (UINavigationController *)navigationController
                              machineList:
        (VMInstanceListViewController *)machineList
    NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

- (void)start;

@end

NS_ASSUME_NONNULL_END
