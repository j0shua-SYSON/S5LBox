/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! S5LBox adaptation: ARMv6 execution using the portable C interpreter.
//! No generated host code, executable allocations, or Dynarmic dependency.

use crate::abi::GuestFunction;
use crate::mem::{GuestUSize, Mem};
use s5lbox_touchhle_cpu::{Cpu as Interpreter, DirectMemory, Memory, Stop};

pub type CpuContext = s5lbox_touchhle_cpu::Context;

pub struct Cpu {
    interpreter: Interpreter,
}

#[cfg(test)]
mod interpreter_tests {
    use super::*;
    use crate::mem::MutPtr;

    #[test]
    fn arm_thumb_svc_memory_context_and_repatched_breakpoint() {
        let mut mem = Mem::new();
        mem.set_null_segment_size(0x1000);
        let code = 0x1000;
        mem.reserve(code, 0x1000);
        // MOV r0,#41; ADD r0,r0,#1; SVC #66.
        for (i, op) in [0xe3a00029u32, 0xe2800001, 0xef000042]
            .into_iter()
            .enumerate()
        {
            mem.write(MutPtr::from_bits(code + (i as u32) * 4), op);
        }
        let mut cpu = Cpu::new(Some(&mut mem));
        cpu.branch(GuestFunction::from_addr_with_thumb_bit(code));
        let mut budget = 7;
        assert!(matches!(
            cpu.run_or_step(&mut mem, Some(&mut budget)),
            CpuState::Svc(66)
        ));
        assert_eq!(cpu.regs()[0], 42);
        assert_eq!(cpu.regs()[Cpu::PC], code + 12);
        assert_eq!(budget, 4);
        let mut context = CpuContext::new();
        cpu.swap_context(&mut context);
        assert_eq!(context.regs[0], 42);
        cpu.swap_context(&mut context);
        assert_eq!(cpu.regs()[0], 42);

        mem.write(MutPtr::from_bits(code), 0xbe00u16); // Thumb BKPT.
        cpu.branch(GuestFunction::from_addr_and_thumb_flag(code, true));
        assert!(matches!(
            cpu.run_or_step(&mut mem, None),
            CpuState::Error(CpuError::Breakpoint)
        ));
        assert_eq!(cpu.regs()[Cpu::PC], code);
        mem.write(MutPtr::from_bits(code), 0xdf7fu16); // Replace with SVC.
        cpu.invalidate_cache_range(code, 2);
        assert!(matches!(
            cpu.run_or_step(&mut mem, None),
            CpuState::Svc(127)
        ));
        assert_eq!(cpu.regs()[Cpu::PC], code + 2);

        cpu.branch(GuestFunction::from_addr_with_thumb_bit(0));
        assert!(matches!(
            cpu.run_or_step(&mut mem, None),
            CpuState::Error(CpuError::MemoryError)
        ));
        assert_eq!(cpu.regs()[Cpu::PC], 0);

        mem.write(MutPtr::from_bits(code), 0xe7f000f0u32); // ARM UDF.
        cpu.branch(GuestFunction::from_addr_with_thumb_bit(code));
        assert!(matches!(
            cpu.run_or_step(&mut mem, None),
            CpuState::Error(CpuError::UndefinedInstruction)
        ));
        assert_eq!(cpu.regs()[Cpu::PC], code);

        // An explicit host branch resumes execution after a stopped fault.
        mem.write(MutPtr::from_bits(code + 4), 0xef000003u32);
        cpu.branch(GuestFunction::from_addr_with_thumb_bit(code + 4));
        assert!(matches!(cpu.run_or_step(&mut mem, None), CpuState::Svc(3)));
    }
}

#[derive(Debug)]
pub enum CpuState {
    Normal,
    Svc(u32),
    Error(CpuError),
}

#[derive(Debug, Clone, PartialEq)]
pub enum CpuError {
    MemoryError,
    UndefinedInstruction,
    Breakpoint,
}

