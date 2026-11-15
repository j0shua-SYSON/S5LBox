/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! The `NSScanner` class.

use crate::frameworks::foundation::ns_string::{from_u16_vec, to_rust_string};
use crate::frameworks::foundation::{unichar, NSNotFound, NSRange, NSUInteger};
use crate::mem::MutPtr;
use crate::objc::{
    autorelease, id, msg, msg_class, nil, objc_classes, release, retain, ClassExports, HostObject,
    NSZonePtr,
};
use crate::Environment;

// TODO: Speed up by optimizing for internal subclasses
#[derive(Default, Clone)]
struct NSScannerHostObject {
    /// NSCharacterSet *, characters to be skipped
    to_be_skipped: id,
    /// NSString *, should always be immutable since it's copied
    string: id,
    /// Length is cached since it is immutable.
    len: NSUInteger,
    pos: NSUInteger,
}
impl HostObject for NSScannerHostObject {}

pub const CLASSES: ClassExports = objc_classes! {

(env, this, _cmd);

@implementation NSScanner: NSObject

+ (id)scannerWithString:(id)string {
    let new: id = msg![env; this alloc];
    let new = msg![env; new initWithString:string];
    autorelease(env, new)
}

+ (id)allocWithZone:(NSZonePtr)zone {
    // NSScanner might be subclassed by something which needs
    // allocWithZone: to have the normal behaviour. Unimplemented: call
    // superclass alloc then.
    assert!(this == env.objc.get_known_class("NSScanner", &mut env.mem));
    msg_class![env; _touchHLE_NSScanner allocWithZone:zone]
}

@end

// Our private subclass that is the single implementation of NSScanner for
// the time being.
@implementation _touchHLE_NSScanner: NSScanner

+ (id)allocWithZone:(NSZonePtr)_zone {
    let host_object = Box::new(NSScannerHostObject::default());
    env.objc.alloc_object(this, host_object, &mut env.mem)
}

- (id)initWithString:(id)string { // NSString *
    assert!(string != nil);
    let string: id = msg![env; string copy]; // Same behaviour as simulator
    let len: NSUInteger = msg![env; string length];
    let default_set = msg_class![env; NSCharacterSet whitespaceAndNewlineCharacterSet];
    retain(env, default_set);
    *env.objc.borrow_mut(this) = NSScannerHostObject {
        to_be_skipped: default_set,
        string,
        len,
        pos: 0
    };
    this
}

- (())dealloc {
    let &NSScannerHostObject {
        to_be_skipped,
        string,
        ..
    } = env.objc.borrow(this);
    release(env, string);
    release(env, to_be_skipped);
    env.objc.dealloc_object(this, &mut env.mem);
}

- (())setCharactersToBeSkipped:(id)to_be_skipped { // NSCharacterSet *
    let old_to_be_skipped = env.objc.borrow::<NSScannerHostObject>(this).to_be_skipped;
    env.objc.borrow_mut::<NSScannerHostObject>(this).to_be_skipped = to_be_skipped;
    retain(env, to_be_skipped);
    release(env, old_to_be_skipped);
}
- (id)charactersToBeSkipped {
    env.objc.borrow::<NSScannerHostObject>(this).to_be_skipped
}

- (NSUInteger)scanLocation {
    env.objc.borrow::<NSScannerHostObject>(this).pos
}

- (())setScanLocation:(NSUInteger)loc {
    let host = env.objc.borrow_mut::<NSScannerHostObject>(this);
    assert!(loc <= host.len); // TODO: raise NSRangeException
    host.pos = loc;
}

- (bool)isAtEnd {
    skip_characters(env, this);
    let NSScannerHostObject { len, pos, .. } = env.objc.borrow::<NSScannerHostObject>(this);
    len == pos
}

- (bool)scanUpToCharactersFromSet:(id)cset intoString:(MutPtr<id>)str {
    skip_characters(env, this);

    let NSScannerHostObject { to_be_skipped, string, len, mut pos } = env.objc.borrow::<NSScannerHostObject>(this).clone();
    if pos >= len {
        // Does nothing (same as simulator)
        return false;
    }
    let first_scan: unichar = msg![env; string characterAtIndex:pos];
    if msg![env; cset characterIsMember:first_scan] {
        // Does nothing (same as simulator)
        return false;
    }
    let mut chars = vec![first_scan];
    pos += 1;
    while pos < len {
        let curr = msg![env; string characterAtIndex:pos];
        if msg![env; cset characterIsMember:curr] {
            break
        }
        pos += 1;
        chars.push(curr);
    }
    if !str.is_null() {
        let out = from_u16_vec(env, chars);
        autorelease(env, out);
        env.mem.write(str, out);
    }

    *env.objc.borrow_mut::<NSScannerHostObject>(this) = NSScannerHostObject { to_be_skipped, string, len, pos };
    true
}

- (bool)scanCharactersFromSet:(id)cset intoString:(MutPtr<id>)str {
    let inv_cset: id = msg![env; cset invertedSet];
    msg![env; this scanUpToCharactersFromSet:inv_cset intoString:str]
}

- (bool)scanHexInt:(MutPtr<u32>)result {
    let parsed = scan_hex(env, this, u32::MAX as u64);
    if let Some(value) = parsed { if !result.is_null() { env.mem.write(result, value as u32); } }
    parsed.is_some()
}

- (bool)scanHexLongLong:(MutPtr<u64>)result {
    let parsed = scan_hex(env, this, u64::MAX);
    if let Some(value) = parsed { if !result.is_null() { env.mem.write(result, value); } }
    parsed.is_some()
}

- (bool)scanFloat:(MutPtr<f32>)result {
    let parsed = scan_decimal(env, this);
    if let Some(value) = parsed { if !result.is_null() { env.mem.write(result, value as f32); } }
    parsed.is_some()
}

- (bool)scanDouble:(MutPtr<f64>)result {
    let parsed = scan_decimal(env, this);
    if let Some(value) = parsed { if !result.is_null() { env.mem.write(result, value); } }
    parsed.is_some()
}

- (bool)scanUpToString:(id)stop_string // NSString *
            intoString:(MutPtr<id>)result { // NSString **
    skip_characters(env, this);

    let NSScannerHostObject { string, len, pos, .. } = env.objc.borrow::<NSScannerHostObject>(this).clone();
    log_dbg!("scanUpToString:'{}' intoString: from '{}' at {}", to_rust_string(env, stop_string), to_rust_string(env, string), pos);

    // Success requires consuming characters. At EOF an absent delimiter would
    // otherwise produce an empty string and report success forever.
    if pos >= len {
        return false;
    }

    // TODO: avoid string copying
    let left: id = msg![env; string substringFromIndex:pos];
    let range: NSRange = msg![env; left rangeOfString:stop_string];
    if range.location == 0 {
        return false;
    }

    let scan_len = if range.location == NSNotFound as NSUInteger {
        len - pos
    } else {
        range.location
    };
    assert!(pos + scan_len <= len);
    env.objc.borrow_mut::<NSScannerHostObject>(this).pos = pos + scan_len;

    if !result.is_null() {
        let copy: id = msg![env; left substringToIndex:scan_len];
        log_dbg!("scanned '{}' up to {}", to_rust_string(env, copy), pos + scan_len);
        // Note: substring is already autoreleased
        env.mem.write(result, copy);
    }
    true
}

- (bool)scanString:(id)scan_string // NSString *
        intoString:(MutPtr<id>)result { // NSString **
    skip_characters(env, this);

    let NSScannerHostObject { string, len, pos, .. } = env.objc.borrow::<NSScannerHostObject>(this).clone();
    log_dbg!("scanString:{} intoString: from '{}' at {}", to_rust_string(env, scan_string), to_rust_string(env, string), pos);

    let scan_len: NSUInteger = msg![env; scan_string length];
    if scan_len == 0 || pos >= len {
        return false;
    }

    // TODO: avoid string copying
    let left: id = msg![env; string substringFromIndex:pos];
    let same_prefix: bool = msg![env; left hasPrefix:scan_string];
    if !same_prefix {
        return false;
    }

    assert!(pos + scan_len <= len);
    env.objc.borrow_mut::<NSScannerHostObject>(this).pos = pos + scan_len;

    if !result.is_null() {
        let copy: id = msg![env; scan_string copy];
        autorelease(env, copy);
        env.mem.write(result, copy);
    }
    true
}

- (bool)scanInt:(MutPtr<i32>)result {
    skip_characters(env, this);

    let NSScannerHostObject { to_be_skipped, string, len, pos } = std::mem::take(env.objc.borrow_mut::<NSScannerHostObject>(this));
    let left: id = msg![env; string substringFromIndex:pos];
    if left == nil {
        *env.objc.borrow_mut::<NSScannerHostObject>(this) = NSScannerHostObject { to_be_skipped, string, len, pos };
        return false;
    }

    let st = to_rust_string(env, left);
    let mut cutoff = st.len();
    for (i, c) in st.char_indices() {
        if !c.is_ascii_digit() && c != '+' && c != '-' {
            cutoff = i;
            break;
        }
    }
    if cutoff == 0 {
        log_dbg!("scanInt: no valid int found for '{}'", st);
        *env.objc.borrow_mut::<NSScannerHostObject>(this) = NSScannerHostObject { to_be_skipped, string, len, pos };
        return false;
    }

    if !result.is_null() {
        // TODO: handle over/underflow properly
        let res = st[..cutoff].parse().unwrap_or(0);
        log_dbg!("scanInt: from '{}' -> {}", st, res);
        env.mem.write(result, res);
    }

    *env.objc.borrow_mut::<NSScannerHostObject>(this) = NSScannerHostObject { to_be_skipped, string, len, pos: pos + cutoff as NSUInteger };
    true
}

@end

};

