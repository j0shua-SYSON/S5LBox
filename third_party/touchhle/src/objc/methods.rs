/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Handling of Objective-C methods.
//!
//! Resources:
//! - [Apple's documentation of `class_addMethod`](https://developer.apple.com/documentation/objectivec/1418901-class_addmethod?language=objc)

use super::{
    id, nil, objc_super, Class, ClassHostObject, MsgSendSignature, MsgSendSuperSignature, ObjC, SEL,
};
use crate::abi::{CallFromGuest, DotDotDot, GuestArg, GuestFunction, GuestRet};
use crate::mem::{guest_size_of, ConstPtr, GuestUSize, Mem, Ptr, SafeRead};
use crate::Environment;
use std::any::TypeId;

/// Type for any function implementating a method.
///
/// The name is standard Objective-C.
///
/// In our implementation, we have both "host methods" (Rust functions) and
/// "guest methods" (functions in the guest app). Either way, the function needs
/// to conform to the same ABI: [id] and [SEL] must be its first two parameters.
#[allow(clippy::upper_case_acronyms)]
pub enum IMP {
    Host(&'static dyn HostIMP),
    Guest(GuestIMP),
}
impl Clone for IMP {
    fn clone(&self) -> Self {
        match self {
            IMP::Guest(guest_imp) => IMP::Guest(*guest_imp),
            IMP::Host(host) => IMP::Host(*host),
        }
    }
}
impl std::fmt::Debug for IMP {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            IMP::Guest(guest_imp) => write!(f, "guest method IMP at {:?}", guest_imp),
            IMP::Host(host) => write!(f, "host method IMP {:p}", *host),
        }
    }
}
impl IMP {
    pub fn guest_null() -> IMP {
        IMP::Guest(GuestIMP::from_addr_with_thumb_bit(0))
    }
}

impl GuestArg for IMP {
    const REG_COUNT: usize = <GuestFunction as GuestArg>::REG_COUNT;
    fn from_regs(regs: &[u32]) -> Self {
        IMP::Guest(<GuestFunction as GuestArg>::from_regs(regs))
    }
    fn to_regs(self, regs: &mut [u32]) {
        match self {
            IMP::Guest(guest_imp) => guest_imp.to_regs(regs),
            IMP::Host(_) => unimplemented!(),
        }
    }
}
impl GuestRet for IMP {
    fn from_regs(regs: &[u32]) -> Self {
        IMP::Guest(<GuestFunction as GuestArg>::from_regs(regs))
    }
    fn to_regs(self, regs: &mut [u32]) {
        match self {
            IMP::Guest(guest_imp) => guest_imp.to_regs(regs),
            IMP::Host(_) => unimplemented!(),
        }
    }
}

