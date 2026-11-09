// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//! Safe, single-owner Rust interface to S5LBox's non-JIT ARMv6 interpreter.
use std::{ffi::c_void, marker::PhantomData, ptr::NonNull, rc::Rc};

#[repr(C)]
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Context {
    pub regs: [u32; 16],
    pub vfp: [u32; 32],
    pub cpsr: u32,
    pub fpscr: u32,
    pub tpidrurw: u32,
    pub tpidruro: u32,
}
impl Context {
    pub fn new() -> Self { Self::default() }
}
pub trait Memory {
    /// Return false without changing output/memory if the range is inaccessible.
    fn read(&mut self, address: u32, bytes: &mut [u8]) -> bool;
    fn write(&mut self, address: u32, bytes: &[u8]) -> bool;
}
/// Optional stable plain-RAM mappings for the interpreter's existing caches.
///
/// # Safety
/// Non-null mappings must cover the entire requested range, alias `read` and
/// `write`, and remain valid throughout a `run_mapped` call. Other callbacks
/// cannot move, free, revoke or borrow the mapped memory. A writable mapping
/// permits stores without invoking `write`; read-only mappings forbid that.
/// Returning null always falls back to the ordinary callbacks.
pub unsafe trait DirectMemory: Memory {
    fn map(&mut self, address: u32, length: usize, writable: bool) -> *mut u8;
}
#[derive(Debug, PartialEq, Eq)]
pub enum Stop { Budget, Svc(u32), MemoryFault, Undefined, Breakpoint }
#[repr(C)]
struct MemoryCallbacks {
    context: *mut c_void,
    read: unsafe extern "C" fn(*mut c_void, u32, *mut c_void, usize) -> bool,
    write: unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize) -> bool,
    map: Option<unsafe extern "C" fn(*mut c_void, u32, usize, bool) -> *mut u8>,
}
extern "C" {
    fn arm_user_create() -> *mut c_void;
    fn arm_user_destroy(cpu: *mut c_void);
    fn arm_user_regs(cpu: *mut c_void) -> *mut u32;
    fn arm_user_cpsr(cpu: *const c_void) -> u32;
    fn arm_user_set_cpsr(cpu: *mut c_void, value: u32);
    fn arm_user_get_context(cpu: *const c_void, context: *mut Context);
    fn arm_user_set_context(cpu: *mut c_void, context: *const Context);
    fn arm_user_swap_context(cpu: *mut c_void, context: *mut Context);
    fn arm_user_run(cpu: *mut c_void, memory: *const MemoryCallbacks,
                    ticks: *mut u64, svc: *mut u32) -> i32;
}
pub struct Cpu {
    raw: NonNull<c_void>,
    // The C object, guest memory and framework state are confined to one worker.
    _single_thread: PhantomData<Rc<()>>,
}
impl Drop for Cpu {
    fn drop(&mut self) { unsafe { arm_user_destroy(self.raw.as_ptr()) } }
}
impl Cpu {
    pub fn new() -> Option<Self> {
        NonNull::new(unsafe { arm_user_create() })
            .map(|raw| Self { raw, _single_thread: PhantomData })
    }
    pub fn regs(&self) -> &[u32; 16] {
        unsafe { &*arm_user_regs(self.raw.as_ptr()).cast::<[u32; 16]>() }
    }
    pub fn regs_mut(&mut self) -> &mut [u32; 16] {
        unsafe { &mut *arm_user_regs(self.raw.as_ptr()).cast::<[u32; 16]>() }
    }
    pub fn cpsr(&self) -> u32 { unsafe { arm_user_cpsr(self.raw.as_ptr()) } }
    pub fn set_cpsr(&mut self, value: u32) {
        unsafe { arm_user_set_cpsr(self.raw.as_ptr(), value) }
    }
    pub fn context(&self) -> Context {
        let mut value = Context::default();
        unsafe { arm_user_get_context(self.raw.as_ptr(), &mut value) };
        value
    }
    pub fn set_context(&mut self, context: &Context) {
        unsafe { arm_user_set_context(self.raw.as_ptr(), context) }
    }
    pub fn swap_context(&mut self, context: &mut Context) {
        unsafe { arm_user_swap_context(self.raw.as_ptr(), context) }
    }
    pub fn run<M: Memory>(&mut self, memory: &mut M, ticks: Option<&mut u64>) -> Stop {
        self.run_inner(memory, ticks, None)
    }
    pub fn run_mapped<M: DirectMemory>(&mut self, memory: &mut M, ticks: Option<&mut u64>) -> Stop {
        unsafe extern "C" fn map<M: DirectMemory>(ctx: *mut c_void, address: u32,
            len: usize, writable: bool) -> *mut u8 {
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                unsafe { &mut *ctx.cast::<M>() }.map(address, len, writable)
            })).unwrap_or(std::ptr::null_mut())
        }
        self.run_inner(memory, ticks, Some(map::<M>))
    }
    fn run_inner<M: Memory>(&mut self, memory: &mut M, ticks: Option<&mut u64>,
        map: Option<unsafe extern "C" fn(*mut c_void, u32, usize, bool) -> *mut u8>) -> Stop {
        unsafe extern "C" fn read<M: Memory>(ctx: *mut c_void, address: u32,
            out: *mut c_void, len: usize) -> bool {
            // Never unwind a Rust memory-provider panic through the C interpreter.
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let memory = unsafe { &mut *ctx.cast::<M>() };
                let bytes = unsafe { std::slice::from_raw_parts_mut(out.cast::<u8>(), len) };
                memory.read(address, bytes)
            })).unwrap_or(false)
        }
        unsafe extern "C" fn write<M: Memory>(ctx: *mut c_void, address: u32,
            input: *const c_void, len: usize) -> bool {
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let memory = unsafe { &mut *ctx.cast::<M>() };
                let bytes = unsafe { std::slice::from_raw_parts(input.cast::<u8>(), len) };
                memory.write(address, bytes)
            })).unwrap_or(false)
        }
        let callbacks = MemoryCallbacks { context: (memory as *mut M).cast(),
            read: read::<M>, write: write::<M>, map };
        let mut svc = 0;
        let result = unsafe { arm_user_run(self.raw.as_ptr(), &callbacks,
            ticks.map_or(std::ptr::null_mut(), |t| t), &mut svc) };
        match result {
            0 => Stop::Budget, 1 => Stop::Svc(svc), 2 => Stop::MemoryFault,
            3 => Stop::Undefined, 4 => Stop::Breakpoint,
            _ => unreachable!("invalid interpreter result {result}"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Ram(Vec<u8>);
    impl Memory for Ram {
        fn read(&mut self, address: u32, bytes: &mut [u8]) -> bool {
            let Some(src) = self.0.get(address as usize..)
                .and_then(|s| s.get(..bytes.len())) else { return false };
            bytes.copy_from_slice(src); true
        }
        fn write(&mut self, address: u32, bytes: &[u8]) -> bool {
            let Some(dst) = self.0.get_mut(address as usize..)
                .and_then(|s| s.get_mut(..bytes.len())) else { return false };
            dst.copy_from_slice(bytes); true
        }
    }
    // Test RAM never reallocates or changes permissions during a CPU call.
    unsafe impl DirectMemory for Ram {
        fn map(&mut self, address: u32, length: usize, _: bool) -> *mut u8 {
            if (address as u64) + (length as u64) > self.0.len() as u64 {
                return std::ptr::null_mut();
            }
            unsafe { self.0.as_mut_ptr().add(address as usize) }
        }
    }
    #[test]
    fn mapped_execution_releases_borrow_between_runs() {
        let mut ram = Ram(vec![0; 2048]);
        ram.0[..12].copy_from_slice(&[0xe5810000u32, 0xe5912000, 0xef000042]
            .into_iter().flat_map(u32::to_le_bytes).collect::<Vec<_>>());
        let mut cpu = Cpu::new().unwrap();
        cpu.regs_mut()[0] = 123; cpu.regs_mut()[1] = 1024;
        let mut ticks = 3;
        assert_eq!(cpu.run_mapped(&mut ram, Some(&mut ticks)), Stop::Svc(66));
        assert_eq!(cpu.regs()[2], 123);
        // Replace all backing memory while retaining the CPU and guest address.
        let mut replacement = Ram(vec![0; 2048]);
        replacement.0[..8].copy_from_slice(&[0xe5912000u32, 0xef000043]
            .into_iter().flat_map(u32::to_le_bytes).collect::<Vec<_>>());
        replacement.0[1024..1028].copy_from_slice(&456u32.to_le_bytes());
        drop(ram);
        cpu.regs_mut()[15] = 0; ticks = 2;
        assert_eq!(cpu.run_mapped(&mut replacement, Some(&mut ticks)), Stop::Svc(67));
        assert_eq!(cpu.regs()[2], 456);
    }
    #[test]
    fn executes_and_switches_without_jit() {
        let mut ram = Ram([0xe3a00029u32, 0xe2800001, 0xef000042]
            .into_iter().flat_map(u32::to_le_bytes).collect());
        let mut cpu = Cpu::new().unwrap(); let mut ticks = 5;
        assert_eq!(cpu.run(&mut ram, Some(&mut ticks)), Stop::Svc(66));
        assert_eq!(cpu.regs()[0], 42); assert_eq!(ticks, 2);
        let saved = cpu.context(); let mut empty = Context::new();
        cpu.swap_context(&mut empty); assert_eq!(empty, saved);
        assert_eq!(cpu.regs()[0], 0);
        cpu.swap_context(&mut empty); assert_eq!(cpu.context(), saved);
        assert_eq!(cpu.run(&mut ram, None), Stop::MemoryFault);
    }
    #[test]
    fn catches_provider_panics_at_ffi_boundary() {
        struct Broken;
        impl Memory for Broken {
            fn read(&mut self, _: u32, _: &mut [u8]) -> bool { panic!("bad memory") }
            fn write(&mut self, _: u32, _: &[u8]) -> bool { false }
        }
        assert_eq!(Cpu::new().unwrap().run(&mut Broken, None), Stop::MemoryFault);
    }
}
