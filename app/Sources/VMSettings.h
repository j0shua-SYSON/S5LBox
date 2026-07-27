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
/*
 * DEVELOPER MODE. Off by default, and it is the switch that decides which app
 * this is.
 *
 * Off, the app shows a screen, five buttons, and a short settings list whose
 * rows are things a person can decide. On, it additionally shows the fourteen
 * device-tree and kernel-patch toggles that mirror the desktop harness, the
 * instruction cap, the raw guest console, and the diagnostics pages.
 *
 * Those fourteen rows are not hidden because they are dangerous -- none of
 * them does anything in this app yet -- but because leading with "MBX GPU ·
 * /arm-io/mbx" answers a question almost nobody arrived with. The desktop
 * harness is where bisecting a boot happens; this is a phone.
 */
/*
 * JAILBREAK, as one switch.
 *
 * The option table splits it in two, because the halves are independent and
 * fail independently: jb-codesign disables the guest's signature enforcement,
 * jb-payload installs Cydia into the work image. On the desktop that split is
 * exactly right -- either half alone is a useful experiment.
 *
 * Nobody arriving at a phone wants to choose between them. This is the pair,
 * and it is the harness's own --jailbreak compound.
 */
- (BOOL)jailbreakEnabled;
- (void)setJailbreakEnabled:(BOOL)enabled;

/*
 * Put the guest's console back under the picture, the way it was before the
 * console moved to its own screen. Developer-mode only, and off by default.
 *
 * Its one real use is watching output arrive WHILE the guest runs -- a
 * separate screen is better for reading and useless for noticing the moment
 * something appears. It costs a third of the picture, which is why it is a
 * choice rather than the layout.
 */
- (BOOL)inlineConsole;
- (void)setInlineConsole:(BOOL)inline_;

- (BOOL)developerMode;
- (void)setDeveloperMode:(BOOL)enabled;

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