/// Type for any host function implementing a method (see also [IMP]).
pub trait HostIMP: CallFromGuest {
    /// See [MsgSendSignature::type_info].
    fn type_info(&self) -> (TypeId, &'static str);
}

macro_rules! impl_HostIMP {
    ( $($P:ident),* ) => {
        impl<R, $($P,)*> HostIMP for fn(&mut Environment, id, SEL, $($P,)*) -> R
        where
            R: GuestRet + 'static,
            $($P: GuestArg + 'static,)*
        {
            fn type_info(&self) -> (TypeId, &'static str) {
                <(R, (id, SEL, $($P,)*)) as MsgSendSignature>::type_info()
            }
        }
        impl<R, $($P,)*> HostIMP for fn(&mut Environment, id, SEL, $($P,)* DotDotDot) -> R
        where
            R: GuestRet + 'static,
            $($P: GuestArg + 'static,)*
        {
            fn type_info(&self) -> (TypeId, &'static str) {
                todo!("host-to-host message calls with var-args"); // TODO
            }
        }

        // Currently there is a one-to-one mapping between valid host IMP
        // parameters and valid host message send arguments, so the traits for
        // the latter are also implemented here for convenience.

        impl<R, $($P,)*> MsgSendSignature for (R, (id, SEL, $($P,)*))
        where
            R: GuestRet + 'static,
            $($P: GuestArg + 'static,)*
        {
        }
        impl<R, $($P,)*> MsgSendSuperSignature for (R, (ConstPtr<objc_super>, SEL, $($P,)*))
        where
            R: GuestRet + 'static,
            $($P: GuestArg + 'static,)*
        {
            type WithoutSuper = (R, (id, SEL, $($P,)*));
        }
    }
}

impl_HostIMP!();
impl_HostIMP!(P1);
impl_HostIMP!(P1, P2);
impl_HostIMP!(P1, P2, P3);
impl_HostIMP!(P1, P2, P3, P4);
impl_HostIMP!(P1, P2, P3, P4, P5);

/// Type for a guest function implementing a method. See [GuestFunction].
pub type GuestIMP = GuestFunction;

/// The layout of a method list in an app binary.
///
/// The name, field names and field layout are based on what Ghidra outputs.
#[repr(C, packed)]
pub(super) struct method_list_t {
    entsize: GuestUSize,
    count: GuestUSize,
    // entries follow the struct
}
unsafe impl SafeRead for method_list_t {}

/// The layout of a method in an app binary.
///
/// The name, field names and field layout are based on what Ghidra outputs.
#[repr(C, packed)]
struct method_t {
    name: ConstPtr<u8>,
    types: ConstPtr<u8>,
    imp: GuestIMP,
}
unsafe impl SafeRead for method_t {}

#[repr(C, packed)]
pub(super) struct RuntimeMethod {
    owner: Class,
    selector: SEL,
}
unsafe impl SafeRead for RuntimeMethod {}
type Method = ConstPtr<RuntimeMethod>;

pub(super) fn class_getInstanceMethod(
    env: &mut Environment,
    mut class: Class,
    selector: SEL,
) -> Method {
    if selector.is_null() {
        return Ptr::null();
    }
    while class != nil {
        let owner = env.objc.borrow::<ClassHostObject>(class);
        if owner.methods.contains_key(&selector) {
            let key = (class, selector);
            if let Some(handle) = env.objc.method_handles.get(&key) {
                return handle.cast();
            }
            let handle = env
                .mem
                .alloc_and_write(RuntimeMethod {
                    owner: class,
                    selector,
                })
                .cast_const();
            env.objc.method_handles.insert(key, handle.cast());
            return handle;
        }
        class = owner.superclass;
    }
    Ptr::null()
}

pub(super) fn class_getClassMethod(env: &mut Environment, class: Class, selector: SEL) -> Method {
    if class == nil {
        return Ptr::null();
    }
    let metaclass = ObjC::read_isa(class, &env.mem);
    class_getInstanceMethod(env, metaclass, selector)
}

pub(super) fn method_getName(env: &mut Environment, method: Method) -> SEL {
    if method.is_null() {
        return <SEL as GuestArg>::from_regs(&[0]);
    }
    env.mem.read(method).selector
}

pub(super) fn method_getImplementation(env: &mut Environment, method: Method) -> IMP {
    if method.is_null() {
        return IMP::guest_null();
    }
    let RuntimeMethod { owner, selector } = env.mem.read(method);
    super::classes::class_getMethodImplementation(env, owner, selector)
}

pub(super) fn method_getTypeEncoding(env: &mut Environment, method: Method) -> ConstPtr<u8> {
    if method.is_null() {
        return Ptr::null();
    }
    let RuntimeMethod { owner, selector } = env.mem.read(method);
    // Preserve the original guest encoding, including its offsets and names.
    // Host declarations currently carry Rust ABI types, not ObjC encodings.
    env.objc
        .borrow::<ClassHostObject>(owner)
        .guest_method_signatures
        .get(&selector)
        .copied()
        .unwrap_or(Ptr::null())
}

pub(super) fn method_setImplementation(env: &mut Environment, method: Method, imp: IMP) -> IMP {
    if method.is_null() {
        return IMP::guest_null();
    }
    let previous = method_getImplementation(env, method);
    let RuntimeMethod { owner, selector } = env.mem.read(method);
    env.objc
        .borrow_mut::<ClassHostObject>(owner)
        .methods
        .insert(selector, imp);
    previous
}

pub(super) fn method_exchangeImplementations(env: &mut Environment, first: Method, second: Method) {
    if first.is_null() || second.is_null() || first == second {
        return;
    }
    let RuntimeMethod {
        owner: a_owner,
        selector: a_selector,
    } = env.mem.read(first);
    let RuntimeMethod {
        owner: b_owner,
        selector: b_selector,
    } = env.mem.read(second);
    let first_imp = env.objc.borrow::<ClassHostObject>(a_owner).methods[&a_selector].clone();
    let second_imp = env.objc.borrow::<ClassHostObject>(b_owner).methods[&b_selector].clone();
    // Preserve host implementations as host values for typed message dispatch.
    env.objc
        .borrow_mut::<ClassHostObject>(a_owner)
        .methods
        .insert(a_selector, second_imp);
    env.objc
        .borrow_mut::<ClassHostObject>(b_owner)
        .methods
        .insert(b_selector, first_imp);
}

pub(super) fn class_addMethod(
    env: &mut Environment,
    class: Class,
    selector: SEL,
    imp: IMP,
    types: ConstPtr<u8>,
) -> bool {
    if class == nil || selector.is_null() || env.objc.class_has_uninherited_method(class, selector)
    {
        return false;
    }
    let types = if types.is_null() {
        Ptr::null()
    } else {
        crate::libc::string::strdup(env, types).cast_const()
    };
    let owner = env.objc.borrow_mut::<ClassHostObject>(class);
    owner.methods.insert(selector, imp);
    if !types.is_null() {
        owner.guest_method_signatures.insert(selector, types);
    }
    true
}

impl ClassHostObject {
    // See classes.rs for host method parsing

    pub(super) fn add_methods_from_bin(
        &mut self,
        method_list_ptr: ConstPtr<method_list_t>,
        mem: &Mem,
        objc: &mut ObjC,
    ) {
        let method_list_t { entsize, count } = mem.read(method_list_ptr);
        assert!(entsize >= guest_size_of::<method_t>());

        let methods_base_ptr: ConstPtr<method_t> = (method_list_ptr + 1).cast();

        for i in 0..count {
            let method_ptr: ConstPtr<method_t> =
                Ptr::from_bits(methods_base_ptr.to_bits() + i * entsize);

            let method_t { name, types, imp } = mem.read(method_ptr);

            // There is no guarantee this string is unique or known.
            // We must deduplicate it like any other.
            let sel = objc.register_bin_selector(name, mem);
            self.methods.insert(sel, IMP::Guest(imp));
            // TODO: avoid storing duplicated signatures globally
            self.guest_method_signatures.insert(sel, types);
        }
    }
}

impl ObjC {
    /// Checks if the provided class has a method in its class chain (that is
    /// to say, objects of the given class respond to a selector).
    pub fn class_has_method(&self, class: Class, sel: SEL) -> bool {
        let mut class = class;
        loop {
            let &ClassHostObject {
                superclass,
                ref methods,
                ..
            } = self.borrow(class);
            if methods.contains_key(&sel) {
                return true;
            } else if superclass == nil {
                return false;
            } else {
                class = superclass;
            }
        }
    }

    /// Variant of `class_has_method` which doesn't account for inheritance.
    pub fn class_has_uninherited_method(&self, class: Class, sel: SEL) -> bool {
        let ClassHostObject { methods, .. } = self.borrow(class);
        methods.contains_key(&sel)
    }

    pub fn class_get_method_signature(&self, class: Class, sel: SEL) -> Option<&ConstPtr<u8>> {
        // TODO: support `host` method signatures
        let mut class = class;
        loop {
            let &ClassHostObject {
                superclass,
                ref methods,
                ref guest_method_signatures,
                ..
            } = self.borrow(class);
            if methods.contains_key(&sel) {
                return guest_method_signatures.get(&sel);
            } else if superclass == nil {
                return None;
            } else {
                class = superclass;
            }
        }
    }

    /// Rust signature of a host method, for typed Foundation accessor dispatch.
    pub(crate) fn class_get_host_method_signature(
        &self,
        mut class: Class,
        sel: SEL,
    ) -> Option<TypeId> {
        while class != nil {
            let owner = self.borrow::<ClassHostObject>(class);
            if let Some(method) = owner.methods.get(&sel) {
                return match method {
                    IMP::Host(host) => Some(host.type_info().0),
                    IMP::Guest(_) => None,
                };
            }
            class = owner.superclass;
        }
        None
    }

    /// Same as [Self::class_has_method], but using a named selector (rather
    /// than a pointer).
    pub fn class_has_method_named(&self, class: Class, sel_name: &str) -> bool {
        if let Some(sel) = self.lookup_selector(sel_name) {
            self.class_has_method(class, sel)
        } else {
            false
        }
    }

    /// Checks if a given object has a method (responds to a selector).
    pub fn object_has_method(&self, mem: &Mem, obj: id, sel: SEL) -> bool {
        self.class_has_method(ObjC::read_isa(obj, mem), sel)
    }

    /// Variant of `object_has_method` which doesn't account for inheritance.
    pub fn object_has_uninherited_method(&self, mem: &Mem, obj: id, sel: SEL) -> bool {
        self.class_has_uninherited_method(ObjC::read_isa(obj, mem), sel)
    }

    #[allow(dead_code)]
    pub fn object_get_method_signature(
        &self,
        mem: &Mem,
        obj: id,
        sel: SEL,
    ) -> Option<&ConstPtr<u8>> {
        self.class_get_method_signature(ObjC::read_isa(obj, mem), sel)
    }

    /// Same as [Self::object_has_method], but using a named selector (rather
    /// than a pointer).
    pub fn object_has_method_named(&self, mem: &Mem, obj: id, sel_name: &str) -> bool {
        if let Some(sel) = self.lookup_selector(sel_name) {
            self.object_has_method(mem, obj, sel)
        } else {
            false
        }
    }

    /// Checks if a class overrides a method provided by its superclass.
    ///
    /// This looks through a superclass chain looking for the selector, stopping
    /// when the superclass is hit (and panicking if it never is). It does not
    /// check whether the selector is actually a method on the superclass.
    pub fn class_overrides_method_of_superclass(
        &self,
        class: Class,
        sel: SEL,
        superclass: Class,
    ) -> bool {
        let mut class = class;
        loop {
            if class == superclass {
                return false;
            }

            let &ClassHostObject {
                superclass,
                ref methods,
                ..
            } = self.borrow(class);
            if methods.contains_key(&sel) {
                return true;
            } else if superclass == nil {
                panic!();
            } else {
                class = superclass;
            }
        }
    }

    pub fn debug_all_class_selectors_as_strings(&self, mem: &Mem, class: Class) -> Vec<String> {
        let mut class = class;
        let mut selector_strings = Vec::new();
        loop {
            let &ClassHostObject {
                superclass,
                ref methods,
                ..
            } = self.borrow(class);
            let mut class_selector_strings = methods
                .keys()
                .map(|sel| sel.as_str(mem).to_string())
                .collect();
            selector_strings.append(&mut class_selector_strings);
            if superclass == nil {
                break;
            } else {
                class = superclass;
            }
        }
        selector_strings
    }
}
