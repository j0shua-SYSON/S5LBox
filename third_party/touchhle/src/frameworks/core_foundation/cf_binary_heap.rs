/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! CFBinaryHeap: a minimum-priority queue with guest comparison and ownership
//! callbacks. The opaque reference uses an internal ObjC class for CFRelease.
//! API: <https://developer.apple.com/documentation/corefoundation/cfbinaryheap>

use super::cf_allocator::{kCFAllocatorDefault, CFAllocatorRef};
use super::{CFComparisonResult, CFIndex, CFTypeRef};
use crate::abi::{CallFromHost, GuestFunction};
use crate::dyld::{export_c_func, ConstantExports, FunctionExports, HostConstant, HostFunction};
use crate::mem::{ConstPtr, ConstVoidPtr, MutPtr, MutVoidPtr, SafeRead};
use crate::objc::{id, msg, objc_classes, ClassExports, HostObject};
use crate::Environment;
use std::cmp::Ordering;

type CFBinaryHeapRef = CFTypeRef;

#[derive(Clone, Copy)]
#[repr(C, packed)]
struct Callbacks {
    version: CFIndex,
    retain: GuestFunction,
    release: GuestFunction,
    copy_description: GuestFunction,
    compare: GuestFunction,
}
unsafe impl SafeRead for Callbacks {}
impl Default for Callbacks {
    fn default() -> Self {
        Self {
            version: 0,
            retain: GuestFunction::null_ptr(),
            release: GuestFunction::null_ptr(),
            copy_description: GuestFunction::null_ptr(),
            compare: GuestFunction::null_ptr(),
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C, packed)]
struct CompareContext {
    version: CFIndex,
    info: MutVoidPtr,
    retain: GuestFunction,
    release: GuestFunction,
    copy_description: GuestFunction,
}
unsafe impl SafeRead for CompareContext {}

struct Heap {
    values: Vec<ConstVoidPtr>,
    allocator: CFAllocatorRef,
    callbacks: Callbacks,
    context: Option<CompareContext>,
}
impl HostObject for Heap {}

fn create(
    env: &mut Environment,
    allocator: CFAllocatorRef,
    capacity: CFIndex,
    callbacks: Callbacks,
    context: Option<CompareContext>,
) -> CFBinaryHeapRef {
    assert!(allocator == kCFAllocatorDefault || env.mem.read(allocator).is_system_default());
    assert!(capacity >= 0);
    assert!(callbacks.version == 0);
    assert!(context.is_none_or(|c| c.version == 0));
    // Capacity is only a hint. Do not allocate an arbitrarily large buffer for
    // an empty heap. Native CF copies the context without retaining its info.
    let state = Box::new(Heap {
        values: Vec::new(),
        allocator,
        callbacks,
        context,
    });
    let class = env
        .objc
        .get_known_class("_touchHLE_CFBinaryHeap", &mut env.mem);
    env.objc.alloc_object(class, state, &mut env.mem)
}

fn CFBoundedHeapCompare(
    env: &mut Environment,
    heap: CFBinaryHeapRef,
    a: ConstVoidPtr,
    b: ConstVoidPtr,
) -> Ordering {
    let state = env.objc.borrow::<Heap>(heap);
    let callback = state.callbacks.compare;
    let context = state.context.map_or(MutVoidPtr::null(), |c| c.info);
    if callback.to_ptr().is_null() {
        a.to_bits().cmp(&b.to_bits())
    } else {
        // Never keep a host borrow across guest callbacks: they may call CF
        // APIs themselves. Mutating the heap from a comparator is unsupported.
        let result: CFComparisonResult = callback.call_from_host(env, (a, b, context));
        result.cmp(&0)
    }
}

fn CFBinaryHeapCreate(
    env: &mut Environment,
    allocator: CFAllocatorRef,
    capacity: CFIndex,
    callbacks: ConstPtr<Callbacks>,
    context: ConstPtr<CompareContext>,
) -> CFBinaryHeapRef {
    let callbacks = if callbacks.is_null() {
        Callbacks::default()
    } else {
        env.mem.read(callbacks)
    };
    let context = if context.is_null() {
        None
    } else {
        Some(env.mem.read(context))
    };
    create(env, allocator, capacity, callbacks, context)
}

fn CFBinaryHeapCreateCopy(
    env: &mut Environment,
    allocator: CFAllocatorRef,
    capacity: CFIndex,
    original: CFBinaryHeapRef,
) -> CFBinaryHeapRef {
    let state = env.objc.borrow::<Heap>(original);
    let (callbacks, context, values) = (state.callbacks, state.context, state.values.clone());
    let copy = create(env, allocator, capacity, callbacks, context);
    for value in values {
        CFBinaryHeapAddValue(env, copy, value);
    }
    copy
}

fn CFBinaryHeapGetCount(env: &mut Environment, heap: CFBinaryHeapRef) -> CFIndex {
    env.objc
        .borrow::<Heap>(heap)
        .values
        .len()
        .try_into()
        .unwrap()
}

fn CFBinaryHeapGetMinimum(env: &mut Environment, heap: CFBinaryHeapRef) -> ConstVoidPtr {
    env.objc
        .borrow::<Heap>(heap)
        .values
        .first()
        .copied()
        .unwrap_or(ConstVoidPtr::null())
}

fn CFBinaryHeapGetMinimumIfPresent(
    env: &mut Environment,
    heap: CFBinaryHeapRef,
    output: MutPtr<ConstVoidPtr>,
) -> bool {
    let first = env.objc.borrow::<Heap>(heap).values.first().copied();
    if let Some(value) = first {
        if !output.is_null() {
            env.mem.write(output, value);
        }
        true
    } else {
        false
    }
}

fn CFBinaryHeapGetCountOfValue(
    env: &mut Environment,
    heap: CFBinaryHeapRef,
    value: ConstVoidPtr,
) -> CFIndex {
    let values = env.objc.borrow::<Heap>(heap).values.clone();
    values
        .into_iter()
        .filter(|&v| CFBoundedHeapCompare(env, heap, v, value) == Ordering::Equal)
        .count()
        .try_into()
        .unwrap()
}

fn CFBinaryHeapContainsValue(
    env: &mut Environment,
    heap: CFBinaryHeapRef,
    value: ConstVoidPtr,
) -> bool {
    CFBinaryHeapGetCountOfValue(env, heap, value) != 0
}

fn sorted_values(env: &mut Environment, heap: CFBinaryHeapRef) -> Vec<ConstVoidPtr> {
    let mut values = env.objc.borrow::<Heap>(heap).values.clone();
    values.sort_by(|&a, &b| CFBoundedHeapCompare(env, heap, a, b));
    values
}

fn CFBinaryHeapGetValues(
    env: &mut Environment,
    heap: CFBinaryHeapRef,
    output: MutPtr<ConstVoidPtr>,
) {
    for (i, value) in sorted_values(env, heap).into_iter().enumerate() {
        env.mem.write(output + u32::try_from(i).unwrap(), value);
    }
}

fn CFBinaryHeapApplyFunction(
    env: &mut Environment,
    heap: CFBinaryHeapRef,
    function: GuestFunction,
    context: MutVoidPtr,
) {
    for value in sorted_values(env, heap) {
        () = function.call_from_host(env, (value, context));
    }
}

fn CFBinaryHeapAddValue(env: &mut Environment, heap: CFBinaryHeapRef, mut value: ConstVoidPtr) {
    let state = env.objc.borrow::<Heap>(heap);
    let (callback, allocator) = (state.callbacks.retain, state.allocator);
    if !callback.to_ptr().is_null() {
        value = callback.call_from_host(env, (allocator, value));
    }
    let values = &mut env.objc.borrow_mut::<Heap>(heap).values;
    let mut index = values.len();
    values.push(value);
    while index > 0 {
        let parent = (index - 1) / 2;
        let parent_value = env.objc.borrow::<Heap>(heap).values[parent];
        if CFBoundedHeapCompare(env, heap, value, parent_value) != Ordering::Less {
            break;
        }
        env.objc.borrow_mut::<Heap>(heap).values.swap(index, parent);
        index = parent;
    }
}

fn release_value(env: &mut Environment, heap: CFBinaryHeapRef, value: ConstVoidPtr) {
    let state = env.objc.borrow::<Heap>(heap);
    let (callback, allocator) = (state.callbacks.release, state.allocator);
    if !callback.to_ptr().is_null() {
        () = callback.call_from_host(env, (allocator, value));
    }
}

fn CFBinaryHeapRemoveMinimumValue(env: &mut Environment, heap: CFBinaryHeapRef) {
    let values = &mut env.objc.borrow_mut::<Heap>(heap).values;
    if values.is_empty() {
        return;
    }
    let value = values.swap_remove(0);
    let len = values.len();
    let mut index = 0;
    while index * 2 + 1 < len {
        let mut child = index * 2 + 1;
        if child + 1 < len {
            let values = &env.objc.borrow::<Heap>(heap).values;
            let (a, b) = (values[child], values[child + 1]);
            if CFBoundedHeapCompare(env, heap, b, a) == Ordering::Less {
                child += 1;
            }
        }
        let values = &env.objc.borrow::<Heap>(heap).values;
        let (a, b) = (values[index], values[child]);
        if CFBoundedHeapCompare(env, heap, b, a) != Ordering::Less {
            break;
        }
        env.objc.borrow_mut::<Heap>(heap).values.swap(index, child);
        index = child;
    }
    release_value(env, heap, value);
}

fn CFBinaryHeapRemoveAllValues(env: &mut Environment, heap: CFBinaryHeapRef) {
    let values = std::mem::take(&mut env.objc.borrow_mut::<Heap>(heap).values);
    for value in values {
        release_value(env, heap, value);
    }
}

pub const CLASSES: ClassExports = objc_classes! {
(env, this, _cmd);
@implementation _touchHLE_CFBinaryHeap: NSObject
- (())dealloc {
    CFBinaryHeapRemoveAllValues(env, this);
    env.objc.dealloc_object(this, &mut env.mem);
}
@end
};

fn string_retain(
    env: &mut Environment,
    _allocator: CFAllocatorRef,
    value: ConstVoidPtr,
) -> ConstVoidPtr {
    crate::objc::retain(env, value.cast_mut().cast())
        .cast_const()
        .cast()
}
fn string_release(env: &mut Environment, _allocator: CFAllocatorRef, value: ConstVoidPtr) {
    crate::objc::release(env, value.cast_mut().cast());
}
fn string_description(env: &mut Environment, value: ConstVoidPtr) -> id {
    let object: id = value.cast_mut().cast();
    let description: id = msg![env; object description];
    crate::objc::retain(env, description)
}
fn string_compare(
    env: &mut Environment,
    a: ConstVoidPtr,
    b: ConstVoidPtr,
    _context: MutVoidPtr,
) -> CFComparisonResult {
    let (a, b): (id, id) = (a.cast_mut().cast(), b.cast_mut().cast());
    msg![env; a compare:b]
}

pub const CONSTANTS: ConstantExports = &[(
    "_kCFStringBinaryHeapCallBacks",
    HostConstant::Custom(|env| {
        let callbacks = Callbacks {
            version: 0,
            retain: env.dyld.create_guest_function(
                &mut env.mem,
                "__touchHLE_heap_string_retain",
                &(string_retain as fn(&mut Environment, _, _) -> _) as HostFunction,
            ),
            release: env.dyld.create_guest_function(
                &mut env.mem,
                "__touchHLE_heap_string_release",
                &(string_release as fn(&mut Environment, _, _)) as HostFunction,
            ),
            copy_description: env.dyld.create_guest_function(
                &mut env.mem,
                "__touchHLE_heap_string_description",
                &(string_description as fn(&mut Environment, _) -> _) as HostFunction,
            ),
            compare: env.dyld.create_guest_function(
                &mut env.mem,
                "__touchHLE_heap_string_compare",
                &(string_compare as fn(&mut Environment, _, _, _) -> _) as HostFunction,
            ),
        };
        env.mem.alloc_and_write(callbacks).cast_void().cast_const()
    }),
)];

pub const FUNCTIONS: FunctionExports = &[
    export_c_func!(CFBinaryHeapCreate(_, _, _, _)),
    export_c_func!(CFBinaryHeapCreateCopy(_, _, _)),
    export_c_func!(CFBinaryHeapGetCount(_)),
    export_c_func!(CFBinaryHeapGetCountOfValue(_, _)),
    export_c_func!(CFBinaryHeapContainsValue(_, _)),
    export_c_func!(CFBinaryHeapGetMinimum(_)),
    export_c_func!(CFBinaryHeapGetMinimumIfPresent(_, _)),
    export_c_func!(CFBinaryHeapGetValues(_, _)),
    export_c_func!(CFBinaryHeapApplyFunction(_, _, _)),
    export_c_func!(CFBinaryHeapAddValue(_, _)),
    export_c_func!(CFBinaryHeapRemoveMinimumValue(_)),
    export_c_func!(CFBinaryHeapRemoveAllValues(_)),
];
