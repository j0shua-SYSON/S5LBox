/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `UITextField`.
//!
//! Useful resources:
//! - [UITextFieldDelegate overview](https://developer.apple.com/documentation/uikit/uitextfielddelegate?language=objc)

use crate::dyld::{ConstantExports, HostConstant};
use crate::frameworks::core_graphics::{CGFloat, CGPoint, CGRect};
use crate::frameworks::foundation::ns_string::to_rust_string;
use crate::frameworks::foundation::{ns_string, NSInteger, NSRange, NSUInteger};
use crate::frameworks::uikit::ui_font::{UITextAlignment, UITextAlignmentLeft};
use crate::frameworks::uikit::ui_view::ui_window::{
    UIKeyboardDidHideNotification, UIKeyboardDidShowNotification, UIKeyboardWillHideNotification,
    UIKeyboardWillShowNotification,
};
use crate::impl_HostObject_with_superclass;
use crate::objc::{
    id, msg, msg_class, msg_super, nil, objc_classes, release, todo_objc_setter, ClassExports,
    NSZonePtr, SEL,
};
use crate::Environment;
use unicode_segmentation::UnicodeSegmentation;

type UIKeyboardAppearance = NSInteger;
type UIKeyboardType = NSInteger;
pub type UIReturnKeyType = NSInteger;
type UITextAutocapitalizationType = NSInteger;
type UITextAutocorrectionType = NSInteger;

const UITextFieldTextDidChangeNotification: &str = "UITextFieldTextDidChangeNotification";

/// `NSNotificationName` values.
pub const CONSTANTS: ConstantExports = &[(
    "_UITextFieldTextDidChangeNotification",
    HostConstant::NSString(UITextFieldTextDidChangeNotification),
)];

struct UITextFieldHostObject {
    superclass: super::UIControlHostObject,
    delegate: id,
    editing: bool,
    text_label: id,
}
impl_HostObject_with_superclass!(UITextFieldHostObject);
impl Default for UITextFieldHostObject {
    fn default() -> Self {
        UITextFieldHostObject {
            superclass: Default::default(),
            delegate: nil,
            editing: false,
            text_label: nil,
        }
    }
}

