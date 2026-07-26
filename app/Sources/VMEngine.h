//
//  iOS3-VM — the emulator run loop.
//
//  The core in core/ is single-threaded and has no locks in it, by design. So
//  exactly one thread ever calls into it: the one this class owns. The UI never
//  touches the machine; it reads a snapshot this class publishes under a mutex.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>
#import "VMGuest.h"
#import "VMTouchMap.h"

/* The iPhone 3G's physical inputs, in the order the control bar shows them.
 * The ringer is a sliding switch rather than a button on the real device; it is
 * reported the same way here because nothing downstream can tell the difference
 * yet. See +inputUnavailableReason. */
typedef NS_ENUM(NSUInteger, VMButton) {
    VMButtonHome = 0,
    VMButtonPower,
    VMButtonVolumeUp,
    VMButtonVolumeDown,
    VMButtonRingerSilent,
    VMButtonCount
};

@interface VMEngine : NSObject

/* Allocate the machine, install the demo guest, and start interpreting on a
 * background thread. Returns YES when already running, and NO if startup fails
 * or a start/stop transition is already in progress. */
- (BOOL)start;

/* Ask the emulator thread to finish and release the machine. */
- (void)stop;

/* Suspend interpretation (used when the app leaves the foreground: burning a
 * core in the background is the fastest way to be terminated). */
- (void)setPaused:(BOOL)paused;

/* The emulator's own flags, so the run controls can be drawn from what the
 * machine is actually doing rather than from a copy the UI keeps and forgets
 * to update. -isPaused is the flag -setPaused: writes; -isRunning is whether a
 * live machine exists, which a guest halt or a reached instruction cap ends
 * without anyone pressing anything. */
- (BOOL)isPaused;
- (BOOL)isRunning;

/* Stop after this many retired instructions, 0 for no limit. Checked between
 * chunks on the emulator thread, so a change takes effect within a few tens of
 * milliseconds; setting a cap already passed stops at the next check. The last
 * frame is deliberately left on screen, because a diagnostic limit is a place
 * to look at rather than a failure to clear. */
- (void)setInstructionCap:(uint64_t)cap;
- (uint64_t)instructionCap;

/*
 * Copy the most recent framebuffer snapshot into `dst`. Returns NO — without
 * copying — if nothing new has been published since the last call, so the UI
 * can skip rebuilding an identical image.
 *
 * The geometry comes back with the pixels because it is not ours to assume:
 * vm_guest_display() reads it out of whichever CLCD window the guest enabled,
 * and the guest is free to program one that is not 320x480. `outStride` is
 * bytes per row; the copied region is `outStride * outHeight` bytes and never
 * exceeds VM_FB_BYTES. `outARGB` reports whether the bytes are A,R,G,B (YES)
 * or B,G,R,A (NO). Every out parameter may be NULL.
 */
- (BOOL)copyFrameInto:(void *)dst
             capacity:(size_t)capacity
                width:(uint32_t *)outWidth
               height:(uint32_t *)outHeight
               stride:(uint32_t *)outStride
                 argb:(BOOL *)outARGB;

/* Everything the guest has written to the UART since the last call, or nil. */
- (NSString *)takePendingConsoleText;

/* One line for the status bar: work done, rate, and memory footprint. */
- (NSString *)statusLine;

/* This process's phys_footprint — the number jetsam actually judges — in
 * bytes, or 0 if the kernel would not tell us. */
+ (uint64_t)physFootprintBytes;

#pragma mark - Guest input

/*
 * GUEST INPUT DOES NOT WORK, AND THIS IS WHERE THAT IS STATED.
 *
 * core/ models no digitizer, no PMU button line and no ringer GPIO, and the
 * app runs the synthetic guest in VMGuest.c, which would not read them if they
 * existed. So every method below records what was asked for and returns NO.
 *
 * It is a reason string rather than a bare BOOL on purpose: a control the user
 * cannot press must say why, the same way bootkernel's run header prints
 * "requested but NOT APPLIED" instead of quietly doing nothing. The UI asks
 * this class rather than hard-coding the answer, so the day a digitizer exists
 * the controls light up on their own.
 *
 * Returns nil once input really is delivered; non-nil, one line, until then.
 */
+ (NSString *)inputUnavailableReason;

/* Short label for a button, for the control bar and for logs. */
+ (NSString *)nameForButton:(VMButton)button;

/* Record a physical button's state. Returns whether the guest was told, which
 * is NO for as long as +inputUnavailableReason is non-nil. */
- (BOOL)setButton:(VMButton)button pressed:(BOOL)pressed;

/* Whether that button is currently held, as last recorded. */
- (BOOL)isButtonPressed:(VMButton)button;

/* Record a touch, already converted to guest pixels by vm_touch_map(). Same
 * contract as -setButton:pressed:. */
- (BOOL)sendTouchAtGuestX:(int)x y:(int)y phase:(vm_touch_phase_t)phase;

@end
