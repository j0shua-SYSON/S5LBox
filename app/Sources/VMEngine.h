//
//  S5LBox — the emulator run loop.
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
 * WHAT REACHES THE GUEST, AND WHAT DOES NOT.
 *
 * These are reason strings rather than bare BOOLs on purpose: a control the
 * user cannot press must say why, the same way bootkernel's run header prints
 * "requested but NOT APPLIED" instead of quietly doing nothing. The UI asks
 * this class rather than hard-coding the answer, so each control lights up on
 * its own the day its path exists.
 *
 * BUTTONS NOW WORK — as far as the board. core/src/soc/buttons.c models all
 * five switches on the GPIO pins and interrupt lines /device-tree/buttons
 * names, and -setButton:pressed: queues a transition that the emulator thread
 * hands to that model. This class method therefore returns nil: the path
 * exists. Whether a particular guest is listening is a different and live
 * question, and -buttonUnavailableReason below is where it is asked.
 */
+ (NSString *)buttonUnavailableReason;

/*
 * Whether the GUEST can currently see a button, as opposed to whether this app
 * can send one.
 *
 * Returns nil once the board has accepted at least one transition; otherwise
 * one line saying what it is waiting for. The most likely answer while this app
 * runs the synthetic guest in VMGuest.c is that no driver has armed the button
 * interrupt lines — which is the truthful answer, because AppleM68Buttons never
 * polls a pin. It samples only after an interrupt it armed itself, so a press
 * on a line nobody armed is one the guest provably cannot observe, and the
 * board says so rather than pretending.
 */
- (NSString *)buttonUnavailableReason;

/* Button delivery counters, for the status line and the log. `delivered` counts
 * transitions the BOARD accepted; it is the only one that means the guest was
 * offered anything. `refused` counts board refusals, which are retried. */
- (void)buttonCountersQueued:(uint64_t *)queued
                   delivered:(uint64_t *)delivered
                     refused:(uint64_t *)refused
                     dropped:(uint64_t *)dropped;

/*
 * TOUCH does work — as far as the device. -sendTouchAtGuestX:y:phase: queues a
 * report and the emulator thread hands it to the emulated Z2 controller, which
 * reports it through its own registers when the guest's own driver reads them.
 *
 * Whether the guest then does anything with it is NOT this class's claim to
 * make, and this method will not make it. It reports only what it can see: the
 * device either accepted a report or refused it. A refusal is the truthful
 * answer while this app runs the synthetic guest in VMGuest.c, which has no
 * AppleMultitouchZ2 driver to have announced itself — the device declines to
 * queue a report that provably could not be read.
 *
 * Returns nil once the device has accepted at least one report; otherwise one
 * line saying what it last refused for.
 */
- (NSString *)touchUnavailableReason;

/* Short label for a button, for the control bar and for logs. */
+ (NSString *)nameForButton:(VMButton)button;

/*
 * Move a physical switch.
 *
 * Returns whether the transition was QUEUED — not whether the guest saw it. The
 * emulator thread hands it to the board between chunks, and the board is
 * entitled to refuse while the guest has not armed that line or has not
 * serviced the previous transition; a refusal is retried, not lost, and shows
 * up in -buttonUnavailableReason. A NO here means it never got that far: the
 * machine is not running, the button is not one of the five, or the queue was
 * full of edges that must not be coalesced away.
 */
- (BOOL)setButton:(VMButton)button pressed:(BOOL)pressed;

/* Whether that button is currently held, as last recorded. */
- (BOOL)isButtonPressed:(VMButton)button;

/*
 * Queue a touch, already converted to guest pixels by vm_touch_map().
 *
 * Returns whether the report was QUEUED — not whether the guest saw it. The
 * emulator thread hands it to the device between chunks, and the device is
 * entitled to refuse; -touchUnavailableReason is where that shows up. A NO
 * here means the report never even got that far: the machine is not running,
 * the coordinate is off the panel, or the queue was full of phase edges that
 * must not be coalesced away.
 */
- (BOOL)sendTouchAtGuestX:(int)x y:(int)y phase:(vm_touch_phase_t)phase;

/* Touch delivery counters, for the status line and the log. `delivered` counts
 * reports the DEVICE accepted; it is the only one of these that means the
 * guest was actually offered a frame. */
- (void)touchCountersQueued:(uint64_t *)queued
                  delivered:(uint64_t *)delivered
                  coalesced:(uint64_t *)coalesced
                    dropped:(uint64_t *)dropped;

@end
