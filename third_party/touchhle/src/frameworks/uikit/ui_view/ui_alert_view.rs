/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Modal UIKit alerts. Presentation animations and iOS 5 text-entry styles
//! are not implemented; buttons and delegate lifecycle use the normal UI loop.

use super::UIViewHostObject;
use crate::frameworks::core_graphics::cg_affine_transform::CGAffineTransform;
use crate::frameworks::core_graphics::{CGFloat, CGPoint, CGRect, CGSize};
use crate::frameworks::foundation::NSInteger;
use crate::objc::{
    id, impl_HostObject_with_superclass, msg, msg_class, msg_send, msg_super, nil, objc_classes,
    release, retain, ClassExports, NSZonePtr,
};
use crate::window::DeviceOrientation;
use crate::Environment;

struct UIAlertViewHostObject {
    superclass: UIViewHostObject,
    title: id,
    message: id,
    delegate: id,     // weak, as on iPhone OS
    buttons: Vec<id>, // copied strings
    cancel_index: NSInteger,
    window: id,          // owned while presented
    previous_window: id, // retained until dismissal
    content: id,         // owned generated content; caller subviews are preserved
    visible: bool,
    dismissing: bool,
    handling_click: bool,
    presentation: u64,
}
impl_HostObject_with_superclass!(UIAlertViewHostObject);
impl Default for UIAlertViewHostObject {
    fn default() -> Self {
        Self {
            superclass: Default::default(),
            title: nil,
            message: nil,
            delegate: nil,
            buttons: vec![],
            cancel_index: -1,
            window: nil,
            previous_window: nil,
            content: nil,
            visible: false,
            dismissing: false,
            handling_click: false,
            presentation: 0,
        }
    }
}

fn notify(env: &mut Environment, alert: id, name: &str, index: Option<NSInteger>) {
    let delegate = env.objc.borrow::<UIAlertViewHostObject>(alert).delegate;
    if delegate == nil {
        return;
    }
    let selector = env
        .objc
        .register_host_selector(name.to_string(), &mut env.mem);
    if !msg![env; delegate respondsToSelector:selector] {
        return;
    }
    retain(env, delegate);
    if let Some(index) = index {
        () = msg_send(env, (delegate, selector, alert, index));
    } else {
        () = msg_send(env, (delegate, selector, alert));
    }
    release(env, delegate);
}

fn frame(x: CGFloat, y: CGFloat, width: CGFloat, height: CGFloat) -> CGRect {
    CGRect {
        origin: CGPoint { x, y },
        size: CGSize { width, height },
    }
}

fn label(
    env: &mut Environment,
    parent: id,
    text: id,
    width: CGFloat,
    y: CGFloat,
    bold: bool,
) -> CGFloat {
    if text == nil {
        return y;
    }
    let font: id = if bold {
        msg_class![env; UIFont boldSystemFontOfSize:18.0f32]
    } else {
        msg_class![env; UIFont systemFontOfSize:16.0f32]
    };
    let size: CGSize = msg![env; text sizeWithFont:font
        constrainedToSize:(CGSize { width: width - 24.0, height: 10000.0 })
        lineBreakMode:0i32];
    let height = size.height.max(22.0);
    let label: id = msg_class![env; UILabel alloc];
    let label: id = msg![env; label initWithFrame:(frame(12.0, y, width - 24.0, height))];
    let white: id = msg_class![env; UIColor whiteColor];
    let clear: id = msg_class![env; UIColor clearColor];
    () = msg![env; label setText:text];
    () = msg![env; label setFont:font];
    () = msg![env; label setTextColor:white];
    () = msg![env; label setBackgroundColor:clear];
    () = msg![env; label setTextAlignment:1i32];
    () = msg![env; label setLineBreakMode:0i32];
    () = msg![env; label setNumberOfLines:0i32];
    () = msg![env; parent addSubview:label];
    release(env, label);
    y + height + 10.0
}

