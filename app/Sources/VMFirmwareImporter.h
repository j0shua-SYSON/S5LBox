//
//  S5LBox -- running the C firmware importer off the main thread.
//
//  VMFirmwareImport.h is the whole of the importer, and it is plain C: no
//  threads in it, no file API, no Foundation. This is the shell a phone needs
//  around that -- a queue to run it on, the four-call file abstraction the core
//  asks for pointed at the app's own directories, a security-scoped read of
//  whatever the user picked, and progress and completion handed back on the
//  main queue.
//
//  WHAT IT DOES NOT DO, stated first because it is the question people ask.
//  This class holds no keys. There is no bundled key table, no download, no
//  keychain item and no NSUserDefaults key. The setters below take the text the
//  user typed, hand it straight to the C parser, and keep the parsed bytes in
//  this object's own memory until -forgetKeys or -dealloc overwrites them.
//  Nothing here writes a key to a file, a log, or the pasteboard.
//
//  THREADING, stated once so no caller has to guess. Call every method on the
//  main queue. The import itself runs on a private serial queue, and both
//  delegate callbacks arrive on the main queue. A run in flight holds a strong
//  reference to its importer, so this object cannot be deallocated out from
//  under the C core -- that is prevention, not tolerance, and it is the reason
//  the core can be handed a bridged pointer to it at all. The delegate is weak,
//  so a screen that goes away mid-import simply stops being told about it.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>

#import "VMFirmwareImport.h"

NS_ASSUME_NONNULL_BEGIN

@class VMFirmwareImporter;

/*
 * Both methods are optional and both arrive on the main queue. The C pointers
 * are annotated by hand: NS_ASSUME_NONNULL covers Objective-C pointers, and
 * leaving a plain C pointer unannotated inside an audited region is a warning
 * rather than a default.
 */
@protocol VMFirmwareImporterDelegate <NSObject>
@optional

/* Roughly every 4 MB of output, and at every stage change. `fraction` is
 * negative when the core has not said how much work there is -- the early
 * stages genuinely do not know, and drawing a bar at 0% for them would claim a
 * precision that does not exist. */
- (void)importer:(VMFirmwareImporter *)importer
   didReachStage:(vm_fw_stage_t)stage
     forArtefact:(vm_fw_artefact_t)artefact
        fraction:(double)fraction;

/* Exactly once per accepted -importIPSWAtURL:, including when the run failed
 * before the C core was reached; in that case the report carries a status and a
 * detail sentence and every artefact is still VM_FW_STATE_NOT_STARTED.
 *
 * `report` is valid only for the duration of this call. Copy it if you keep it;
 * it is a plain C struct and assignment is enough. */
- (void)importer:(VMFirmwareImporter *)importer
    didFinishWithStatus:(vm_fw_status_t)status
                 report:(const vm_fw_report_t *_Nonnull)report;

@end

@interface VMFirmwareImporter : NSObject

/*
 * THE ONE IMPORTER, and it outlives every screen.
 *
 * This used to be allocated by the firmware view controller, which meant the
 * keys the user typed lived exactly as long as that screen did: enter three
 * keys, leave, come back, and they were gone with no indication that anything
 * had been lost. Worse, dismissing the screen during a 433 MB extraction
 * destroyed the importer mid-run.
 *
 * The keys are still never written anywhere -- not to a file, not to
 * NSUserDefaults, not to the keychain -- which was always the point. What
 * changed is that "this session" now means the app, not the view.
 */
+ (instancetype)sharedImporter;

@property (nonatomic, weak, nullable) id<VMFirmwareImporterDelegate> delegate;

/* Safe from any thread. */
- (BOOL)isRunning;

/*
 * Start. Does nothing at all -- no callback, no delegate message -- if a run is
 * already in flight, so a double tap cannot start two.
 *
 * `url` is expected to be what UIDocumentPickerViewController hands back in
 * open mode: a security-scoped file URL. Access is taken here and released when
 * the run ends.
 */
- (void)importIPSWAtURL:(NSURL *)url;

/* Ask the run to stop. It ends with VM_FW_ERR_CANCELLED and every partly
 * written output deleted by the core's own close(keep=false). Harmless when
 * nothing is running. */
- (void)cancelImport;

#pragma mark - Keys the user supplies

/*
 * `which` must be VM_FW_KERNEL or VM_FW_DEVICE_TREE; the root filesystem takes
 * one combined string and has its own setter. The returned status is the C
 * parser's own -- VM_FW_ERR_KEY_NOT_HEX, VM_FW_ERR_KEY_WRONG_LENGTH -- so a
 * caller can say which of the two went wrong instead of "invalid key".
 */
- (vm_fw_status_t)setKeyHex:(NSString *)keyHex
                      ivHex:(NSString *)ivHex
                forArtefact:(vm_fw_artefact_t)which;

- (vm_fw_status_t)setRootFilesystemKeyHex:(NSString *)keyHex;

- (BOOL)haveKeyForArtefact:(vm_fw_artefact_t)which;

/* Overwrite every key held. Called by -dealloc too. */
- (void)forgetKeys;

#pragma mark - Reports

/* vm_fw_report_render as an NSString, with the buffer sizing done here. nil if
 * the report is nil or renders empty. */
+ (nullable NSString *)renderReport:(const vm_fw_report_t *_Nullable)report;

@end

NS_ASSUME_NONNULL_END
