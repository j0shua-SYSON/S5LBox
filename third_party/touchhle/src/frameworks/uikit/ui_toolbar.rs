/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Toolbars, bar items, NIB decoding, and target/action delivery.
//! System items use text labels; legacy artwork and insertion animations are
//! not reproduced. Appearance-proxy APIs remain unsupported.

use super::ui_view::UIViewHostObject;
use crate::frameworks::core_graphics::{CGFloat, CGPoint, CGRect, CGSize};
use crate::frameworks::foundation::{ns_string::get_static_str, NSInteger, NSUInteger};
use crate::objc::{
    id, impl_HostObject_with_superclass, msg, msg_class, msg_send, msg_super, nil,
    objc_classes, release, retain, ClassExports, HostObject, NSZonePtr, SEL,
};
use crate::Environment;

struct BarItem {
    title: id,
    image: id,
    enabled: bool,
    tag: NSInteger,
    // Non-owning. A toolbar removes itself before releasing its items.
    toolbars: Vec<id>,
}
impl Default for BarItem {
    fn default() -> Self {
        Self { title: nil, image: nil, enabled: true, tag: 0, toolbars: Vec::new() }
    }
}
impl HostObject for BarItem {}

#[derive(Default)]
struct BarButtonItem {
    superclass: BarItem,
    style: NSInteger,
    system: Option<NSInteger>,
    width: CGFloat,
    custom_view: id,
    target: id, // weak
    action: Option<SEL>,
}
impl_HostObject_with_superclass!(BarButtonItem);

#[derive(Default)]
struct Toolbar {
    superclass: UIViewHostObject,
    items: id,
    item_list: Vec<id>, // retained by items
    views: Vec<id>, // retained by UIView's subviews
    style: NSInteger,
    translucent: bool,
    tint: id,
    delegate: id, // weak
    laying_out: bool,
    layout_again: bool,
}
impl_HostObject_with_superclass!(Toolbar);

fn changed(env: &mut Environment, item: id) {
    let toolbars = env.objc.borrow::<BarItem>(item).toolbars.clone();
    for toolbar in toolbars { let _: () = msg![env; toolbar layoutSubviews]; }
}

fn system_title(system: NSInteger) -> &'static str {
    match system {
        0 => "Done", 1 => "Cancel", 2 => "Edit", 3 => "Save", 4 => "+",
        7 => "Compose", 8 => "Reply", 9 => "Action", 10 => "Organize",
        11 => "Bookmarks", 12 => "Search", 13 => "Refresh", 14 => "Stop",
        15 => "Camera", 16 => "Trash", 17 => "Play", 18 => "Pause",
        19 => "Rewind", 20 => "Fast Forward", 21 => "Undo", 22 => "Redo",
        23 => "Page Curl", _ => "",
    }
}