struct GuestMemory<'a>(&'a mut Mem);
impl Memory for GuestMemory<'_> {
    fn read(&mut self, address: u32, bytes: &mut [u8]) -> bool {
        self.0.interpreter_read(address, bytes)
    }

    fn write(&mut self, address: u32, bytes: &[u8]) -> bool {
        self.0.interpreter_write(address, bytes)
    }
}
// The borrow excludes framework calls and VM changes until run_mapped returns.
// Backing::pointer returns allocation-owned pointers, not temporary slice borrows.
unsafe impl DirectMemory for GuestMemory<'_> {
    fn map(&mut self, address: u32, length: usize, _writable: bool) -> *mut u8 {
        self.0.interpreter_mapping(address, length)
    }
}

impl Cpu {
    pub const SP: usize = 13;
    pub const LR: usize = 14;
    pub const PC: usize = 15;
    pub const CPSR_THUMB: u32 = 0x20;
    pub const CPSR_USER_MODE: u32 = 0x10;

    /// Memory is borrowed only during execution, never retained by the CPU.
    pub fn new(_direct_memory_access: Option<&mut Mem>) -> Self {
        Self {
            interpreter: Interpreter::new().expect("could not allocate ARM interpreter"),
        }
    }

    pub fn regs(&self) -> &[u32; 16] {
        self.interpreter.regs()
    }
    pub fn regs_mut(&mut self) -> &mut [u32; 16] {
        self.interpreter.regs_mut()
    }
    pub fn cpsr(&self) -> u32 {
        self.interpreter.cpsr()
    }
    pub fn set_cpsr(&mut self, value: u32) {
        self.interpreter.set_cpsr(value)
    }
    pub fn swap_context(&mut self, context: &mut CpuContext) {
        self.interpreter.swap_context(context)
    }

    pub fn dump_regs(&self) {
        Self::echo_regs(self.regs())
    }
    pub fn echo_regs(regs: &[u32; 16]) {
        let _ = std::panic::catch_unwind(|| {
            for (row, values) in regs.chunks_exact(4).enumerate() {
                echo!(
                    "R{}..R{}: {:08x} {:08x} {:08x} {:08x}",
                    row * 4,
                    row * 4 + 3,
                    values[0],
                    values[1],
                    values[2],
                    values[3]
                );
            }
        });
    }

    pub fn pc_with_thumb_bit(&self) -> GuestFunction {
        GuestFunction::from_addr_and_thumb_flag(
            self.regs()[Self::PC],
            self.cpsr() & Self::CPSR_THUMB != 0,
        )
    }

    pub fn branch(&mut self, new_pc: GuestFunction) {
        let mut context = self.interpreter.context();
        context.regs[Self::PC] = new_pc.addr_without_thumb_bit();
        context.cpsr = (context.cpsr & !Self::CPSR_THUMB)
            | if new_pc.is_thumb() {
                Self::CPSR_THUMB
            } else {
                0
            };
        self.interpreter.set_context(&context);
    }

    pub fn branch_with_link(
        &mut self,
        new_pc: GuestFunction,
        new_lr: GuestFunction,
    ) -> (GuestFunction, GuestFunction) {
        let old_pc = self.pc_with_thumb_bit();
        let old_lr = GuestFunction::from_addr_with_thumb_bit(self.regs()[Self::LR]);
        self.branch(new_pc);
        self.regs_mut()[Self::LR] = new_lr.addr_with_thumb_bit();
        (old_pc, old_lr)
    }

    pub fn invalidate_cache_range(&mut self, _base: u32, _size: GuestUSize) {
        // Instructions are fetched afresh. Clear a debugger fault latch after
        // replacing a breakpoint, without changing the architectural state.
        let context = self.interpreter.context();
        self.interpreter.set_context(&context);
    }

    #[must_use]
    pub fn run_or_step(&mut self, mem: &mut Mem, ticks: Option<&mut u64>) -> CpuState {
        match self.interpreter.run_mapped(&mut GuestMemory(mem), ticks) {
            Stop::Budget => CpuState::Normal,
            Stop::Svc(number) => CpuState::Svc(number),
            Stop::MemoryFault => CpuState::Error(CpuError::MemoryError),
            Stop::Undefined => CpuState::Error(CpuError::UndefinedInstruction),
            Stop::Breakpoint => CpuState::Error(CpuError::Breakpoint),
        }
    }
}