pub const CLASSES: ClassExports = objc_classes! {

(env, this, _cmd);

@implementation UITextField: UIControl

// TODO: additional rendering (e.g. placeholder, border, clear button, etc.)
// TODO: more properties
// TODO: notifications

+ (id)allocWithZone:(NSZonePtr)_zone {
    let host_object = Box::<UITextFieldHostObject>::default();
    env.objc.alloc_object(this, host_object, &mut env.mem)
}

- (id)initWithFrame:(CGRect)frame {
    let this: id = msg_super![env; this initWithFrame:frame];

    () = msg![env; this setOpaque:true];
    let bg_color: id = msg_class![env; UIColor whiteColor];
    () = msg![env; this setBackgroundColor:bg_color];

    let text_label: id = msg_class![env; UILabel new];
    () = msg![env; text_label setBackgroundColor:bg_color];
    () = msg![env; text_label setTextAlignment:UITextAlignmentLeft];

    let text_color: id = msg_class![env; UIColor blackColor];
    () = msg![env; text_label setTextColor:text_color];

    let host_obj = env.objc.borrow_mut::<UITextFieldHostObject>(this);
    host_obj.text_label = text_label;

    () = msg![env; this addSubview:text_label];

    this
}

- (id)initWithCoder:(id)coder {
    let this: id = msg_super![env; this initWithCoder: coder];

    // TODO: actual decoding of properties

    let text_label: id = msg_class![env; UILabel new];

    let host_obj = env.objc.borrow_mut::<UITextFieldHostObject>(this);
    host_obj.text_label = text_label;

    () = msg![env; this addSubview:text_label];

    this
}

- (())dealloc {
    let text_label = std::mem::replace(
        &mut env.objc.borrow_mut::<UITextFieldHostObject>(this).text_label, nil);

    release(env, text_label);
    msg_super![env; this dealloc]
}

- (())layoutSubviews {
    let text_label = env.objc.borrow_mut::<UITextFieldHostObject>(this).text_label;
    let bounds: CGRect = msg![env; this bounds];

    () = msg![env; text_label setFrame:bounds];
}

- (id)text {
    let text_label = env.objc.borrow_mut::<UITextFieldHostObject>(this).text_label;
    msg![env; text_label text]
}
- (())setText:(id)text { // NSString*
    let text_label = env.objc.borrow_mut::<UITextFieldHostObject>(this).text_label;
    () = msg![env; text_label setText:text];

    // This will work only if all the text changes will call setText:!
    // This is the case right now.
    // (see `handle_text` and `handle_backspace` helper functions below)
    // TODO: actually check if setText: send this notif on each change
    // (e.g. does it send the notif if text hasn't changed)
    let center: id = msg_class![env; NSNotificationCenter defaultCenter];
    let name = ns_string::get_static_str(env, UITextFieldTextDidChangeNotification);
    // TODO: userInfo
    let _: () = msg![env; center postNotificationName:name object:this userInfo:nil];
}

- (())setTextColor:(id)color { // UIColor*
    let text_label = env.objc.borrow_mut::<UITextFieldHostObject>(this).text_label;
    msg![env; text_label setTextColor:color]
}

- (())setTextAlignment:(UITextAlignment)text_alignment {
    let text_label = env.objc.borrow_mut::<UITextFieldHostObject>(this).text_label;
    () = msg![env; text_label setTextAlignment:text_alignment];
}

- (())setFont:(id)new_font { // UIFont*
    let text_label = env.objc.borrow_mut::<UITextFieldHostObject>(this).text_label;
    msg![env; text_label setFont:new_font]
}

- (())setMinimumFontSize:(CGFloat)size {
    let text_label = env.objc.borrow_mut::<UITextFieldHostObject>(this).text_label;
    () = msg![env; text_label setMinimumFontSize:size];
}

- (())setClearsOnBeginEditing:(bool)clear {
    todo_objc_setter!(this, clear);
}

- (())setClearButtonMode:(NSInteger)mode {
    todo_objc_setter!(this, mode);
}

- (())setSecureTextEntry:(bool)secure {
    todo_objc_setter!(this, secure);
}

- (())setPlaceholder:(id)placeholder { // NSString*
    todo_objc_setter!(this, to_rust_string(env, placeholder));
}

- (())setPosition:(CGPoint)position {
    todo_objc_setter!(this, position);
}

// weak/non-retaining
- (())setDelegate:(id)delegate { // something implementing UITextFieldDelegate
    log_dbg!("setDelegate:{:?}", delegate);
    let host_object = env.objc.borrow_mut::<UITextFieldHostObject>(this);
    host_object.delegate = delegate;
}
- (id)delegate {
    env.objc.borrow::<UITextFieldHostObject>(this).delegate
}

// UITextInputTraits implementation
- (())setAutocapitalizationType:(UITextAutocapitalizationType)type_ {
    todo_objc_setter!(this, type_);
}
- (())setAutocorrectionType:(UITextAutocorrectionType)type_ {
    todo_objc_setter!(this, type_);
}
- (())setReturnKeyType:(UIReturnKeyType)type_ {
    todo_objc_setter!(this, type_);
}
- (())setKeyboardAppearance:(UIKeyboardAppearance)appearance {
    todo_objc_setter!(this, appearance);
}
- (())setKeyboardType:(UIKeyboardType)type_ {
    todo_objc_setter!(this, type_);
}
- (())setBorderStyle:(NSInteger)style {
    todo_objc_setter!(this, style);
}
- (())setEnablesReturnKeyAutomatically:(bool)enables {
    todo_objc_setter!(this, enables);
}

- (())touchesBegan:(id)_touches // NSSet* of UITouch*
         withEvent:(id)_event { // UIEvent*
    let _: bool = msg![env; this becomeFirstResponder];
}

- (bool)isEditing {
    env.objc.borrow::<UITextFieldHostObject>(this).editing
}

- (bool)becomeFirstResponder {
    log_dbg!("becomeFirstResponder");

    if env.objc.borrow::<UITextFieldHostObject>(this).editing {
        return true;
    }

    let delegate: id = env.objc.borrow::<UITextFieldHostObject>(this).delegate;
    let sel: SEL = env.objc.register_host_selector("textFieldShouldBeginEditing:".to_string(), &mut env.mem);
    let responds: bool = msg![env; delegate respondsToSelector:sel];
    if delegate != nil && responds && !msg![env; delegate textFieldShouldBeginEditing:this] {
        return false;
    }

    // If text is nil, it becomes an empty string
    // on becoming the first responder.
    // This behaviour was validated on the Aspen Simulator
    let text_label = env
        .objc
        .borrow_mut::<UITextFieldHostObject>(this)
        .text_label;
    let curr_text: id = msg![env; text_label text];
    if curr_text == nil {
        let empty = ns_string::get_static_str(env, "");
        () = msg![env; text_label setText:empty];
    }

    let center: id = msg_class![env; NSNotificationCenter defaultCenter];
    let name = ns_string::get_static_str(env, UIKeyboardWillShowNotification);
    // TODO: userInfo
    let _: () = msg![env; center postNotificationName:name object:this userInfo:nil];

    env.framework_state.uikit.ui_responder.first_responder = this;
    env.on_parent_stack_in_coroutine(|window, _| window.start_text_input());

    let name = ns_string::get_static_str(env, UIKeyboardDidShowNotification);
    // TODO: userInfo
    let _: () = msg![env; center postNotificationName:name object:this userInfo:nil];

    // TODO: is it the right spot?
    env.objc.borrow_mut::<UITextFieldHostObject>(this).editing = true;

    let sel: SEL = env.objc.register_host_selector("textFieldDidBeginEditing:".to_string(), &mut env.mem);
    if msg![env; delegate respondsToSelector:sel] {
        () = msg![env; delegate textFieldDidBeginEditing:this];
    }

    true
}

- (bool)resignFirstResponder {
    log_dbg!("resignFirstResponder");

    if !env.objc.borrow::<UITextFieldHostObject>(this).editing {
        return true;
    }

    let delegate: id = env.objc.borrow::<UITextFieldHostObject>(this).delegate;
    let sel: SEL = env.objc.register_host_selector("textFieldShouldEndEditing:".to_string(), &mut env.mem);
    let responds: bool = msg![env; delegate respondsToSelector:sel];
    if delegate != nil && responds && !msg![env; delegate textFieldShouldEndEditing:this] {
        return false;
    }

    let center: id = msg_class![env; NSNotificationCenter defaultCenter];
    let name = ns_string::get_static_str(env, UIKeyboardWillHideNotification);
    // TODO: userInfo
    let _: () = msg![env; center postNotificationName:name object:this userInfo:nil];

    env.framework_state.uikit.ui_responder.first_responder = nil;
    env.on_parent_stack_in_coroutine(|window, _| window.stop_text_input());

    let name = ns_string::get_static_str(env, UIKeyboardDidHideNotification);
    // TODO: userInfo
    let _: () = msg![env; center postNotificationName:name object:this userInfo:nil];

    // TODO: is it the right spot?
    env.objc.borrow_mut::<UITextFieldHostObject>(this).editing = false;

    let sel: SEL = env.objc.register_host_selector("textFieldDidEndEditing:".to_string(), &mut env.mem);
    if msg![env; delegate respondsToSelector:sel] {
        () = msg![env; delegate textFieldDidEndEditing:this];
    }

    true
}

@end

};