fn layout(env: &mut Environment, toolbar: id) {
    let state = env.objc.borrow_mut::<Toolbar>(toolbar);
    if state.laying_out { state.layout_again = true; return; }
    state.laying_out = true;
    state.layout_again = false;
    let retained_items = state.items;
    let items = state.item_list.clone();
    let old_views = std::mem::take(&mut state.views);
    let (style, translucent, tint) = (state.style, state.translucent, state.tint);
    retain(env, toolbar);
    retain(env, retained_items);
    for view in old_views { let _: () = msg![env; view removeFromSuperview]; }

    let bounds: CGRect = msg![env; toolbar bounds];
    let alpha = if translucent { 0.8f32 } else { 1.0 };
    let color: id = if tint != nil { tint }
        else if style == 0 { msg_class![env; UIColor colorWithRed:0.35f32 green:0.41f32 blue:0.5f32 alpha:alpha] }
        else { msg_class![env; UIColor colorWithWhite:0.1f32 alpha:alpha] };
    () = msg![env; toolbar setBackgroundColor:color];
    let font: id = msg_class![env; UIFont boldSystemFontOfSize:13.0f32];
    let mut widths = Vec::with_capacity(items.len());
    let mut flexible = 0;
    for &item in &items {
        let state = env.objc.borrow::<BarButtonItem>(item);
        let (system, width, custom) = (state.system, state.width, state.custom_view);
        let width = if system == Some(5) {
            flexible += 1; 0.0
        } else if width > 0.0 || system == Some(6) {
            width.max(0.0)
        } else if custom != nil {
            let frame: CGRect = msg![env; custom frame]; frame.size.width.max(0.0)
        } else {
            let title: id = msg![env; item title];
            let title = system.map(|system| get_static_str(env, system_title(system))).unwrap_or(title);
            let size: CGSize = msg![env; title sizeWithFont:font];
            let image: id = msg![env; item image];
            let image_size: CGSize = msg![env; image size];
            (size.width + image_size.width + 16.0).max(32.0)
        };
        widths.push(width);
    }
    let gaps = items.len().saturating_sub(1) as f32 * 6.0;
    let available = (bounds.size.width - 12.0 - gaps).max(0.0);
    let fixed: f32 = widths.iter().sum();
    let flex_width = if flexible > 0 { (available - fixed).max(0.0) / flexible as f32 } else { 0.0 };
    let shrink = if fixed > available && fixed > 0.0 { available / fixed } else { 1.0 };
    let mut x = bounds.origin.x + 6.0;
    let mut views = Vec::new();
    for (item, width) in items.into_iter().zip(widths) {
        let state = env.objc.borrow::<BarButtonItem>(item);
        let (system, custom) = (state.system, state.custom_view);
        let width = if system == Some(5) { flex_width } else { width * shrink };
        if system == Some(5) || system == Some(6) { x += width + 6.0; continue; }
        let view = if custom != nil { custom } else {
            let button: id = msg_class![env; UIButton buttonWithType:1i32];
            let title: id = msg![env; item title];
            let title = system.map(|system| get_static_str(env, system_title(system))).unwrap_or(title);
            () = msg![env; button setTitle:title forState:0u32];
            let label: id = msg![env; button titleLabel];
            () = msg![env; label setFont:font];
            let image: id = msg![env; item image];
            () = msg![env; button setImage:image forState:0u32];
            let enabled: bool = msg![env; item isEnabled];
            () = msg![env; button setEnabled:enabled];
            () = msg![env; button setAlpha:(if enabled { 1.0f32 } else { 0.45f32 })];
            let action = env.objc.register_host_selector("_touchHLEActivate:".to_owned(), &mut env.mem);
            () = msg![env; button addTarget:item action:action forControlEvents:64u32];
            button
        };
        let height = if custom != nil {
            let frame: CGRect = msg![env; custom frame]; frame.size.height.min(bounds.size.height).max(0.0)
        } else { (bounds.size.height - 10.0).max(0.0) };
        let frame = CGRect { origin: CGPoint { x, y: bounds.origin.y + (bounds.size.height - height) / 2.0 }, size: CGSize { width, height } };
        () = msg![env; view setFrame:frame];
        () = msg![env; toolbar addSubview:view];
        views.push(view);
        x += width + 6.0;
    }
    let state = env.objc.borrow_mut::<Toolbar>(toolbar);
    state.views = views;
    state.laying_out = false;
    let again = state.layout_again;
    release(env, retained_items);
    if again { layout(env, toolbar); }
    release(env, toolbar);
}

pub const CLASSES: ClassExports = objc_classes! {
(env, this, _cmd);

@implementation UIBarItem: NSObject
+ (id)allocWithZone:(NSZonePtr)_zone {
    env.objc.alloc_object(this, Box::<BarItem>::default(), &mut env.mem)
}
- (id)initWithCoder:(id)coder {
    let title: id = msg![env; coder decodeObjectForKey:(get_static_str(env, "UITitle"))];
    let image: id = msg![env; coder decodeObjectForKey:(get_static_str(env, "UIImage"))];
    let tag: NSInteger = msg![env; coder decodeIntegerForKey:(get_static_str(env, "UITag"))];
    () = msg![env; this setTitle:title];
    () = msg![env; this setImage:image];
    () = msg![env; this setTag:tag];
    let key = get_static_str(env, "UIEnabled");
    if msg![env; coder containsValueForKey:key] {
        let enabled: bool = msg![env; coder decodeBoolForKey:key];
        () = msg![env; this setEnabled:enabled];
    }
    this
}
- (id)title { env.objc.borrow::<BarItem>(this).title }
- (())setTitle:(id)value {
    let value = msg![env; value copy];
    let old = std::mem::replace(&mut env.objc.borrow_mut::<BarItem>(this).title, value);
    release(env, old); changed(env, this);
}
- (id)image { env.objc.borrow::<BarItem>(this).image }
- (())setImage:(id)value {
    retain(env, value);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<BarItem>(this).image, value);
    release(env, old); changed(env, this);
}
- (bool)isEnabled { env.objc.borrow::<BarItem>(this).enabled }
- (())setEnabled:(bool)value { env.objc.borrow_mut::<BarItem>(this).enabled = value; changed(env, this); }
- (NSInteger)tag { env.objc.borrow::<BarItem>(this).tag }
- (())setTag:(NSInteger)value { env.objc.borrow_mut::<BarItem>(this).tag = value; }
- (())dealloc {
    let state = env.objc.borrow::<BarItem>(this);
    let (title, image) = (state.title, state.image);
    release(env, title); release(env, image);
    env.objc.dealloc_object(this, &mut env.mem);
}
@end

