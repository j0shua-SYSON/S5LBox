# iPhone 3GS / iOS 6 bring-up

The initial iOS 6 target is iPhone 3GS running 6.1.6. The foundation includes
a distinct Cortex-A8 instruction profile and a partial S5L8920 memory and
interrupt fabric. A complete machine, kernel boot, and SpringBoard have not
been demonstrated. The existing iPhone
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

The physical bus can now report a latched host failure through the optional
`access_failed` callback. The interpreter stops with `ARM_HALT` at the failing
instruction, discards failed read values, and preserves PC, CPSR, fault registers
and retirement count. This is a missing host capability or backing-I/O failure,
not a fabricated guest external abort. The bus owner retains diagnostics and
explicitly clears the latch before retrying; failed writes must not commit.
Earlier completed bus transfers remain observable, and a failed exclusive store
retains its monitor for retry. Both instruction halfwords, data transfers, and
page-table reads follow this contract; host page-table failures are never cached.
Native execution requires an ARM1176 CPU and a bus without this hook. Legacy
buses omit it and retain their existing behavior. The host callback adds no
serialized guest state; ARM1176 snapshot bytes and version remain unchanged.
This CPU/MMU boundary does not establish checked DMA or a complete S5L8920 board.

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

Thumb DMB/DSB/ISB now use the same synchronous execution model, including
all reserved option values. They obey IT conditions in User and privileged
modes, preserve flags and exclusive state, and require both instruction
halfwords to be fetched before retirement. Tests also exercise instruction
replacement through a populated direct-write cache and a real CP15 TTBR0
change followed by ISB and execution from the new mapping. ARM1176 keeps
its legacy 16-bit framing for these first-halfword patterns.

