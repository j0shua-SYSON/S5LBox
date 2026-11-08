/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `CFPreferences`.
//!
//! According to Apple's docs, it's not toll-free bridged to `NSUserDefaults`,
//! but we are still implementing one atop of another.

use super::cf_string::CFStringRef;
use super::CFTypeRef;
use crate::dyld::{export_c_func, ConstantExports, FunctionExports, HostConstant};
use crate::frameworks::foundation::{ns_string, ns_value};
use crate::mem::MutPtr;
use crate::objc::{id, msg, msg_class, nil, release, ObjC};
use crate::Environment;

type CFPropertyListRef = CFTypeRef;

fn defaults_for_app(env: &mut Environment, app_id: CFStringRef) -> id {
    let app = ns_string::to_rust_string(env, app_id);
    assert!(
        app == kCFPreferencesCurrentApplication || app == env.bundle.bundle_identifier(),
        "CFPreferences domains other than the current app are not implemented"
    );
    msg_class![env; NSUserDefaults standardUserDefaults]
}

fn is_kind(env: &mut Environment, value: id, name: &str) -> bool {
    if value == nil {
        return false;
    }
    let family = env.objc.get_known_class(name, &mut env.mem);
    env.objc
        .class_is_subclass_of(ObjC::read_isa(value, &env.mem), family)
}

fn CFPreferencesCopyAppValue(
    env: &mut Environment,
    key: CFStringRef,
    app_id: CFStringRef,
) -> CFPropertyListRef {
    let user_defaults = defaults_for_app(env, app_id);
    let value: id = msg![env; user_defaults objectForKey:key];
    msg![env; value copy]
}

fn CFPreferencesSetAppValue(
    env: &mut Environment,
    key: CFStringRef,
    value: CFPropertyListRef,
    app_id: CFStringRef,
) {
    let user_defaults = defaults_for_app(env, app_id);
    if value == nil {
        msg![env; user_defaults removeObjectForKey:key]
    } else {
        msg![env; user_defaults setObject:value forKey:key]
    }
}

fn CFPreferencesAppSynchronize(env: &mut Environment, app_id: CFStringRef) -> bool {
    let user_defaults = defaults_for_app(env, app_id);
    msg![env; user_defaults synchronize]
}

fn CFPreferencesGetAppBooleanValue(
    env: &mut Environment,
    key: CFStringRef,
    app: CFStringRef,
    valid: MutPtr<bool>,
) -> bool {
    let value = CFPreferencesCopyAppValue(env, key, app);
    let result = if is_kind(env, value, "NSString") {
        match ns_string::to_rust_string(env, value)
            .to_ascii_lowercase()
            .as_str()
        {
            "true" | "yes" => Some(true),
            "false" | "no" => Some(false),
            _ => None,
        }
    } else if is_kind(env, value, "NSNumber") && !ns_value::is_floating_point(env, value) {
        Some(msg![env; value boolValue])
    } else {
        None
    };
    release(env, value);
    if !valid.is_null() {
        env.mem.write(valid, result.is_some());
    }
    result.unwrap_or(false)
}

fn CFPreferencesGetAppIntegerValue(
    env: &mut Environment,
    key: CFStringRef,
    app: CFStringRef,
    valid: MutPtr<bool>,
) -> i32 {
    let value = CFPreferencesCopyAppValue(env, key, app);
    let result = if is_kind(env, value, "NSString") {
        ns_string::to_rust_string(env, value)
            .trim_start_matches(|c: char| c.is_ascii_whitespace())
            .parse::<i32>()
            .ok()
    } else if is_kind(env, value, "NSNumber")
        && !ns_value::is_boolean(env, value)
        && !ns_value::is_floating_point(env, value)
    {
        Some(msg![env; value intValue])
    } else {
        None
    };
    release(env, value);
    if !valid.is_null() {
        env.mem.write(valid, result.is_some());
    }
    result.unwrap_or(0)
}

pub const kCFPreferencesCurrentApplication: &str = "kCFPreferencesCurrentApplication";

pub const CONSTANTS: ConstantExports = &[(
    "_kCFPreferencesCurrentApplication",
    HostConstant::NSString(kCFPreferencesCurrentApplication),
)];

pub const FUNCTIONS: FunctionExports = &[
    export_c_func!(CFPreferencesCopyAppValue(_, _)),
    export_c_func!(CFPreferencesSetAppValue(_, _, _)),
    export_c_func!(CFPreferencesAppSynchronize(_)),
    export_c_func!(CFPreferencesGetAppBooleanValue(_, _, _)),
    export_c_func!(CFPreferencesGetAppIntegerValue(_, _, _)),
];
