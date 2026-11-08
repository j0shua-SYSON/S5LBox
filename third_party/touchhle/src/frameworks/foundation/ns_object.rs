/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `NSObject`, the root of most class hierarchies in Objective-C.
//!
//! Resources:
//! - Apple's [Advanced Memory Management Programming Guide](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/MemoryMgmt/Articles/MemoryMgmt.html)
//!   explains how reference counting works. Note that we are interested in what
//!   it calls "manual retain-release", not ARC.
//! - Apple's [Key-Value Coding Programming Guide](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/KeyValueCoding/SearchImplementation.html)
//!   explains the algorithm `setValue:forKey:` should follow.
//!
//! See also: [crate::objc], especially the `objects` module.

use super::ns_string::{from_rust_string, to_rust_string};
use super::{NSTimeInterval, NSUInteger};
use crate::frameworks::core_graphics::{CGPoint, CGRect, CGSize};
use crate::frameworks::foundation::ns_run_loop::{
    add_perform_request, cancel_all_perform_requests, cancel_perform_requests,
};
use crate::frameworks::foundation::ns_thread::detach_new_thread_inner;
use crate::libc::semaphore::{host_destroy_semaphore, sem_wait};
use crate::mem::MutVoidPtr;
use crate::objc::{
    autorelease, id, msg, msg_class, msg_send, msg_send_no_type_checking, nil, objc_classes,
    release, retain, Class, ClassExports, NSZonePtr, ObjC, TrivialHostObject, IMP, SEL,
};
use crate::Environment;
use std::any::TypeId;

