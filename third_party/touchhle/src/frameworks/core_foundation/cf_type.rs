/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `CFType` (type-generic functions etc).

use super::{CFHashCode, CFIndex};
use crate::dyld::{export_c_func, FunctionExports};
use crate::frameworks::foundation::NSUInteger;
use crate::objc::{Class, ObjC};
use crate::{msg, objc};
use crate::{msg_class, Environment};

pub type CFTypeRef = objc::id;
pub type CFTypeID = u32;

// Type IDs are opaque values, not ABI constants. Mutable subclasses must have
// the same ID as their immutable family. Keep the registry and exports together
// so CFGetTypeID and the per-type functions cannot disagree.
macro_rules! type_ids {
    ($(($function:ident, $class:literal, $value:literal)),+ $(,)?) => {
        $(fn $function(_env: &mut Environment) -> CFTypeID { $value })+
        const TYPE_CLASSES: &[(&str, CFTypeID)] = &[$(($class, $value)),+];
        pub const FUNCTIONS: FunctionExports = &[
            export_c_func!(CFRetain(_)),
            export_c_func!(CFRelease(_)),
            export_c_func!(CFGetRetainCount(_)),
            export_c_func!(CFEqual(_, _)),
            export_c_func!(CFHash(_)),
            export_c_func!(CFGetTypeID(_)),
            export_c_func!(CFBooleanGetTypeID()),
            $(export_c_func!($function()),)+
        ];
    };
}

fn CFBooleanGetTypeID(_env: &mut Environment) -> CFTypeID {
    1
}

type_ids! {
    (CFStringGetTypeID, "NSString", 2),
    (CFNumberGetTypeID, "NSNumber", 3),
    (CFArrayGetTypeID, "NSArray", 4),
    (CFDictionaryGetTypeID, "NSDictionary", 5),
    (CFDataGetTypeID, "NSData", 6),
    (CFDateGetTypeID, "NSDate", 7),
    (CFURLGetTypeID, "NSURL", 8),
    (CFSetGetTypeID, "NSSet", 9),
    (CFNullGetTypeID, "NSNull", 10),
    (CFCharacterSetGetTypeID, "NSCharacterSet", 11),
    (CFLocaleGetTypeID, "NSLocale", 12),
    (CFTimeZoneGetTypeID, "NSTimeZone", 13),
    (CFRunLoopGetTypeID, "NSRunLoop", 14),
    (CFRunLoopTimerGetTypeID, "NSTimer", 15),
    (CFBundleGetTypeID, "NSBundle", 16),
    (CFUUIDGetTypeID, "_touchHLE_CFUUID", 17),
    (CFErrorGetTypeID, "NSError", 18),
}

fn CFGetTypeID(env: &mut Environment, object: CFTypeRef) -> CFTypeID {
    assert!(!object.is_null(), "CFGetTypeID requires a non-null object");
    if crate::frameworks::foundation::ns_value::is_boolean(env, object) {
        return CFBooleanGetTypeID(env);
    }
    let class = ObjC::read_isa(object, &env.mem);
    for &(name, type_id) in TYPE_CLASSES {
        let family = env.objc.get_known_class(name, &mut env.mem);
        if env.objc.class_is_subclass_of(class, family) {
            return type_id;
        }
    }
    unimplemented!("CFGetTypeID for {}", env.objc.get_class_name(class))
}

pub fn CFRetain(env: &mut Environment, object: CFTypeRef) -> CFTypeRef {
    assert!(!object.is_null()); // not allowed, unlike for normal objc objects
    objc::retain(env, object)
}
pub fn CFRelease(env: &mut Environment, object: CFTypeRef) {
    objc::release(env, object);
}

pub fn CFGetRetainCount(env: &mut Environment, object: CFTypeRef) -> CFIndex {
    let count: NSUInteger = msg![env; object retainCount];
    count as CFIndex
}

pub fn CFEqual(env: &mut Environment, object1: CFTypeRef, object2: CFTypeRef) -> bool {
    if object1 == object2 {
        return true;
    }
    // TODO: other classes
    let str_class: Class = msg_class![env; NSString class];
    let object1_class: Class = msg![env; object1 class];
    assert!(msg![env; object1_class isKindOfClass:str_class]);
    let object2_class: Class = msg![env; object2 class];
    assert!(msg![env; object2_class isKindOfClass:str_class]);
    // TODO: use isEqual: once it is fixed
    msg![env; object1 isEqualToString:object2]
}

pub fn CFHash(env: &mut Environment, object: CFTypeRef) -> CFHashCode {
    msg![env; object hash]
}
