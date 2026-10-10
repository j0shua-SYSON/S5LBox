//
//  S5LBox -- the screen that turns a user's own IPSW into the three files the
//  emulator accepts.
//
//  Until now the Firmware section named three files and left the user to
//  produce them by hand, which meant docs/BOOT_CHAIN.md, a desktop, and five
//  command-line tools. This screen does the parts a program can do: open the
//  archive, read Apple's manifest, unwrap the containers, decompress the
//  kernel, expand the root filesystem's partition, and say precisely what came
//  out and what did not.
//
//  Public keys resolve automatically for the supported manifest identity.
//  Manual session-only overrides and detailed reports remain here in Settings;
//  VMSetupViewController presents the simpler first-run experience.
//
//  Push it. It has no Done button of its own -- it belongs under Settings >
//  Firmware, and the navigation bar's Back is the way out.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface VMFirmwareImportViewController : UITableViewController
@end

NS_ASSUME_NONNULL_END
