/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Key-path and selector based `NSSortDescriptor` comparisons.

use super::NSComparisonResult;
use crate::objc::{
    autorelease, id, msg, msg_send, nil, objc_classes, release, retain, ClassExports,
    HostObject, NSZonePtr, SEL,
};

struct SortDescriptor {
    key: id,
    ascending: bool,
    selector: SEL,
}
impl HostObject for SortDescriptor {}

pub const CLASSES: ClassExports = objc_classes! {
(env, this, _cmd);

@implementation NSSortDescriptor: NSObject
+ (id)allocWithZone:(NSZonePtr)_zone {
    let selector = env.objc.register_host_selector("compare:".to_owned(), &mut env.mem);
    env.objc.alloc_object(this, Box::new(SortDescriptor {
        key: nil, ascending: true, selector,
    }), &mut env.mem)
}
+ (id)sortDescriptorWithKey:(id)key ascending:(bool)ascending {
    let object: id = msg![env; this alloc];
    let object: id = msg![env; object initWithKey:key ascending:ascending];
    autorelease(env, object)
}
+ (id)sortDescriptorWithKey:(id)key ascending:(bool)ascending selector:(SEL)selector {
    let object: id = msg![env; this alloc];
    let object: id = msg![env; object initWithKey:key ascending:ascending selector:selector];
    autorelease(env, object)
}
- (id)initWithKey:(id)key ascending:(bool)ascending {
    let selector = env.objc.register_host_selector("compare:".to_owned(), &mut env.mem);
    msg![env; this initWithKey:key ascending:ascending selector:selector]
}
- (id)initWithKey:(id)key ascending:(bool)ascending selector:(SEL)selector {
    let key: id = msg![env; key copy];
    let state = env.objc.borrow_mut::<SortDescriptor>(this);
    let old_key = state.key;
    *state = SortDescriptor { key, ascending, selector };
    release(env, old_key);
    this
}
- (id)key { env.objc.borrow::<SortDescriptor>(this).key }
- (bool)ascending { env.objc.borrow::<SortDescriptor>(this).ascending }
- (SEL)selector { env.objc.borrow::<SortDescriptor>(this).selector }
- (id)copyWithZone:(NSZonePtr)_zone { retain(env, this) }
- (id)reversedSortDescriptor {
    let key: id = msg![env; this key];
    let ascending: bool = msg![env; this ascending];
    let selector: SEL = msg![env; this selector];
    let class = msg![env; this class];
    let object: id = msg![env; class alloc];
    let object: id = msg![env; object initWithKey:key ascending:(!ascending) selector:selector];
    autorelease(env, object)
}
- (NSComparisonResult)compareObject:(id)left toObject:(id)right {
    let key: id = msg![env; this key];
    let ascending: bool = msg![env; this ascending];
    let selector: SEL = msg![env; this selector];
    let (left, right) = if key == nil {
        (left, right)
    } else {
        (msg![env; left valueForKeyPath:key], msg![env; right valueForKeyPath:key])
    };
    // Missing values sort before non-nil values in ascending order. Do not send
    // compare: to nil: its zero return would incorrectly make everything equal.
    let result: NSComparisonResult = if left == right { 0 }
        else if left == nil { -1 }
        else if right == nil { 1 }
        else { msg_send(env, (left, selector, right)) };
    // Normalize before reversing so a guest comparator returning INT_MIN cannot
    // overflow. Only the sign of a comparison result determines the ordering.
    if ascending { result.signum() } else { -result.signum() }
}
- (())dealloc {
    let key = env.objc.borrow::<SortDescriptor>(this).key;
    release(env, key);
    env.objc.dealloc_object(this, &mut env.mem);
}
@end
};
