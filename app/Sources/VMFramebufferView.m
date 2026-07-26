//
//  iOS3-VM — the guest's screen.
//
//  WHY A CGImage ON A CALayer AND NOT METAL
//
//  The panel is 320x480 at 32 bpp: 600 KB a frame, at most 60 times a second.
//  That is nothing. The expensive part of a display path at this size is not
//  pixel throughput, it is the amount of machinery that has to be right before
//  a single pixel appears — and a CAMetalLayer costs a device, a queue, a
//  pipeline state, a shader source file in the build, and a drawable lifecycle,
//  every piece of which is another thing that can be wrong on a phone I cannot
//  attach a debugger to. Handing CoreAnimation an immutable CGImage is two
//  calls, and the compositor already does the scaling on the GPU for free.
//
//  Nearest-neighbour comes from layer.magnificationFilter; aspect-correct
//  scaling from contentsGravity. The view controller *also* lays this view out
//  at the exact 320:480 aspect, so the two agree and no interpretation of
//  contentsScale can stretch the picture.
//
//  If the guest ever runs fast enough that per-frame texture upload matters,
//  this is the one file to replace. It is not the bottleneck today: the ARM
//  interpreter is, by four orders of magnitude.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMFramebufferView.h"
#import <QuartzCore/QuartzCore.h>
#include <limits.h>
#include <stdint.h>
#import <stdlib.h>
#import <string.h>

// CoreGraphics owns the copy until the CGImage dies; then it hands it back.
static void vm_fb_release_data(void *info, const void *data, size_t size) {
    (void)info;
    (void)size;
    free((void *)data);
}

// Declared up front so every call below is checked against a prototype.
@interface VMFramebufferView ()
- (void)reportTouches:(NSSet<UITouch *> *)touches phase:(vm_touch_phase_t)phase;
@end

@implementation VMFramebufferView {
    CGColorSpaceRef _colorSpace;

    /* What the last presented frame was, so a touch is mapped against the
     * geometry actually on screen rather than against an assumption. */
    unsigned _guestWidth;
    unsigned _guestHeight;

    /* The gesture state machine lives in VMTouchMap.c, tested by
     * app/Tests/test_vmtouchmap.c, because its rules are the contract this
     * view's header states and a contract deserves assertions. This file is
     * then only the UIKit half: read the point, map it, feed the tracker. */
    vm_touch_tracker_t _tracker;
}

@synthesize touchDelegate = _touchDelegate;

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;

    _colorSpace = CGColorSpaceCreateDeviceRGB();

    /* The original panel, until a frame says otherwise. Written as the numbers
     * rather than VMGuest.h's macros so this view stays a display of whatever
     * it is handed rather than a display of one particular guest. */
    _guestWidth  = 320u;
    _guestHeight = 480u;
    vm_touch_tracker_reset(&_tracker);

    self.backgroundColor = [UIColor blackColor];
    self.opaque = YES;
    self.userInteractionEnabled = NO;
    self.clearsContextBeforeDrawing = NO;

    self.layer.magnificationFilter = kCAFilterNearest;
    self.layer.minificationFilter  = kCAFilterNearest;
    self.layer.contentsGravity     = kCAGravityResizeAspect;
    self.layer.needsDisplayOnBoundsChange = NO;
    self.layer.backgroundColor = [UIColor blackColor].CGColor;
    return self;
}

- (void)dealloc {
    if (_colorSpace) CGColorSpaceRelease(_colorSpace);
}

- (void)setTouchDelegate:(id<VMFramebufferViewTouchDelegate>)touchDelegate {
    _touchDelegate = touchDelegate;
    /* Interaction follows the delegate rather than being a second switch the
     * caller can forget to throw. Turning it off also drops any gesture in
     * progress, so nothing is left half-reported. */
    self.userInteractionEnabled = (touchDelegate != nil);
    if (!touchDelegate) vm_touch_tracker_reset(&_tracker);
}

- (void)presentPixels:(const void *)pixels
                width:(size_t)w
               height:(size_t)h
               stride:(size_t)stride
                 argb:(BOOL)argb {
    if (!pixels || w == 0 || h == 0 || !_colorSpace ||
        w > SIZE_MAX / 4 || stride < w * 4 || h > SIZE_MAX / stride) return;

    /* Only after the frame has been accepted: a rejected frame must not move
     * the coordinate system a touch is measured against. */
    if (w <= UINT_MAX && h <= UINT_MAX) {
        _guestWidth  = (unsigned)w;
        _guestHeight = (unsigned)h;
    }

    const size_t bytes = stride * h;

    // The CGImage must own immutable pixels: the emulator thread is free to
    // repaint its framebuffer the instant this method returns.
    void *copy = malloc(bytes);
    if (!copy) return;
    memcpy(copy, pixels, bytes);

    CGDataProviderRef provider =
        CGDataProviderCreateWithData(NULL, copy, bytes, vm_fb_release_data);
    if (!provider) { free(copy); return; }

    // Alpha is always skipped, never honoured: the guest's alpha byte is not
    // trustworthy (XNU's console leaves it zero), and composited it would erase
    // the whole screen. That leaves only the component order to get right, and
    // it is the same "alpha first" layout either way, differing only in endian:
    //   B,G,R,A in memory  == 0xAARRGGBB little-endian  -> ByteOrder32Little
    //   A,R,G,B in memory  == 0xAARRGGBB big-endian     -> ByteOrder32Big
    CGBitmapInfo info = (CGBitmapInfo)(kCGImageAlphaNoneSkipFirst
        | (argb ? kCGBitmapByteOrder32Big : kCGBitmapByteOrder32Little));

    CGImageRef image = CGImageCreate(
        w, h, 8, 32, stride, _colorSpace, info,
        provider, NULL, false, kCGRenderingIntentDefault);

    CGDataProviderRelease(provider);   // frees `copy` too, if the image failed
    if (!image) return;

    self.layer.contents = (__bridge id)image;
    CGImageRelease(image);
}

#pragma mark - Touches

/*
 * multipleTouchEnabled is left at its default of NO, so UIKit delivers one
 * touch at a time and -anyObject is that touch. A guest that predates
 * multi-touch APIs on the host side is not the reason — the reason is that
 * nothing downstream can carry a second finger, and pretending otherwise here
 * would be a shape to unpick later rather than a feature.
 */
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    [self reportTouches:touches phase:VM_TOUCH_BEGAN];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];
    [self reportTouches:touches phase:VM_TOUCH_MOVED];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];
    [self reportTouches:touches phase:VM_TOUCH_ENDED];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesCancelled:touches withEvent:event];
    [self reportTouches:touches phase:VM_TOUCH_CANCELLED];
}

- (void)reportTouches:(NSSet<UITouch *> *)touches phase:(vm_touch_phase_t)phase {
    id<VMFramebufferViewTouchDelegate> delegate = self.touchDelegate;
    UITouch *touch = [touches anyObject];
    if (!delegate || !touch) {
        vm_touch_tracker_reset(&_tracker);
        return;
    }

    const CGSize size = self.bounds.size;
    const CGPoint point = [touch locationInView:self];
    const vm_touch_point_t guest =
        vm_touch_map((double)size.width, (double)size.height,
                     _guestWidth, _guestHeight,
                     (double)point.x, (double)point.y);

    int x = 0, y = 0;
    if (!vm_touch_track(&_tracker, phase, guest, &x, &y)) return;

    [delegate framebufferView:self touchAtGuestX:x guestY:y phase:phase];
}

@end
