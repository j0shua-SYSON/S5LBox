/*
 * iOS3-VM — turning a finger on the host's screen into a guest coordinate.
 *
 * The guest panel is 320x480 and the host's is not, so the picture is fitted
 * inside the view with its aspect ratio preserved and the leftover space is
 * black bars. A touch therefore has to be un-fitted before the guest could ever
 * be told about it: the bars are not part of the panel, and the scale factor
 * between the two is almost never a round number.
 *
 * That is arithmetic, and arithmetic is the part of a UI that can be tested
 * without a device, a simulator, or an Apple toolchain — so it lives here, in
 * plain C11 with no UIKit and no Objective-C, and core/CMakeLists.txt builds
 * app/Tests/test_vmtouchmap.c against it on every host CI runner. The
 * Objective-C in VMFramebufferView.m is then only glue: read the point, call
 * this, hand the result to a delegate.
 *
 * NOTHING HERE DELIVERS A TOUCH TO THE GUEST. core/ models no digitizer, so a
 * mapped coordinate is currently displayed and discarded; see
 * +[VMEngine inputUnavailableReason]. This file exists so that the day a
 * digitizer is modelled, the coordinate handed to it is already correct.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_APP_VMTOUCHMAP_H
#define IOS3VM_APP_VMTOUCHMAP_H

#include <stdbool.h>

/* Which end of a gesture a report belongs to. Declared here rather than in
 * VMEngine.h so the view that produces touches and the engine that will one day
 * consume them agree on a type neither of them owns. */
typedef enum {
    VM_TOUCH_BEGAN = 0,
    VM_TOUCH_MOVED,
    VM_TOUCH_ENDED,
    VM_TOUCH_CANCELLED
} vm_touch_phase_t;

/* A point on the guest's panel. `x` and `y` are meaningful only when `inside`
 * is true; they are zeroed otherwise, so a caller that forgets to check gets a
 * corner rather than an out-of-range index. */
typedef struct {
    int  x;
    int  y;
    bool inside;
} vm_touch_point_t;

/*
 * Map a point in a view's coordinate space (points, origin top-left) to a
 * guest pixel, assuming the guest's `guest_w` x `guest_h` image is drawn
 * centred inside `view_w` x `view_h` at the largest scale that fits without
 * distortion — which is what CALayer's kCAGravityResizeAspect does, and what
 * EmulatorViewController's own frame arithmetic agrees with.
 *
 * The panel is treated as a HALF-OPEN rectangle: the point exactly on the far
 * edge belongs to the pixel after the last one and is reported outside, the
 * same way pixel 320 of a 320-wide row does not exist. That is deliberate --
 * clamping the edge inwards would mean a touch in the black bar, one hair
 * outside the picture, silently became a touch on the guest's last column.
 *
 * Returns inside == false, and zeroed coordinates, for: a zero or negative
 * view, a zero-sized guest, a guest too large to index with an int, any
 * non-finite input, and any point in the letterbox.
 */
vm_touch_point_t vm_touch_map(double view_w, double view_h,
                              unsigned guest_w, unsigned guest_h,
                              double point_x, double point_y);

/*
 * The gesture state machine that sits on top of the mapping.
 *
 * Down here rather than in the view for the same reason as the arithmetic: it
 * is the part with STATE, and stateful UI code is exactly what cannot be
 * exercised without a device. The rules it enforces are the contract
 * VMFramebufferView.h states, so they are worth an assertion rather than a
 * paragraph:
 *
 *   - a gesture that begins in the letterbox is not reported at all, and also
 *     ends any gesture the tracker still thought was running, so a lost end
 *     can never let one gesture inherit the previous one's coordinates;
 *   - a move or an end without a matching begin is never reported;
 *   - a move that leaves the panel is dropped, but the gesture stays live, so
 *     its end is still reported -- at the last coordinate that was on-panel.
 *     Nothing downstream is left holding a touch that is no longer down.
 */
typedef struct {
    bool active;   /* a gesture that began on the panel is in progress */
    int  x, y;     /* the last coordinate of it that was on the panel  */
} vm_touch_tracker_t;

/* Forget any gesture in progress. */
void vm_touch_tracker_reset(vm_touch_tracker_t *tracker);

/*
 * Feed one event: its phase, and what vm_touch_map() made of its location.
 * Returns true when the event should be reported onwards, and only then writes
 * the coordinate to report through `out_x` and `out_y`, either of which may be
 * NULL. A NULL tracker reports nothing.
 */
bool vm_touch_track(vm_touch_tracker_t *tracker, vm_touch_phase_t phase,
                    vm_touch_point_t mapped, int *out_x, int *out_y);

#endif /* IOS3VM_APP_VMTOUCHMAP_H */