pub const CLASSES: ClassExports = objc_classes! {

(env, this, _cmd);

@implementation NSObject

+ (id)alloc {
    msg![env; this allocWithZone:(MutVoidPtr::null())]
}
+ (id)allocWithZone:(NSZonePtr)_zone { // struct _NSZone*
    log_dbg!("[{:?} allocWithZone:]", this);
    env.objc.alloc_object(this, Box::new(TrivialHostObject), &mut env.mem)
}

+ (id)new {
    let new_object: id = msg![env; this alloc];
    msg![env; new_object init]
}

+ (Class)class {
    this
}
+ (bool)isSubclassOfClass:(Class)class {
    env.objc.class_is_subclass_of(this, class)
}

// See the instance method section for the normal versions of these.
+ (id)retain {
    this // classes are not refcounted
}
+ (())release {
    // classes are not refcounted
}
+ (())autorelease {
    // classes are not refcounted
}

+ (bool)instancesRespondToSelector:(SEL)selector {
    env.objc.class_has_method(this, selector)
}

+ (IMP)instanceMethodForSelector:(SEL)selector {
    crate::objc::class_getMethodImplementation(env, this, selector)
}

+ (IMP)methodForSelector:(SEL)selector {
    let class = ObjC::read_isa(this, &env.mem);
    crate::objc::class_getMethodImplementation(env, class, selector)
}

+ (())cancelPreviousPerformRequestsWithTarget:(id)target selector:(SEL)selector object:(id)arg {
    let run_loop: id = msg_class![env; NSRunLoop currentRunLoop];
    cancel_perform_requests(env, run_loop, target, selector, arg);
}

+ (())cancelPreviousPerformRequestsWithTarget:(id)target {
    let run_loop: id = msg_class![env; NSRunLoop currentRunLoop];
    cancel_all_perform_requests(env, run_loop, target);
}

+ (bool)accessInstanceVariablesDirectly {
    true
}

+ (id)description {
    let name = env.objc.get_class_name(this);
    let str = from_rust_string(env, name.to_string());
    autorelease(env, str)
}

+ (id)debugDescription {
    msg![env; this description]
}

+ (id)instanceMethodSignatureForSelector:(SEL)sel {
    // TODO: support `host` method signatures
    let sig = *env.objc.class_get_method_signature(this, sel).unwrap();
    log_dbg!("instanceMethodSignatureForSelector: '{}' -> {:?}", sel.as_str(&env.mem), env.mem.cstr_at_utf8(sig));
    msg_class![env; NSMethodSignature signatureWithObjCTypes:sig]
}

+ (())initialize {
    // Do nothing
}
+ (())load {
    // The loader invokes guest +load methods. NSObject itself has no startup
    // work, but subclasses may explicitly call [super load].
}

- (id)init {
    this
}

- (NSUInteger)retainCount {
    env.objc.get_refcount(this).into()
}

- (id)retain {
    log_dbg!("[{:?} retain]", this);
    env.objc.increment_refcount(this);
    this
}
- (())release {
    log_dbg!("[{:?} release]", this);
    if env.objc.decrement_refcount(this) {
        () = msg![env; this dealloc];
    }
}
- (id)autorelease {
    () = msg_class![env; NSAutoreleasePool addObject:this];
    this
}

- (())dealloc {
    log_dbg!("[{:?} dealloc]", this);
    env.objc.dealloc_object(this, &mut env.mem)
}

- (Class)class {
    ObjC::read_isa(this, &env.mem)
}
- (bool)isMemberOfClass:(Class)class {
    let this_class: Class = msg![env; this class];
    class == this_class
}
- (bool)isKindOfClass:(Class)class {
    let this_class: Class = msg![env; this class];
    env.objc.class_is_subclass_of(this_class, class)
}

- (NSUInteger)hash {
    this.to_bits()
}

// To not confuse with isEqualTo:, which is
// a category of NSWhoseSpecifier!
// Reference https://nshipster.com/equality
- (bool)isEqual:(id)other {
    this == other
}

// TODO: Instance description and debugDescription.
// This is not hard to add, but before adding a fallback implementation of it,
// we should make sure all the Foundation classes' overrides of it are there,
// to prevent weird behavior.
// TODO: localized description methods also? (not sure if NSObject has them)

// Helper for NSCopying
- (id)copy {
    msg![env; this copyWithZone:(MutVoidPtr::null())]
}

// Helper for NSMutableCopying
- (id)mutableCopy {
    msg![env; this mutableCopyWithZone:(MutVoidPtr::null())]
}

// NSKeyValueCoding
// https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/KeyValueCoding/SearchImplementation.html
- (id)valueForKey:(id)key {
    let name = to_rust_string(env, key).into_owned();
    let mut chars = name.chars();
    let capitalized = chars.next().map(|first| first.to_uppercase().collect::<String>() + chars.as_str()).unwrap_or_default();
    let class = ObjC::read_isa(this, &env.mem);
    for getter in [format!("get{capitalized}"), name.clone(), format!("is{capitalized}"), format!("_{name}")] {
        if let Some(selector) = env.objc.lookup_selector(&getter) {
            if env.objc.class_has_method(class, selector) {
                let encoding = env.objc.class_get_method_signature(class, selector)
                    .map(|ptr| env.mem.cstr_at_utf8(*ptr).unwrap().to_owned());
                let signature = env.objc.class_get_host_method_signature(class, selector);
                return box_key_value(env, this, Some(selector), MutVoidPtr::null(), encoding.as_deref(), signature);
            }
        }
    }
    // Collection accessor proxies are not implemented yet.
    if env.objc.class_has_method_named(class, &format!("countOf{capitalized}")) {
        unimplemented!("Collection key-value accessor for {name}");
    }
    let direct: bool = msg![env; class accessInstanceVariablesDirectly];
    if direct {
        for name in [format!("_{name}"), format!("_is{capitalized}"), name.clone(), format!("is{capitalized}")] {
            if let Some((address, encoding)) = env.objc.object_lookup_typed_ivar(&env.mem, this, &name) {
                let encoding = env.mem.cstr_at_utf8(encoding).unwrap().to_owned();
                return box_key_value(env, this, None, address, Some(&encoding), None);
            }
        }
    }
    msg![env; this valueForUndefinedKey:key]
}

- (id)valueForKeyPath:(id)key_path {
    let path = to_rust_string(env, key_path).into_owned();
    let mut result = this;
    for component in path.split('.') {
        let key = from_rust_string(env, component.to_owned());
        result = msg![env; result valueForKey:key];
        autorelease(env, key);
    }
    result
}

- (id)valueForUndefinedKey:(id)key {
    let name = to_rust_string(env, key);
    panic!("No key-value getter for {name} on object {this:?}");
}

- (())setValue:(id)value
       forKey:(id)key { // NSString*
    let name = to_rust_string(env, key).into_owned();
    let mut chars = name.chars();
    let capitalized = chars.next().map(|first| first.to_uppercase().collect::<String>() + chars.as_str()).unwrap_or_default();
    let class = ObjC::read_isa(this, &env.mem);
    for setter in [format!("set{capitalized}:"), format!("_set{capitalized}:")] {
        if let Some(selector) = env.objc.lookup_selector(&setter) {
            if env.objc.class_has_method(class, selector) {
                let encoding = env.objc.class_get_method_signature(class, selector)
                    .map(|ptr| env.mem.cstr_at_utf8(*ptr).unwrap().to_owned());
                let signature = env.objc.class_get_host_method_signature(class, selector);
                let argument = encoding.as_deref().map(setter_argument_encoding);
                set_key_value(env, this, key, value, Some(selector), MutVoidPtr::null(), argument, signature);
                return;
            }
        }
    }
    let direct: bool = msg![env; class accessInstanceVariablesDirectly];
    if direct {
        for name in [format!("_{name}"), format!("_is{capitalized}"), name.clone(), format!("is{capitalized}")] {
            if let Some((address, encoding)) = env.objc.object_lookup_typed_ivar(&env.mem, this, &name) {
                let encoding = env.mem.cstr_at_utf8(encoding).unwrap().to_owned();
                set_key_value(env, this, key, value, None, address, Some(&encoding), None);
                return;
            }
        }
    }
    () = msg![env; this setValue:value forUndefinedKey:key];
}

- (())setValue:(id)value forKeyPath:(id)key_path {
    let path = to_rust_string(env, key_path).into_owned();
    if let Some((parent, name)) = path.rsplit_once('.') {
        let parent = from_rust_string(env, parent.to_owned());
        let target: id = msg![env; this valueForKeyPath:parent];
        autorelease(env, parent);
        let key = from_rust_string(env, name.to_owned());
        () = msg![env; target setValue:value forKey:key];
        autorelease(env, key);
    } else {
        () = msg![env; this setValue:value forKey:key_path];
    }
}

- (())setNilValueForKey:(id)key {
    // TODO: Raise NSInvalidArgumentException.
    let name = to_rust_string(env, key);
    panic!("Cannot set nil for non-object key {name} on object {this:?}");
}

- (())setValue:(id)_value
forUndefinedKey:(id)key { // NSString*
    // TODO: Raise NSUnknownKeyException
    let class: Class = ObjC::read_isa(this, &env.mem);
    let class_name_string = env.objc.get_class_name(class).to_owned(); // TODO: Avoid copying
    let key_string = to_rust_string(env, key);
    panic!("Object {:?} of class {:?} ({:?}) does not have a setter for {} ({:?})\
        \nAvailable selectors: {}\nAvailable ivars: {}",
        this, class_name_string, class, key_string, key,
        env.objc.debug_all_class_selectors_as_strings(&env.mem, class).join(", "),
        env.objc.debug_all_class_ivars_as_strings(class).join(", "));
}

- (())willChangeValueForKey:(id)_key { // NSString *
    log_once!("TODO: NSObject willChangeValueForKey:");
}
- (())didChangeValueForKey:(id)_key { // NSString *
    log_once!("TODO: NSObject didChangeValueForKey:");
}

- (bool)respondsToSelector:(SEL)selector {
    env.objc.object_has_method(&env.mem, this, selector)
}

- (IMP)methodForSelector:(SEL)selector {
    let class = ObjC::read_isa(this, &env.mem);
    crate::objc::class_getMethodImplementation(env, class, selector)
}

- (id)performSelector:(SEL)sel {
    assert!(!sel.is_null());
    msg_send_no_type_checking(env, (this, sel))
}

- (id)performSelector:(SEL)sel
           withObject:(id)o1 {
    assert!(!sel.is_null());
    msg_send_no_type_checking(env, (this, sel, o1))
}

- (id)performSelector:(SEL)sel
           withObject:(id)o1
           withObject:(id)o2 {
    assert!(!sel.is_null());
    msg_send_no_type_checking(env, (this, sel, o1, o2))
}

- (())performSelectorInBackground:(SEL)sel
                       withObject:(id)arg {
    detach_new_thread_inner(env, sel, this, arg, /* tolerate_type_mismatch: */ true)
}

- (())performSelector:(SEL)sel withObject:(id)arg afterDelay:(NSTimeInterval)delay {
    let run_loop: id = msg_class![env; NSRunLoop currentRunLoop];
    add_perform_request(env, run_loop, this, sel, arg, Some(delay), false);
}

- (())performSelectorOnMainThread:(SEL)sel withObject:(id)arg waitUntilDone:(bool)wait {
    log_dbg!("performSelectorOnMainThread:{} withObject:{:?} waitUntilDone:{}", sel.as_str(&env.mem), arg, wait);
    if wait && env.current_thread == 0 {
        if sel.as_str(&env.mem).ends_with(':') {
            () = msg_send(env, (this, sel, arg));
        } else {
            assert!(arg.is_null());
            () = msg_send(env, (this, sel));
        }
        return;
    }
    if env.bundle.bundle_identifier().starts_with("com.gameloft.POP") && (sel == env.objc.lookup_selector("startMovie:").unwrap() || sel == env.objc.lookup_selector("stopMovie").unwrap()) && wait {
        log!("Applying game-specific hack for PoP: WW: ignoring performSelectorOnMainThread:SEL({}) waitUntilDone:true", sel.as_str(&env.mem));
        return;
    }
    if env.bundle.bundle_identifier().starts_with("com.gameloft.Asphalt5") && (sel == env.objc.lookup_selector("startMovie:").unwrap() || sel == env.objc.lookup_selector("stopMovie:").unwrap()) && wait {
        log!("Applying game-specific hack for Asphalt5: ignoring performSelectorOnMainThread:SEL({}) waitUntilDone:true", sel.as_str(&env.mem));
        return;
    }
    if env.bundle.bundle_identifier().starts_with("com.gameloft.SplinterCell") && sel == env.objc.lookup_selector("startMovie:").unwrap() && wait {
        log!("Applying game-specific hack for SplinterCell: ignoring performSelectorOnMainThread:SEL({}) waitUntilDone:true", sel.as_str(&env.mem));
        return;
    }
    if env.bundle.bundle_identifier().starts_with("com.gameloft.AssassinsCreed") && sel == env.objc.lookup_selector("moviePlayerInit:").unwrap() && wait {
        log!("Applying game-specific hack for AssassinsCreed: ignoring performSelectorOnMainThread:SEL(moviePlayerInit:) waitUntilDone:true");
        return;
    }
    if env.bundle.bundle_identifier().starts_with("com.gameloft.Ferrari") && wait {
        if sel == env.objc.lookup_selector("startMovie:").unwrap() {
            log!("Applying game-specific hack for Ferrari GT: ignoring performSelectorOnMainThread:SEL({}) waitUntilDone:true", sel.as_str(&env.mem));
            return;
        }
        if sel == env.objc.lookup_selector("initTextInput:").unwrap() || sel == env.objc.lookup_selector("removeTextField:").unwrap() {
            log!("Applying game-specific hack for Ferrari GT: performing performSelectorOnMainThread:SEL({}) waitUntilDone:true on thread {}", sel.as_str(&env.mem), env.current_thread);
            () = msg_send(env, (this, sel, arg));
            return;
        }
    }
    if env.bundle.bundle_identifier().starts_with("com.gameloft.HOS2") && wait {
        if sel == env.objc.lookup_selector("loadMovie:").unwrap() || sel == env.objc.lookup_selector("sendGameInfo").unwrap() || sel == env.objc.lookup_selector("setStatusBar:").unwrap() {
            log!("Applying game-specific hack for HOS2: performing performSelectorOnMainThread:SEL({}) waitUntilDone:true on thread {}", sel.as_str(&env.mem), env.current_thread);
            if sel.as_str(&env.mem).ends_with(':') {
                () = msg_send(env, (this, sel, arg));
            } else {
                assert!(arg.is_null());
                () = msg_send(env, (this, sel));
            }
            return;
        }
        if sel == env.objc.lookup_selector("startMovie:").unwrap() || sel == env.objc.lookup_selector("stopMovie:").unwrap() {
            log!("Applying game-specific hack for HOS2: ignoring performSelectorOnMainThread:SEL({}) waitUntilDone:true", sel.as_str(&env.mem));
            return;
        }
    }

    let run_loop: id = msg_class![env; NSRunLoop mainRunLoop];
    let sem = add_perform_request(env, run_loop, this, sel, arg, None, wait);
    if wait {
        sem_wait(env, sem);
        host_destroy_semaphore(env, sem);
    }
}

// UINibLoadingAdditions protocol
- (())awakeFromNib {
    // no-op
}

@end

};