Cortex-A8 CP15 accesses validate the complete opcode and register selector
before using existing storage. Unimplemented identity, cache-size, security,
performance and other banks are refused, preventing accidental ARM1176
identity and register aliases. User access is limited to the documented
barriers and thread-ID permissions. Both CP15 MRC to APSR and MCR from PC
are refused as unpredictable, following the system-register restrictions in
[DDI0406C.b, B3.15.2](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
The generic MRC flag-transfer behavior does not establish CP15 permission.
The legacy CP15 WFI encoding is a no-op on this
processor and does not call the ARM1176 wait hook. These rules follow
[DDI0344K, table 3-3 and sections 3.1, 3.2.40/41/73](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde).
Cortex-A8 SCTLR now resets to `0x00c50078` for the selected ARM-state,
little-endian, low-vector, maskable-FIQ reset configuration. Its defined
fixed bits persist across writes. NMFI and the absent VE/FI/HA/RR controls
ignore writes; reserved SBZP/SBOP violations are refused. Supported controls
are AFE, V, I, Z, C, A and M. TE, TRE and EE enable requests are refused
before mutation because Thumb exception entry, TEX remapping and big-endian
table walks are not implemented. This follows
[DDI0344K, section 3.2.25](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde)
and [DDI0406C.b, B3.15.2/B4.1.130](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests exercise the actual next instruction fetch after enabling the MMU,
stored readback, refused writes without TLB changes, and the N88 entry's
read/modify/write sequence. Other CPU profiles retain their reset/write
behavior. Other control registers and the complete MMU/security model remain
separate work.

Cortex-A8 ACTLR (`p15,0,c1,c0,1`) resets to `0x00000002`, enabling L2 in
the control register. The selected L1RSTDISABLE/L2RSTDISABLE inputs are low;
their read-only monitor bits ignore writes. Nonzero reserved fields are
refused before state changes. Defined controls retain their values, with
WFINOP controlling the platform wait hook. Cache/pipeline controls operate
within the synchronous, coherent memory model; this adds no cache timing or
parity-error injection. The rules follow
[DDI0344K, section 3.2.26](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde).
Tests cover ARM/Thumb transfers in each implemented privilege mode, reset,
readback, reserved-write refusal and real guest writes that disable and
restore WFI waiting. These selected reset inputs do not establish the N88
hardware configuration. Legacy ACTLR behavior is unchanged.

Cortex-A8 CPACR keeps the CP10/CP11 access fields. Fields for absent
coprocessors and the unsupported optional ASEDIS/D32DIS/TRCDIS controls
read zero and ignore writes. Reserved bit 29, permission value `2`, and
mismatched CP10/CP11 permissions are refused before changing availability.
The reset state denies access. This follows the implemented fields in
[DDI0344K, section 3.2.27](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde)
and the absent-field rules in
[DDI0406C.b, B4.1.40](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Regressions program CPACR through guest MCR instructions and verify permitted
VFP access and denied access entering the guest Undefined handler. Legacy
CPACR behavior is unchanged. This establishes access control; the full
Cortex-A8 VFP/NEON register and instruction model remains separate work.

Cortex-A8 VMRS/VMSR now use a separate system-register path in ARM and
Thumb state. FPSCR requires FPEXC.EN; every other defined system register
is privileged regardless of EN. CPACR and privilege/enable denials enter the
guest Undefined handler. FPSCR preserves its Cortex-A8 fields, including QC,
and refuses nonzero DNM/SBZP fields and unsupported exception trap enables.
FPEXC supports EN; requests for EX or other extra-state controls stop before
mutation. Privileged FPSID writes serialize the synchronous FP unit. FPSID
and MVFR reads remain explicit capability stops until the target identity is
established; MVFR writes and reserved selectors are refused. These rules use
[DDI0344K, section 13.4](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde)
and [DDI0406C.b, B9.3.21/22](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
The latter defines FPSID serialization but no valid MVFR VMSR selector.

Tests cover access modes, control bits, core-register restrictions, APSR
NZCV-only transfers, and complete instruction fetch before effects. A guest
Undefined handler enables FPEXC and returns to the original Thumb IT slot,
then the retried VMRS changes the condition seen by the next slot. Undefined
entry saves fault PC+2 in Thumb even for a 32-bit instruction, following
[DDI0406C.b, B1.9.2](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Unknown identity reads remain capability stops even with EN clear. The
legacy VFP11 transfer rules are preserved. FP arithmetic remains separate
from these system transfers.

Cortex-A8 now stores all 32 doubleword registers. `d0-d15` retain their
`s0-s31` aliases; `d16-d31` have independent storage. ARM and Thumb VMOV
transfer raw words between core registers and any D register, either one
half or both halves, and between core registers and one or two consecutive
S registers. NaN payloads and other bit patterns are preserved without
consulting host rounding. Encoding, SP/PC and duplicate destination
restrictions follow
[DDI0406C.b, A8.8.341-345](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Byte/halfword VMOV lane transfers remain unsupported. CPACR/EN denials
enter the guest Undefined handler; reserved encodings stop without mutation.

Tests fill and read every D register through different VMOV forms, verify
lower-bank S aliases and upper-bank independence, preserve untouched halves,
and cover permissions, register overlaps, reset and conditional execution.
The extra 128 bytes are inactive on legacy profiles. Snapshot version 32
still accepts only ARM1176, omits the inactive A8 bank and clears it on
restore. Its byte stream is unchanged.

ARM and Thumb VDUP from a core register duplicate the low 8, 16 or 32 bits
across any D or Q destination. The checked core-transfer path preserves
FPSCR, CPSR and the host floating-point environment. Odd Q destinations,
the reserved size/low bits, PC sources and Thumb SP sources stop before
access checks; ARM SP sources are valid. Conditional execution and Thumb
fetch/retry rules apply as for the other checked transfers. Tests cover
all register indices, raw bit patterns, permission combinations and
nondefault controls. See
[DDI0406C.b, A8.8.314](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

ARM and Thumb VFP VMOV register/immediate, VABS and VNEG now operate on
all 32 S or D registers through a checked A8 path. Copies preserve raw bits;
absolute and negate only clear or invert the sign bit. Immediate expansion
uses the architectural eight-bit constant format. These operations preserve
FPSCR, ARM flags and the host floating-point environment, including for
signaling NaNs, denormals and signed zero. Access denials enter the guest
Undefined handler; reserved immediate bits stop before mutation. See
[DDI0406C.b, A7.5.1 and A8.8.280/339/340/355](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Their short-vector behavior follows
[DDI0344K, section 13.3](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde)
and [DDI0406C.b, Appendix K](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
D16-D19 is a second scalar bank: a destination there stays scalar, and a
source there broadcasts into a vector destination. Vector registers wrap
within their bank. Invalid length/stride combinations are refused, including
the global stride-two limit with a scalar destination. Tests cover all
register pairs, all 256 immediates, bank/stride boundaries, conditional
execution and access checks. Thumb tests cross unrelated physical pages and
verify a guest handler enabling VFP and retrying the original IT slot.
Other upper-bank arithmetic and Advanced SIMD encodings remain separate work.

ARM and Thumb VCMP/VCMPE now compare any S0-S31 or D0-D31 pair, or one
register against positive zero. The checked A8 path classifies and orders
the raw IEEE bit patterns without using host floating-point operations.
Signed zeros compare equal; NaNs are unordered; signaling NaNs, and any
NaN for VCMPE, accumulate IOC. FZ flushes either denormal input to signed
zero and accumulates IDC, including when the other operand is a NaN.
Comparisons replace FPSCR.NZCV, retain cumulative flags and leave ARM flags
unchanged until VMRS explicitly copies them. See
[DDI0406C.b, A2.7.8 and A8.8.303](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Comparisons remain scalar regardless of LEN/STRIDE, as specified by
[DDI0344K, 13.3.2](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde)
and DDI0406C.b K.1.1. Reserved zero-form operand fields and unsupported
FPSCR state stop before execution; valid access denials enter the guest
Undefined handler. Tests cover all register pairs, ordered boundary values,
both NaN classes, FZ/DN controls, host rounding/exception preservation,
conditional execution, split Thumb fetch faults and guest enable/retry.
These are host instruction tests; no complete firmware boot follows from them.

ARM and Thumb NEON VLD1/VST1 multiple-single-element forms now transfer
one to four consecutive D registers with 32-bit or 64-bit elements. They
support naturally aligned little-endian accesses, optional 8/16/32-byte
alignment assertions, and immediate or register post-index writeback.
An assertion failure, or a standard alignment failure with SCTLR.A set,
enters the guest Data Abort handler. Standard unaligned accesses with A=0,
8/16-bit elements and big-endian accesses remain
unsupported. See [DDI0406C.b, A3.2.1, A7.7.1 and A8.8.320/404](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

VLD2/VST2 multiple-structure forms additionally interleave or deinterleave
32-bit elements into adjacent or spaced pairs of D registers, or four
consecutive D registers. They use the same alignment and writeback rules;
two-register forms reject a 32-byte alignment assertion. Each completed
word publishes to its own interleaved register lane. Other element widths,
single-lane/replicate forms and the remaining structure families are not
implemented. See
[DDI0406C.b, A8.8.323/406](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Transfers use the current guest memory permissions. Writeback occurs only
after completion, preserving the required base-restored abort behavior.
Completed store words remain in memory; loads publish completed 32-bit
elements or complete 64-bit register pairs. Checked host-bus failures stop
without entering a guest exception and allow retry from the original base.
Tests cover all register lists, alignment fields, post-index aliases,
permissions, conditional execution, MMU and bus faults, and split Thumb
fetch faults. The A8 ARM decoder handles NEON memory before its broad preload
hint check, so D15/D31 transfers and unsupported structure forms cannot be
swallowed as hints.
For VLD2/VST2, independent register-word permutations verify the untouched
spaced register and every partial-transfer boundary, including User page
translation/permission faults and checked-bus failure followed by retry.

ARM and Thumb NEON register VAND, VBIC, VORR, VORN, VEOR, VBSL, VBIT and
VBIF now operate on all 32 D registers or 16 Q registers, including VORR's
VMOV alias. They preserve FPSCR, ARM flags and the host FP environment;
masked operations read the original destination before writing their result.
Odd Q-register encodings stop before access checks, and valid permission
denials enter the guest Undefined handler. These rules follow
[DDI0406C.b, A8.8.287/289/290/315/358/360](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests use independent bit truth tables and cover register aliases, denied
access, conditional execution, neighboring encodings, split Thumb fetch
faults and guest enable/retry.

NEON VEXT extracts a byte window from two concatenated D or Q operands,
including every byte offset and aliases between sources and destination.
The implementation stages both complete inputs before publishing results
and preserves status and host FP state. Invalid D offsets and odd Q
operands stop before access checks. ARM uses its unconditional encoding;
Thumb obeys IT and complete-fetch/retry rules. A byte-array oracle checks
register indices, operand order and boundaries against the word-based
implementation. See
[DDI0406C.b, A8.8.316](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

NEON VTRN transposes 8-, 16- or 32-bit elements across two D or Q operands,
staging both complete results before updating either operand. It preserves
ARM flags, FPSCR and the host FP environment. Reserved widths and odd Q
operands stop before access checks. Identical operands have an architecturally
UNKNOWN result; that case remains an explicit capability stop after valid
access checks. Tests cover every register pairing, an independent byte-array
oracle, permission and enable checks, IT execution, neighboring encodings,
split fetch faults, checked-bus retry and guest enable/exception-return retry.
See [DDI0406C.b, A8.8.420](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

NEON immediate VMOV, VMVN, VORR and VBIC also cover the full D/Q bank.
The decoder expands the encoded integer or F32 constant as raw bits,
including byte masks for VMOV.I64 and the trailing-one forms. It rejects
the reserved cmode=15/op=1 allocation, odd Q destinations and the zero
immediates marked UNPREDICTABLE before checking access. These rules follow
[DDI0406C.b, A7.4.6 and A8.8.288/339/353/359](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests independently construct the constant bytes for every immediate/mode
combination and cover all register indices, permissions, conditionals,
host FP preservation, split fetch faults and guest enable/retry.

NEON VABS.F32 and VNEG.F32 operate on every D/Q register by clearing or
inverting each lane's sign bit. Signaling NaN payloads and denormals remain
intact, and FPSCR controls, exception flags, ARM flags and host FP state
are preserved. Operands are staged before aliased writes. Reserved floating
point widths and odd Q operands stop before access checks; integer forms
remain separate work. Tests cover register combinations, raw value classes,
access denials, IT execution, split fetch faults and checked-bus retry. See
[DDI0406C.b, A8.8.280/355](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

The matching cache's unchanged ARM
[`vDSP_vabs`](https://developer.apple.com/documentation/accelerate/vdsp_vabs)
and [`vDSP_vneg`](https://developer.apple.com/documentation/accelerate/vdsp_vneg)
forward vector paths now complete 34 prepared cases. These use unit strides,
zero or 32/64/96/128 elements, all four word-aligned input positions within
16 bytes, and aligned separate output buffers. Before this change, the first
nonempty absolute-value case stopped at `VABS.F32` at `0x3083ee70` after
32 source bytes were read. The unchanged fixture verifies raw output bits,
exact per-byte access counts, whole-page canaries, preserved registers and
FPSCR, the fifth stack argument and ARM-to-Thumb return. Its explicit
CPUFAMILY_ARM_13 commpage input selects the prepared firmware path; actual
commpage initialization, other paths, process launch and boot remain unverified.

The opposite-output-stride paths now also return correctly in all 34 prepared
cases, using the unchanged ARM_13-selected code, input stride 1 and output
stride -1. These cover zero or 32/64/96/128 elements, all four word-aligned
input positions within 16 bytes, and aligned separate output blocks. Before
VLD2 support, the first nonempty absolute-value case stopped at `0x3083f350`;
after pair transfers it reached `VTRN.32` at `0x3083f374`, with 80 source bytes
read and no output written. Adding VTRN lets the unchanged fixture verify
the complete reversed raw-bit results, exactly one read/write per requested
source/output byte, whole-page canaries, one saved/restored R4, all FP
registers, preserved ABI registers, FPSCR and ARM-to-Thumb return. The generic
CPU-family branch, scalar tails, actual commpage setup and boot remain unverified.

The unchanged ARM [`vDSP_vfill`](https://developer.apple.com/documentation/accelerate/vdsp_vfill)
at `0x3083cddc` completes 744 additional cases using existing instructions.
These cover 25 boundary lengths from 0 through 257, strides +/-1, +/-2 and
+/-3, all four word-aligned output positions within 16 bytes, and 18 raw
F32 value classes. The fixture verifies scalar and vector paths, alignment
prefixes and scalar tails, one scalar read for nonempty calls, exactly one
write per selected output byte, untouched gaps and page canaries, all FP
registers, preserved ABI registers, FPSCR and return state. Its explicit
ARM_13 family input excludes the other-family integer path; no stack access
is prepared. Source/output aliasing and actual commpage setup remain unverified.

NEON register VMUL.F32, VADD.F32 and VSUB.F32 operate on two or four lanes
across the full D/Q bank, using integer arithmetic and nearest-even rounding.
Multiplication retains an exact 48-bit product; addition and subtraction
retain guard, round and sticky bits through alignment and normalization.
ARM standard NEON arithmetic fixes default-NaN and flush-to-zero behavior independently
of FPSCR rounding/FZ/DN controls. Both inputs are unpacked before NaN
selection, and a tiny nonzero result flushes to signed zero before rounding,
setting UFC without IXC. Overflow sets OFC/IXC; denormal inputs and invalid
operations set their sticky flags. Existing FPSCR flags and controls, ARM
flags and host FP state are preserved. See
[DDI0406C.b, A2.7 and A8.8.283/351/415](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests compare independent binary64 numeric oracles with raw
boundary cases, all register aliases, sampled finite inputs, guest/host
rounding controls, access denials, conditional execution and split fetches.
The addition oracle retains a residual to detect inexact results across
large exponent gaps. Tests also cover cancellation, signed zeros and
sticky flags across an exceptional result, an exact result and VMRS.
Odd Q operands and the reserved size encoding stop before access checks.

NEON VMLA.F32 and VMLS.F32 also cover the full D/Q bank. The product is
rounded and flushed before a separate addition; VMLS negates the rounded
product. Both steps contribute their cumulative exception flags. Original
accumulator and source operands are staged before publication. These use
the same standard NEON controls, with permission, invalid encoding and
complete-fetch checks before execution. Tests cover every register triple
using an exact small-integer oracle, 25 analytical F32 cases, guest controls,
host rounding/exception preservation, IT, checked-bus retry and guest
enable/exception-return retry. See
[DDI0406C.b, A8.8.337](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

The F32 forms of VMUL, VMLA and VMLS by scalar select either 32-bit lane
from D0-D15 and apply it across a D or Q vector. They reuse the same integer
arithmetic and standard NEON controls, including separate multiply/add
rounding. All original operands are read before publication, including when
the scalar overlaps the destination. Size 0/1 and odd Q operands stop
before access checks; size 3 remains with its related decoders, including
VEXT. Tests cover every register/lane combination, an ignored signaling NaN
in the unselected lane, analytical FP cases, guest and host controls, IT,
access priority, complete instruction fetch and both host/guest retry.
See [DDI0406C.b, A8.8.338/352](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Other NEON arithmetic remains separate work.

The unchanged 528-byte ARM [`vDSP_vramp`](https://developer.apple.com/documentation/accelerate/vdsp_vramp)
at `0x30827720..0x30827930` completes 1,824 prepared calls after this addition.
Before it, the first aligned empty call stopped after 16 instructions at
`0x30827774`, on `VMUL.F32 q2, q2, d6[0]`, after reading both scalar inputs
and before any output. The unchanged fixture checks four exact-integer
ramps, selected lengths 0..129, strides +/-1, +/-2 and +/-3, and four word
alignments. It verifies every selected output byte once, untouched gaps and
page canaries, the four-byte argument and saved frame, ABI registers and
unaffected FP state. The original VFP scalar setup runs with nearest-even,
FZ and DN. Fractional/special inputs, other VFP rounding modes, process
execution and boot remain outside this fixture; it supplies no CPU-family
identity input.

Cortex-A8 VFP VADD/VSUB now support F32 and F64 across all S/D registers
in ARM and Thumb. Integer significands retain guard, round and sticky bits;
FPSCR selects all four rounding modes, gradual underflow or FZ, and DN or
original NaN payloads. Signaling NaNs take priority over quiet NaNs, with
the first operand winning within each class. Subtraction selects NaNs
before changing the second operand's sign. FZ unpacks both inputs before
NaN selection and flushes tiny results before rounding, raising UFC without
IXC. Short vectors wrap within their banks, including the D16-D19 scalar
bank, and stage all original operands before publishing results. Invalid
FPSCR fields and vector shapes stop before access checks. See
[DDI0406C.b, A2.7.8, A8.8.283/415 and Appendix K](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover every scalar register triple, vector shapes and aliases,
explicit rounding boundaries, finite native IEEE oracles, NaN/infinity
combinations, cumulative flags, guest/host controls, conditional execution,
access denials, checked-bus retry and User instruction-fetch faults. A
guest handler enables VFP and returns to the interrupted Thumb IT slot.
The first focused run exposed a missing Thumb dispatch registration; after
that correction all three focused suites passed. The numerical implementation
and test expectations needed no correction.

The unchanged Thumb [`vDSP_vrampD`](https://developer.apple.com/documentation/accelerate/vdsp_vrampd)
at `0x3082bf90` now completes all 256 prepared small-count calls. On the
preceding commit, 64 empty calls returned, then the first nonempty call
stopped after 16 instructions at `0x3082bfc0`, on
`VADD.F64 d18, d17, d16`, after reading both eight-byte inputs and before
any output. The unchanged fixture executes only `0x3082bf90..0x3082bfd6`
and the return at `0x3082c1fc` from the original 622-byte image. Counts 0..3,
strides +/-2 and +/-3, four word alignments and four exact-integer ramps
verify output bits, exact selected-byte writes, untouched gaps and page
canaries, the eight-byte saved frame, four-byte argument, all FP registers,
ABI registers and return state. It uses nearest-even, FZ and DN; larger or
unit-stride paths, fractional/special inputs, process execution and boot
remain outside this fixture. Other full-bank VFP arithmetic remains unfinished.

With the new VFP addition path, the existing F32 ramp and F64 remainder
fixtures also retain all 1,824 and 112 passing calls. The guarded kernel-entry
fixture matches its entire prior trace, with only line endings normalized:
61,650 steps to the same unestablished S5L8920 L2 parity/ECC configuration.

VFP VMUL/VNMUL now use the same checked full-bank scalar and short-vector
paths in ARM and Thumb. Four 32-bit limb products retain the exact 106-bit
binary64 product before normalization and guest-controlled rounding. They
share input flushing, NaN selection and rounding with VADD/VSUB. VNMUL
flips the sign of the rounded product, including zeros and NaNs; it does
not reverse the rounding direction. Tininess is checked before rounding,
including when an inexact tiny product rounds to the smallest normal value.
See [DDI0406C.b, A2.7.8 and A8.8.351/356](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests compare native IEEE results with raw analytical anchors. An independent
binary-division check determines exact tininess, so the exception oracle
accounts for ARM's rule even on hosts that check underflow after rounding.
Tests cover every register triple, signed zeros, gradual and flushed tiny
results, overflow, NaN payloads and negation, all guest rounding/FZ/DN
controls, host FP-state preservation, vector aliases and cumulative flags.
The existing vector-shape, access, invalid-state, conditional, checked-fetch
and guest enable/return retry matrices also cover both multiply forms.

The unchanged positive unit-stride path of `vDSP_vrampD` completes 336
prepared calls: 21 selected lengths from 0 through 257, four word alignments
and four exact-integer ramps. Before multiplication support, the first empty
aligned call stopped after 17 instructions at `0x3082c000`, on
`VMUL.F64 d20, d16, d18`, after reading both eight-byte inputs and the
eight-byte zero literal at `0x3082c268`, with no output. The unchanged
736-byte image covers `0x3082bf90..0x3082c270`; execution is limited to the
entry, positive unit-stride body and return at `0x3082c1fc`. The fixture
verifies every input/literal/frame/argument/output byte count, exact outputs,
page canaries, ABI registers and unaffected FP state. It does not provide
a complete oracle for clobbered FP temporaries, other strides, fractional or
special inputs, other FPSCR modes, process execution or boot. The existing
nonunit F64 ramp, F64 remainder and F32 ramp fixtures retain their 256, 112
and 1,824 passing calls after the shared arithmetic refactor.

VFP VDIV supports F32 and F64 through the same full-bank ARM/Thumb scalar
and short-vector paths. Integer long division produces the significand and
three rounding bits; its remainder supplies sticky information. It uses
the shared guest rounding, input-flushing and NaN rules without host FP
operations. Zero divided by zero and infinity divided by infinity raise
IOC. Finite nonzero values divided by zero raise DZC; infinity divided by
zero returns signed infinity without DZC. See
[DDI0406C.b, A2.7.8 and A8.8.312](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Division tests cover every register triple, signed powers of two, all vector
shapes and overlapping banks, NaNs, zeros, infinities and cumulative flags.
Native finite-result oracles use an independent exact ratio comparison for
ARM's tininess-before-rounding rule. Analytical anchors include signed
thirds, halfway subnormals, tiny results that round to normal, FZ and overflow
under all rounding modes. The access, conditional, invalid-state and fetch
matrices include division. Inexact flags appear only after a complete fetch
or successful guest enable/exception-return retry. The first build caught
an unavailable symbolic constant in the new ARM test; the assertion was
corrected to its architectural bit value before tests ran.

The unchanged 62-byte Thumb
[`vDSP_vdivD`](https://developer.apple.com/documentation/accelerate/vdsp_vdivd)
at `0x308610b8..0x308610f6` completes all 1,536 prepared calls. Its first
input is denominator B, its second is numerator A, and its output is A/B.
Before division support, all 256 empty calls returned; the first nonempty
call stopped after 17 instructions at `0x308610e8`, on
`VDIV.F64 d16, d17, d16`, after reading eight bytes from each input and
before writing output. The same fixture covers counts 0, 1, 3, 7, 16 and 33,
four signed-stride tuples, four word-alignment tuples and all 16 combinations
of rounding mode, FZ and DN. Sixteen analytical operand pairs cover exact
quotients, thirds, zeros, infinities, distinct NaN payloads, overflow and
underflow boundaries. Checks include every selected input/output byte count,
untouched gaps and page canaries, the eight-byte saved frame, twelve-byte
argument area, all FP registers, FPSCR, ABI state and return. Inputs and output
occupy separate prepared User mappings. Aliasing, other lengths or routines,
process execution and board boot remain outside this fixture.

The existing unit and nonunit F64 ramp and F64 remainder fixtures retain
all 336, 256 and 112 passing calls after division support. The guarded
kernel-entry fixture still matches the complete prior trace, normalizing
line endings only, and stops after 61,650 steps at the same L2 parity/ECC
configuration boundary.

VFP VSQRT supports F32 and F64 in the checked ARM/Thumb scalar and
short-vector paths, including upper D registers and scalar-bank broadcasts.
It extracts the root two input bits at a time with integer arithmetic, then
uses the common rounding path with remainder-derived sticky information.
Positive finite nonzero inputs always produce normal roots. Negative zero
keeps its sign; FZ flushes subnormal inputs before the negative-input check.
Other negative inputs raise IOC. NaNs preserve their payload/sign unless DN
selects the default NaN, and signaling NaNs raise IOC. See
[DDI0406C.b, A2.7.8 and A8.8.401](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover every source/destination register pair, exact squares, every
finite exponent, every subnormal normalization shift, rounding boundaries,
all input classes, vector aliases, cumulative flags and guest/host controls.
The arithmetic access, invalid-state, conditional, full-fetch and guest
enable/return retry matrices also include square root. The first focused
run exposed double rounding in the Windows host oracle: its x87 square root
of the double just below one rounded again to one. The emulator's raw
analytical anchors were correct. The oracle now treats native results as
candidates and proves adjacent roots and their midpoint using exact integer
squares; this also determines inexact status. The numerical implementation
and raw anchors required no correction.

The unchanged libsystem_m routines `sqrtf` (16 ARM bytes at
`0x39295a08..0x39295a18`) and `sqrt` (14 Thumb bytes at
`0x39295a18..0x39295a26`) complete all 640 prepared calls. Before this
addition, the first double-precision case stopped after two instructions
at `0x39295a1c`, on `VSQRT.F64 d16, d16`, with eight instruction bytes read.
The unchanged fixture uses twenty raw inputs per precision and all sixteen
rounding/FZ/DN combinations. Its forty analytical anchor rows are separately
proved by exact rational squares and squared midpoints before execution.
Each completed call executes four original instructions, with exact fetch
byte counts and no data or stack access. Checks cover all FP and general
registers, FPSCR, CPSR, the exclusive monitor, return state and unchanged
RAM. These are isolated calls with prepared User code mappings; they do not
establish process execution, board boot, timing or physical behavior.

The existing vector division, unit F64 ramp, F32 ramp and F64 remainder
fixtures retain all 1,536, 336, 1,824 and 112 passing calls. The complete
guarded kernel-entry trace remains unchanged through its 61,650-step L2
parity/ECC stop.

VFP VCVT between F32 and F64 uses the checked ARM/Thumb path with all source
and destination registers, including overlapping S/D aliases. These
conversions are scalar and ignore every LEN/STRIDE combination, as specified
by [DDI0406C.b, A2.7.8, A8.8.309 and K.1.1](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Integer normalization and the shared rounding function implement all guest
rounding modes, overflow and tininess before rounding. FZ flushes subnormal
inputs with IDC and tiny results with UFC without IXC. Finite F32-to-F64
conversion is exact after input handling. NaNs retain sign and the specified
payload bits unless DN selects the default NaN; signaling NaNs raise IOC.
Source values are read before destination writes, and the host FP state is
untouched.

Tests cover every mixed-format register pair and all 32 LEN/STRIDE settings,
raw rounding-boundary anchors, every finite exponent and every subnormal
normalization shift, all input classes and sixteen rounding/FZ/DN controls.
Native finite casts provide an independent oracle, with tininess checked
against the original F64 input instead of the host's underflow flag.
Additional checks cover cumulative flags, host rounding and pending
exceptions, CPACR/FPEXC access, invalid FPSCR fields, conditional execution,
legacy profile isolation, checked-fetch failure, User split-page fetch faults
and guest enable/exception-return retries. The first focused build and all
three focused tests passed without corrections.

The unchanged Thumb routines `vDSP_vdpsp` at `0x3083b3ec..0x3083b48e` and
`vDSP_vspdp` at `0x3083b688..0x3083b72a` now complete all 4,096 prepared calls.
Each routine contains 162 original bytes. Before conversion support, all
256 empty calls returned; the first nonempty call stopped after 16
instructions at `0x3083b416`, on `VCVT.F32.F64 s0, d16`, after eight input
bytes and before any output write. The fixture and runner were unchanged
for the first successful run. A separate exact-rational scaling/division
oracle supplies 640 raw result/exception rows, checked against fourteen
fixed anchors. Calls cover both directions, counts 0/1/2/3/4/7/16/33,
four signed-stride pairs, four alignment tuples, twenty raw inputs
per direction and all sixteen controls. They exercise scalar tails and
unrolled four-element loops with prepared User mappings, read-only inputs
and a separate output page. Checks cover exact input/output/frame/argument
byte counts, all FP/general registers, flags, return state, instruction
counts and whole-RAM canaries. These isolated routine results do not
establish process launch, board boot, timing or physical behavior.

After precision conversion support, the existing square-root, vector
division, unit F64 ramp, F32 ramp and F64 remainder fixtures retain all
640, 1,536, 336, 1,824 and 112 passing calls. Both complete local suites pass
(77 strict and 72 shipping tests). The entire guarded kernel-entry trace
still matches the prior trace through its 61,650-step L2 parity/ECC stop.

VFP conversions between signed/unsigned 32-bit integers and F32/F64 now use
the checked Cortex-A8 path in both ARM and Thumb. VCVT from floating point
forces rounding toward zero; VCVTR uses FPSCR.RMode. Integer-to-FP conversion
also uses FPSCR.RMode and is always exact for F64. All forms ignore LEN and
STRIDE. Integer arithmetic implements normalization, rounding and saturation
without changing the host FP environment. The rounded integer is checked
against its destination range: overflow saturates and raises IOC without a
new IXC, while an in-range inexact result raises IXC. All NaNs yield zero
with IOC; infinities saturate with IOC; FZ input flushing produces zero with
IDC. Prior cumulative flags remain. See
[DDI0406C.b, A2.7.8, A8.8.306 and K.1.1](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover all twelve forms, every source/destination register pair and
all LEN/STRIDE combinations, including mixed-format aliases. Explicit raw
anchors cover half-way rounding, signed and unsigned range boundaries,
negative values that round to zero, and integer-to-F32 precision loss.
Independent native casts and integer rounding with separate range checks
cover every finite exponent, every subnormal normalization shift, and
random and power-boundary integer inputs. All sixteen guest controls,
host rounding/pending exceptions, special values, cumulative flags, access
denial, invalid state, conditions, neighboring allocations and legacy
profiles are checked. Fetch and guest enable/return tests cover every form,
including the difference between VCVT and VCVTR on retry. The first focused
build and all three focused tests passed without corrections.

The first Windows MSVC CI run exposed 576 failed assertions in three raw
anchor rows: unsuffixed negative literals at or below -2^31 were interpreted
using unsigned negation. The expected values now use explicit `INT64_C`
operands. The native oracle and emulator arithmetic are unchanged; the
other seven core CI jobs passed on that original commit.

The unchanged 40-byte Thumb routines `vDSP_vfix32D` at
`0x308612bc..0x308612e4` and `vDSP_vflt32D` at
`0x30861720..0x30861748` complete all 4,096 prepared calls. Before this
addition, all 256 empty calls returned and the first nonempty call stopped
after ten instructions at `0x308612d6`, on `VCVT.S32.F64 s0, d16`, after
eight input bytes and before any output write. The fixture and runner were
unchanged for the first successful run. Its exact-rational oracle supplies
640 result/exception rows, checked against eighteen raw anchors and two
independent range-boundary identities. Calls cover eight counts from zero
through 33, four signed-stride pairs, four alignment tuples, twenty inputs
per direction and all sixteen rounding/FZ/DN controls. Separate prepared
User input/output pages and a four-byte stack argument have exact access
counts; the routines need no stack frame. Checks cover each fetched byte,
all FP/general registers, flags, the exclusive monitor, return state,
instruction counts and whole RAM. These signed-integer/F64 routine results
do not establish unsigned, VCVTR or F32 routine execution, process launch,
board boot, timing or physical behavior.

After integer conversion support, the precision, square-root, division,
unit F64 ramp, F32 ramp and F64 remainder fixtures retain all 4,096, 640,
1,536, 336, 1,824 and 112 passing calls. Both complete local suites pass
(77 strict and 72 shipping tests). The entire guarded kernel-entry trace
remains identical through the same 61,650-step L2 parity/ECC stop.

VFP VMLA/VMLS now cover F32/F64 and the full register bank, including short
vectors. The product and addition round separately using guest controls;
VMLS negates the rounded product before addition, including a NaN product.
The original accumulator is the first addition operand, which also determines
NaN precedence. Both stages contribute cumulative exception flags. Staged
results preserve the original sources and accumulators for overlapping
vectors. Guest controls and access are checked before arithmetic. This
follows [DDI0406C.b, A8.8.337 and Appendix K](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover every scalar register triple and both formats/operations/ISAs,
short-vector banks and strides, independent native arithmetic for each stage,
raw anchors distinguishing fused arithmetic, intermediate overflow/underflow,
and all triples of the 22 value classes under sixteen guest controls. Host
rounding and pending exceptions remain intact. Tests also cover cumulative
flags, access/conditions, invalid state, legacy and neighboring allocations,
checked host fetches, split-page User faults and guest enable/return retries.
The first run found one mistaken NaN-sign anchor and two obsolete refusal
sites: VMLS preserves the negated invalid-product NaN when DN is clear, and
VMLA is now supported. Correcting those expectations removed nine failures;
the production arithmetic and native oracle were unchanged. All three
focused tests then passed.

The unchanged 1,036-byte ARM `vDSP_dotprD` routine at
`0x3085763c..0x30857a48` completes 3,456 prepared calls on paths that do not
read the CPU-family commpage. Before this addition, 1,024 calls passed and
the first four-element unit-stride call stopped after seventeen instructions
at `0x308579bc`, on `VMLA.F64 d0, d16, d24`, after reading both 32-byte inputs
and before the output write. The fixture and runner were unchanged for the
first successful run. Unit strides cover nine counts from zero through 15;
three nonunit signed-stride pairs additionally cover 16, 17, 31, 32, 33 and
65. Four word-alignment tuples, four small-integer input patterns and all
four FZ/DN combinations use nearest-even rounding and LEN/STRIDE zero.
Independent closed-form sums and twenty raw integer anchors check results.
The fixture also checks every instruction fetch and input/output byte,
the 64-byte saved FP frame and eight-byte argument block, all FP/general
registers, flags, exclusive state, return state, instruction counts and
whole RAM. No CPU identity is supplied or overridden. These selected paths
do not establish large unit-stride execution, fractional/special inputs,
process launch, board boot, timing or physical behavior.

After VFP multiply-accumulate support, all 77 strict and 72 shipping tests
pass. The NEON integer, VFP integer, precision, square-root, division,
unit F64 ramp, F32 ramp and F64 remainder fixtures retain their 6,144,
4,096, 4,096, 640, 1,536, 336, 1,824 and 112 passing calls. The entire
guarded kernel-entry trace is identical through the 61,650-step ECC stop.

Advanced SIMD conversions between F32 and signed/unsigned 32-bit integers
now cover every D/Q register and both instruction sets. They use the same
integer arithmetic with standard NEON controls: nearest-even for integer
inputs, truncation for FP inputs, FZ/DN enabled and traps disabled. Guest
rounding, FZ/DN, LEN/STRIDE and trap enables do not select the arithmetic.
Each active lane contributes its cumulative exceptions; inactive registers
and lanes remain unchanged. Reserved sizes and odd Q register operands stop
before access checks. This follows
[DDI0406C.b, A2.7.8 and A8.8.305](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover all source/destination D/Q pairs and aliases, raw result/flag
anchors under all 512 rounding/FZ/DN/LEN/STRIDE combinations, independent
native conversion oracles, every finite F32 exponent and subnormal shift,
special values and random/power-boundary integers. Four host rounding modes
and pending exceptions remain intact. Tests also cover cumulative flags,
VMRS, access denial, invalid encodings, IT conditions, legacy refusals and
adjacent reciprocal allocations. Checked host fetch failures, split-page
User translation/XN/AP faults, and guest enable/return retries exercise
all four operations and both vector widths. The first build and all three
focused tests passed without corrections.

Three unchanged 44-byte Thumb routines now complete 6,144 prepared calls:
`vDSP_vflt32` at `0x308616f4..0x30861720`, `vDSP_vfltu32` at
`0x30861800..0x3086182c`, and `vDSP_vfixu32` at
`0x308615e4..0x30861610`. Before this addition, all 256 initial empty calls
returned and the first nonempty call stopped after nine instructions at
`0x3086170a`, on `VCVT.F32.S32 d0, d0`, after four input bytes and before
any output write. The fixture and runner were unchanged for the first
successful run. Each routine converts both lanes of D0 even though it stores
only S0; the fixture checks D0 and partial FPSCR/CPSR after every conversion.
An independent exact-rational oracle supplies 60 main-input rows and 396
upper-lane trajectory rows, checked against 24 raw anchors. Calls cover
eight counts from zero through 33, four signed-stride pairs, four alignment
tuples, twenty main inputs per routine, all sixteen guest rounding/FZ/DN
controls and four prepared S1 seeds. Separate User input/output pages and a
four-byte stack argument have exact access counts; no stack frame is used.
Checks cover each fetched byte, all FP/general registers, flags, exclusive
monitor, return state, instruction counts and whole RAM. These results do
not establish a signed F32-to-integer routine, process launch, board boot,
timing or physical behavior.

After NEON conversion support, all 77 strict and 72 shipping tests pass.
The earlier integer, precision, square-root, division, unit F64 ramp, F32
ramp and F64 remainder fixtures retain their 4,096, 4,096, 640, 1,536, 336,
1,824 and 112 passing calls. The whole guarded kernel-entry trace remains
identical through the 61,650-step L2 parity/ECC stop.

The unchanged ARM [`vDSP_vma`](https://developer.apple.com/documentation/accelerate/vdsp_vma)
at `0x30825f94` now completes 144 calls through its scalar body. Before
multiply-accumulate support, the first nonempty case stopped at `0x30826144`
after one element was read from each input and before any output write.
The same fixture covers selected counts from 0 through 31, four stride
tuples, four pointer alignments and 12 analytical input triples. It verifies
intermediate rounding/overflow/underflow, NaNs, signed zero, final FP flags,
exact buffer access counts and canaries, the 28-byte frame and five stack
arguments, all FP registers, preserved ABI registers and return state.
Unused upper scalar lanes start at zero. Vectorized paths, process launch
and boot are outside this fixture.

The matching cache's unchanged `fmodf` now returns the expected raw results
for eight normal-input cases in an isolated User-mode fixture. These cover
smaller/equal/greater magnitudes, both operand signs and signed zero, with
exact stack contents and access counts, callee-register preservation and
NEON stack save/restore. Before Boolean support, the same fixture stopped
at its first VORR. This fixture does not establish process launch or a
complete firmware boot.

A separate fixture completes eight tiny-result paths from the same unchanged
entry, including the multiply at `0x3929abd2`. Each returns the signed zero
required by NEON flush-to-zero, with UFC set and exact stack accesses and
register restoration. The earlier prefix fixture established D16/D17 at
the multiply; the full-function baseline stopped on that instruction.
These remain isolated host fixtures without a loaded guest process.

A third fixture executes 16 special-input cases through the matching
six-byte Thumb `fabsf` helper on a separate User code page. Signed zeros,
infinities, zero divisors, quiet/signaling NaNs and NaNs paired with
denormals return the expected bits, flags, stack accesses and preserved
registers. The NaN paths execute the formerly unsupported VADD, retaining
the earlier VCMPE status and adding IDC when NEON flushes a denormal input.
Only this exact helper and the original function are prepared for execution.

The unchanged ARM `fmod` at `0x392904b0` also completes 112 finite, nonzero
input cases using existing instructions: 14 analytical pairs with both
operand signs and both FPSCR.FZ settings. Normal, subnormal and large
exponent-gap remainders match exact bit patterns with a 24-byte saved stack
and preserved registers. Tiny computed results survive with FZ clear and
flush with UFC set when FZ is set; an integer early return preserves its
input without consulting FZ. These isolated results exercise the existing
lower-bank VFP arithmetic. The mathematical contract follows
[Apple's fmod(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/fmod.3.html).

The cache's Cortex-A8-selected `memset` body now completes 21 bounded cases
from its unchanged Thumb entry at `0x391ce5f8`. Tests check every destination
byte and access count, untouched surrounding bytes, saved registers and
exact stack accesses across empty, short, misaligned, vector and large-block
fills. The baseline stopped at VDUP on its first vector case. Selection is
a static resolver/pointer audit using Apple's
[Cortex-A8 to ARM_13 mapping](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.41.3/osfmk/arm/cpuid.c);
the fixture calls the selected body directly without a commpage or dyld.

The same static audit resolves `memcpy` and `memmove` to a shared Thumb body
at `0x391ceb00`. Its unchanged bytes now complete 30 bounded cases covering
empty copies, equal pointers, alignment, forward copying and both overlap directions.
Unaligned vector paths execute VEXT and reproduce an independent initial-buffer
copy, with one write to each destination byte and preserved surrounding data
and registers. Source padding for the aligned vector reads is explicitly
prepared and bounded. These cases use lengths below 1024 bytes and exclude
the separate large-block/stack paths. They remain isolated User-mode fixtures.

Eight additional cases exercise the same copy body's large-block paths with
1024–1536-byte buffers, including overlap in both directions. Every requested
source byte is read once, every destination byte is written once, and the
24-byte stack save/restore is checked. R9 is checked against the last loaded
source word: Apple's [ARMv6 ABI](https://developer.apple.com/documentation/xcode/writing-armv6-code-for-ios)
makes it volatile in iOS 3 and later, and the
[ARMv7 ABI](https://developer.apple.com/documentation/xcode/writing-armv7-code-for-ios)
inherits those core-register rules. Both observed PLD forms already execute
as functional hints; these checks establish no cache timing.

The cache's selected ARM `memset_pattern16` entry at `0x391d5fc8` and its
shared fill body complete 560 cases with unchanged instructions. They cover
lengths 0–31, all 32 destination alignments with all 16 tail lengths at
64–79 bytes, and 16 larger fills. Checks compare the repeated pattern and
truncated tail independently, including unaligned pattern pointers, exact
source-read counts, untouched surrounding bytes, saved registers and the
32-byte stack. These inputs follow Apple's
[pattern-fill contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/memset_pattern16.3.html).
Both copy and pattern fixtures use synthetic User mappings and direct entry;
they do not establish resolver execution, a process or a boot.

ARM and Thumb VLDR/VSTR now move one S or D register through the translating
memory accessors, including D16-D31. They use signed, scaled immediate
offsets and require word alignment even for doubleword registers. Thumb
literal loads use aligned PC+4; ARM PC bases use PC+8, and Thumb stores
with PC as their base are refused. These rules follow
[DDI0406C.b, A8.8.333/413](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Access denials enter the guest Undefined handler before memory access.
Big-endian transfers remain explicitly unsupported.

Tests cover every S/D register, offset limits, PC/SP bases, conditional
execution and access-denial priority. Page-crossing tests use separate
physical frames with host memory shortcuts enabled and disabled. For the
partial effects left UNKNOWN by
[DDI0406C.b, B1.9.8](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92),
this implementation preserves a failed load's destination and retains any
completed first store word. Alignment, translation and
permission faults preserve the base and report the faulting address, access
direction and exception state.

VLDM/VSTM now cover the full bank in ARM and Thumb, including VPUSH/VPOP.
They support increment-after with optional writeback and decrement-before
with writeback, with at most 16 consecutive D or 32 S registers. PC bases
are permitted only in ARM without writeback. The deprecated odd-length
doubleword form is restricted to D0-D15: its trailing word reserves address
space without a memory access, while writeback includes that word. See
[DDI0406C.b, A8.8.332/367/368/412 and A8.8.50](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

On a synchronous data abort, multiple transfers restore the original base,
as required by
[DDI0406C.b, B1.9.8](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Completed register loads or stores remain; the failed D-register load keeps
both original halves. Tests cover complete register lists, both stack
aliases, invalid modes/ranges, access/conditional behavior and faults after
partial progress, including with host memory shortcuts enabled. A separate
test places the odd-length trailing gap on an unmapped page. Upper-bank
VFP arithmetic beyond VADD/VSUB/VMUL/VNMUL/VDIV/VSQRT/VMLA/VMLS, F32/F64 precision
conversion and 32-bit integer conversion, and the remaining NEON families,
are still unfinished.

An isolated host fixture executed the matching kernel's
`_enable_kernel_vfp_context` routine with unchanged instructions and synthetic
context objects. All eight owner/enable/interrupt-mask cases passed. The
save path preserved all 32 D registers and saved their exact words; the
previous core library stopped on its first D16-D19 store at `0x80088d4c`.
This verifies the routine in that fixture, without establishing a scheduler,
full context switch or board boot.

Cortex-A8 L2 auxiliary control (`p15,1,c9,c0,2`) now stores its defined
fields separately from ARM1176 CP15 state and resets to `0x00000042`.
The implemented configuration has no L2 parity/ECC RAM, so bit 21 stays clear
after writes, as specified by
[DDI0344K, section 3.2.55](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde).
This is a CPU configuration, not evidence of the S5L8920 cache integration.
Cache allocation and latency controls persist; coherent synchronous memory
has no dirty cache lines, timing model or error-protection unit. Nonzero
reserved-bit writes are refused before mutation. Execution remains in the
Secure reset state, with privileged access only; SCR, SMC and Monitor-mode
transitions are refused. Nonsecure execution still requires implementation.
Legacy snapshot bytes and version stay unchanged, with inactive L2 state
cleared on ARM1176 restore and Cortex-A8 save/load still refused.

Cortex-A8 Thumb MRC/MCR transfers use the same checked CP15 selector and
access path. Both halfwords must be fetched before any register changes;
Thumb additionally forbids SP in the transfer register. Refused accesses leave
flags and IT state unchanged. IT conditions, privileged
state changes and permitted User thread-ID/barrier accesses retain their
normal semantics. CP14 and Swift CP15 transfers remain unsupported in
Thumb; supported VFP instructions cover the A8 transfers, raw data operations,
comparisons, VADD/VSUB/VMUL/VNMUL/VDIV/VSQRT/VMLA/VMLS, F32/F64 precision conversion
and 32-bit integer conversion above.
CP15 MRC2/MCR2 encodings are undefined and refused. The ARM1176
instruction path is unchanged.
See [DDI0406C.b, A8.8.98/107](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
This adds transfer execution without supplying unverified CPU identity.

Cortex-A8 WFI now executes in ARM and both Thumb encodings, including User
mode and conditional IT slots. It calls the existing synchronous platform
wait hook unless IRQ/FIQ is already pending or ACTLR.WFINOP is set. Interrupt
masks affect subsequent exception delivery, not whether WFI wakes. Retirement
and IT progression finish before the next step takes an interrupt, preserving
the correct return address and saved state. A missing or nonprogressing hook
permits early completion, as allowed by
[DDI0406C.b, A8.8.425/B1.8.14](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests cover synchronous platform work, pending and newly asserted IRQ/FIQ,
masked continuation, conditional skips and second-halfword fetch faults.
Other profiles and the legacy CP15 WFI path retain their behavior. This is a
CPU wait mechanism; S5L8920 device timing and complete sleep/wake remain work.

The N88 tree specifies three PL192 banks at `0xbf200000`, with a `0x10000`
stride. Its `vic,pl192` compatible string matches the prelinked
AppleARMPL192VIC personality. A separate `pl192` component implements programmed vector values,
all 16 priority levels, priority masking, nested acknowledge/end-of-interrupt,
IRQ/FIQ routing and the documented VIC0 blocking daisy interface. Status
registers retain pending sources while priority masks suppress IRQ delivery.
Protection gates User register access, and unsupported accesses are refused
without changing state. Its register identity is the revision 0, 32-source
configuration documented in
[DDI0273A, chapters 2 and 3](https://documentation-service.arm.com/static/5e8e218b88295d1e18d37489);
the N88 controller revision is not established by that manual.
The component uses stable logical input levels; bus/synchronizer latency,
integration-test circuitry and a complete S5L8920 machine are separate work.
The existing S5L8900 controller is unchanged. A host regression executes
guest vector programming, WFI, three-controller IRQ dispatch, source masking,
end-of-interrupt and exception return. This is component execution evidence,
not an iOS kernel boot.

The separate `s5l8920` fabric now supplies the matching 256 MiB RAM aperture
at `0x40000000` and connects all three PL192 banks to its Cortex-A8 IRQ/FIQ
inputs. It exposes only each controller's first 4 KiB; the gaps between banks,
legacy S5L8900 addresses and other peripherals stop through the checked bus.
The first failure retains physical address, width, direction, write value and
CPU PC. RAM access is little-endian and bounded against address/size overflow.
Functional reset preserves RAM and external input levels while resetting CPU
and controller state. Reset PC remains zero, so missing ROM stops explicitly.

The standalone PL192 protection logic is not enabled in this fabric: the CPU
bus has no privilege sideband for unprivileged transfer instructions or page
walks, so inferring access privilege from CPSR would be incorrect. Protection
register accesses and unverified board identity registers stop. Timing, power
sequencing, CPU revision/ECC selection, firmware handoff, snapshots, storage
and the remaining devices still require implementation or evidence. This
fabric is not selected by the existing application. Synthetic host tests cover
CPU IRQ/FIQ entry, translated guest acknowledgement/EOI and exception return;
they do not establish a complete firmware boot.

An isolated matching-firmware fixture also runs the unchanged N88 vector
initialization, unmask and unregistered-handler methods through this fabric.
Prepared page tables map the original virtual code/object addresses into its
physical RAM. All 96 sources pass both held and withdrawn-at-acknowledgement
cases, with 62/81/119 handler steps and 2/3/5 MMIO accesses by source bank.
The fixture checks descending EOI writes, cleared service levels and final
CPU IRQ line state. It uses synthetic objects, explicit method entry and masked
interrupts; it does not demonstrate kernel driver instantiation, a registered
callback, a firmware IRQ-vector path or physical controller timing.

Cortex-A8 access-flag faults are not cached. After an AF-clear descriptor
faults, software can set its flag and retry without a TLB invalidation, as
required by [DDI0406C.b, B3.7.4](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
The previous fault cache returned the stale fault on that retry. Tests cover
sections, supersections, small and large pages, all access kinds and domain
tags, repeated AF-clear walks and subsequent caching of valid translations.
The legacy CPU profiles retain their existing cache policy.

Thumb-2 framing fetches both halfwords and retires once. The second halfword
is translated independently, including across noncontiguous physical pages.
A fetch fault there vectors before any instruction result is committed.
MOVW and MOVT are implemented with the split immediate and ARMv7 register
restrictions from DDI0406C.b A8.8.102/106. Unimplemented wide operations stop as
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

Cortex-A8 ARM and Thumb now share byte, halfword, word and doubleword
exclusive accesses, with Thumb CLREX and scaled word offsets. Register
validation follows [DDI0406C.b, A8.8.32/75–78/212–215](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92),
including independent Thumb doubleword registers and permitted SP bases.
Only naturally aligned Normal memory is prepared; multi-byte big-endian
accesses remain unsupported. A cleared local monitor makes a store fail
before alignment checks or translation, as specified for Cortex-A8 in
[DDI0344K, 8.5.2](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde).
ARM1176 retains its existing exclusive-access path.

The A8 monitor records the translated physical address and transfer size.
Matching ARM/Thumb pairs can share the claim; a physical remap or different
size cannot reuse it. Completed store-exclusive attempts, CLREX and guest exceptions clear the
claim. Tests cover register aliases, all scaled offsets, failed-monitor
stores to invalid addresses, permission/alignment faults, memory-type
refusals and instruction conditions. Checked failures during either fetch
halfword, either table-walk level or either data word preserve registers and
the monitor for an explicit host retry. Completed physical store words
remain while the machine is halted. The synchronous interpreter has no
second guest observer between doubleword accesses; DMA and multiprocessor
global-monitor behavior remain unimplemented. The added size byte occupies
existing structure padding and is cleared, not serialized, by the unchanged
ARM1176 snapshot format.

The unchanged cache `OSAtomicAdd64` routine at `0x391d4668` now returns in all
16 isolated cases. Eight carry/wraparound inputs take 13 instructions; the
same inputs take 21 after the fixture explicitly clears the monitor once
before the first STREXD, forcing the guest's retry loop. Checks cover the
64-bit result, exact cell and stack accesses, preserved registers and final
monitor state. The baseline stopped at its first LDREXD after three steps.
This is direct User-mode routine execution with synthetic mappings, without
a guest scheduler, real contention, DMA, process launch or full boot.
After this change, a fresh guarded kernel-entry run also reproduced the
complete archived trace through the same 61,650-step L2 ECC configuration stop.

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
access; base/register overlap and invalid source registers are refused
before accessing data. Unprivileged encodings use their separate decoder.
PC loads use the
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
the hinted address is unmapped. Register-offset and unprivileged forms are
described below.

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

Thumb LSL/LSR/ASR/ROR with register-supplied shift counts now use the low
byte of the count register. Zero leaves the value and carry unchanged;
larger counts follow the architectural shift/rotate rules. The explicit S
bit controls NZC updates, including inside IT; V and other flags remain
unchanged. SP and PC operands are refused. See
[DDI0406C.b, A8.8.17/95/97/150](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests compare every count against repeated one-bit operations and cover
overlapping operands, conditional skips, complete instruction fetch and
legacy framing.

Thumb CLZ and RBIT now count leading zero bits and reverse a 32-bit word,
respectively. They preserve flags, support overlapping operands, and validate
both encoded copies of the source register before changing state. SP/PC and
inconsistent source fields are refused, following
[DDI0406C.b, A8.8.33/144](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests cover zero, every single-bit position, mixed patterns, IT conditions,
separately mapped instruction halves and ARM1176's existing framing.

Thumb-wide REV, REV16 and REVSH reverse the word's bytes, reverse bytes
within each halfword, or reverse and sign-extend the low halfword. The
decoder shares the duplicated-source and SP/PC checks with RBIT; it reads
the source before writing an overlapping destination and preserves flags.
See [DDI0406C.b, A8.8.145..147](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests use independent byte selection across all byte values and all register
pairings, including aliases, invalid operands, IT skips and legacy framing.
Guest translation/permission/XN faults and checked-bus failures verify that
both instruction halves are fetched before effects or condition evaluation;
an owner-cleared bus failure permits one complete retry.

An isolated fixture runs the unchanged 82-byte Thumb `memcmp` at
`0x391d214c..0x391d219e` from the verified N88 cache. Before wide REV,
the first aligned four-byte difference stopped after 13 instructions at
`0x391d216c`, having read four bytes from each input. With REV, all 1,458
prepared calls return and match an independent unsigned-byte comparison
sign. They cover selected lengths 0..257, every pair of byte alignments,
equal inputs, and first/middle/last/two differences, including embedded NULs
and high-bit bytes. The fixture checks the exact consumed prefix of each
separate User buffer, whole-page canaries, an eight-byte saved frame,
preserved general/FP registers and FP status. It does not establish aliased
inputs, process execution, a complete boot or physical execution.

The unchanged ARM `strlen` at `0x391d2d58..0x391d2db4` also passes 1,332
isolated calls using existing UQSUB8, REV and CLZ support. Cases include
selected lengths 0..511, all byte alignments, every nonzero byte value,
three padding patterns and terminators at the end of a User page. The
fixture explicitly prepares up to three bytes before the input and after
its terminator for aligned word loads, and checks each consumed byte once,
the literal read, eight-byte saved frame, canaries and preserved registers.
This is routine-level evidence with synthetic mappings; it adds no CPU
behavior and does not establish process or boot execution.

Thumb PKHBT/PKHTB combine halfwords after shifting the second operand.
The dedicated decode validates S/T fields and excludes SP/PC, then uses the
existing A32 packing arithmetic. Encoded zero means no shift for PKHBT and
ASR by 32 for PKHTB. All shifts, sign boundaries, aliases, IT behavior and
split instruction fetches are tested against individual source-bit selection.
See [DDI0406C.b, A8.8.125](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Thumb TBB and TBH read an unsigned byte or halfword from a table and
branch by twice that value. A PC table base means the instruction address
plus four without rounding to a word boundary. Index and destination
arithmetic wrap at 32 bits. Within an IT block they require its final slot;
failed conditions suppress table reads. Guest faults save the current IT
state for retry, and checked-bus failures halt without retirement.
The target is fetched on the following step. Tests cover
these boundaries, translated tables, permissions and checked-bus retry.
See [DDI0406C.b, A8.8.236](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Unaligned TBH raises an alignment abort when SCTLR.A is set. With A clear,
Cortex-A8 now reads two bytes in address order when each translation has a
validated Normal-memory type. Each byte uses its own translation, including
across page boundaries and 32-bit address wrap. A second-byte fault preserves
the first completed bus read but does not publish the branch destination or
advance IT state. Checked-bus failures require an explicit host retry.
Device and Strongly-ordered unaligned accesses are unpredictable on processors
without virtualization extensions; they stop before the affected byte's data
access. No virtualization-specific alignment fault is fabricated.
Unaligned TBH remains unsupported on Swift; big-endian TBH remains unsupported
on both profiles. These rules follow
[DDI0406C.b, A3.2.2/B2.4.5](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

The Cortex-A8 translator retains the memory type alongside each cached
physical mapping. Sections, supersections, large pages and small pages use
the documented TEX/C/B encodings without remapping. Ordinary translations
populate the metadata too; a later type query uses that same cached mapping
until invalidation. Reserved and undetermined encodings, extended physical
addresses, legacy descriptor formats, TEX remapping and big-endian page tables
are not certified for type-dependent accesses. With the MMU disabled, A8 data
is Strongly-ordered and instruction fetches are Normal. See
[DDI0406C.b, B3.2.1/B3.8.2](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests cover all 32 attribute encodings in all four descriptor forms, cached
mapping changes, profile/control changes, faults and invalidation, plus TBH
byte ordering and retry. Derived metadata is cleared on reset/restore and
omitted from ARM1176 snapshots; the version and serialized fields are unchanged.
This type check serves unaligned TBH, register-offset halfword loads and
unprivileged halfword/word transfers. Other shared load/store
paths do not yet enforce these memory types, and cache timing, shareability
and the complete Cortex-A8 MMU remain separate work.

A private fixture executes the unchanged matching AppleSamsungSerial baud
method with explicit synthetic objects, MMU mappings and clock inputs. It
captures only the method's four expected writes and refuses every UART read;
there is no UART device behind this fixture. With a 100 MHz input argument
and baud argument 230400, the first unsupported instruction was Thumb CLZ
at `0x8083cbaa`, after 59 steps; adding CLZ/RBIT exposed Thumb PKHBT at
`0x8083c59c`, after 1,148 steps. With packing implemented, that method returns
after 1,760 steps and writes UFCON `0x1c1`, UBRDIV `0x35`, and both offset
registers `0x1000`. All six combinations of input clocks 100/24 MHz and baud
arguments 230400/19200/921600 return with the four expected writes in order.
These are recorded method outputs from prepared inputs. They do not establish
complete driver setup, selected board clocks, UART traffic or boot.

A separate fixture enters its unchanged enclosing line-configuration method
at `0x8083c388`, with prepared UCON `0x405` and ULCON `3` input values. Before
TBB support it stopped after 14 steps at `0x8083c3ac`. All five parity-argument
cases now return through the real baud method with the expected two reads and
seven writes, including line-control values `0x23/0x2b/0x33/0x3b/3` and restored
control `0x405`. The fixture supplies synthetic objects and the exact two
input reads, refuses other accesses, and does not implement a UART device.

Thumb register-offset STRB/STRH/STR add the full offset register shifted
left by 0..3, with no writeback or flag changes. Valid operands can alias;
SP is allowed as the base and as a word-store source, but never as the
offset register. PC operands and reserved offset fields are refused. The
common memory path handles alignment and translation faults, including
separately mapped pages and completed bytes before a later fault. Multibyte
big-endian accesses remain explicit capability stops. See
[DDI0406C.b, A8.8.205/208/218](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests cover full-width offsets and address wrap, aliases, IT, data faults,
complete instruction fetch and legacy framing with host RAM access enabled
and disabled. An unchanged matching-driver fixture now passes the PL192
unmask method's `LSL.W` at `0x807e5d80` and handler's register-offset
`STRB.W` at `0x807e5ca2`. Thumb barriers also pass its `DMB ISH` at
`0x807e5caa`, allowing all 64 isolated cases to complete: each of 32 local
sources held active or withdrawn after acknowledgement. The real guest
methods initialize a vector, unmask the source, read its vector and issue
end-of-interrupt. Checks verify those accesses, the resulting IRQ level and
cleared service records. The fixture uses synthetic objects, unregistered
records and one controller bank; it does not establish CPU interrupt entry,
registered callbacks, daisy-chain wiring or board boot.

Thumb register-offset word LDR now uses the full offset register shifted
left by 0..3 and permits aliased base, offset and destination registers.
SP can be the base or destination. Loads to PC enforce word-aligned data
addresses, ARM/Thumb interworking and final-slot IT placement; Rn=PC still
selects the earlier literal decoder. Reserved fields, SP/PC offsets and
big-endian data accesses remain refused. See
[DDI0406C.b, A8.8.65](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests cover address wrap, populated read caches, operand restrictions,
complete fetch, and data faults before destination commit, including
aliased destinations and nonadjacent physical pages.

A separate registered-path fixture retains guards on every missing callback
argument and function pointer. It now passes the unchanged driver's
`LDR.W r0,[r1,r5,LSL #2]` at `0x807e5cca`, reads the matching IPI mask and
clears the injected software interrupt. It stops before the first guarded
callback argument read at `0x807e5ce0`. This is partial execution of a
synthetic registered record, with no callback or CPU IRQ-entry evidence.

Thumb register-offset LDRB/LDRH/LDRSB/LDRSH also use full-width offsets shifted
left by 0..3, without writeback. Byte and halfword results are zero-extended
or sign-extended as encoded, after the complete read succeeds. SP is a valid
base; SP destinations and SP/PC offsets are refused. PC bases keep their
literal interpretation, and PC destinations select memory hints. PLD/PLI
and Swift's PLDW validate their offset registers but perform no data access;
A8's PLDW and signed-halfword PC encodings are unallocated hints and act as
NOPs before interpreting those registers. See
[DDI0406C.b, A6.3.8/9 and A8.8.70/82/86/90/128/130](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
The Swift profile's MP extension agrees with
[LLVM's processor definition](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/ARM/ARMProcessors.td).

Unaligned register halfword loads share TBH's bounded Cortex-A8 Normal-memory
path. They check alignment before translation, preserve a completed first
byte across a second-byte fault, and leave the destination unchanged on a
fault or capability stop. Swift unaligned register halfword loads and all
big-endian register halfword loads remain unsupported; byte loads are endian
independent. Existing immediate/literal load paths retain their documented
memory-type limitations. Tests cover shifted offsets and wrap, extension
boundaries, aliases, IT execution and skips, hint allocation, separately mapped
instruction halves, memory permissions, partial reads and explicit host retry.

A private fixture executes the matching kernel's unchanged `strncat` routine
at `0x8027034c` with bounded buffers, stack and Normal-memory mappings.
Previously it stopped on its 14th instruction, the register-offset `LDRB.W`
at `0x8027035e`. All eight cases now return in 19..59 instructions, covering
empty strings, zero and truncated counts, longer limits and high bytes.
Checks verify the entire destination buffer, unchanged source, return/stack
state, and exact data-access and executed register-load counts. Every
unprepared bus access stops. This is isolated real guest code; it does not
establish kernel boot, device behavior or physical execution.

Thumb LDRT/LDRBT/LDRHT/LDRSBT/LDRSHT and STRT/STRBT/STRHT now add an
unsigned byte offset without writeback. Their memory accesses use User
permissions, including when privileged direct-data caches have been populated;
the current CPU mode and register banks stay unchanged. SP is a valid base,
but SP/PC data registers are refused. PC load bases retain their ordinary
literal interpretation and actual privilege; PC store bases are undefined.
Invalid nonliteral LDRT-to-PC encodings are checked only when the IT condition
passes, while literal PC loads retain their final-slot requirement. These
rules follow [DDI0406C.b, A8.8.71/83/87/91/92/209/219/220](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Unaligned transfers use the same bounded Cortex-A8 Normal-memory path as
TBH. Each byte is translated with User permissions in address order. A later
fault leaves completed store bytes visible and preserves load destinations,
base registers and saved IT state. Unsupported memory types stop before the
affected data access. Swift unaligned transfers and multibyte big-endian
transfers remain unsupported. Tests cover both profiles, User/SVC/System/FIQ
modes, cached permission checks, AP/APX combinations, offsets and wrap,
register aliases, literal precedence, split instruction fetches, access-flag
and alignment faults, partial stores, and explicit checked-bus retry.
ARM1176 framing and A32 unprivileged addressing remain unchanged.

Thumb UBFX/SBFX and BFI/BFC implement bitfield extraction, sign extension,
insertion and clearing. They validate ranges before shifting and preserve
flags and unrelated destination bits. Tests compare every encoded field
range against a bit-by-bit reference, including full-width fields, register
overlaps, invalid SP/PC operands and reserved encoding bits.

A32 BFC/BFI/SBFX/UBFX now use the same bit-selection arithmetic behind an
explicit Cortex-A8/Swift feature gate. ARM1176 continues to refuse them.
The ARM forms permit SP operands, reject PC destinations and extraction
sources, and interpret an insertion source of PC as BFC. Failed conditions
suppress even invalid fields or operands. Tests cover all field encodings,
source/destination overlap, sign boundaries, register roles, condition skips,
flags and exclusive-state preservation, with host fetch caches enabled and
disabled. These distinctions follow
[DDI0406C.b, A8.8.19/20/164/246](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
The matching kernel's `initcode` contains `BFC r0,#8,#8` (`0xe7cf041f`) at
`0x802b9adc`; that exact encoding is covered by the regression. This is
instruction-level evidence, not execution of the enclosing boot routine.

Thumb MUL/MLA/MLS, SMULL/UMULL, SMLAL/UMLAL and UMAAL preserve flags and
support source/destination overlaps. Standard long forms reuse the existing
A32 arithmetic after validating Thumb register constraints. UMAAL adds the
two destination words separately. Tests use an independent shift/add product
reference and cover signed extremes, accumulation carry/wrap, nonadjacent
destination pairs, IT execution/skips and reserved or forbidden operands.
Separate DSP multiply and Thumb divide encodings remain unsupported.

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
  literals, doubleword transfers, A8 exclusive accesses, extend/add forms, bitfields and word/long
  multiply/accumulate, CLZ, RBIT, REV/REV16/REVSH and PKH. Wide B/BL/BLX, CBZ/CBNZ and IA/DB multiple
  transfers, narrow register loads, TBB and bounded TBH are also implemented. Other instruction
  families remain to implement. IT state
  and conditional execution are implemented for the supported Thumb families.
  Cortex-A8 MRC/MCR transfers cover the currently implemented CP15 registers.
- Cortex-A8 stores d0-d31 and supports system/core/memory transfers plus
  VFP copies, immediate constants and sign operations with short vectors,
  scalar comparisons, F32/F64 precision conversion, 32-bit integer conversion and
  VADD/VSUB/VMUL/VNMUL/VDIV/VSQRT/VMLA/VMLS across the full register bank, and the bounded
  32/64-bit NEON VLD1/VST1 and 32-bit VLD2/VST2 memory forms,
  register Boolean operations, VEXT, 8/16/32-bit VTRN, F32 VABS/VNEG,
  immediate constants, core-register VDUP, register VMUL/VADD/VSUB/VMLA/VMLS.F32,
  F32 VMUL/VMLA/VMLS by scalar, and F32/signed/unsigned 32-bit NEON conversion
  described above.
  Other upper-bank VFP arithmetic, the remaining NEON families, and full
  context-switch semantics remain to implement. Remaining shared lower-bank
  arithmetic derives from VFP11 and requires a Cortex-A8 semantic audit.
- The partial S5L8920 RAM and interrupt fabric does not yet supply its UART,
  clocks, storage, graphics, input or power devices. Build those components
  from the N88 firmware requirements.
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
`0x8008b968`, halfwords `fba6 0101` (`UMULL r0,r1,r6,r1`). Multiply execution
then reaches 75,770 steps at `0x80088278`, halfwords `ee10 0f10`
(`MRC p15,0,r0,c0,c0,0`). Thumb CP15 transfers were unsupported at that point;
they now execute, while the CPU identity itself remains unimplemented. A subsequent
CP15 selector audit invalidates that trace as the current execution boundary:
the old path silently returned zero for an earlier L2 auxiliary-control read.
With explicit Cortex-A8 refusal, the current probe stops after 61,644 steps
at physical `0x40086348`, A32 `ee39bf50` (`MRC p15,1,r11,c9,c0,2`). That
register requires implementation before returning to the later CPU-ID read.
With L2 auxiliary-control storage implemented, the guarded diagnostic reaches
61,650 steps at physical `0x40086360`, A32 `ee29bf50`. The kernel requests
`0x10600000`, including ECC enable. The diagnostic stops before that write:
the current CPU configuration has no parity/ECC RAM, and neither the kernel's
request nor iBoot's bit-25 write-combining toggles establishes its presence
in S5L8920. This remains a hardware-configuration gap, not a passed boot stage.
Unprepared tree properties and remaining argument fields retain their guards.

Host tests, exact-commit builds, firmware analysis, guest boot traces, and
physical app behavior are separate evidence. No iOS 6 boot or usability claim
follows from passing instruction tests.
