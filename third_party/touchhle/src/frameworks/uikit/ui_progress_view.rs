/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! The iPhone OS 2/3 progress view API. The track is drawn locally; its shading
//! approximates the original controls rather than copying UIKit image assets.

use crate::frameworks::core_graphics::cg_context::{CGContextFillRect, CGContextSetRGBFillColor};
use crate::frameworks::core_graphics::{CGPoint, CGRect, CGSize};
use crate::frameworks::foundation::{ns_string::get_static_str, NSInteger};
use crate::frameworks::uikit::ui_graphics::UIGraphicsGetCurrentContext;
use crate::objc::{
    id, impl_HostObject_with_superclass, msg, msg_super, objc_classes, ClassExports, NSZonePtr,
};

#[derive(Default)]
pub struct ProgressView {
    superclass: super::ui_view::UIViewHostObject,
    progress: f32,
    style: NSInteger,
}
impl_HostObject_with_superclass!(ProgressView);

pub const CLASSES: ClassExports = objc_classes! {
(env, this, _cmd);
@implementation UIProgressView: UIView
+ (id)allocWithZone:(NSZonePtr)_zone {
    env.objc.alloc_object(this, Box::<ProgressView>::default(), &mut env.mem)
}
- (id)initWithFrame:(CGRect)frame {
    let this: id = msg_super![env; this initWithFrame:frame];
    () = msg![env; this setOpaque:false];
    () = msg![env; this setUserInteractionEnabled:false];
    this
}
- (id)initWithProgressViewStyle:(NSInteger)style {
    let frame = CGRect { origin: CGPoint::default(), size: CGSize { width: 0.0, height: 9.0 } };
    let this: id = msg![env; this initWithFrame:frame];
    () = msg![env; this setProgressViewStyle:style];
    this
}
- (id)initWithCoder:(id)coder {
    let this: id = msg_super![env; this initWithCoder:coder];
    let key = get_static_str(env, "UIProgressViewStyle");
    let style: NSInteger = msg![env; coder decodeIntForKey:key];
    () = msg![env; this setProgressViewStyle:style];
    let key = get_static_str(env, "UIProgress");
    let progress: f32 = msg![env; coder decodeFloatForKey:key];
    () = msg![env; this setProgress:progress];
    () = msg![env; this setOpaque:false];
    () = msg![env; this setUserInteractionEnabled:false];
    this
}
- (f32)progress { env.objc.borrow::<ProgressView>(this).progress }
- (())setProgress:(f32)progress {
    // The public API pins finite values and infinities to [0, 1]. Treat NaN
    // as an empty track so it cannot contaminate drawing coordinates.
    env.objc.borrow_mut::<ProgressView>(this).progress = if progress.is_nan() { 0.0 } else { progress.clamp(0.0, 1.0) };
    () = msg![env; this setNeedsDisplay];
}
- (NSInteger)progressViewStyle { env.objc.borrow::<ProgressView>(this).style }
- (())setProgressViewStyle:(NSInteger)style {
    assert!(style == 0 || style == 1, "Unsupported UIProgressViewStyle: {}", style);
    env.objc.borrow_mut::<ProgressView>(this).style = style;
    () = msg![env; this setNeedsDisplay];
}
- (CGSize)sizeThatFits:(CGSize)size { CGSize { width: size.width, height: 9.0 } }
- (())setFrame:(CGRect)frame {
    () = msg_super![env; this setFrame:frame];
    () = msg![env; this setNeedsDisplay];
}
- (())setBounds:(CGRect)bounds {
    () = msg_super![env; this setBounds:bounds];
    () = msg![env; this setNeedsDisplay];
}
- (())drawRect:(CGRect)_rect {
    let bounds: CGRect = msg![env; this bounds];
    let progress = env.objc.borrow::<ProgressView>(this).progress;
    let style = env.objc.borrow::<ProgressView>(this).style;
    if bounds.size.width <= 0.0 || bounds.size.height <= 0.0 { return; }
    let context = UIGraphicsGetCurrentContext(env);
    let height = bounds.size.height.min(9.0);
    let track = CGRect { origin: CGPoint { x: bounds.origin.x, y: bounds.origin.y + (bounds.size.height-height)/2.0 },
        size: CGSize { width: bounds.size.width, height } };
    let border = if style == 0 { 0.42 } else { 0.24 };
    CGContextSetRGBFillColor(env, context, border, border, border, 1.0);
    CGContextFillRect(env, context, track);
    let width = (track.size.width-2.0).max(0.0);
    let inner_height = (height-2.0).max(0.0);
    // A bounded number of horizontal strips also handles fractional heights.
    for row in 0..inner_height.ceil() as usize {
        let y = row as f32;
        let shade = y / inner_height.max(1.0);
        let strip = CGRect { origin: CGPoint { x: track.origin.x+1.0, y: track.origin.y+1.0+y },
            size: CGSize { width, height: (inner_height-y).min(1.0) } };
        let gray = if style == 0 { 0.78+0.16*shade } else { 0.40+0.18*shade };
        CGContextSetRGBFillColor(env, context, gray, gray, gray, 1.0);
        CGContextFillRect(env, context, strip);
        let (r,g,b) = if style == 0 { (0.15+0.12*shade, 0.43+0.16*shade, 0.86+0.12*shade) }
            else { (0.70+0.22*shade, 0.75+0.19*shade, 0.82+0.16*shade) };
        CGContextSetRGBFillColor(env, context, r, g, b, 1.0);
        CGContextFillRect(env, context, CGRect { size: CGSize { width: width*progress, ..strip.size }, ..strip });
    }
}
@end
};
