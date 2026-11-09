/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `CAAnimation` and its subclasses

use super::composition::RenderLayer;
use crate::dyld::{ConstantExports, HostConstant};
use crate::frameworks::core_animation::ca_media_timing_function::kCAMediaTimingFunctionDefault;
use crate::frameworks::core_foundation::time::CFTimeInterval;
use crate::frameworks::foundation::ns_string::{from_rust_string, get_static_str, to_rust_string};
use crate::objc::{
    autorelease, id, msg, nil, objc_classes, release, retain, ClassExports, HostObject, NSZonePtr,
};
use crate::Environment;
use crate::{impl_HostObject_with_superclass, msg_class, msg_super};
use std::rc::Rc;

type CATransitionType = id; // NSString*
const kCATransitionFade: &str = "fade";
const kCATransitionMoveIn: &str = "moveIn";
const kCATransitionPush: &str = "push";
const kCATransitionReveal: &str = "reveal";

pub type CAMediaTimingFillMode = id; // NSString*
pub const kCAFillModeBackwards: &str = "backwards";
pub const kCAFillModeBoth: &str = "both";
pub const kCAFillModeForwards: &str = "forwards";
pub const kCAFillModeRemoved: &str = "removed";

pub const CONSTANTS: ConstantExports = &[
    ("_kCATransition", HostConstant::NSString("transition")),
    (
        "_kCATransitionFromRight",
        HostConstant::NSString("fromRight"),
    ),
    ("_kCATransitionFromLeft", HostConstant::NSString("fromLeft")),
    ("_kCATransitionFromTop", HostConstant::NSString("fromTop")),
    (
        "_kCATransitionFromBottom",
        HostConstant::NSString("fromBottom"),
    ),
    // `CATransitionType` values.
    (
        "_kCATransitionFade",
        HostConstant::NSString(kCATransitionFade),
    ),
    (
        "_kCATransitionMoveIn",
        HostConstant::NSString(kCATransitionMoveIn),
    ),
    (
        "_kCATransitionPush",
        HostConstant::NSString(kCATransitionPush),
    ),
    (
        "_kCATransitionReveal",
        HostConstant::NSString(kCATransitionReveal),
    ),
    // `CAMediaTimingFillMode` values.
    (
        "_kCAFillModeBackwards",
        HostConstant::NSString(kCAFillModeBackwards),
    ),
    ("_kCAFillModeBoth", HostConstant::NSString(kCAFillModeBoth)),
    (
        "_kCAFillModeForwards",
        HostConstant::NSString(kCAFillModeForwards),
    ),
    (
        "_kCAFillModeRemoved",
        HostConstant::NSString(kCAFillModeRemoved),
    ),
];

#[derive(Clone)]
struct CAAnimationHostObject {
    removed_on_completion: bool,
    timing_function: id, // CAMediaTimingFunction*
    delegate: id,        // CAAnimationDelegate*
    autoreverses: bool,
    repeat_count: f32,
    begin_time: CFTimeInterval,
    duration: CFTimeInterval,
    fill_mode: &'static str,
    started_at: Option<CFTimeInterval>,
    finished: bool,
}
impl HostObject for CAAnimationHostObject {}
impl Default for CAAnimationHostObject {
    fn default() -> Self {
        Self {
            removed_on_completion: true,
            timing_function: Default::default(),
            delegate: Default::default(),
            autoreverses: Default::default(),
            repeat_count: Default::default(),
            begin_time: Default::default(),
            duration: Default::default(),
            fill_mode: kCAFillModeRemoved,
            started_at: None,
            finished: false,
        }
    }
}

#[derive(Clone, Default)]
struct CAPropertyAnimationHostObject {
    superclass: CAAnimationHostObject,
    key_path: id, // NSString*
}
impl_HostObject_with_superclass!(CAPropertyAnimationHostObject);

#[derive(Clone, Default)]
struct CABasicAnimationHostObject {
    superclass: CAPropertyAnimationHostObject,
    from_value: id,
    to_value: id,
    by_value: id,
}
impl_HostObject_with_superclass!(CABasicAnimationHostObject);

#[derive(Clone)]
struct CATransitionHostObject {
    superclass: CAAnimationHostObject,
    kind: String,
    subtype: Option<String>,
    start_progress: f32,
    end_progress: f32,
    previous: Option<Rc<RenderLayer>>,
}
impl Default for CATransitionHostObject {
    fn default() -> Self {
        Self {
            superclass: Default::default(),
            kind: kCATransitionFade.into(),
            subtype: None,
            start_progress: 0.0,
            end_progress: 1.0,
            previous: None,
        }
    }
}
impl_HostObject_with_superclass!(CATransitionHostObject);