pub fn handle_text(env: &mut Environment, text_field: id, text: String) {
    if text.is_empty() {
        return;
    }
    log_dbg!("Calling handle_text for {:?} with '{}'", text_field, text);
    let txt = ns_string::from_rust_string(env, text);

    let text_label = env
        .objc
        .borrow_mut::<UITextFieldHostObject>(text_field)
        .text_label;
    let mut curr_text = msg![env; text_label text];
    if curr_text == nil {
        curr_text = ns_string::get_static_str(env, "");
    }
    log_dbg!(
        "handle_text, curr_text: {}",
        ns_string::to_rust_string(env, curr_text)
    );

    let len = msg![env; curr_text length];
    let range = NSRange {
        location: len,
        length: 0,
    };

    let delegate: id = env
        .objc
        .borrow::<UITextFieldHostObject>(text_field)
        .delegate;
    let sel: SEL = env.objc.register_host_selector(
        "textField:shouldChangeCharactersInRange:replacementString:".to_string(),
        &mut env.mem,
    );
    let responds: bool = msg![env; delegate respondsToSelector:sel];
    let should = delegate == nil
        || !responds
        || msg![env; delegate textField:text_field shouldChangeCharactersInRange:range replacementString:txt];
    if should {
        let new_text: id = msg![env; curr_text stringByAppendingString:txt];
        log_dbg!(
            "handle_text, new_text: {}",
            ns_string::to_rust_string(env, new_text)
        );
        update_edited_text(env, text_field, new_text);
        release(env, new_text);
    }
    release(env, txt);
}