@implementation UIBarButtonItem: UIBarItem
+ (id)allocWithZone:(NSZonePtr)_zone {
    env.objc.alloc_object(this, Box::<BarButtonItem>::default(), &mut env.mem)
}
- (id)initWithCoder:(id)coder {
    let this: id = msg_super![env; this initWithCoder:coder];
    let style: NSInteger = msg![env; coder decodeIntegerForKey:(get_static_str(env, "UIStyle"))];
    let width: CGFloat = msg![env; coder decodeFloatForKey:(get_static_str(env, "UIWidth"))];
    let is_system: bool = msg![env; coder decodeBoolForKey:(get_static_str(env, "UIIsSystemItem"))];
    if is_system {
        let system: NSInteger = msg![env; coder decodeIntegerForKey:(get_static_str(env, "UISystemItem"))];
        env.objc.borrow_mut::<BarButtonItem>(this).system = Some(system);
    }
    let custom: id = msg![env; coder decodeObjectForKey:(get_static_str(env, "UICustomView"))];
    () = msg![env; this setStyle:style];
    () = msg![env; this setWidth:width];
    () = msg![env; this setCustomView:custom];
    // NIB event connections assign the non-retained target and action after
    // the object graph has finished decoding.
    this
}
- (id)initWithTitle:(id)title style:(NSInteger)style target:(id)target action:(SEL)action {
    () = msg![env; this setTitle:title];
    let state = env.objc.borrow_mut::<BarButtonItem>(this);
    state.style = style; state.target = target; state.action = (!action.is_null()).then_some(action);
    this
}
- (id)initWithImage:(id)image style:(NSInteger)style target:(id)target action:(SEL)action {
    () = msg![env; this setImage:image];
    let state = env.objc.borrow_mut::<BarButtonItem>(this);
    state.style = style; state.target = target; state.action = (!action.is_null()).then_some(action);
    this
}
- (id)initWithBarButtonSystemItem:(NSInteger)system target:(id)target action:(SEL)action {
    let state = env.objc.borrow_mut::<BarButtonItem>(this);
    state.system = Some(system); state.target = target; state.action = (!action.is_null()).then_some(action);
    this
}
- (id)initWithCustomView:(id)view { () = msg![env; this setCustomView:view]; this }
- (id)target { env.objc.borrow::<BarButtonItem>(this).target }
- (())setTarget:(id)target { env.objc.borrow_mut::<BarButtonItem>(this).target = target; }
- (SEL)action { env.objc.borrow::<BarButtonItem>(this).action.unwrap_or_else(SEL::null) }
- (())setAction:(SEL)action { env.objc.borrow_mut::<BarButtonItem>(this).action = (!action.is_null()).then_some(action); }
- (NSInteger)style { env.objc.borrow::<BarButtonItem>(this).style }
- (())setStyle:(NSInteger)style { env.objc.borrow_mut::<BarButtonItem>(this).style = style; changed(env, this); }
- (CGFloat)width { env.objc.borrow::<BarButtonItem>(this).width }
- (())setWidth:(CGFloat)width { env.objc.borrow_mut::<BarButtonItem>(this).width = if width.is_finite() { width.max(0.0) } else { 0.0 }; changed(env, this); }
- (id)customView { env.objc.borrow::<BarButtonItem>(this).custom_view }
- (())setCustomView:(id)view {
    retain(env, view);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<BarButtonItem>(this).custom_view, view);
    changed(env, this); release(env, old);
}
- (())_touchHLEActivate:(id)button {
    if !env.objc.borrow::<BarItem>(this).enabled { return; }
    let state = env.objc.borrow::<BarButtonItem>(this);
    let Some(action) = state.action else { return; };
    let mut target = state.target;
    if target == nil {
        target = env.framework_state.uikit.ui_responder.first_responder;
        if target == nil { target = button; }
        let mut visited = Vec::new();
        while target != nil && !visited.contains(&target) {
            if msg![env; target respondsToSelector:action] { break; }
            visited.push(target);
            target = msg![env; target nextResponder];
        }
        if visited.contains(&target) { target = nil; }
    }
    if target == nil { return; }
    // The callback may replace the toolbar's items and release this item.
    retain(env, this);
    match action.as_str(&env.mem).bytes().filter(|&c| c == b':').count() {
        0 => { () = msg_send(env, (target, action)); }
        1 => { () = msg_send(env, (target, action, this)); }
        2 => { () = msg_send(env, (target, action, this, nil)); }
        _ => panic!("Unsupported bar-button action signature"),
    }
    release(env, this);
}
- (())dealloc {
    let view = env.objc.borrow::<BarButtonItem>(this).custom_view;
    release(env, view);
    msg_super![env; this dealloc]
}
@end

