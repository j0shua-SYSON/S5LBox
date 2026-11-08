/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `dlfcn.h` (`dlopen()` and friends)

use crate::dyld::{export_c_func, FunctionExports};
use crate::mem::{ConstPtr, MutPtr, MutVoidPtr, Ptr};
use crate::{Environment, ThreadId};
use std::collections::HashMap;

#[derive(Default)]
pub struct State {
    errors: HashMap<ThreadId, (MutPtr<u8>, bool)>,
    handles: HashMap<MutVoidPtr, (&'static str, u32)>,
}

fn fail(env: &mut Environment, message: &str) -> MutVoidPtr {
    let current = env.current_thread;
    if let Some((old, _)) = env.libc_state.dlfcn.errors.remove(&current) {
        env.mem.free(old.cast());
    }
    let message = env.mem.alloc_and_write_cstr(message.as_bytes());
    env.libc_state.dlfcn.errors.insert(current, (message, true));
    Ptr::null()
}

fn dlerror(env: &mut Environment) -> ConstPtr<u8> {
    match env.libc_state.dlfcn.errors.get_mut(&env.current_thread) {
        Some((message, pending)) if *pending => {
            *pending = false;
            message.cast_const()
        }
        _ => Ptr::null(),
    }
}

const RTLD_DEFAULT: MutVoidPtr = Ptr::from_bits(-2 as _);

fn known_library(path: &str) -> Option<&'static str> {
    crate::dyld::DYLIB_LIST
        .iter()
        .find(|dylib| dylib.path == path || dylib.aliases.contains(&path))
        .map(|dylib| dylib.path)
}

fn dlopen(env: &mut Environment, path: ConstPtr<u8>, _mode: i32) -> MutVoidPtr {
    if path.is_null() {
        return RTLD_DEFAULT;
    }
    // TODO: dlopen() support for real dynamic libraries.
    let name = env.mem.cstr_at_utf8(path).unwrap();
    let Some(name) = known_library(name) else {
        let message = format!("dlopen: library is not loaded: {name}");
        return fail(env, &message);
    };
    for (handle, (existing, references)) in &mut env.libc_state.dlfcn.handles {
        if *existing == name {
            *references += 1;
            return *handle;
        }
    }
    let handle = env.mem.alloc(4).cast();
    env.libc_state.dlfcn.handles.insert(handle, (name, 1));
    handle
}

fn dlsym(env: &mut Environment, handle: MutVoidPtr, symbol: ConstPtr<u8>) -> MutVoidPtr {
    if handle.is_null() || symbol.is_null() {
        return fail(env, "dlsym: invalid handle or symbol");
    }
    if handle != RTLD_DEFAULT && !env.libc_state.dlfcn.handles.contains_key(&handle) {
        return fail(env, "dlsym: invalid handle");
    }
    // For some reason, the symbols passed to dlsym() don't have the leading _.
    let symbol = format!("_{}", env.mem.cstr_at_utf8(symbol).unwrap());
    if handle == RTLD_DEFAULT {
        for binary in env.bins.iter() {
            if let Some(&address) = binary.dynamic_symbols.get(&symbol) {
                return Ptr::from_bits(address);
            }
        }
    }
    // TODO: Symbol lookup should be scoped to the specific library requested,
    // where appropriate!
    let addr = env
        .dyld
        .create_proc_address(&mut env.mem, &mut env.cpu, &symbol);
    match addr {
        Ok(addr) => Ptr::from_bits(addr.addr_with_thumb_bit()),
        Err(()) => fail(env, &format!("dlsym: symbol not found: {symbol}")),
    }
}

fn dlclose(env: &mut Environment, handle: MutVoidPtr) -> i32 {
    if handle == RTLD_DEFAULT {
        return 0;
    }
    let Some((_, references)) = env.libc_state.dlfcn.handles.get_mut(&handle) else {
        fail(env, "dlclose: invalid handle");
        return -1;
    };
    *references -= 1;
    if *references == 0 {
        env.libc_state.dlfcn.handles.remove(&handle);
        env.mem.free(handle);
    }
    0 // success
}

pub const FUNCTIONS: FunctionExports = &[
    export_c_func!(dlopen(_, _)),
    export_c_func!(dlsym(_, _)),
    export_c_func!(dlclose(_)),
    export_c_func!(dlerror()),
];
