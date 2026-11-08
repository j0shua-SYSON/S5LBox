/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `NSIndexPath`.

use super::{NSComparisonResult, NSInteger, NSNotFound, NSUInteger};
use crate::mem::{ConstPtr, MutPtr};
use crate::objc::{
    autorelease, id, msg, msg_class, nil, objc_classes, retain, Class, ClassExports, HostObject,
    NSZonePtr,
};

#[derive(Default)]
struct IndexPath(Vec<NSUInteger>);
impl HostObject for IndexPath {}

pub const CLASSES: ClassExports = objc_classes! {

(env, this, _cmd);

@implementation NSIndexPath: NSObject

+ (id)allocWithZone:(NSZonePtr)_zone {
    env.objc.alloc_object(this, Box::<IndexPath>::default(), &mut env.mem)
}
+ (id)indexPathWithIndex:(NSUInteger)index {
    let object: id = msg![env; this alloc];
    let object: id = msg![env; object initWithIndex:index];
    autorelease(env, object)
}
+ (id)indexPathWithIndexes:(ConstPtr<NSUInteger>)indexes length:(NSUInteger)length {
    let object: id = msg![env; this alloc];
    let object: id = msg![env; object initWithIndexes:indexes length:length];
    autorelease(env, object)
}
// UIKit's iPhone OS table-view additions.
+ (id)indexPathForRow:(NSInteger)row inSection:(NSInteger)section {
    let object: id = msg![env; this alloc];
    env.objc.borrow_mut::<IndexPath>(object).0 = vec![section as NSUInteger, row as NSUInteger];
    autorelease(env, object)
}

- (id)initWithIndex:(NSUInteger)index {
    env.objc.borrow_mut::<IndexPath>(this).0 = vec![index];
    this
}
- (id)initWithIndexes:(ConstPtr<NSUInteger>)indexes length:(NSUInteger)length {
    let values = (0..length).map(|index| env.mem.read(indexes + index)).collect();
    env.objc.borrow_mut::<IndexPath>(this).0 = values;
    this
}
- (NSUInteger)length {
    env.objc.borrow::<IndexPath>(this).0.len().try_into().unwrap()
}
- (NSUInteger)indexAtPosition:(NSUInteger)position {
    env.objc.borrow::<IndexPath>(this).0.get(position as usize).copied().unwrap_or(NSNotFound as NSUInteger)
}
- (())getIndexes:(MutPtr<NSUInteger>)indexes {
    let values = &env.objc.borrow::<IndexPath>(this).0;
    for (index, value) in values.iter().enumerate() {
        env.mem.write(indexes + index as NSUInteger, *value);
    }
}
- (NSInteger)section {
    let index: NSUInteger = msg![env; this indexAtPosition:0u32];
    index as NSInteger
}
- (NSInteger)row {
    let index: NSUInteger = msg![env; this indexAtPosition:1u32];
    index as NSInteger
}
- (id)indexPathByAddingIndex:(NSUInteger)index {
    let mut values = env.objc.borrow::<IndexPath>(this).0.clone();
    values.push(index);
    let object: id = msg_class![env; NSIndexPath alloc];
    env.objc.borrow_mut::<IndexPath>(object).0 = values;
    autorelease(env, object)
}
- (id)indexPathByRemovingLastIndex {
    let mut values = env.objc.borrow::<IndexPath>(this).0.clone();
    values.pop();
    let object: id = msg_class![env; NSIndexPath alloc];
    env.objc.borrow_mut::<IndexPath>(object).0 = values;
    autorelease(env, object)
}
- (id)copyWithZone:(NSZonePtr)_zone {
    retain(env, this)
}
- (bool)isEqual:(id)other {
    if other == nil { return false; }
    let class: Class = msg_class![env; NSIndexPath class];
    if !msg![env; other isKindOfClass:class] { return false; }
    env.objc.borrow::<IndexPath>(this).0 == env.objc.borrow::<IndexPath>(other).0
}
- (NSUInteger)hash {
    env.objc.borrow::<IndexPath>(this).0.iter().fold(0u32, |hash, value| hash.wrapping_mul(31).wrapping_add(*value))
}
- (NSComparisonResult)compare:(id)other {
    super::ns_string::from_rust_ordering(env.objc.borrow::<IndexPath>(this).0.cmp(&env.objc.borrow::<IndexPath>(other).0))
}
- (())dealloc {
    env.objc.dealloc_object(this, &mut env.mem);
}
@end

};
