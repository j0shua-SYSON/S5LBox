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
//  It leads with what it cannot do, because that is the larger half. Every
//  payload in a 3.x IPSW is encrypted with a key that is not in the archive and
//  cannot be worked out from it. S5LBox ships no keys, downloads none and
//  computes none; where one is needed the screen names the artefact, says what
//  kind of key it is, and offers a field for the user to paste their own. It
//  names no source for one, and it never will.
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