// Decimal digits in the BMP, expressed as the zero of each ten-character block.
// NSScanner scans UTF-16 units: native Foundation accepts these decimal digits
// (including in exponents), but does not combine surrogate pairs into digits.
const DECIMAL_ZEROES: &[u16] = &[
    0x0030, 0x0660, 0x06f0, 0x07c0, 0x0966, 0x09e6, 0x0a66, 0x0ae6, 0x0b66, 0x0be6,
    0x0c66, 0x0ce6, 0x0d66, 0x0de6, 0x0e50, 0x0ed0, 0x0f20, 0x1040, 0x1090, 0x17e0,
    0x1810, 0x1946, 0x19d0, 0x1a80, 0x1a90, 0x1b50, 0x1bb0, 0x1c40, 0x1c50, 0xa620,
    0xa8d0, 0xa900, 0xa9d0, 0xa9f0, 0xaa50, 0xabf0, 0xff10,
];

/// Decimal syntax for a scanner with the default (nil) locale. The result's
/// length is in UTF-16 units, even when its normalized digits were non-ASCII.
fn decimal_prefix(text: &str) -> Option<(usize, f64)> {
    let bytes: Vec<u8> = text.encode_utf16().map_while(|unit| {
        if matches!(unit, 0x2b | 0x2d | 0x2e | 0x45 | 0x65) { return Some(unit as u8); }
        DECIMAL_ZEROES.iter().find_map(|&zero| {
            let digit = unit.wrapping_sub(zero);
            (digit < 10).then(|| b'0' + digit as u8)
        })
    }).collect();
    let mut end = usize::from(matches!(bytes.first(), Some(b'+' | b'-')));
    let mut digits = 0;
    while bytes.get(end).is_some_and(u8::is_ascii_digit) { end += 1; digits += 1; }
    if bytes.get(end) == Some(&b'.') {
        end += 1;
        while bytes.get(end).is_some_and(u8::is_ascii_digit) { end += 1; digits += 1; }
    }
    if digits == 0 { return None; }
    if matches!(bytes.get(end), Some(b'e' | b'E')) {
        let exponent = end;
        end += 1;
        if matches!(bytes.get(end), Some(b'+' | b'-')) { end += 1; }
        let first_digit = end;
        while bytes.get(end).is_some_and(u8::is_ascii_digit) { end += 1; }
        if first_digit == end { end = exponent; }
    }
    let value = std::str::from_utf8(&bytes[..end]).unwrap().parse().ok()?;
    Some((end, value))
}