/// Box a getter or ivar with its actual ABI type. In particular, float/double
/// accessors must not be dispatched as object-returning methods.
fn box_key_value(
    env: &mut Environment,
    object: id,
    selector: Option<SEL>,
    address: MutVoidPtr,
    encoding: Option<&str>,
    host_signature: Option<TypeId>,
) -> id {
    let encoding = encoding
        .map(|s| s.trim_start_matches(['r', 'n', 'N', 'o', 'O', 'R', 'V']))
        .unwrap_or("");
    macro_rules! read {
        ($type:ty) => {{
            let value: $type = if let Some(selector) = selector {
                msg_send(env, (object, selector))
            } else {
                env.mem.read(address.cast())
            };
            value
        }};
    }
    macro_rules! matches_type {
        ($type:ty, $code:expr) => {
            if let Some(signature) = host_signature {
                signature == TypeId::of::<($type, (id, SEL))>()
            } else {
                encoding.starts_with($code)
            }
        };
    }
    macro_rules! number {
        ($type:ty, $code:expr, $factory:ident) => {
            if matches_type!($type, $code) { let value = read!($type); return msg_class![env; NSNumber $factory:value]; }
        };
    }
    if matches_type!(id, "@") || matches_type!(Class, "#") {
        return read!(id);
    }
    number!(bool, "B", numberWithBool);
    number!(i8, "c", numberWithChar);
    if matches_type!(u8, "C") {
        let value = read!(u8) as u32;
        return msg_class![env; NSNumber numberWithUnsignedInt:value];
    }
    number!(i16, "s", numberWithShort);
    number!(u16, "S", numberWithUnsignedShort);
    number!(i32, "i", numberWithInt);
    number!(i32, "l", numberWithLong);
    number!(u32, "I", numberWithUnsignedInt);
    number!(u32, "L", numberWithUnsignedLong);
    number!(i64, "q", numberWithLongLong);
    number!(u64, "Q", numberWithUnsignedLongLong);
    number!(f32, "f", numberWithFloat);
    number!(f64, "d", numberWithDouble);
    if matches_type!(CGPoint, "{CGPoint=") {
        let value = read!(CGPoint);
        return msg_class![env; NSValue valueWithCGPoint:value];
    }
    if matches_type!(CGSize, "{CGSize=") {
        let value = read!(CGSize);
        return msg_class![env; NSValue valueWithCGSize:value];
    }
    if matches_type!(CGRect, "{CGRect=") {
        let value = read!(CGRect);
        return msg_class![env; NSValue valueWithCGRect:value];
    }
    unimplemented!("Key-value coding for type {encoding}");
}

