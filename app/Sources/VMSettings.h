//
//  S5LBox — the app's settings, and an honest account of which of them do
//  anything.
//
//  Two kinds of value live here and they are deliberately kept apart:
//
//  RECORDED ONLY. The boolean rows, which mirror bootkernel's BOOT_TOGGLES
//  through VMOptions.c. Every one of them describes what happens when Apple's
//  firmware is booted, and this app boots no firmware — it runs the synthetic
//  guest in VMGuest.c. They are stored, shown, and rendered back as a
//  bootkernel command line so a phone session and a desktop session can be
//  compared. They change nothing about the machine on screen, and the settings
//  screen says so above the first switch.
//
//  APPLIED. The instruction cap and the pause-on-background switch. Both are
//  real properties of this app's run loop and both take effect immediately.
//
//  Firmware paths are a third case: read-only. The app ships no firmware, has
//  no file picker, and downloads nothing, so all it can honestly do is name the
//  directory it would read and report whether a file is there.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>

#import "VMOptions.h"

/* Posted on the main thread after anything here changes, so the emulator screen
 * can re-apply the two settings that are actually applied. No object, no user
 * info: a reader re-reads, which is cheap and cannot go stale. */
extern NSString *const VMSettingsDidChangeNotification;

/* The file names the app looks for, so the settings screen and this class
 * cannot disagree about what to tell the user to copy where. */
extern NSString *const VMFirmwareKernelFile;
extern NSString *const VMFirmwareDeviceTreeFile;
extern NSString *const VMFirmwareRootFilesystemFile;
extern NSString *const VMFirmwareJailbreakPayloadFile;

@interface VMSettings : NSObject

+ (instancetype)sharedSettings;

#pragma mark - Recorded only (see the file note)

/* `index` is an index into VMOptions.c's table. An unset key reads as that
 * row's default rather than as NO, so adding a row to the table does not
 * silently turn it off for everyone who has already run the app. */
- (BOOL)valueForOptionIndex:(NSUInteger)index;
- (void)setValue:(BOOL)value forOptionIndex:(NSUInteger)index;

/* The bootkernel arguments these switches correspond to, or a stated
 * "everything is at its default" when they correspond to none. */
- (NSString *)equivalentToggleArguments;

#pragma mark - Applied

/* Retired instructions to stop after; 0 for no limit. */
- (uint64_t)instructionCap;
- (void)setInstructionCap:(uint64_t)cap;

/* The next cap in the cycle the settings screen offers, wrapping round. */
- (uint64_t)nextInstructionCap;

/* Suspend the emulator while the app is not frontmost. On by default: iOS
 * terminates apps that keep a core busy in the background. */
- (BOOL)pausesInBackground;
- (void)setPausesInBackground:(BOOL)pauses;

#pragma mark - Firmware (read-only: none is shipped and none can be chosen)

/* Where the app looks. Returned whether or not it exists, because the point of
 * printing it is to say where to put files. */
- (NSString *)firmwareDirectory;

/* The full path if that file is present there, or nil. */
- (NSString *)firmwarePathForFile:(NSString *)file;

/* "not supplied", or the file's size — one line for a table row. */
- (NSString *)statusForFirmwareFile:(NSString *)file;

#pragma mark - Housekeeping

/* Forget every key this class owns, so each one reads as its default again. */
- (void)resetToDefaults;

@end