#[derive(Clone)]
pub(super) struct TransitionFrame {
    pub kind: String,
    pub subtype: Option<String>,
    pub progress: f32,
    pub previous: Option<Rc<RenderLayer>>,
}

pub(super) fn is_transition(env: &mut Environment, animation: id) -> bool {
    let class = msg![env; animation class];
    let family = env.objc.get_known_class("CATransition", &mut env.mem);
    env.objc.class_is_subclass_of(class, family)
}

pub(super) fn capture_transition(env: &mut Environment, animation: id, layer: id) {
    let previous = super::composition::previous_layer(env, layer);
    env.objc
        .borrow_mut::<CATransitionHostObject>(animation)
        .previous = previous;
}

pub(super) fn transition_frame(env: &Environment, animation: id, progress: f32) -> TransitionFrame {
    let state = env.objc.borrow::<CATransitionHostObject>(animation);
    TransitionFrame {
        kind: state.kind.clone(),
        subtype: state.subtype.clone(),
        progress: state.start_progress + (state.end_progress - state.start_progress) * progress,
        previous: state.previous.clone(),
    }
}

pub const CLASSES: ClassExports = objc_classes! {

(env, this, _cmd);

// CAAnimation is an abstract class.
@implementation CAAnimation: NSObject

+ (id)allocWithZone:(NSZonePtr)_zone {
    let host_object = Box::<CAAnimationHostObject>::default();
    env.objc.alloc_object(this, host_object, &mut env.mem)
}

+ (id)animation {
    let object = msg![env; this new];
    autorelease(env, object)
}

- (id)init {
    let default_timing_function_name: id = get_static_str(env, kCAMediaTimingFunctionDefault);
    let default_timing_function: id = msg_class![env; CAMediaTimingFunction functionWithName: default_timing_function_name];
    () = msg![env; this setTimingFunction: default_timing_function];
    this
}

- (id)copyWithZone:(NSZonePtr)_zone {
    let mut state = env.objc.borrow::<CAAnimationHostObject>(this).clone();
    retain(env, state.delegate); retain(env, state.timing_function);
    state.started_at = None; state.finished = false;
    let class = msg![env; this class];
    env.objc.alloc_object(class, Box::new(state), &mut env.mem)
}

- (())setRemovedOnCompletion:(bool)removed_on_completion {
    log_dbg!("[(CAAnimation*){:?} setRemovedOnCompletion:{:?}]", this, removed_on_completion);
    env.objc.borrow_mut::<CAAnimationHostObject>(this).removed_on_completion = removed_on_completion;
}
- (bool)isRemovedOnCompletion {
    env.objc.borrow::<CAAnimationHostObject>(this).removed_on_completion
}

- (())setDelegate:(id)delegate { // CAAnimationDelegate*
    log_dbg!("[(CAAnimation*){:?} setDelegate:{:?}]", this, delegate);
    retain(env, delegate);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<CAAnimationHostObject>(this).delegate, delegate);
    release(env, old);
}
- (id)delegate {
    env.objc.borrow::<CAAnimationHostObject>(this).delegate
}

- (())setTimingFunction:(id)timingFunction { // CAMediaTimingFunction*
    log_dbg!("[(CAAnimation*){:?} setTimingFunction:{:?}]", this, timingFunction);
    retain(env, timingFunction);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<CAAnimationHostObject>(this).timing_function, timingFunction);
    release(env, old);
}
- (id)timingFunction {
    env.objc.borrow::<CAAnimationHostObject>(this).timing_function
}

// CAMediaTiming protocol implementation
- (())setAutoreverses:(bool)autoreverses {
    log_dbg!("[(CAAnimation*){:?} setAutoreverses:{:?}]", this, autoreverses);
    env.objc.borrow_mut::<CAAnimationHostObject>(this).autoreverses = autoreverses;
}
- (bool)autoreverses {
    env.objc.borrow::<CAAnimationHostObject>(this).autoreverses
}

- (())setRepeatCount:(f32)repeatCount {
    log_dbg!("[(CAAnimation*){:?} setRepeatCount:{:?}]", this, repeatCount);
    env.objc.borrow_mut::<CAAnimationHostObject>(this).repeat_count = repeatCount;
}
- (f32)repeatCount {
    env.objc.borrow::<CAAnimationHostObject>(this).repeat_count
}