/// KVC setters return void and take self, _cmd, then the property value.
/// Leave the complete last type intact, including struct names and qualifiers.
fn setter_argument_encoding(mut signature: &str) -> &str {
    for expected in ['v', '@', ':'] {
        signature = signature.trim_start_matches(['r', 'n', 'N', 'o', 'O', 'R', 'V']);
        signature = signature
            .strip_prefix(expected)
            .expect("Invalid KVC setter signature");
        signature = signature.trim_start_matches(|c: char| c.is_ascii_digit());
    }
    signature
}

#[allow(clippy::too_many_arguments)]
fn set_key_value(
    env: &mut Environment,
    object: id,
    key: id,
    value: id,
    selector: Option<SEL>,
    address: MutVoidPtr,
    encoding: Option<&str>,
    host_signature: Option<TypeId>,
) {
    let encoding = encoding
        .map(|s| s.trim_start_matches(['r', 'n', 'N', 'o', 'O', 'R', 'V']))
        .unwrap_or("");
    macro_rules! matches_type {
        ($type:ty, $code:expr) => {
            if let Some(signature) = host_signature {
                signature == TypeId::of::<((), (id, SEL, $type))>()
            } else {
                encoding.starts_with($code)
            }
        };
    }
    if matches_type!(id, "@") || matches_type!(Class, "#") {
        if let Some(selector) = selector {
            // The accessor owns its retain/copy policy.
            () = msg_send(env, (object, selector, value));
        } else {
            let previous: id = env.mem.read(address.cast());
            retain(env, value);
            env.mem.write(address.cast(), value);
            release(env, previous);
        }
        return;
    }
    if value == nil {
        () = msg![env; object setNilValueForKey:key];
        return;
    }
    macro_rules! unbox {
        ($type:ty, $code:expr, $getter:ident) => {
            if matches_type!($type, $code) {
                let unboxed: $type = msg![env; value $getter];
                if let Some(selector) = selector {
                    () = msg_send(env, (object, selector, unboxed));
                } else {
                    env.mem.write(address.cast(), unboxed);
                }
                return;
            }
        };
    }
    unbox!(bool, "B", boolValue);
    unbox!(i8, "c", charValue);
    unbox!(u8, "C", unsignedCharValue);
    unbox!(i16, "s", shortValue);
    unbox!(u16, "S", unsignedShortValue);
    unbox!(i32, "i", intValue);
    unbox!(i32, "l", longValue);
    unbox!(u32, "I", unsignedIntValue);
    unbox!(u32, "L", unsignedLongValue);
    unbox!(i64, "q", longLongValue);
    unbox!(u64, "Q", unsignedLongLongValue);
    unbox!(f32, "f", floatValue);
    unbox!(f64, "d", doubleValue);
    unbox!(CGPoint, "{CGPoint=", CGPointValue);
    unbox!(CGSize, "{CGSize=", CGSizeValue);
    unbox!(CGRect, "{CGRect=", CGRectValue);
    unimplemented!("Key-value setter for type {encoding}");
}