fn layout(env: &mut Environment, alert: id) {
    let state = env.objc.borrow::<UIAlertViewHostObject>(alert);
    let (window, title, message, buttons) = (
        state.window,
        state.title,
        state.message,
        state.buttons.clone(),
    );
    if window == nil {
        return;
    }
    let old = std::mem::replace(
        &mut env.objc.borrow_mut::<UIAlertViewHostObject>(alert).content,
        nil,
    );
    if old != nil {
        () = msg![env; old removeFromSuperview];
        release(env, old);
    }
    let bounds: CGRect = msg![env; window bounds];
    let rotation = env
        .window
        .as_ref()
        .map(|w| w.current_rotation())
        .unwrap_or(DeviceOrientation::Portrait);
    let (available_width, available_height, angle) = match rotation {
        DeviceOrientation::LandscapeLeft => (
            bounds.size.height,
            bounds.size.width,
            std::f32::consts::FRAC_PI_2,
        ),
        DeviceOrientation::LandscapeRight => (
            bounds.size.height,
            bounds.size.width,
            -std::f32::consts::FRAC_PI_2,
        ),
        DeviceOrientation::PortraitUpsideDown => {
            (bounds.size.width, bounds.size.height, std::f32::consts::PI)
        }
        DeviceOrientation::Portrait => (bounds.size.width, bounds.size.height, 0.0),
    };
    let width = (available_width - 40.0).clamp(160.0, 420.0);
    let content: id = msg_class![env; UIScrollView alloc];
    let content: id = msg![env; content initWithFrame:(frame(0.0, 0.0, width, 1.0))];
    let clear: id = msg_class![env; UIColor clearColor];
    () = msg![env; content setBackgroundColor:clear];
    let mut y = label(env, content, title, width, 12.0, true);
    y = label(env, content, message, width, y, false);
    let action = env
        .objc
        .register_host_selector("_touchHLE_alertButton:".to_string(), &mut env.mem);
    let columns = if buttons.len() == 2 { 2 } else { 1 };
    let button_width = (width - 24.0 - 8.0 * (columns - 1) as f32) / columns as f32;
    for (index, title) in buttons.iter().enumerate() {
        let button: id = msg_class![env; UIButton buttonWithType:1i32];
        let button_frame = frame(
            12.0 + (index % columns) as f32 * (button_width + 8.0),
            y + (index / columns) as f32 * 46.0,
            button_width,
            38.0,
        );
        () = msg![env; button setFrame:button_frame];
        () = msg![env; button setTitle:(*title) forState:0u32];
        () = msg![env; button setTag:(index as NSInteger)];
        () = msg![env; button addTarget:alert action:action forControlEvents:64u32];
        () = msg![env; content addSubview:button];
    }
    y += buttons.len().div_ceil(columns) as f32 * 46.0 + 8.0;
    let height = y.min(available_height - 40.0).max(40.0);
    () = msg![env; content setFrame:(frame(0.0, 0.0, width, height))];
    () = msg![env; content setContentSize:(CGSize { width, height: y })];
    () = msg![env; alert setBounds:(frame(0.0, 0.0, width, height))];
    () = msg![env; alert setCenter:(CGPoint { x: bounds.size.width / 2.0, y: bounds.size.height / 2.0 })];
    () = msg![env; alert setTransform:(CGAffineTransform::make_rotation(angle))];
    let background: id =
        msg_class![env; UIColor colorWithRed:0.12f32 green:0.17f32 blue:0.28f32 alpha:1.0f32];
    () = msg![env; alert setBackgroundColor:background];
    () = msg![env; alert addSubview:content];
    env.objc.borrow_mut::<UIAlertViewHostObject>(alert).content = content;
}

