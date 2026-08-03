//
//  S5LBox — the app's settings, and an honest account of which of them do
//  anything.
//
//  Two kinds of value live here and they are deliberately kept apart:
//
//  THE OPTION ROWS, which mirror bootkernel's BOOT_TOGGLES through VMOptions.c.
//  This class only STORES them. What each one does to a real bring-up — reach
//  the request, get written into the work image, or reach nothing at all — is
//  decided in app/Sources/VMBootOptions.c and is different per row. That used
//  to be one sentence here saying none of them did anything, which stopped
//  being true the day the app booted firmware; it is a tested table now
//  precisely so it cannot go stale again.
//
//  APPLIED HERE. The instruction cap and the pause-on-background switch. Both
//  are real properties of this app's run loop and both take effect immediately.
//
//  APP-WIDE, NOT PER MACHINE. These are NSUserDefaults keys, so every machine
//  shares them. VMInstanceStore carries a per-instance option array that
//  nothing writes yet; when the settings screen becomes per-machine it becomes
//  the source and -[VMEngine copyOptionValuesInto:capacity:] is the one place
//  that changes.
//
//  Firmware paths are a third case: reported, not stored. The app ships no
//  firmware and downloads none, so all this class does is name the directory it
//  would read and say whether a file is there. Putting files in that directory
//  is VMFirmwareImporter's job, from an IPSW the user already has.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>

#import "VMOptions.h"

/* Posted on the main thread after anything here changes, so the emulator screen
 * can re-apply the two settings that are actually applied. No object, no user
 * info: a reader re-reads, which is cheap and cannot go stale. */
extern NSString *const VMSettingsDidChangeNotification;

/* The three file names the emulator accepts, which are also exactly what the
 * importer writes -- vm_fw_artefact_filename() in VMFirmwareImport.h produces
 * the same three strings. Named here as well so the settings screen, this
 * class and the importer cannot disagree about what a present file is called.
 * The fourth is the jailbreak payload, which no importer produces. */
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
 * rows are things a person can decide. On, it additionally shows the sixteen
 * device-tree and kernel-patch toggles that mirror the desktop harness, the
 * instruction cap, the raw guest console, and the diagnostics pages.
 *
 * Those rows are not hidden because they are dangerous. Several now reach the
 * real bring-up or work-image provisioner and the rest state their limits;
 * they stay behind Developer Mode because leading with device-tree paths and
 * experimental MBX controls answers a question almost nobody arrived with.
 * The desktop harness is where bisecting a boot happens; this is a phone.
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

#pragma mark - Firmware (reported only: none is shipped and none is downloaded)

/* Where the app looks. Returned whether or not it exists, because the point of
 * printing it is to say where to put files. */
/*
 * Create the folders the user is expected to put things into, and return YES
 * if they exist afterwards.
 *
 * This is not housekeeping. UIFileSharingEnabled shows Documents in Files, and
 * a Documents that contains nothing shows an EMPTY folder -- so a user told to
 * "drop an IPSW in the firmware folder" opens Files, finds no such folder, and
 * has nowhere to put it. The folders have to exist before anyone looks, which
 * means at launch rather than at first use.
 *
 * It also drops a short README beside them, because an empty folder in Files
 * explains nothing about what belongs in it.
 */
- (BOOL)ensureUserVisibleDirectories;

/* Every *.ipsw sitting in the firmware folder or at the top of Documents,
 * newest first. This is what makes "detect" possible without a picker. */
- (NSArray<NSString *> *)detectedArchivePaths;

- (NSString *)firmwareDirectory;

/* The full path if that file is present there, or nil. */
- (NSString *)firmwarePathForFile:(NSString *)file;

/* "not supplied", or the file's size — one line for a table row. */
- (NSString *)statusForFirmwareFile:(NSString *)file;

#pragma mark - Housekeeping

/* Forget every key this class owns, so each one reads as its default again. */
- (void)resetToDefaults;

@end