- (())setBeginTime:(CFTimeInterval)beginTime {
    log_dbg!("[(CAAnimation*){:?} setBeginTime:{:?}]", this, beginTime);
    env.objc.borrow_mut::<CAAnimationHostObject>(this).begin_time = beginTime;
}
- (CFTimeInterval)beginTime {
    env.objc.borrow::<CAAnimationHostObject>(this).begin_time
}

- (())setDuration:(CFTimeInterval)duration {
    log_dbg!("[(CAAnimation*){:?} setDuration:{:?}]", this, duration);
    env.objc.borrow_mut::<CAAnimationHostObject>(this).duration = duration;
}
- (CFTimeInterval)duration {
    env.objc.borrow::<CAAnimationHostObject>(this).duration
}

- (())setFillMode:(CAMediaTimingFillMode)fill_mode {
    let fill_mode_str = to_rust_string(env, fill_mode);
    log_dbg!("[(CAAnimation*){:?} setFillMode:{:?} ({})]", this, fill_mode, fill_mode_str);
    let fill_mode_str = match &*fill_mode_str {
        kCAFillModeBackwards => kCAFillModeBackwards,
        kCAFillModeBoth => kCAFillModeBoth,
        kCAFillModeForwards => kCAFillModeForwards ,
        kCAFillModeRemoved => kCAFillModeRemoved ,
        _ => panic!("Unknown fill mode \"{}\"", fill_mode_str)
    };
    env.objc.borrow_mut::<CAAnimationHostObject>(this).fill_mode = fill_mode_str;
}
- (CAMediaTimingFillMode)fillMode {
    let fill_mode = env.objc.borrow::<CAAnimationHostObject>(this).fill_mode;
    get_static_str(env, fill_mode)
}

- (())dealloc {
    let &CAAnimationHostObject { delegate, timing_function, .. } = env.objc.borrow(this);
    if delegate != nil {
        release(env, delegate);
    }
    if timing_function != nil {
        release(env, timing_function);
    }

    env.objc.dealloc_object(this, &mut env.mem)
}

@end


@implementation CAPropertyAnimation: CAAnimation

+ (id)allocWithZone:(NSZonePtr)_zone {
    let host_object = Box::<CAPropertyAnimationHostObject>::default();
    env.objc.alloc_object(this, host_object, &mut env.mem)
}

+ (id)animationWithKeyPath:(id)path { // NSString*
    let object = msg![env; this new];
    log_dbg!("[CAPropertyAnimation animationWithKeyPath:{:?} ({:?})] -> {:?}", path, to_rust_string(env, path), object);
    () = msg![env; object setKeyPath:path];
    autorelease(env, object)
}

- (())setKeyPath:(id)path { // NSString*
    log_dbg!("[(CAPropertyAnimation*){:?} setKeyPath:{:?} ({:?})]", this, path, to_rust_string(env, path));
    let path_copy: id = msg![env; path copy];
    let old = std::mem::replace(&mut env.objc.borrow_mut::<CAPropertyAnimationHostObject>(this).key_path, path_copy);
    release(env, old);
}
- (id)keyPath {
    env.objc.borrow::<CAPropertyAnimationHostObject>(this).key_path
}

- (id)copyWithZone:(NSZonePtr)_zone {
    let mut state = env.objc.borrow::<CAPropertyAnimationHostObject>(this).clone();
    retain(env, state.key_path);
    retain(env, state.superclass.delegate); retain(env, state.superclass.timing_function);
    state.superclass.started_at = None; state.superclass.finished = false;
    let class = msg![env; this class];
    env.objc.alloc_object(class, Box::new(state), &mut env.mem)
}

- (())dealloc {
    let &CAPropertyAnimationHostObject { key_path, .. } = env.objc.borrow(this);
    if key_path != nil {
        release(env, key_path);
    }

    msg_super![env; this dealloc]
}

@end


@implementation CABasicAnimation: CAPropertyAnimation

+ (id)allocWithZone:(NSZonePtr)_zone {
    let host_object = Box::<CABasicAnimationHostObject>::default();
    env.objc.alloc_object(this, host_object, &mut env.mem)
}

- (())setFromValue:(id)value {
    log_dbg!("[(CABasicAnimation*){:?} setFromValue:{:?}]", this, value);
    retain(env, value);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<CABasicAnimationHostObject>(this).from_value, value);
    release(env, old);
}
- (id)fromValue {
    env.objc.borrow::<CABasicAnimationHostObject>(this).from_value
}

- (())setToValue:(id)value {
    log_dbg!("[(CABasicAnimation*){:?} setToValue:{:?}]", this, value);
    retain(env, value);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<CABasicAnimationHostObject>(this).to_value, value);
    release(env, old);
}
- (id)toValue {
    env.objc.borrow::<CABasicAnimationHostObject>(this).to_value
}