/// UIKit delegate ranges use UTF-16 offsets, not UTF-8 bytes or scalar counts.
/// Delete the last composed character as a unit so a backspace cannot leave an
/// unpaired surrogate (or half of a combining/emoji sequence) in the field.
fn backspace_range(text: &str) -> NSRange {
    let len = text.encode_utf16().count() as NSUInteger;
    let tail = text
        .graphemes(true)
        .next_back()
        .map_or(0, |last| last.encode_utf16().count() as NSUInteger);
    NSRange {
        location: len - tail,
        length: tail,
    }
}

fn update_edited_text(env: &mut Environment, text_field: id, text: id) {
    () = msg![env; text_field setText:text];
    () = msg![env; text_field setNeedsDisplay];
    super::send_actions(env, text_field, nil, super::UIControlEventEditingChanged);
}

pub fn handle_backspace(env: &mut Environment, text_field: id) {
    log_dbg!("Calling handle_backspace for {:?}", text_field);
    let text_label = env
        .objc
        .borrow_mut::<UITextFieldHostObject>(text_field)
        .text_label;
    let curr_text: id = msg![env; text_label text];

    let len: NSUInteger = msg![env; curr_text length];
    if len == 0 {
        return;
    }
    let range = backspace_range(&ns_string::to_rust_string(env, curr_text));
    let prefix_len = range.location;
    let empty = ns_string::get_static_str(env, "");

    let delegate: id = env
        .objc
        .borrow::<UITextFieldHostObject>(text_field)
        .delegate;
    let sel: SEL = env.objc.register_host_selector(
        "textField:shouldChangeCharactersInRange:replacementString:".to_string(),
        &mut env.mem,
    );
    let responds: bool = msg![env; delegate respondsToSelector:sel];
    let should = delegate == nil
        || !responds
        || msg![env; delegate textField:text_field shouldChangeCharactersInRange:range replacementString:empty];
    if should {
        let new_text: id = msg![env; curr_text substringToIndex:prefix_len];
        log_dbg!(
            "handle_backspace, new_text: {}",
            ns_string::to_rust_string(env, new_text)
        );
        update_edited_text(env, text_field, new_text);
        release(env, new_text);
    }
}

#[cfg(test)]
mod tests {
    use super::backspace_range;

    #[test]
    fn backspace_preserves_utf16_and_composed_characters() {
        for (text, location, length) in [
            ("", 0, 0),
            ("abc", 2, 1),
            ("a\u{1f600}", 1, 2),
            ("\u{1f600}x", 2, 1),
            ("a\u{301}", 0, 2),
            ("x\u{1f1fa}\u{1f1f8}", 1, 4),
            ("x\u{1f469}\u{200d}\u{1f4bb}", 1, 5),
            ("x\u{1100}\u{1161}", 1, 2),
        ] {
            let range = backspace_range(text);
            assert_eq!((range.location, range.length), (location, length));
        }
    }
}

pub fn handle_return(env: &mut Environment, text_field: id) {
    log_dbg!("Calling handle_return for {:?}", text_field);
    let delegate: id = env
        .objc
        .borrow::<UITextFieldHostObject>(text_field)
        .delegate;
    let sel: SEL = env
        .objc
        .register_host_selector("textFieldShouldReturn:".to_string(), &mut env.mem);
    if msg![env; delegate respondsToSelector:sel] {
        log_dbg!("handle_return");
        () = msg![env; delegate textFieldShouldReturn:text_field];
    }
}