pub const CLASSES: ClassExports = objc_classes! {
(env, this, _cmd);

@implementation UIAlertView: UIView

+ (id)allocWithZone:(NSZonePtr)_zone {
    env.objc.alloc_object(this, Box::<UIAlertViewHostObject>::default(), &mut env.mem)
}

- (id)initWithTitle:(id)title message:(id)message delegate:(id)delegate
    cancelButtonTitle:(id)cancelButtonTitle otherButtonTitles:(id)otherButtonTitles, ...args {
    let this: id = msg_super![env; this init];
    () = msg![env; this setTitle:title];
    () = msg![env; this setMessage:message];
    () = msg![env; this setDelegate:delegate];
    if cancelButtonTitle != nil {
        let index: NSInteger = msg![env; this addButtonWithTitle:cancelButtonTitle];
        () = msg![env; this setCancelButtonIndex:index];
    }
    if otherButtonTitles != nil {
        let _: NSInteger = msg![env; this addButtonWithTitle:otherButtonTitles];
        let mut arguments = args.start();
        loop {
            let title: id = arguments.next(env);
            if title == nil { break; }
            let _: NSInteger = msg![env; this addButtonWithTitle:title];
        }
    }
    this
}

- (id)title { env.objc.borrow::<UIAlertViewHostObject>(this).title }
- (())setTitle:(id)title {
    let title: id = msg![env; title copy];
    let old = std::mem::replace(&mut env.objc.borrow_mut::<UIAlertViewHostObject>(this).title, title);
    release(env, old);
    layout(env, this);
}
- (id)message { env.objc.borrow::<UIAlertViewHostObject>(this).message }
- (())setMessage:(id)message {
    let message: id = msg![env; message copy];
    let old = std::mem::replace(&mut env.objc.borrow_mut::<UIAlertViewHostObject>(this).message, message);
    release(env, old);
    layout(env, this);
}
- (id)delegate { env.objc.borrow::<UIAlertViewHostObject>(this).delegate }
- (())setDelegate:(id)delegate { env.objc.borrow_mut::<UIAlertViewHostObject>(this).delegate = delegate; }
- (bool)isVisible { env.objc.borrow::<UIAlertViewHostObject>(this).visible }
- (NSInteger)numberOfButtons { env.objc.borrow::<UIAlertViewHostObject>(this).buttons.len() as NSInteger }
- (NSInteger)cancelButtonIndex { env.objc.borrow::<UIAlertViewHostObject>(this).cancel_index }
- (())setCancelButtonIndex:(NSInteger)index { env.objc.borrow_mut::<UIAlertViewHostObject>(this).cancel_index = index; }
- (NSInteger)firstOtherButtonIndex {
    let state = env.objc.borrow::<UIAlertViewHostObject>(this);
    (0..state.buttons.len() as NSInteger).find(|index| *index != state.cancel_index).unwrap_or(-1)
}
- (id)buttonTitleAtIndex:(NSInteger)index {
    env.objc.borrow::<UIAlertViewHostObject>(this).buttons.get(index as usize).copied().unwrap_or(nil)
}
- (NSInteger)addButtonWithTitle:(id)title {
    if title == nil { return -1; }
    let title: id = msg![env; title copy];
    let buttons = &mut env.objc.borrow_mut::<UIAlertViewHostObject>(this).buttons;
    let index = buttons.len() as NSInteger;
    buttons.push(title);
    layout(env, this);
    index
}
- (())show {
    let state = env.objc.borrow::<UIAlertViewHostObject>(this);
    if state.visible || state.dismissing { return; }
    retain(env, this);
    let screen: id = msg_class![env; UIScreen mainScreen];
    let bounds: CGRect = msg![env; screen bounds];
    let window: id = msg_class![env; UIWindow alloc];
    let window: id = msg![env; window initWithFrame:bounds];
    let dim: id = msg_class![env; UIColor colorWithWhite:0.0f32 alpha:0.45f32];
    () = msg![env; window setBackgroundColor:dim];
    let previous = env.framework_state.uikit.ui_view.ui_window.key_window.unwrap_or(nil);
    retain(env, previous);
    let state = env.objc.borrow_mut::<UIAlertViewHostObject>(this);
    state.window = window;
    state.previous_window = previous;
    state.visible = true;
    state.presentation = state.presentation.wrapping_add(1);
    let presentation = state.presentation;
    layout(env, this);
    () = msg![env; window addSubview:this];
    notify(env, this, "willPresentAlertView:", None);
    let state = env.objc.borrow::<UIAlertViewHostObject>(this);
    if state.visible && state.presentation == presentation {
        () = msg![env; window makeKeyAndVisible];
        notify(env, this, "didPresentAlertView:", None);
    }
    release(env, this);
}
- (())_touchHLE_alertButton:(id)button {
    let state = env.objc.borrow::<UIAlertViewHostObject>(this);
    if !state.visible || state.dismissing || state.handling_click { return; }
    let presentation = state.presentation;
    let index: NSInteger = msg![env; button tag];
    retain(env, this);
    env.objc.borrow_mut::<UIAlertViewHostObject>(this).handling_click = true;
    notify(env, this, "alertView:clickedButtonAtIndex:", Some(index));
    env.objc.borrow_mut::<UIAlertViewHostObject>(this).handling_click = false;
    let state = env.objc.borrow::<UIAlertViewHostObject>(this);
    if state.visible && state.presentation == presentation {
        () = msg![env; this dismissWithClickedButtonIndex:index animated:true];
    }
    release(env, this);
}
- (())dismissWithClickedButtonIndex:(NSInteger)index animated:(bool)_animated {
    let state = env.objc.borrow::<UIAlertViewHostObject>(this);
    if !state.visible || state.dismissing { return; }
    retain(env, this);
    env.objc.borrow_mut::<UIAlertViewHostObject>(this).dismissing = true;
    notify(env, this, "alertView:willDismissWithButtonIndex:", Some(index));
    let state = env.objc.borrow_mut::<UIAlertViewHostObject>(this);
    let window = std::mem::replace(&mut state.window, nil);
    let previous = std::mem::replace(&mut state.previous_window, nil);
    state.visible = false;
    () = msg![env; this removeFromSuperview];
    () = msg![env; window setHidden:true];
    if env.framework_state.uikit.ui_view.ui_window.key_window == Some(window) {
        if previous != nil && !msg![env; previous isHidden] {
            () = msg![env; previous makeKeyWindow];
        } else {
            env.framework_state.uikit.ui_view.ui_window.key_window = None;
            let windows = env.framework_state.uikit.ui_view.ui_window.windows.clone();
            for candidate in windows.into_iter().rev() {
                if candidate != window && !msg![env; candidate isHidden] {
                    () = msg![env; candidate makeKeyWindow];
                    break;
                }
            }
        }
    }
    release(env, window);
    release(env, previous);
    env.objc.borrow_mut::<UIAlertViewHostObject>(this).dismissing = false;
    notify(env, this, "alertView:didDismissWithButtonIndex:", Some(index));
    release(env, this);
}
- (())dealloc {
    let state = env.objc.borrow_mut::<UIAlertViewHostObject>(this);
    let (title, message, content, window, previous) =
        (state.title, state.message, state.content, state.window, state.previous_window);
    let buttons = std::mem::take(&mut state.buttons);
    for object in [title, message, content, window, previous].into_iter().chain(buttons) {
        release(env, object);
    }
    msg_super![env; this dealloc]
}
@end
};