fn scan_decimal(env: &mut Environment, scanner: id) -> Option<f64> {
    let original = env.objc.borrow::<NSScannerHostObject>(scanner).pos;
    skip_characters(env, scanner);
    let NSScannerHostObject { string, pos, len, .. } = env.objc.borrow::<NSScannerHostObject>(scanner).clone();
    let parsed = if pos < len {
        let suffix: id = msg![env; string substringFromIndex:pos];
        decimal_prefix(&to_rust_string(env, suffix))
    } else { None };
    env.objc.borrow_mut::<NSScannerHostObject>(scanner).pos =
        parsed.map_or(original, |(length, _)| pos + length as NSUInteger);
    parsed.map(|(_, value)| value)
}

fn scan_hex(env: &mut Environment, scanner: id, maximum: u64) -> Option<u64> {
    skip_characters(env, scanner);
    let NSScannerHostObject {
        string, pos, len, ..
    } = env.objc.borrow::<NSScannerHostObject>(scanner).clone();
    if pos >= len {
        return None;
    }
    let suffix: id = msg![env; string substringFromIndex:pos];
    let text = to_rust_string(env, suffix);
    let bytes = text.as_bytes();
    let prefix = if bytes.len() >= 3
        && bytes[0] == b'0'
        && matches!(bytes[1], b'x' | b'X')
        && bytes[2].is_ascii_hexdigit()
    {
        2
    } else {
        0
    };
    let mut consumed = prefix;
    let mut value = 0u64;
    for &byte in &bytes[prefix..] {
        let digit = match byte {
            b'0'..=b'9' => byte - b'0',
            b'a'..=b'f' => byte - b'a' + 10,
            b'A'..=b'F' => byte - b'A' + 10,
            _ => break,
        };
        value = value
            .saturating_mul(16)
            .saturating_add(digit as u64)
            .min(maximum);
        consumed += 1;
    }
    if consumed == prefix {
        return None;
    }
    // Scanned digits and the prefix are ASCII, so byte and UTF-16 lengths agree.
    env.objc.borrow_mut::<NSScannerHostObject>(scanner).pos = pos + consumed as u32;
    Some(value)
}

// Skip characters from the scanner's charactersToBeSkipped set.
fn skip_characters(env: &mut Environment, scanner: id) {
    let &NSScannerHostObject {
        to_be_skipped,
        string,
        len,
        mut pos,
    } = env.objc.borrow::<NSScannerHostObject>(scanner);
    loop {
        if pos >= len {
            break;
        }
        let c: unichar = msg![env; string characterAtIndex:pos];
        if msg![env; to_be_skipped characterIsMember:c] {
            pos += 1;
        } else {
            break;
        }
    }
    env.objc.borrow_mut::<NSScannerHostObject>(scanner).pos = pos;
}