- (())setByValue:(id)value {
    log_dbg!("[(CABasicAnimation*){:?} setByValue:{:?}]", this, value);
    retain(env, value);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<CABasicAnimationHostObject>(this).by_value, value);
    release(env, old);
}
- (id)byValue {
    env.objc.borrow::<CABasicAnimationHostObject>(this).by_value
}

- (id)copyWithZone:(NSZonePtr)_zone {
    let mut state = env.objc.borrow::<CABasicAnimationHostObject>(this).clone();
    retain(env, state.from_value); retain(env, state.to_value); retain(env, state.by_value);
    retain(env, state.superclass.key_path);
    retain(env, state.superclass.superclass.delegate); retain(env, state.superclass.superclass.timing_function);
    state.superclass.superclass.started_at = None; state.superclass.superclass.finished = false;
    let class = msg![env; this class];
    env.objc.alloc_object(class, Box::new(state), &mut env.mem)
}

- (())dealloc {
    let &CABasicAnimationHostObject { from_value, to_value, by_value, .. } = env.objc.borrow(this);
    release(env, by_value);
    if from_value != nil {
        release(env, from_value);
    }
    if to_value != nil {
        release(env, to_value);
    }

    msg_super![env; this dealloc]
}

@end


@implementation CATransition : CAAnimation

+ (id)allocWithZone:(NSZonePtr)_zone {
    let host_object = Box::<CATransitionHostObject>::default();
    env.objc.alloc_object(this, host_object, &mut env.mem)
}

- (())setType:(CATransitionType)transitionType {
    let kind = to_rust_string(env, transitionType).into_owned();
    assert!(matches!(kind.as_str(), kCATransitionFade | kCATransitionMoveIn | kCATransitionPush | kCATransitionReveal),
        "Unsupported transition type: {}", kind);
    env.objc.borrow_mut::<CATransitionHostObject>(this).kind = kind;
}
- (id)type {
    let kind = env.objc.borrow::<CATransitionHostObject>(this).kind.clone();
    let value = from_rust_string(env, kind); autorelease(env, value)
}
- (())setSubtype:(id)value {
    let value = if value == nil { None } else { Some(to_rust_string(env, value).into_owned()) };
    env.objc.borrow_mut::<CATransitionHostObject>(this).subtype = value;
}
- (id)subtype {
    let value = env.objc.borrow::<CATransitionHostObject>(this).subtype.clone();
    value.map(|value| { let value = from_rust_string(env, value); autorelease(env, value) }).unwrap_or(nil)
}
- (())setStartProgress:(f32)value { env.objc.borrow_mut::<CATransitionHostObject>(this).start_progress = value.clamp(0.0, 1.0); }
- (f32)startProgress { env.objc.borrow::<CATransitionHostObject>(this).start_progress }
- (())setEndProgress:(f32)value { env.objc.borrow_mut::<CATransitionHostObject>(this).end_progress = value.clamp(0.0, 1.0); }
- (f32)endProgress { env.objc.borrow::<CATransitionHostObject>(this).end_progress }
- (id)copyWithZone:(NSZonePtr)_zone {
    let mut state = env.objc.borrow::<CATransitionHostObject>(this).clone();
    retain(env, state.superclass.delegate); retain(env, state.superclass.timing_function);
    state.superclass.started_at = None;
    state.superclass.finished = false;
    let class = msg![env; this class];
    env.objc.alloc_object(class, Box::new(state), &mut env.mem)
}

@end

};

pub fn get_animation_start_time(
    env: &mut Environment,
    animation: id,
) -> &mut Option<CFTimeInterval> {
    &mut env
        .objc
        .borrow_mut::<CAAnimationHostObject>(animation)
        .started_at
}

pub(super) fn mark_finished(env: &mut Environment, animation: id) -> bool {
    !std::mem::replace(
        &mut env
            .objc
            .borrow_mut::<CAAnimationHostObject>(animation)
            .finished,
        true,
    )
}

pub(super) fn cancel(env: &mut Environment, animation: id) {
    if !mark_finished(env, animation) {
        return;
    }
    let delegate: id = msg![env; animation delegate];
    let selector = env
        .objc
        .register_host_selector("animationDidStop:finished:".into(), &mut env.mem);
    if delegate != nil && msg![env; delegate respondsToSelector:selector] {
        retain(env, delegate);
        retain(env, animation);
        () = msg![env; delegate animationDidStop:animation finished:false];
        release(env, animation);
        release(env, delegate);
    }
}
