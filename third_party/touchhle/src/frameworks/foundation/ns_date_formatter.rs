/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `NSDateFormatter`.
//!
//! Resources:
//! - Apple's [Introduction to Data Formatting Programming Guide For Cocoa](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/DataFormatting/DataFormatting.html)
//! - [Unicode Technical Standard #35](https://unicode.org/reports/tr35/tr35-10.html#Date_Format_Patterns)

use crate::frameworks::core_foundation::time::SECS_FROM_UNIX_TO_APPLE_EPOCHS;
use crate::frameworks::foundation::{ns_string, NSInteger, NSTimeInterval, NSUInteger};
use crate::libc::time::{timestamp_to_calendar_date_wide, tm};
use crate::objc::{
    autorelease, id, msg, msg_class, nil, objc_classes, release, retain, ClassExports, HostObject,
    NSZonePtr,
};

#[derive(Default)]
struct NSDateFormatterHostObject {
    date_format: Option<id>,
    date_style: NSUInteger,
    time_style: NSUInteger,
    time_zone: id,
}
impl HostObject for NSDateFormatterHostObject {}

pub const CLASSES: ClassExports = objc_classes! {

(env, this, _cmd);

@implementation NSDateFormatter: NSObject

+ (id)allocWithZone:(NSZonePtr)_zone {
    let host_object = Box::<NSDateFormatterHostObject>::default();
    env.objc.alloc_object(this, host_object, &mut env.mem)
}

- (())setDateFormat:(id)format { // NSString *
    let new: id = msg![env; format copy];
    let old = env.objc.borrow_mut::<NSDateFormatterHostObject>(this).date_format
        .replace(new);
    if new == nil { env.objc.borrow_mut::<NSDateFormatterHostObject>(this).date_format = None; }
    if let Some(old) = old { release(env, old); }
}

- (id)dateFormat {
    let state = env.objc.borrow::<NSDateFormatterHostObject>(this);
    if let Some(format) = state.date_format { return format; }
    let format = style_pattern(state.date_style, state.time_style);
    let result = ns_string::from_rust_string(env, format);
    autorelease(env, result)
}

- (NSUInteger)dateStyle { env.objc.borrow::<NSDateFormatterHostObject>(this).date_style }
- (NSUInteger)timeStyle { env.objc.borrow::<NSDateFormatterHostObject>(this).time_style }
- (())setDateStyle:(NSUInteger)style {
    assert!(style <= 4);
    let state = env.objc.borrow_mut::<NSDateFormatterHostObject>(this);
    state.date_style = style;
    let old = state.date_format.take();
    if let Some(old) = old { release(env, old); }
}
- (())setTimeStyle:(NSUInteger)style {
    assert!(style <= 4);
    let state = env.objc.borrow_mut::<NSDateFormatterHostObject>(this);
    state.time_style = style;
    let old = state.date_format.take();
    if let Some(old) = old { release(env, old); }
}

- (())setTimeZone:(id)time_zone {
    retain(env, time_zone);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<NSDateFormatterHostObject>(this).time_zone, time_zone);
    release(env, old);
}
- (id)timeZone {
    let zone = env.objc.borrow::<NSDateFormatterHostObject>(this).time_zone;
    if zone == nil { msg_class![env; NSTimeZone defaultTimeZone] } else { zone }
}

- (id)stringFromDate:(id)date {
    if date == nil { return nil; }
    let pattern: id = msg![env; this dateFormat];
    let pattern = ns_string::to_rust_string(env, pattern).into_owned();
    let ti: NSTimeInterval = msg![env; date timeIntervalSinceReferenceDate];
    let zone: id = msg![env; this timeZone];
    let offset: NSInteger = msg![env; zone secondsFromGMT];
    let abbreviation: id = msg![env; zone abbreviation];
    let abbreviation = if abbreviation == nil { "GMT".into() }
        else { ns_string::to_rust_string(env, abbreviation).into_owned() };
    let unix = ti + SECS_FROM_UNIX_TO_APPLE_EPOCHS as f64 + f64::from(offset);
    assert!(unix.is_finite() && unix >= i64::MIN as f64 && unix < i64::MAX as f64);
    // NSDateFormatter's calendar rounds to milliseconds before formatting,
    // including a carry into the next second, even without a fraction field.
    let mut seconds = unix.floor() as i64;
    let mut millis = ((unix - unix.floor()) * 1000.0).round() as u32;
    if millis == 1000 { seconds += 1; millis = 0; }
    let date = timestamp_to_calendar_date_wide(seconds);
    let formatted = format_date(&pattern, &date, millis, offset, &abbreviation);
    let res = ns_string::from_rust_string(env, formatted);
    autorelease(env, res)
}