@implementation UIToolbar: UIView
+ (id)allocWithZone:(NSZonePtr)_zone {
    env.objc.alloc_object(this, Box::<Toolbar>::default(), &mut env.mem)
}
- (id)initWithFrame:(CGRect)frame {
    let this: id = msg_super![env; this initWithFrame:frame];
    layout(env, this); this
}
- (id)initWithCoder:(id)coder {
    let this: id = msg_super![env; this initWithCoder:coder];
    let style: NSInteger = msg![env; coder decodeIntegerForKey:(get_static_str(env, "UIBarStyle"))];
    let key = get_static_str(env, "UITranslucent");
    let translucent: bool = if msg![env; coder containsValueForKey:key] {
        msg![env; coder decodeBoolForKey:key]
    } else { style == 2 };
    let tint: id = msg![env; coder decodeObjectForKey:(get_static_str(env, "UITintColor"))];
    let items: id = msg![env; coder decodeObjectForKey:(get_static_str(env, "UIItems"))];
    () = msg![env; this setBarStyle:style];
    () = msg![env; this setTranslucent:translucent];
    () = msg![env; this setTintColor:tint];
    () = msg![env; this setItems:items];
    this
}
- (())setFrame:(CGRect)frame { () = msg_super![env; this setFrame:frame]; layout(env, this); }
- (CGSize)sizeThatFits:(CGSize)size { CGSize { width: size.width, height: 44.0 } }
- (())sizeToFit {
    let mut frame: CGRect = msg![env; this frame]; frame.size.height = 44.0;
    if frame.size.width == 0.0 {
        let screen: id = msg_class![env; UIScreen mainScreen];
        let bounds: CGRect = msg![env; screen bounds]; frame.size.width = bounds.size.width;
    }
    () = msg![env; this setFrame:frame];
}
- (id)items { env.objc.borrow::<Toolbar>(this).items }
- (())setItems:(id)items {
    let items: id = msg![env; items copy];
    let count: NSUInteger = msg![env; items count];
    let new: Vec<id> = (0..count).map(|index| msg![env; items objectAtIndex:index]).collect();
    let state = env.objc.borrow_mut::<Toolbar>(this);
    let old = std::mem::replace(&mut state.items, items);
    let old_list = std::mem::replace(&mut state.item_list, new.clone());
    for item in old_list { env.objc.borrow_mut::<BarItem>(item).toolbars.retain(|&bar| bar != this); }
    for item in new {
        let owners = &mut env.objc.borrow_mut::<BarItem>(item).toolbars;
        if !owners.contains(&this) { owners.push(this); }
    }
    layout(env, this); release(env, old);
}
- (())setItems:(id)items animated:(bool)_animated { () = msg![env; this setItems:items]; }
- (NSInteger)barStyle { env.objc.borrow::<Toolbar>(this).style }
- (())setBarStyle:(NSInteger)style { env.objc.borrow_mut::<Toolbar>(this).style = style; layout(env, this); }
- (bool)isTranslucent { env.objc.borrow::<Toolbar>(this).translucent }
- (())setTranslucent:(bool)value { env.objc.borrow_mut::<Toolbar>(this).translucent = value; layout(env, this); }
- (id)tintColor { env.objc.borrow::<Toolbar>(this).tint }
- (())setTintColor:(id)color {
    retain(env, color);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<Toolbar>(this).tint, color);
    release(env, old); layout(env, this);
}
- (id)delegate { env.objc.borrow::<Toolbar>(this).delegate }
- (())setDelegate:(id)delegate { env.objc.borrow_mut::<Toolbar>(this).delegate = delegate; }
- (())layoutSubviews { layout(env, this); }
- (())dealloc {
    let state = env.objc.borrow_mut::<Toolbar>(this);
    let (items, tint) = (state.items, state.tint);
    let list = std::mem::take(&mut state.item_list);
    let views = std::mem::take(&mut state.views);
    for item in list { env.objc.borrow_mut::<BarItem>(item).toolbars.retain(|&bar| bar != this); }
    for view in views { let _: () = msg![env; view removeFromSuperview]; }
    release(env, items); release(env, tint);
    msg_super![env; this dealloc]
}
@end
};
