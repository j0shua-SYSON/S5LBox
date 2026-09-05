# iPhone 3GS / iOS 6 bring-up

The initial iOS 6 target is iPhone 3GS running 6.1.6. The current change adds
an instruction-profile boundary for its Cortex-A8. A complete S5L8920 machine,
kernel boot, and SpringBoard have not been demonstrated. The existing iPhone
OS 3 machine and application defaults remain ARM1176/S5L8900.

## Target evidence

[Apple's iOS 6.1.6 bulletin](https://support.apple.com/en-us/103607) confirms
the device/version pairing. `BuildManifest.plist` and `Restore.plist` inside
the [Apple restore archive](https://secure-appldnld.apple.com/iOS6.1/091-3457.20140221.Btt3e/iPhone2,1_6.1.6_10B500_Restore.ipsw)
were retrieved with validated HTTP 206 ranges and ZIP member CRC checks.
They identify:

| Property | Manifest value |
| --- | --- |
| Product | `iPhone2,1` |
| Version / build | `6.1.6` / `10B500` |
| Board / board ID | `n88ap` / `0x00` |
| Platform / chip ID | `s5l8920x` / `0x8920` |
| Kernel | `kernelcache.release.n88` |
| Device tree | `Firmware/all_flash/all_flash.n88ap.production/DeviceTree.n88ap.img3` |
| System image | `048-2955-001.dmg` |

The archive is 822,970,962 bytes; it has not been downloaded in full or
independently hashed. SHA-256 of the retrieved manifest is
`87175db6ed8043e215add63f16e774d63f4d2792b2bbb852cd1d966156b7cf00`.
The decrypted device tree has 89 nodes and parses to its exact 57,856-byte
length. Its CPU compatibility is `ARM,cortex-a8` / `ARM,v7`; the CPU revision,
memory size, and clock properties are zero placeholders filled during boot.
They must not be treated as measured hardware values. The peripheral mapping
also differs from S5L8900: its UART0 maps to `0x82500000` and the PL192 VIC
window to `0xbf200000`, using the tree's parent ranges.

The matching `iBoot.n88ap.RELEASE.img3` supplies the missing RAM geometry.
In its decrypted `iBoot-1537.9.55`, code at `0x4ff10b56` constructs physical
base `0x40000000` and stores it into boot arguments at `0x4ff10b5e`.
The size routine at `0x4ff13928` returns `0x10000000` (256 MiB); the caller
initially reserves 16 KiB before further boot allocations. Vector literals
and the relocation code establish iBoot's own base as `0x4ff00000`.
The decrypted iBoot SHA-256 is
`ef527ad3d131cc220c73f9e1dd3ae06acfd4463f3cdb60ac717aadf8f09e031c`.

The target kernel checks boot-argument version **5** at `0x8027acec`.
iBoot constructs revision 1 and copies a `0x138`-byte structure. Its N88
display timing table at `0x4ff2b244` specifies 320 × 480; the RGB888 surface
mode uses depth 32 and stride 1280. The allocator reserves three page-rounded
buffers below RAM top minus 16 KiB, placing the first at `0x4fe3a000` and
reducing the boot-argument memory size accordingly. This establishes a
firmware configuration, not a working display model. The device-tree pointer
at argument offset `0x30` is virtual; its byte length is at `0x34`.

## CPU boundary

`arm_arch_t` values are identifiers, not ordered architecture levels.
ARM1176 remains zero and Swift remains one; Cortex-A8 has its own identifier.
Explicit predicates allow A32 MOVW/MOVT on Swift and Cortex-A8, while A32
SDIV/UDIV require Swift. Unknown profiles cannot execute or reset CPU state.
Arm documents Cortex-A8 as having no integer division in either instruction
set in [DDI0344K, section 3.2.15](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde)
and its [division support table](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/divide-and-conquer).

A32 DMB, DSB and ISB also have explicit ARMv7 gates. Previously they fell
through a broad PLD hint decoder, including on ARM1176. The interpreter
completes bus accesses and CP15 changes synchronously and reads instruction
bytes on every step, satisfying the full-system barrier requirements.
[DDI0406C.b, A8.8.43/44/53](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92)
also requires reserved options to execute as full-system barriers. Tests
cover all options in User and SVC modes, ARM1176/unknown-profile refusals,
exclusive-monitor preservation and a store followed by instruction refetch.

Thumb-2 framing fetches both halfwords and retires once. The second halfword
is translated independently, including across noncontiguous physical pages.
A fetch fault there vectors before any instruction result is committed.
MOVW and MOVT are implemented with the split immediate and ARMv7 register
restrictions from DDI0406C.b A8.8.102/106. Other wide operations stop as
unsupported, rather than being misread as legacy BL halves. ARM1176 retains
its existing two-step BL/BLX behavior.

Wide B/BL/BLX implement their signed offsets and distinct J-bit rules,
including conditional B and ARM/Thumb interworking. BL/BLX update LR only
after the complete instruction fetch. Tests cover displacement limits,
all conditions, halfword-aligned BLX, an ARM callee returning to Thumb,
and a call straddling an unmapped or denied page.
The narrow CBZ/CBNZ extension is gated to Cortex-A8/Swift and refuses active
IT state. It tests the register directly, preserves flags, and uses its
unsigned forward displacement. ARM1176 continues to reject it.

IT executes one to four conditional instructions using the split CPSR state.
Narrow implicit flag updates are suppressed inside a block; CMP/CMN/TST and
explicit wide flag updates retain their normal effects. Skipped instructions
still fetch their full width and retire once, without data accesses. Invalid
IT encodings, nested blocks, forbidden instructions and early PC writes are
refused. Condition-failed unsupported encodings consistently act as NOPs under
the permitted ARMv7-A policy; BKPT remains unconditional and unsupported.

IT state is saved in SPSR and cleared on entry to the existing ARM-state
exception path. Faults and interrupts preserve retry state; SVC saves the
advanced state for the following instruction. Tests cover exception returns,
failed second-half fetches, SVC hook rollback/retirement, and signed-static
fallback with mixed instruction widths. This does not complete the separate
Cortex-A8 CP15/exception-control audit, including SCTLR.TE.

Wide STRB/STRH with unsigned imm12 offsets use the common byte/halfword memory
paths. They accept SP as a base, reject SP/PC sources and PC bases, and leave
flags and base registers unchanged. Tests cover adjacent-byte preservation,
unaligned accesses, page crossings, translation/permission faults and abort
state with host RAM access enabled and disabled. On a second-page store fault,
the completed first byte remains visible.

Wide LDM/STM implement increment-after and decrement-before addressing,
including PUSH/POP aliases. Thumb register-list restrictions are checked
before using the common multiple-transfer path. Tests check register order,
writeback, loaded-PC interworking and transactional register restoration
on a data abort; previously completed stores remain visible.

Indexed wide LDR/STR and STRB/STRH implement signed imm8 offsets, pre/post
indexing and single-register PUSH/POP. Writeback waits for a successful
access; base/register overlap, invalid source registers and the separate
unprivileged encodings are refused before accessing data. PC loads use the
same alignment and interworking checks as unsigned-offset loads. Tests cover
all P/U/W combinations, offset limits, stack aliases, invalid loaded targets
and preserved base/result state after first/second-page faults.

Modified-immediate AND/BIC/ORR/ORN/EOR implement logical results and optional
NZC updates with V/Q preserved. MOV/MVN use the PC-base alias encodings;
TST/TEQ use the flag-only destination encodings. Tests exercise replicated
and rotated constants, carry preservation/replacement, aliases inside IT,
and rejected registers and reserved immediate forms.

Thumb LDRD/STRD immediate and LDRD literal use explicit independent data
registers and scaled offsets. They share the existing A32 ordered word
transfer path after validating the stricter Thumb register constraints.
Loads and base writeback commit only after both words succeed; a completed
first store remains visible if the second word faults. Tests cover offset,
pre/post indexing, literal alignment, nonadjacent registers, legal duplicate
store sources, two separately translated pages, alignment and permission
faults, and exception IT state. The shared data path is little-endian;
these new forms explicitly refuse CPSR.E rather than execute incorrect
big-endian accesses. General big-endian data support remains unimplemented.

Wide LDRB/LDRH/LDRSB/LDRSH implement immediate, literal and pre/post-indexed
forms, with byte offsets and the required zero/sign extension. Result and
writeback commit only after a successful access. Tests cover addressing
limits, SP bases, literal alignment, page crossings, permission/alignment
faults and saved IT state. Halfword forms refuse the unsupported CPSR.E
mode; byte loads are endian-independent. PC-destination aliases encode
PLD/PLDW/PLI or unallocated hints and perform no data access, including when
the hinted address is unmapped. Register-offset and unprivileged forms
remain unsupported.

Wide signed/unsigned extend and extend-and-add instructions support bytes,
halfwords and independent paired-byte lanes, with rotations of 0/8/16/24
bits. They reuse the A32 arithmetic after enforcing Thumb's SP/PC constraints.
Tests cover sign boundaries, wrapped additions, lane isolation, overlapping
operands, IT conditions and preservation of NZCV/Q/GE. ARM1176 keeps its
legacy 16-bit framing for these first-halfword bit patterns.

Shifted-register logical and arithmetic operations share the existing Thumb
ALU with modified immediates. Their split shift amounts implement LSL, LSR,
ASR, ROR and RRX, including the MOV/MVN and flag-only aliases. Logical flags
use the shifter carry; ADC/SBC retain the original carry input for arithmetic.
Non-flag-setting plain MOV permits SP in exactly one operand. ADD/SUB updates
to SP require an SP base and LSL of 0..3. Tests cover those constraints,
shift boundaries, sign/carry behavior, aliases, IT and ARM1176 framing.

Thumb UBFX/SBFX and BFI/BFC implement bitfield extraction, sign extension,
insertion and clearing. They validate ranges before shifting and preserve
flags and unrelated destination bits. Tests compare every encoded field
range against a bit-by-bit reference, including full-width fields, register
overlaps, invalid SP/PC operands and reserved encoding bits. A32 bitfield
instructions remain a separate unimplemented family.

The wide MOV/MOVS immediate form implements Thumb's byte replication and
rotation rules from A6.3.2. MOV preserves flags; MOVS updates N/Z and updates
C only as prescribed by the immediate form, preserving V. Invalid zero
replication and SP/PC destinations are refused before changing state.
ADD/ADC/SUB/SBC/RSB now use the same immediate expansion with arithmetic
carry and overflow, including the CMN/CMP aliases and permitted SP forms.
Tests cover carry input, borrow, signed overflow, flag preservation and
invalid register/replication encodings.

Thumb STR immediate T3 supports its unsigned 12-bit byte offset and SP/LR
operands, without writeback. It uses the shared data translation and abort
path. Tests cover noncontiguous pages, permission and translation faults on
either side, alignment checking, and the Thumb data-abort return address.
Bytes stored before a second-page fault remain committed.

Wide LDR supports the unsigned imm12 form and signed PC-relative literals.
Literal addresses use the word-aligned Thumb PC. SP/LR destinations and
base/destination aliases are permitted; loading PC interworks. Tests cover
literal alignment, offset limits, page faults without partial register
updates, and refusal of unaligned PC accesses or ARM branch targets.

ARMv7 always provides modern unaligned access support (DDI0406C.b A3.2 and
AppxP.7.29), independent of raw SCTLR.U. Ordinary accesses use byte addresses
unless SCTLR.A requires an alignment fault; multiword, exclusive and SWP
accesses retain their stricter alignment requirements. ARM1176 keeps its
selectable legacy behavior. This does not complete the separate CP15 reset,
readback or memory-attribute audit.

`arm_reset()` explicitly selects ARM1176 and is safe on uninitialized storage.
`arm_reset_profile()` resets the implemented state with a validated explicit
profile. Board reset/wake paths must select their own profile when a new board
is introduced. Directly setting the field does not create an S5L8920 machine.

The legacy snapshot format describes S5L8900 and has no architecture field.
Its bytes and version remain unchanged; save/load now reject a non-ARM1176
machine before dropping the profile or changing live state. A future S5L8920
format must explicitly identify its CPU and board.

Signed-static fast paths continue to require ARM1176. A Cortex-A8 fixture
checks interpreter fallback, including refusal of unsupported divide, across
the basic, persistent, graph, and compact configurations where available.
Native handler execution requires an AArch64 host; an x86 Windows build cannot
establish that result.

## Remaining architecture and boot gaps

- The CP15 identification, cache/TLB controls, reset values, exception state,
  and memory translation paths still need a Cortex-A8 audit and implementation.
  The instruction profile is not a complete system-register model.
- Thumb-2 currently implements MOVW/MOVT, modified-immediate logical operations
  and arithmetic (also with shifted registers), immediate LDR/STR and
  byte/halfword transfers (including signed loads and pre/post indexing),
  literals, and doubleword transfers.
  Wide B/BL/BLX, CBZ/CBNZ and IA/DB multiple transfers are also implemented. Other
  instruction families remain to implement. IT state and conditional execution
  are implemented for the supported Thumb decoder families.
- VFP stores only d0-d15 and models VFP11/VFPv2. Cortex-A8 VFPv3 and NEON,
  including the wider register file and context-switch semantics, are absent.
- The SoC, interrupt wiring, storage, graphics, input and power devices are
  currently S5L8900-specific. Build them from the N88 firmware requirements.
- Boot arguments, device-tree relocation, importer/storage selection, and
  any compatibility patches need explicit target/version guards. Existing
  iPhone OS 3 patches are not evidence of iOS 6 compatibility.

## Complete kernel extraction

IMG3 decryption now reads the entire final AES block within the DATA tag,
including bytes counted as padding, and returns only the logical payload.
It validates that extent before changing the destination and supports the
importer's in-place operation. Previously the partial block was copied as
plaintext, causing a 36-byte decompression shortfall in the N88 kernel.
The production helpers now produce all 11,821,056 bytes with matching
Adler-32 `c4bee74e` and SHA-256
`ec4787ac012567f9c10ba2d5ab611058545bce7e17cd95ef310eba94bfca9b78`.
An independent padded-block probe produces the same bytes.
The verified entry at virtual `0x80086084` contains A32 ISB (`f57ff06f`)
at `0x8008609c` and DSB (`f57ff04f`) at `0x800860ac`. These instructions
motivated the barrier audit; their earlier acceptance as hints did not
establish correct profile support or an actual machine boot.

The same correction produces the complete iPhone1,2/7E18 kernel: 7,942,144
bytes, Adler-32 `2671cd74`, SHA-256
`f36a88d611d3b906ae858f377e21853b40b214b2bea99cb2f988e380698e6ce9`.
Its last metadata bytes differ from the historical zero-filled extraction.
The importer and iOS 3 patch gate accept both exact reference hashes. The
patch gate retains its build, segment, loaded-byte and instruction checks;
both real files pass the host patch test. Existing guest files are not
replaced. New imports and `unlzss` reject a
size or checksum mismatch before opening their output file.

## Bounded entry diagnostic

A private RAM-only harness loads the verified kernel at physical base
`0x40000000`, places partial early-entry arguments at `0x41000000`, and
starts at physical entry `0x40086084`. It has no device models or patches;
unmapped accesses and unprepared argument fields stop immediately. CP15
identity reads also stop before inheriting the ARM1176 identity.

With the existing modeled CP15 state, the guest constructs page tables and
enables the MMU. The first wide-Thumb stop was MOVW at `0x802b826a`, after
62,784 steps. Implementing MOVW/MOVT advances this same diagnostic to
`0x802b827c`, Thumb halfwords `f04f 31ff` (`MOV.W r1,#0xffffffff`), after
62,790 steps. This partial CPU execution does not establish a bootloader
handoff, complete S5L8920 realization, kernel initialization or device boot.
Adding the modified-immediate form advances the same trace to `0x802b8288`,
halfwords `f8c4 2224` (`STR.W r2,[r4,#0x224]`), after 62,794 steps.
With STR implemented, execution reaches `0x802b8328`, halfwords `f504 7090`
(`ADD.W r0,r4,#0x120`), after 62,845 steps. Each trace stops explicitly at
the next unsupported operation; none represents a complete kernel boot.
Modified-immediate arithmetic advances the trace to `0x802b832c`, halfwords
`f578 fb04` (`BL 0x80030938`), after 62,846 steps.
Wide branches carry the trace through calls and ARM/Thumb returns to
`0x802b840c`, halfwords `e8bd 40f0` (`POP.W {r4-r7,lr}`), after 63,130 steps.
Multiple transfers advance to `0x802bd23c`, halfwords `f8df c004`
(`LDR.W ip,[pc,#4]`), after 63,132 steps.
Wide loads advance the trace to `0x8027b966`, halfword `bb18`
(`CBNZ r0,0x8027b9b0`), after 63,181 steps.
CBZ/CBNZ advances to a deliberate diagnostic stop after 63,186 steps:
the instruction at `0x8027b970` reads physical `0x41000030`, a boot-argument
field outside the prepared early-entry fields. Device-tree and complete
boot-argument preparation are required before this trace can continue.

An extended diagnostic now supplies the verified version and N88 video
fields, places the matching device tree at physical `0x41100000`, and reserves
through `0x41110000`. It rejects reads of every unprepared all-zero tree
property and remaining argument fields. Changing these allocations and the
video RAM reservation changes the early loop count, so its step counts are
not directly comparable to the earlier harness. It first stops after 62,224
steps at `0x8027b98e`, `f886 0064` (`STRB.W r0,[r6,#0x64]`). With byte stores
implemented, it reaches `0x80089d80`, `f84d 8d04`
(`STR.W r8,[sp,#-4]!`), after 62,237 steps. Indexed transfers then advance
this same harness to `0x8027a3fe`, halfword `bf18` (`IT NE`), after 62,385
steps. IT execution advances the same trace to `0x8027a47e`, halfwords
`f020 0003` (`BIC.W r0,r0,#3`), after 62,446 steps. Logical operations then
advance to 70,735 steps, when the instruction at `0x8027aee8` reads physical
`0x41103204`, the guarded zero-valued `timebase-frequency` property of
`/device-tree/cpus/cpu0`.

Matching iBoot writes 24 MHz into clock state at `0x4ff13c2c..38` and copies
it to that property via getter index 5 at `0x4ff136d6..de`. The matching
iBSS restore bootloader explicitly programs its three PLLs to 600, 162 and
200 MHz. Its table at `0x8400d650` selects PLL2 divided by two for the bus;
iBoot's clock-state reader and getter index 3 propagate the resulting
100 MHz into `bus-frequency`. iBSS SHA-256 is
`30095f39be26acbb13677c7705de7bb9cbd2e3d04b90eb9d6380e1c1c413aa5d`.
The constants, M/P/S calculations and getter branch-table destinations were
cross-checked against both firmware files. This selects a matching firmware
clock configuration; it does not establish physical PLL behavior or measure
the normal LLB boot path.

Preparing those two properties in the private RAM copy advances the trace
to 71,289 steps at `0x8027af1e`, halfwords `e9c4 010c`
(`STRD r0,r1,[r4,#0x30]`). Doubleword execution then reaches 71,736 steps
at `0x8027af58`, reading another guarded property at physical `0x4110318c`.
That property is `memory-frequency`. The same firmware configuration establishes
200 MHz for memory, 600 MHz for the nominal CPU clock, 100 MHz for peripherals
and 24 MHz for the fixed clock. Preparing the remaining four CPU clock
properties advances to 73,287 steps at `0x8027a520`, halfwords `f813 0f01`
(`LDRB.W r0,[r3,#1]!`). Byte/halfword loads then advance the trace to 74,547
steps at `0x80089784`, reading guarded physical `0x411026a0`: the zero-valued
`/device-tree/chosen/debug-enabled` property. Every other unprepared
zero-valued property remains guarded, and the original device-tree file is unchanged.
No board registers are fabricated to advance this trace.

The matching iBoot updater at `0x4ff0ffbe..ffe2` leaves `debug-enabled` zero
unless its security policy enables debugging. The private harness selects
the disabled-debug handoff policy; it does not supply or claim measured fuse
state. That advances to 74,832 steps at `0x800897dc`, reading the guarded
`chosen/firmware-version` property. iBoot copies its literal
`iBoot-1537.9.55` into that 256-byte property at `0x4ff101de..1fc`.
Preparing it advances to 74,907 steps at `0x8027a8a4`, reading the command
line at boot-argument offset `0x38`.

iBoot constructs a 256-byte command line at `0x4ff10c44..82`. The normal
non-ramdisk, non-tethered configuration supplies no kernel options. Using
that configuration reaches 74,914 steps at `0x8027a8c0`, halfwords
`fa5f f088` (`UXTB.W r0,r8`). Extend execution then advances to 75,175 steps
at `0x80089d5e`, halfwords `eb08 0004` (`ADD.W r0,r8,r4`). Shifted-register
execution then reaches 75,223 steps at `0x802b797c`, halfwords `f3c0 0040`
(`UBFX r0,r0,#1,#1`). Bitfield execution then advances to 75,639 steps at
`0x8008b968`, halfwords `fba6 0101` (`UMULL r0,r1,r6,r1`), which remains
unsupported. Unprepared tree properties and remaining argument fields
retain their guards.

Host tests, exact-commit builds, firmware analysis, guest boot traces, and
physical app behavior are separate evidence. No iOS 6 boot or usability claim
follows from passing instruction tests.