- (())dealloc {
    let state = env.objc.borrow::<NSDateFormatterHostObject>(this);
    let (format, zone) = (state.date_format, state.time_zone);
    if let Some(format) = format { release(env, format); }
    release(env, zone);
    env.objc.dealloc_object(this, &mut env.mem);
}

@end

};

// The embedded runtime currently exposes English as its preferred language.
// These Gregorian patterns do not implement configurable locales/calendars.
fn style_pattern(date: NSUInteger, time: NSUInteger) -> String {
    let dates = ["", "M/d/yy", "MMM d, y", "MMMM d, y", "EEEE, MMMM d, y"];
    let times = ["", "h:mm a", "h:mm:ss a", "h:mm:ss a z", "h:mm:ss a zzzz"];
    let (d, t) = (dates[date as usize], times[time as usize]);
    if d.is_empty() {
        t.into()
    } else if t.is_empty() {
        d.into()
    } else if date == 1 {
        format!("{d}, {t}")
    } else {
        format!("{d} 'at' {t}")
    }
}

fn format_date(pattern: &str, date: &tm, millis: u32, offset: i32, zone: &str) -> String {
    const MONTHS: [&str; 12] = [
        "January",
        "February",
        "March",
        "April",
        "May",
        "June",
        "July",
        "August",
        "September",
        "October",
        "November",
        "December",
    ];
    const DAYS: [&str; 7] = [
        "Sunday",
        "Monday",
        "Tuesday",
        "Wednesday",
        "Thursday",
        "Friday",
        "Saturday",
    ];
    let mut chars = pattern.chars().peekable();
    let mut output = String::new();
    let mut quoted = false;
    while let Some(c) = chars.next() {
        if c == '\'' {
            if chars.peek() == Some(&'\'') {
                chars.next();
                output.push('\'');
            } else {
                quoted = !quoted;
            }
            continue;
        }
        if quoted || !c.is_ascii_alphabetic() {
            output.push(c);
            continue;
        }
        let mut count = 1;
        while chars.peek() == Some(&c) {
            chars.next();
            count += 1;
        }
        let number = match c {
            'y' | 'Y' => Some(if count == 2 {
                (date.tm_year + 1900).rem_euclid(100)
            } else {
                date.tm_year + 1900
            }),
            'M' | 'L' if count < 3 => Some(date.tm_mon + 1),
            'd' => Some(date.tm_mday),
            'H' => Some(date.tm_hour),
            'h' => Some(if date.tm_hour % 12 == 0 {
                12
            } else {
                date.tm_hour % 12
            }),
            'm' => Some(date.tm_min),
            's' => Some(date.tm_sec),
            _ => None,
        };
        if let Some(value) = number {
            output.push_str(&format!("{value:0count$}"));
            continue;
        }
        match c {
            'M' | 'L' => {
                let month = MONTHS[date.tm_mon as usize];
                output.push_str(if count == 3 {
                    &month[..3]
                } else if count == 5 {
                    &month[..1]
                } else {
                    month
                });
            }
            'E' => {
                let day = DAYS[date.weekday() as usize];
                output.push_str(if count <= 3 {
                    &day[..3]
                } else if count == 5 {
                    &day[..1]
                } else {
                    day
                });
            }
            'a' => output.push_str(if date.tm_hour < 12 { "AM" } else { "PM" }),
            'z' => output.push_str(if count >= 4 && offset == 0 && zone == "GMT" {
                "Greenwich Mean Time"
            } else {
                zone
            }),
            'Z' => {
                let minutes = i64::from(offset).abs() / 60;
                output.push_str(&format!(
                    "{}{:02}{:02}",
                    if offset < 0 { '-' } else { '+' },
                    minutes / 60,
                    minutes % 60
                ));
            }
            'S' => {
                let precision = count.min(3);
                let value = millis / 10u32.pow(3 - precision as u32);
                output.push_str(&format!("{value:0precision$}"));
                output.push_str(&"0".repeat(count - precision));
            }
            _ => unimplemented!("Unsupported date format field: {c}"),
        }
    }
    output
}
