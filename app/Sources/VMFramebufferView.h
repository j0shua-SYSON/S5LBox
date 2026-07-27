//
//  S5LBox — the guest's screen.
//
//  Displays a 32-bit guest framebuffer, scaled to fit while preserving the
//  original iPhone's 320x480 aspect ratio, with nearest-neighbour filtering so
//  a 2009 panel looks like a 2009 panel rather than a smeared upscale.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

#import "VMTouchMap.h"

@class VMFramebufferView;

/*
 * Where a finger is, in the guest's own coordinates: 0..319 across, 0..479
 * down, with the letterboxing the aspect-preserving fit introduces already
 * removed. Touches in the black bars are not reported at all.
 *
 * A gesture that starts on the panel always ends with an ended or cancelled
 * report, even if the finger has been dragged off the panel first, so a
 * receiver can never be left holding a touch that is no longer down. Moves
 * outside the panel are dropped; the ending coordinate is then the last one
 * that was on it.
 *
 * NOTHING RECEIVES THIS YET. VMEngine records the coordinate and discards it;
 * see +[VMEngine inputUnavailableReason]. The view controller displays it, so
 * that the mapping can be seen working before there is anything to work on.
 */
@protocol VMFramebufferViewTouchDelegate <NSObject>
- (void)framebufferView:(VMFramebufferView *)view
          touchAtGuestX:(int)x
                 guestY:(int)y
                  phase:(vm_touch_phase_t)phase;
@end

@interface VMFramebufferView : UIView

/*
 * Weak, as a delegate must be: the view controller owns the view. Setting it
 * turns user interaction on, and clearing it turns interaction back off — the
 * view is a picture of guest memory by default and only becomes an input
 * surface when something asks for the coordinates.
 */
@property (nonatomic, weak) id<VMFramebufferViewTouchDelegate> touchDelegate;

/*
 * Present one frame. `pixels` is `stride`-bytes-per-row, 32 bits per pixel.
 * When `argb` is NO the bytes are B,G,R,A (the S5L8900 framebuffer's native
 * order, see tools/bootkernel.c); when YES they are A,R,G,B. VMGuest reports
 * the validated host interpretation; the current CLCD model exposes only its
 * evidence-backed BGRA memory layout. The bytes are copied before returning,
 * so the caller may reuse the buffer immediately.
 */
- (void)presentPixels:(const void *)pixels
                width:(size_t)w
               height:(size_t)h
               stride:(size_t)stride
                 argb:(BOOL)argb;

@end
