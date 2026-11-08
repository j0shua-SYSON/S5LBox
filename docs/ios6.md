# iPhone 3GS / iOS 6 bring-up

The initial iOS 6 target is iPhone 3GS running 6.1.6. The foundation includes
a distinct Cortex-A8 instruction profile and partial S5L8920 memory,
interrupt fabric, UART, timebase counter, deadline timer, GPIO, I2C and
bounded PMU clock/control/ADC/configuration/event/boot-state/voltage endpoints, explicitly
configured identification words and
partial clock-gate/selector programming, a PLL model with explicit clock inputs,
and bounded POWERID software-cache fields. Inactive GPIO data readback can
be supplied explicitly, separately from live pin samples. MIU control can
select the existing physical RAM boot window from original firmware.
Six USB clock/PHY control words accept explicit initial values and bounded
guest programming. USB status and transfers remain unavailable.
All five UART banks now support the bootloader's line/FIFO configuration,
explicit initial divisors, receive interrupts and supplied receive-timeout
events. Automatic transmit flow control requires an explicit CTS observation.
Three audio NCO words support bounded clock configuration and coefficient
readback; audio output and the remaining audio registers are unavailable.
The AES key-disable latch accepts explicit initial state and sticky disables.
An optional CDMA component executes the bootloader's polled RAM-to-RAM
AES-CBC path with a supplied software key; hardware keys remain unavailable.
Its NAND receive channels also move explicitly supplied peripheral bytes through
validated descriptor chains. A separate FMI producer clocks backed sector bytes
into explicitly configured staging FIFOs for those channels.
A complete machine, kernel boot, and SpringBoard have not
been demonstrated. The existing iPhone
OS 3 machine and application defaults remain ARM1176/S5L8900.

## Optional AMC software control component

A standalone AMC r2 component supports the start-address register and its
software status latch, plus two interrupt-mask banks. Its owner supplies four
initial read words and a powered, idle context; there are no silicon reset
defaults. Readback bits outside the supported fields remain the supplied,
stable observations. Host reset restores those explicit inputs. Repeating an
identical configuration preserves current state; conflicting inputs refuse.

Aligned word accesses support start address `0x988` (low 16 bits), commands
`0x990` and `0x994` (value 1 only) to set and clear status `0x998` bit 0, and
interrupt-mask set/clear/read at `0xa8c/0xa90/0xa94` (low 30 bits) and
`0xb0c/0xb10/0xb14` (all 32 bits). Mask writes affect only written one bits.
Unsupported widths, directions, fields and addresses preserve state and
refuse. Initial readback and reserved-bit values are explicit inputs, not
values inferred from the software's masks.

The matching 6.1.6 AMC driver provides start-address read/write helpers and
built-in tests that compare two programmed addresses, exercise the status
clear/set/clear sequence, and verify both interrupt-mask banks. The r2 setup
passes flag 1 to its runtime record; the corresponding helper masks the first
bank to 30 bits. The earlier revision's 27-bit mask is not used as the r2
field width. Original program-memory queries independently require hardware
offset and size words spaced `0x14` bytes apart; their values remain unknown.

This component has no board attachment, clock-gate coupling, pending interrupt
causes or IRQ output. Engine-start `0x984`, doorbell `0x98c`, reset `0x99c`,
completion status, FIFO, program memory, DMA and shared audio control remain
unsupported. Software control-register tests do not establish engine
execution, audio output, or further progress through the full kernel boot.

## Optional AMC internal-memory DMA component

A separate channel-0 component copies backed memory from validated descriptors
in link order. The owner supplies the powered context, writable memory, up to
three disjoint internal-memory regions, and full idle, active and stopped
status words. The low three status bits are constrained to 0, 1 through 6,
and 7 respectively. The exact active encoding and other readback bits remain
explicit inputs. No reset values, memory-window capacities or clock source
are inferred from this interface.

The matching driver's DMA test uses channel offsets `0x00` for submission,
`0x10` for commands, and `0x14` for status. Command 4 stops an idle channel;
command `0x10` resets a stopped channel. Submission accepts an internal
descriptor offset tagged with low bits 2. Its four words contain a tagged next
pointer, control, source offset and destination offset. Supported control
forms use internal source/destination modes 1/1 and a word count in bits
31:18. For internal-memory links, the normal driver uses zero flags and burst
field.
A terminal descriptor may also use flags `0x20` with burst zero or `0xf80`,
the two forms used by the DMA test. The role of `0x20` on an intermediate link
is unknown and remains unsupported. A zero next pointer ends the chain.
Lengths are 1 through 16,383 words per descriptor; all descriptor and data
spans must fit the supplied regions. Admission is bounded to 64 descriptors
as a host resource limit, not an inferred silicon capacity.

Submission validates and latches the entire chain without copying bytes. Each
explicit service call with an available clock copies whole words, bounded
by the caller's budget, 16 bytes and the current descriptor's remaining data.
Reads never advance a transfer. The channel stays active between descriptors
and returns to supplied idle status only after the last descriptor finishes;
`completed` records that host-side event. This scheduling is functional and
does not establish hardware timing. Host reset cancels work while preserving
already copied bytes and the configured backing. Later copies can consume
data produced by earlier descriptors. Cycles, overlapping descriptor headers,
data overlapping any header, source/destination overlap within one descriptor,
external memory, other flags/modes/channels and active stop/reset commands
refuse without mutation. An invalid later link rejects the whole submission
before any payload write. Latched metadata prevents subsequent descriptor
edits from redirecting pending work.

The original DMA test block constructs its own descriptor, submits it, polls,
compares 512 destination words and checks a trailing guard. It passes with
this component using two memory placements, both descriptor forms and two
service budgets. Withheld clocks leave it polling with no copied bytes;
missing destination backing refuses submission. The harness supplies map
returns, window registers, software-list BSS and status observations and
enters after the logging call. This is not the complete self-test dispatcher
or driver initialization. The component has no board attachment, IRQ cause
mapping, DSP execution or sound output, and does not advance the full boot.

An additional witness uses the original normal-driver allocation and
descriptor/list helpers to build two linked transfers, then enters the
original submit, poll, comparison and guard block. Both zero-flags and
supported terminal-flag forms copy the complete 2 KB. Withheld clocks leave
the channel polling; missing later data or metadata, a cycle, or unsupported
intermediate flags refuse submission before copying the valid first entry.
The boundary inputs are explicit fixtures; this does not establish complete
normal-driver initialization or execution of its control-register DMA forms.

## Optional SDHC reset and clock component

An explicitly initialized SDHC component models a bounded SDHCI-compatible
reset and preparation-register subset. Software-reset byte `0x2f` latches
all, command or data reset requests; reads never complete them. Explicit
owner service with an available controller clock performs the reset. All
reset clears every modeled register, while command/data resets clear their
separate normal-status domains and preserve preparation, error and enable
state. This is functional scheduling, without a measured reset duration.
No command execution, card responses, present-state samples, capabilities or
interrupt wiring are supplied. Unsupported accesses refuse.

Clock-control halfword `0x2c` stores internal-enable, card-clock request and
the older eight-bit divider, bounded to bypass and power-of-two divisors
2 through 256. The stable bit is read-only; register reads and writes never
establish readiness. Enabling, disabling or changing the divider clears it.
A separate explicit clock-service call establishes readiness only when the
internal clock is enabled and its owner supplies an available source. Source
loss clears readiness while preserving the control requests. Reset service
does not call clock service; pending reset pauses it. All reset clears clock
state, while command/data reset preserves it. No physical settling duration,
frequency, phase, card output clock or card presence is inferred.

The IOP can borrow a separate capability for aligned accesses within
`0x80000000..0x80000fff`, with exact register widths checked by the component.
Refusal stays local to the IOP; callbacks and owners are released on every
return. Earlier standalone service APIs continue withholding this capability.
An optional board attachment now shares one controller between AP and IOP.
The target's `device-clocks` table and original platform methods map logical
SDIO gate `0x10` to raw gate `0x23`, with selector `0x14` and a dependency on
logical gate 1 (selector 2). Register access requires the open raw gate;
explicit reset service also requires a caller-supplied controller clock.
Reads never service resets or alter the AP's exclusive monitor. Board
reset/free detach the caller-owned controller without modifying it. Raw-gate
reset coupling and interrupt routing remain unavailable.
The same gate/fault/reentry checks apply to clock service. A closed raw gate
pauses that service and retains its state, without assuming how the raw gate
is physically coupled to the oscillator.

A control run against the validated component library reproduces all 714
prior conditional trace lines. At IOP `0x79e4`, the guest has opened gate
`0x23` to `0xf` and programmed selectors `0x14=0xd04` and `2=0xe02`. The
original caller at `0x1b7c` requests reset mask 7. The observer reads cached
stack/register state without guest bus accesses or mutation. These values
establish the guest's configuration at that stop, not a physical clock rate
or complete firmware handoff. All earlier audio/PMU/handoff hypotheses remain.

The original IOP reset setter at `0x79e0` previously stopped at its byte read
at `0x79e4`. With this capability, the original setter and getter at `0x79f4`
execute in 128 input/flag combinations, retaining pending state across reads
and a stopped clock, then observing explicit reset completion. The test uses
explicit cold/controller-clock inputs; it does not execute the complete
reset caller or establish whole-kernel advancement. Synthetic tests cover
separate reset domains, width/value refusals, IOP hold/reset and capability
lifetime. The genuine audio-register boundary remains unchanged.

A subsequent conditional run with the board attachment completes reset mask
7 through explicit service, then stops at original IOP `0x7714` reading
`0x8000002c`. It preserves all 544 earlier trace lines before the old stop.
The original `0x7710` setter, `0x7734` stable getter, `0x6fa8` bounded poll and
`0x768c` divider routine establish the next clock-register use. A separate
32-case probe reproduces the clock read refusal before clock implementation.
All earlier audio, PMU, NAND and firmware-handoff hypotheses still apply.

The register layout is checked against the matching target instructions and
the [Linux v2.6.28 SDHCI definitions](https://github.com/torvalds/linux/blob/4a6908a3a050aacc9c3a2f36b276b46c0629ad91/drivers/mmc/host/sdhci.h).
The [Microchip SDMMC reset description](https://onlinedocs.microchip.com/oxy/GUID-4D282FC5-82FC-4934-8BAD-D4A5D8422E6C-en-US-7/GUID-A9EA9B00-152F-4DD1-8092-0422D92B3F8A.html)
provides a generic reset-domain reference. Neither source establishes this
target's physical clock, timing, capabilities or card state.
The [TI SDMMC register reference](https://software-dl.ti.com/simplelink/esd/simplelink_wifi_sdk/10.10.00.18/exports/docs/driverlib/cc35xx/register_descriptions/SDMMC.html)
also distinguishes read-only internal-clock status from enable controls.
Its different offset, divider, clock sources and reset quirks are not imported.

## IOP access to shared NAND controllers

The IOP can access the two explicitly attached FMI controllers through a
capability borrowed for one instruction. Only aligned word accesses in the
existing `0x81200000` and `0x81300000` register windows are forwarded. The
board applies the same attachment, clock-gate, reset and register checks as
its AP path, using the existing controller state. Missing backing and
unsupported accesses stop the IOP with its own first-fault diagnostics;
they do not set the AP bus-failure latch. Standalone service without this
capability still refuses these windows. All callbacks and owners are cleared
when service returns, including instruction and bus failures.

Register accesses do not clock NAND commands, supply data, advance time or
add interrupt routing. They do not count as shared-RAM stores or invalidate
the AP exclusive monitor. IOP board service is refused during a CDMA/FMI
endpoint callback, before either CPU executes.

The matching original IOP instruction at `0x2884`, `STR r8,[lr,#0x400]`,
attempts to enable FMI0 with `0x81200400=1`. An actual instruction regression
failed before routing and now enables the attached controller and retires
once, preserving the AP CPU. Tests also cover both banks, shared readback,
pending commands without endpoints, reset/gating, absent capabilities,
unsupported accesses, first-fault retention and reentry.

A conditional whole-kernel run with explicit cold FMI attachments retired
that original store and reached IOP `0x28b4`, then an unsupported ARMv4T `STRH`.
The IOP retired 202,958 instructions, 268 beyond the previous stop; the AP
reached 324,677,925, an additional 6,700. FMI0 was enabled with the original
zero-mask programming, no pending command, and no supplied NAND bytes. The
first 514 preceding trace lines match the previous run. The experiment keeps
the earlier audio exception and CPU/cache/clock/device hypotheses, and treats
the complete captured NAND device-tree node as final handoff data. These
inputs and cold controllers do not establish a continuous firmware handoff.
Complete iOS 6 boot and usable storage remain unproven.

With ARMv4T halfword transfers implemented, the same inputs execute the
original `STRH` at `0x28b4`, storing `0xffff` at IOP address `0x0171afc8`.
All 518 preceding trace lines match. The IOP reaches 268,669 retired
instructions without a fault, and the AP reaches 326,320,693 before its
word write `0xbf100078=0x500f` is refused at `PC=0x8078a094`. This is clock
gate zero: the original caller clears bit 4, which the earlier model did
not permit changing. That boundary was an AP register refusal; the IOP
remains running at `0x80b0`. FMI0 has no pending commands or supplied bytes.
The same diagnostic hypotheses remain in effect.

With bounded gate-zero idle-control programming implemented, the original
store succeeds with readback `0x500f`. All 536 preceding trace lines match.
The AP reaches 326,349,725 retired instructions, an additional 29,032; the
IOP reaches 269,831 at `0x80c8` without a fault. The next stop leaves an
I2C0 request pending: address `0x74`, register `0x00`, one-byte read,
sequence 5. No acknowledgement, data or completion is supplied. This is
the D1755 address, but the existing bounded PMU endpoints do not serve
register zero. The captured AP is at `0x80084af4`; the pending device request
is a service boundary, not a CPU or MMIO fault. The original capture runner
reported failure because its parser only recognized fault stop labels;
a separate audit verifies this boundary while retaining that failed result.
The earlier diagnostic inputs remain required; genuine boot is unproven.

## Optional polled NAND commands

A separate optional controller now supports the matching iBoot's polled NAND
reset and READID paths on `0x81200000` and `0x81300000`. Original routines
at `0x4ff164b8` and `0x4ff164e8` select raw gates `0x26` and `0x27`; the
reset helper pulses their bit 31. Each controller is borrowed explicitly.
Board reset detaches it, and the existing machine defaults remain unchanged.

The implemented path selects one of eight chip lines, stages command `0xff`
or `0x90`, and queues work through register `0x410`. An explicit service call
invokes a transient device capability. Reset completion requires the endpoint
to perform its reset; READID completion requires eight supplied bytes for the
requested address. Missing or rejecting backing leaves the command pending
without setting completion bits. No IDs, chip count, or absent-chip responses
are installed by default. The original discovery loop also issues commands
with no chip selected. That case requires explicit bus backing for completion
and sampled data; it has no implicit zero/all-ones result. Multiple selected
chips remain unsupported.

Register `0x444` exposes serviced command completion with write-one-to-clear
acknowledgements. Original `0x50`/zero strobes latch one backed ID byte per
explicit service quantum, and repeated reads of `0x448` return that stable
byte. They neither consume additional bytes nor advance device work. Timing
register `0x408` retains programmed configuration; this PIO path requires
zero timing and `0x424=0` for byte transfers. This is functional scheduling,
not measured electrical timing. Reset assertion cancels pending work and
invalidates controller programming; endpoint reset remains a separate NAND
command. Closed gates pause service and refuse MMIO.

Timing writes are also accepted before controller mode is enabled. Original
initialization at `0x4ff026f8` enables the clock gate and writes `0xffff` to
`0x408` before pulsing reset and writing mode `1`. The earlier model rejected
that first timing write. This change preserves the programmed value without
enabling commands or readback. Reset still invalidates it; reset assertion,
pending work, an active byte strobe and callback reentry still refuse writes.

The ECC mask at `0x814` now retains the observed zero programming after mode
enable. Matching controller firmware identifies it by reading `0x810` and
`0x814` for its ECC pending/mask diagnostic. Original iBoot initialization
at `0x4ff037f4` writes zero to the mask before setting its object flag.
Readback requires an explicit write, and reset invalidates that programming.
Nonzero masks remain unavailable. Separate bounded ECC programming,
result-buffer and pending policies are described below. Mask polarity,
ECC processing and interrupt effects are not established; this write neither
completes nor acknowledges a command. Gate, reset, pending-work, active-byte
and callback restrictions still apply.

The optional ECC result buffer at `0x80c` uses disjoint caller-owned storage
selected before guest programming. After mode enable, a word write of `1`
clears queued results and prepares the buffer; other values refuse. Each
word read consumes one explicitly supplied result, with corrected-bit count
in bits 16 through 20 and failure in bit 0. An empty or unselected buffer
refuses access instead of returning a clean result. Full storage refuses
new results without discarding old ones. Reset discards results and preparation
while preserving the host storage selection; explicit init detaches storage.
The capacity and preserved host selection are functional configuration,
not measurements of physical FIFO depth or reset retention.

Original IOP `0x5118` and matching iBoot `0x4ff042e6` write `1` after reset.
The IOP consumer at `0x4968` reads `0x80c` once per result, takes the maximum
corrected-bit count and writes either that count or failure byte `0xff`.
The older [S5L8930 H2FMI model](https://github.com/danzatt/QEMU-s5l89xx-port/blob/919b455415703e6fb0ef10878e0b087ed3d282e4/hw/s5l8930_h2fmi.c)
names its offset `0xc` ECCBUF and clears its buffer on writes. Together these
support the optional clear/consume policy, but do not establish silicon
semantics. Its different aperture and automatic clean results are not used.
No implementation is copied from that model.

This queue performs no ECC computation and supplies no NAND readiness,
completion, status, interrupt, DMA or time effects. A producer must provide
independently backed results; fixtures may supply explicitly labeled inputs.
There is no production ECC producer yet. Pending command/data or byte-strobe
activity refuses clears. AP and IOP access share the queue, retain gate and
reset checks, and refuse during endpoint callbacks. Pending access has a
separate optional policy.

The pending register at `0x810` supports an explicitly selected latch for
the observed `0x68` bit set. Selection and controller reset leave these bits
unknown. Reads refuse until every supported bit is established. Guest
write-one-to-clear acknowledgements establish cleared bits; independently
supplied events establish set bits. A zero write or event establishes
nothing. Unsupported bits refuse atomically. Pending command, byte and data
work continue independently; acknowledgement does not complete or cancel them.
Reset retains policy selection while invalidating pending knowledge; explicit
init discards selection. The result queue and pending latch are independent.

Original IOP helper `0x3964` writes `0x68` at `0x396c`, after the result-buffer
clear. The firmware names this register PND in its diagnostic. Matching iBoot
writes the same acknowledgement, and the older H2FMI implementation reads
pending state and writes it back. The referenced S5L8930 model also clears
written-one pending bits. These support the bounded acknowledgement policy,
without proving a complete silicon bit layout, reset value or event causes.
No zero reset status is supplied. Readback zero after a full acknowledgement
comes from the guest clearing the supported bits. No automatic ECC event
producer, NAND readiness, mask polarity, interrupt delivery, DMA or timing
is inferred. Nonzero ECC masks remain unavailable.

CFG0 also retains the ECC page configuration produced by original IOP
`0x44dc`/`0x4390`: base `0x60000`, bit 14 for mode 16, the same six-bit
auxiliary count in bits 2 through 7 and 8 through 13, and page codes
3/0/1/2 for software sizes 512/2048/4096/8192 bytes. The observed `0x64a29`
selects mode 16, software size 4096 and auxiliary count 10. The original
DMA setup at `0x4694` derives separate data and auxiliary lengths from object
fields; this does not establish physical spare capacity or parity layout.
The original read-programming tail at `0x4630` writes CFG0 then `0xf00` to
ECC control `0x808`. That control value is retained only after ECC page
configuration. Other controls and CFG0/808 readback remain unsupported.

These writes only stage configuration. They require idle command/data paths
and empty FIFOs, preserve result/pending storage, and produce no data,
completion, events or IRQs. Reset invalidates both. The raw 512-byte launch
and producer explicitly reject ECC page modes; already buffered raw bytes
must drain before switching to one. Raw sector pipelining remains supported.
ECC decoding, correction and page execution still require a backed data path.
Original constructor and read-tail fixtures cover both banks, four page
sizes, five auxiliary counts and both modes; they prove instruction execution
and configuration retention, not physical NAND geometry or guest boot.

Address registers `0x418`, `0x41c` and `0x420` retain explicitly programmed
ADDR0, ADDR1 and ANUM values. Matching IOP firmware names and reads these fields
in its diagnostic; its address helper matches original iBoot's page packing.
The supported forms are a one-byte ID address or a zero-column page row in
ADDR0's upper 16 bits, a high row byte in ADDR1, and ANUM values zero or four.
Readback requires explicit programming, which reset invalidates. Existing mode,
gate, pending-work, active-byte and callback restrictions apply. Staging these
fields does not start a transfer. READID additionally requires ANUM zero and an
ADDR0 fitting one byte, so a staged page cannot be truncated into an ID request.
Other address forms and additional ANUM values remain unsupported.

The paired page-read command word `0x3000` now queues with control `0x0b`.
It requires one selected chip, explicitly programmed timing, zero column,
all three address fields and ANUM four. The extended transient endpoint receives
the complete 24-bit row and must initiate the backed read before command
completion bits `0x0b` are set. It owns capacity checks and page/OOB state;
missing or rejecting backing leaves the transaction pending. The original
reset/READID service API cannot complete a page command. Completion and its W1C
acknowledgement represent command delivery, not NAND array readiness or bytes
transferred into guest RAM. No page image or default ready response is installed.

Original `0x4ff03e68` supplies the command/control/poll/ack sequence. The standard
`00h`/`30h` page-read interpretation is corroborated by the vendor's
[SecureNAND data sheet, section 3.2](https://www.skyhighmemory.com/download/ONFI_S34SL01G2_02_04_Page_2KB_002_99252.pdf).
That different device family does not establish the target's geometry, timing
or controller registers. Tests use explicit sparse backing and verify data/spare
bytes in the endpoint cache, refusal without backing, full-width rows, reset
cancellation, reentry and both board gates.

The polled status path now has a separate transient capability for command
`0x70`, status-byte sampling and command `0x00` to return to an issued page read.
No ready byte is supplied by default. Original `0x4ff03800` programs
`RBB_CFG=0x4040`, then original `0x4ff038d0` waits for `FSTATUS` bit 5.
With an active status strobe, each explicit service quantum samples the selected
endpoint once. Busy samples keep the poll pending; a sample with bit 6 set
latches completion and stops sampling. Register reads preserve the latest byte
and never advance the endpoint. Lowering the strobe cancels an unfinished poll.
The subsequent `0x00` command requires an already serviced page request and an
endpoint mode switch; it does not initiate another array read. The vendor's
[status description, section 3.9](https://www.skyhighmemory.com/download/ONFI_S34SL01G2_02_04_Page_2KB_002_99252.pdf)
corroborates this separation between status mode and page-data mode.

This path requires explicit masks at `0x440` and `INT_EN` (`0x10`), zero
PIO programming and IFCTRL bit 20 clear. Zero masks select polling without
interrupts. The observed low-four-bit INTPND acknowledgement at `0x0c` is
accepted after both mask writes. INTPND reports backed sector-completion
bit 1, with a separate W1C of value 2, and the enabled status aggregate below.
FSTATUS supports
W1C for the implemented flags and the firmware's broad `0x0031ffff` clear.
The sampled-status flag does not block subsequent command delivery.
Unit tests copy explicit backing into an endpoint cache in separate work steps,
prove that readiness follows completion, and cover refused/missing callbacks,
poll cancellation, reset, selection, counter limits and both board gates.
Separate FMI sector and CDMA services transfer supplied bytes into guest buffers.
The command/status services above do not advance those transfers.

An optional `s5l8920_nand` endpoint now provides reusable read-only backing for
these capabilities. Each selected chip requires explicit ID bytes, sorted
sparse rows, raw row length, busy/ready status patterns and a separate cache.
Unlisted rows remain unavailable. Reset cancels a pending read and invalidates
its stream; a separate service completes reset. A page read becomes ready only
after explicit service has copied every backed byte into the cache, at most
16 bytes per quantum. Sampling status does no work. Returning from status mode
preserves the data cursor; FIFO backpressure preserves it too. Controller reset
leaves the separately owned chip alone. No image, erased pages or device is
installed by default, and no filesystem or whole-device allocation is required.
The owner keeps descriptors and raw bytes immutable for the attachment lifetime.

The endpoint derives readiness bit 6 from its actual work; other status bits
are explicit owner-supplied patterns. The quanta are functional scheduling,
not measured physical timing. Raw bytes keep their supplied order: this does
not infer large-page data/OOB placement, generate ECC, translate FTL sectors,
or establish physical geometry. Tests cover all eight chip selections, sparse
row bounds, partial final copies, absent backing, cache aliases, status/data
mode changes, reset, FIFO backpressure and exact data/spare-lane bytes.

The controller also carries backed events to the IOP. Event-mask bit 5 at
`0x440` gates FSTATUS bit 5 into INTPND bit 8; `INT_EN` bits 8 and 1 enable
the status and raw-sector sources. INTPND bit 8 is a live aggregate: masking
its FSTATUS source or acknowledging FSTATUS deasserts it, whereas low-bit
INTPND W1C leaves that source intact. Mask and acknowledgement writes work
during pending commands, status strobes and buffered sector transfers without
advancing them. All other enable bits, including `0x2000`, remain refused.
This bounded functional policy is corroborated by the
[older H2FMI implementation](https://github.com/samhenrigold/qemu-ios/blob/35483a99196229aa25767e8b55d35372f9379bdf/hw/arm/s5l8930_h2fmi.c)
and the original target's programming and acknowledgement sequence; it is
not a complete silicon register or timing specification.

Original IOP initialization at `0x3ff0` registers the common handler `0x47e0`
on VIC0 sources 31 and 30 for FMI0 and FMI1. The board now drives those inputs
after successful controller writes/services and between IOP instructions.
It never changes the IOP during a borrowed register callback. Closing a
controller clock pauses work while retaining already latched interrupt levels;
FMI reset clears its events and deasserts its input. CPU reset preserves the
separately configured VIC state. AP interrupt routing is not inferred.

An isolated original-firmware witness first refused the event-mask write at
`0x455c`, after starting the status poll. With this path enabled, the complete
`0x453c`/`0x40b0` setup returns in 62 instructions. Missing/refused samples
and a supplied busy byte do not assert IRQ. A supplied ready byte reaches the
original vector at `0x18`, wrapper `0xa22c`, dispatcher `0x7d30`, registered
handler `0x47e0` and mode-zero status handler `0x483c`. The 206-step exception
path records pending `0x100`, masks and acknowledges it, signals the original
event, performs VIC EOI and restores the interrupted registers/CPSR. The
original initialization supplies callback registration and the empty event
wait list; RAM, caller/task context, vector index, IRQ stack and status samples
are explicit fixtures. No firmware code is patched. This establishes the
isolated status interrupt path, not continuous startup, ECC page completion,
rootfs access or a rendered iOS guest.

An additional witness uses the new endpoint with original page helper
`0x4880`/`0x3928`, original status setup, and the same original interrupt path.
The original VIC framework `0x7ae8` also programs its vectors. With explicit
4,314-byte raw backing, 269 copy quanta leave the chip busy; the final ten-byte
copy makes it ready, and the next status sample raises the interrupt. All eight
bank/chip/RAM-pattern cases complete the original 206-step handler and return.
Caller/task/IRQ-stack context, chip reset, configuration and raw page bytes
remain explicit inputs. This replaces the supplied ready sample with backed
cache completion; it does not execute the unresolved ECC page state machine
or establish a continuous firmware boot.

Isolated original firmware now executes reset at `0x4ff0370c` and READID at
`0x4ff03768`, including its polling, acknowledgements, timebase reads, eight
byte transfers, timing restoration and deselection. Reset returns after 95
instructions, selected-chip READID after 589 and an explicitly backed
unselected-bus READID after 578 with the fixture's explicit
one-tick/service-per-instruction schedule. These fixtures supply a controller
object, initial mode/timing, stack and diagnostic ID backing from two original
match-table records. They also retain an explicit all-`ff` response case.
Those inputs do not select or establish the target's physical NAND geometry.
Unavailable backing/address cases leave the output unchanged, including
unselected chip lines without an explicit bus response.

Further bounded execution runs original banked-stack, BSS, MMU, scheduler and
timer initialization fragments. Original `0x4ff03aac` then completes 16 resets,
cooperative sleep, a real timer interrupt and acknowledgement, context resume,
and 16 READID probes. With source ticks withheld after deadline programming,
it stays asleep without issuing READID. Strict and shipping traces agree.
These remain explicit routine entries with diagnostic endpoints and supplied
physical RAM, not a connected boot sequence or measured timing. Extending into
the original `0x4ff0219c` caller executes its actual heap allocation and
zeroing, then controller initialization. With the timing-order fix it reaches
the ECC-mask write at `0x4ff037f4`. With explicit zero-mask programming it now
completes initialization and reset/sleep/wake/READID on both controllers,
then the original logging, filtering, merge and geometry matching. All 256
scanner bytes and the merged table match the diagnostic endpoint inputs.
The first extended fixture stopped in its original divide-by-zero fatal path
because its timing calculation consumed uninitialized clock state. That capture
is retained. Executing original clock-state producer `0x4ff13b40` first resolves
this dependency: it reads the explicitly configured PLLs and selectors and
produces all 28 frequency slots checked against an independent integer oracle.
The inputs use matching iBSS-selected configuration, a 24 MHz reference and
32 logical reference cycles; the NAND getter returns 81 MHz. No output frequency
or fatal-path result is substituted, and these inputs do not measure physical
clock timing.

With that prerequisite, original `0x4ff0219c` returns zero after 3,107,602
instructions. Both controllers finish their backed discovery work and retain
timing `0x11111` written by original code. The complete cold wrapper at
`0x4ff02134` returns after 3,107,621 instructions and sets its initialized flag.
A second call takes the original reporting path, returns after 71,112
instructions, and preserves controller objects and discovery state without
issuing additional NAND commands. This repeated-call path establishes reporting
behavior, not sleep/wake or resume support. Strict and shipping traces agree
under two initial register/stack poison patterns.

Independent original-firmware fixtures establish that the `#ce` handoff
property is a count derived from 16 eight-byte ID records, accompanied by a
bitmap. An isolated sparse-memory fixture also executes the original 16-slot
scanner, transferring 128 backed bytes in 9,584 instructions. Its output goes
through the original geometry matcher, which checks IDs, chip layout and a
caller-supplied bus count. One tested diagnostic layout matches original tables;
the other tested layouts take the original rejection paths. Earlier publication
stopped at an unprepared state read; the clock/discovery fixture now supplies
the original producers. Full root registration at `0x4ff0cb08`, node lookup at
`0x4ff0cba4` using the original path literal, and publication at `0x4ff16974`
complete. Without root registration, lookup returns zero and leaves its output
and tree untouched. With it, all 57,856 tree bytes match an independent expected
copy containing exactly 24 derived property updates from 25 word stores.
Property names distinguish `#ce-blocks=8192` from `#page-bytes=4096`; other
fixture results include `#block-pages=128`, `#spare-bytes=218`, four chip lines,
bitmap `0x0505`, two buses, ECC correctable count 16 and threshold 12.

An unestablished word at discovery-state offset `0x74` is guarded against reads
before publication. Its value depends on the caller's previous stack contents;
it is not established as a geometry pointer. The original alternate-mode flags
remain zero from BSS initialization. Successful publication therefore does not
establish those alternate modes or the target's physical geometry. The fixtures
still enter complete routines separately, and no resulting tree or diagnostic
property value is supplied to the kernel.

Following discovery, the original table getter at `0x4ff02180` exposes the raw
page callback at `0x4ff02e38`. A guarded invocation uses explicit chip/page inputs
and buffers allocated and zeroed by original firmware. It reaches the address
helper at `0x4ff03628`. Before address staging was implemented, it stopped at
`0x4ff0362e`, writing the page's high byte to unsupported register `0x41c`.
The original helper now completes all three writes, including the remaining
page bits in `0x418` and value `4` in `0x420`, and returns. With explicit sparse
diagnostic page backing, the callback issues `0x3000`/`0x0b`, polls completion
and acknowledges it. The extended fixture copies the 4,096 data and 218 spare
bytes into the endpoint cache in independent 16-byte work quanta. Original
status polling observes 205 busy samples and one ready sample, acknowledges
FSTATUS bit 5, then sends `0x00` and acknowledges that command's completion.
Those counts describe the explicit functional schedule, not physical timing.
Execution reaches original DMA setup `0x4ff01344` through `0x4ff048cc`.
The earlier fixture stopped at `0x4ff01522`, writing value 2 to an unattached
spare-data DMA channel. With CDMA attached and original `0x4ff01328` opening
its configured clock gate, the firmware now builds and queues both chains.
The fixture's CPU heap and DMA use the same board RAM, including original
descriptor pools at `0x4ff32a00` and `0x4ff32e80`. The earlier CFG refusal at
`0x4ff041c0` is now passed with the backed sector producer. All eight original
sector requests transfer 4,314 bytes into the original caller allocations in
the expected order. Two original IRQ entries execute the DMA handler at
`0x4ff018bc`, acknowledge completion, and mark both software records complete.
The next refusal is a read of `0xbf210014` at `0x4ff08d7c`, in the interrupt
disable helper called by the final DMA wait. The raw-read callback has not
returned, and final chip deselection and cleanup have not executed.
The original chip selector checks the bus encoded in the global chip number
before selecting its low-three-bit chip line. The fixture's backed rows are
zero, `0xffff`, `0x10000` and `0xfffff`, within its diagnostic geometry;
unlisted rows have no implicit backing. Earlier `0x123456` testing established
address packing only and exceeded that fixture's capacity.

At that checkpoint, the kernel's guarded `#ce` read was the conditional
frontier. The genuine audio-control write remains unresolved. Full NAND read completion,
physical page/OOB geometry, ECC processing, NAND-controller interrupts, FTL,
alternate initialization modes, the connected
bootloader chain, SpringBoard and physical behavior are unverified.

## NAND DMA receive chains

The optional CDMA component accepts channels 5/6 for FMI0 and 7/8 for FMI1.
Data channels use peripheral offset `0x14` and configuration `0x58`; spare
channels use offset `0x18` and configuration zero. These are the values emitted
by original `0x4ff01344`. Explicit reset, configuration, peripheral address,
byte count and descriptor pointer precede GO. Original `0x4ff01588` builds
32-byte descriptors with flags 3, flags `0x103` on the final data segment,
and a following flags-zero terminal. The supported chain contains at most
31 segments and must cover the complete programmed count. Missing RAM,
malformed links/counts and destination overlap with descriptor backing refuse
before the channel starts. Live descriptor edits remain unsupported.

`s5l8920_cdma_nand_board_service` supplies a transient receive capability for
one selected channel. READY supplies exactly the requested bytes, WAIT leaves
the transfer pending, and UNAVAILABLE refuses without consuming data. Output
from a waiting or refusing callback is discarded. Each call copies at most
16 bytes and stops at a descriptor boundary. Counts, addresses and completion
follow actual stores; MMIO reads and the existing AES service do no NAND work.
Closing the DMA gate pauses service. A store invalidates the local exclusive
monitor without advancing guest instructions or time.

The firmware's `0x1c0009` GO selects completion events. DONE and its enable
drive VIC source `0x2a + channel`; W1C acknowledgement removes the DMA source
while preserving independently asserted external input. Original `0x4ff01344`
registers `0x4ff018bd` on that line, and `0x4ff01868` polls the same completion
before dispatching the handler. The matching platform's
[DMA header](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8920/includes/hardware/dma.h)
corroborates the interrupt base. Error interrupts, hold/abort, outbound DMA,
other settings, partial-chain restarts and encrypted NAND transfers are refused.
The owner must zero-initialize the component before first use; reset/init
discard channel programming, and board reset detaches the borrowed component.
Nested service and board reset/free are refused during a receive callback.

Original raw-read vectors interleave the two controller lanes across the two
caller buffers. Nine data vectors carry 4,096 bytes, with the final sector split
294/218 across those allocations; seven spare vectors carry 218 bytes into the
first allocation. The controller lane names do not imply a direct main/OOB
buffer mapping. Tests use explicit byte streams to reconstruct both complete
outputs and check waiting/refusal, canaries, descriptor limits and mutation,
RAM aliases, clock/reset behavior, reentry and both banks' interrupt sources.
Original firmware now consumes the FMI sector stream through these chains;
the remaining interrupt-disable read prevents a completed raw-read return.

## AMC receive data prefix

The standalone CDMA component now accepts explicit channel 24 reset and the
kernel's receive configuration `0x300d8`, peripheral `0x8480002c`, byte budget,
descriptor address and GO `0x19`. The matching device tree assigns this channel
to AMC. Original kernel methods `0x808a320c` and `0x808a33f2` produce the
configuration; constructor suffix `0x808a31b8`, builder `0x808a3af8` and finalizer
`0x808a3528` produce the 128-entry linked ring. Ordinary entries have flags 3;
the final data entry of a command has flags `0x303`. The same combined marker
occurs in the matching 3GS 3.1.3 driver at `0xc052540c`. This corroborates the
software format, not the hardware effects of its individual completion bits.

`s5l8920_cdma_amc_service` implements only the ordinary data prefix before that
marker. GO preflights and latches 1..127 flags-3 entries plus the backed
`0x303` boundary. The limit is a host bound. Missing backing, cycles including
physical aliases, output overlap with descriptor storage, unsupported flags,
zero lengths and a prefix exhausting the programmed byte budget refuse before
starting. A command consisting of a single `0x303` entry therefore refuses GO.
Software-only descriptor fields remain untouched and are not interpreted.

Each explicit service copies at most 16 supplied bytes, limited by the caller's
budget and current descriptor. A closed supplied clock, zero budget or WAIT
preserves state and output. Missing data/backing, descriptor changes and
callback reentry refuse. Progress registers follow actual stores; after a
segment, the current address retains its end until another transfer occurs.
At the `0x303` boundary, the next unpaused service refuses without consuming
bytes or declaring completion. The channel retains its active state for this
unimplemented transition. No DONE/CIR event or interrupt is generated.

An isolated witness builds the ring with unchanged target instructions and
passes its exact descriptor bytes and captured configuration to this component.
Both ring start positions 0 and 127 and budgets 7 and 16 are covered: eight
cases copy 2,048 or 4,096 bytes from explicit input streams, then refuse the
completion descriptor; four single-entry cases refuse before copying. Waiting,
unavailable input, whole-buffer canaries and unmodified descriptors are checked.
The current-address read during original setup remains an explicit fixture
input; the component does not invent an initial value before GO.

Channel 24 remains absent from the board MMIO fabric. AMC's byte producer,
completion/IRQ behavior, live ring publication, hold/abort, reset/readback
integration and the shared audio-control write remain unresolved. These data
copies do not establish DSP execution, sound or any advance in genuine boot.

## Backed FMI sector data

The supported raw mode writes CFG with `0xa8003` and the same spare count
(0..32) in bits 8..13 and 2..7, then writes 3 to CTRL at offset 4. It queues
512 data-lane bytes followed by that many spare-lane bytes from the selected
NAND's page stream. A backed page command, data mode, explicit zero interrupt
and ECC masks, and configured staging depths are required. Other CFG/CTRL
modes, ECC processing and outbound transfers remain unavailable.

`s5l8920_fmi_data_configure` supplies functional staging depths: 16..512 data
bytes and 16..32 spare bytes. There is no default. These are bounded host
storage parameters, not a measurement of silicon FIFO sizes. The separate
`s5l8920_fmi_data_board_service` clocks at most 16 actual endpoint bytes per
call. WAIT preserves the stream and controller; refusal discards any returned
bytes. A full FIFO applies backpressure. Once all requested sector bytes have
arrived, INTPND bit 1 latches independently of DMA stores. After acknowledgement,
another sector can queue while earlier bytes remain buffered.

`s5l8920_fmi_dma_board_service` consumes the selected lane through CDMA's
validated RAM chain. It never creates FIFO data or executes the producer.
Both controller gates are respected; only actual RAM stores invalidate the
exclusive monitor. Neither service advances guest time or instructions.
Reset cancels pending sectors and discards staging without resetting the
separately owned NAND endpoint. Reads never progress work, and live command,
chip or timing changes with pending/buffered data refuse.

Component tests cover both banks, varying FIFO depths and service schedules,
ring wrap, cross-sector buffering, byte ordering, refusal, reset, gates and
callback reentry. Original firmware executes its CFG/CTRL sequence, eight
INTPND polls and acknowledgements, and both DMA IRQ handlers. Its next
interrupt-disable read uses a register documented as write-only by the
[PL192 manual, section 3.3.6](https://documentation-service.arm.com/static/5e8e218b88295d1e18d37489).
That description does not establish the target's read value, so the read
remains refused. Correct transferred bytes do not establish a returned NAND
operation, ECC/FTL behavior, connected boot or physical-device success.

## Optional IOP shared RAM aperture

The original version1 AppleARM7M address conversion at `0x8065f9bc` subtracts
`0x40000000` from an AP physical address. A conditional kernel trace observes
this conversion for the allocated heap at `0x41718000`, followed by original
stores of `0x01718000` and `0xa000` to the firmware's heap descriptor. The
observer leaves the previous execution trace unchanged apart from its added
records. This establishes the guest software's mapping, not physical testing.

An explicitly selected RAM aperture now implements that conversion. Its
page-aligned size is bounded by the caller; it neither allocates memory nor
infers physical capacity. The programmed low firmware window takes precedence.
Each access requires real backing from the borrowed RAM capability, with
separate diagnostics for unmapped addresses and unavailable backing. AP
physical-address aliases and unknown MMIO remain unavailable. Reset/hold
preserves the aperture selection; component initialization discards it.

The optional cache uses IOP address tags. Firmware and direct-RAM aliases share
backing, while their private cached lines remain independent until maintenance.
This is an explicit functional policy, not a measured cache-coherency contract.
Actual backing stores and writebacks retain the existing AP exclusive-monitor
effects. No shared RAM aperture is configured by default.

Isolated unchanged firmware routines now initialize the observed 40 KiB heap
and allocate a 32-byte block at `0x01718020`. They reproduce the original
unmapped store without the aperture. Uncached execution and an explicit cache
budget of 4,096 entries complete; the 1,024-entry budget stops visibly during
zero-fill. The larger budget is an execution allowance, not physical geometry.
These fixtures supply thread, BSS and stack context and do not establish
complete startup or a later whole-kernel boot stage.

In the separate conditional kernel run, the 1,024-entry budget stops during
heap clearing. With an explicit 4,096-entry budget, the IOP executes 51,583
instructions without an IOP fault or panic. The AP proceeds to the flash
interface driver and stops at `0x809e35ba` while reading the unprepared `#ce`
property under `/device-tree/arm-io/flash-controller0/disk`. No replacement
property value is supplied. This retains the earlier audio-write exception
and explicit board/clock/cache inputs; genuine connected boot is still unproven.

## Optional IOP deadline timer

An explicitly configured IOP timer implements the original timer1 count and
control writes at `0xbf10020c` and `0xbf100224`. The firmware's table at
`0xfb90` identifies both timer pairs and interrupt sources. Startup selects
entry1; the original registration and enable helpers install its callback and
enable source5 in the IOP's first interrupt controller. Configuration requires
those controllers and precedes the first CPU reset. No timer is selected by
default.

Count writes replace the remaining interval and rearm expiry without clearing
an existing cause. Control bit0 enables counting; bit1 acknowledges the cause.
Supplied ticks decrement an enabled interval and latch one event at expiry;
a zero interval waits for the next positive tick. Masking or ending interrupt
service does not acknowledge it. Timer acknowledgement preserves an independent
software request on source5. The AP's timer and interrupt state stay separate.
Count/control readbacks, timer0 access from the IOP, unsupported control bits
and widths remain unavailable.

Board service lends a read-only capability for the actual supplied 64-bit
timebase at `0xbf100200/204`. Each word samples the counter independently,
allowing the original high/low/high retry loop to handle rollover. Standalone
service without this capability refuses those reads. Reads and CPU execution
do not advance time. An attached selected timer receives the same explicit
ticks as the board, even while its CPU is held or gated; this is a functional
clock policy, not a measured frequency or physical gating/reset contract.
Permitted CPU reset preserves the separately configured peripheral, while
component initialization discards it. No borrowed capability survives service.

Bounded original routines now initialize the timer, read stable or rolling
time, program future, clamped, past, equal and null-callback deadlines, and
handle real timer IRQ/FIQ expiration. The original handler acknowledges the
timer before its callback and restores the interrupted registers. These tests
use explicit thread, callback-table and vector state. The isolated startup
passes timer initialization but later reports `grab_chunk: heap overflow`,
then enters its fatal loop at `0x8a3c`; that fixture does not establish a
complete host setup or boot. Its diagnostic executes the original panic path,
including cache cleaning and disable, without patching instructions.

In the separate conditional kernel run, the original AP driver supplies heap
address `0x01718000` and length `0xa000`. With the timer selected, the IOP
retires 3,354 instructions and stops at `0xd448` on its first word store to
that heap when only the low firmware window is selected. This run retains
the earlier audio-write exception and explicit clock, cache and board inputs;
it does not establish a genuine connected boot. The genuine audio-control
stop and the previous cache-only kernel trace remain unchanged.

## Optional functional IOP cache

The IOP can explicitly select a generic unified write-back cache for its
selected shared RAM. This is a functional policy, not an identification of the
physical cache. Its 16-byte granules follow the original maintenance loop's
stride; physical geometry, instruction/data topology, replacement and timing
remain unmeasured. The caller lends a finite array of resident entries. A full
miss stops with `S5L8920_IOP_CACHE_LIMIT` instead of inventing eviction behavior.
No cache is configured by default.

The original initialization writes `1` to `0x86300020`, then `4` to
`0x86300000`. These initialize invalid tags and enable the selected policy.
Fetches and loads retain real backing bytes; stores change private dirty data.
AP writes remain invisible to populated IOP entries until invalidation, and
IOP writes remain invisible to the AP until cleaning. The clean/invalidate
interpretation is inferred from original queue dataflow: the sender uses flag1
after writing a slot, while the receiver uses flag2 before reading one.

An aligned mapped RAM address written to `+0x18` selects a granule. Zero-valued
commands at `+0x1c`/`+0x24` clean/invalidate that entry; `+0x0c`/`+0x20` operate
on all entries. Clean writes actual dirty bytes to RAM and retains the entry.
Invalidation discards even dirty data. `+0x00=0` disables without flushing.
Only actual backing stores/writebacks count in `bytes_written` and invalidate
AP exclusives. Signaling an interrupt does not implicitly publish cache data.
A failed backing transfer preserves earlier completed writes and records the
transfer address, size and original instruction PC separately from AP faults.

Unknown readbacks, command values, unaligned selections, the `+0x08` wait
command and AP access to this register window remain unavailable. Warm
initialization, CPU reset and window remapping after cache initialization are
refused because their physical coupling is unknown. Hold/resume retains state;
component reinitialization detaches the caller's storage.

Bounded fixtures execute the unchanged original initialization and maintenance
helpers plus queue send/receive routines. They verify stale reads, dirty
publication, whole/range maintenance and the sender's real AP software
interrupt, with explicit queue/thread context and two RAM fill patterns.
The original startup with this policy and cold IOP interrupt controllers
retires 3,170 instructions before refusing `PC=0x6d60`, a write of `2` to
`0xbf100224`. This is a later isolated boundary, not a completed IOP boot.
Whole-cache flag2 and flag8 challenge calls enter the firmware's assertion
path at `0x6dc8` after losing uncleaned critical-context data. Those failures
remain unresolved; the observed direct zero-length callers use clean-first
flags1/9, but the static caller audit is not exhaustive. These results do not
establish a physical cache contract, a connected kernel boot or SpringBoard.

## IOP interrupt controllers

The optional IOP interrupt configuration provides the three controller windows
used by the original firmware at `0xbf300000`, `0xbf310000` and `0xbf320000`.
Software requests, enables, IRQ/FIQ selection, priority masks, vector
acknowledgement and end-of-interrupt use the existing functional PL192 model.
An AP write to `+0x18` sets pending bits; a guest write to `+0x1c` clears them.
Requests survive CPU masking and vector acknowledgement until explicitly
cleared. The IOP interpreter samples actual IRQ/FIQ inputs between instructions.
The AP's own interrupt controllers retain separate state. IOP word writes to
`0xbf200018`, `0xbf210018` and `0xbf220018` set pending software sources in
those controllers. The original outgoing helper at `0x7e84` computes these
selectors; the `0x8ae8` wrapper requests source3. Delivery uses the existing
AP enable, priority and IRQ/FIQ routing, and the AP takes an exception only
when separately stepped. A signal does not acknowledge a message, count as a
RAM store or invalidate the AP exclusive monitor. Board service lends this
write capability for one IOP instruction; standalone service has no AP link.
Reads, other AP selectors and unsupported widths remain unavailable.

Configuration explicitly selects cold PL192 state and aggregates independent
controller outputs. It does not establish the physical daisy topology, revision
or synchronization delay. CPU reset preserves these separately configured
controllers; reinitializing the component discards them. Physical reset wiring
is not inferred. Both observed clock gates must be open for AP access. The
IOP bus has the same aligned-word register semantics. Protection programming
and identification replies remain unavailable, as do external peripheral
levels and mailbox message completion. The optional timer and cache each
require explicit configuration as described above.

The unmodified IOP IRQ vector, exception wrappers, dispatcher and software-clear
helper execute with an explicit interrupted context, thread and callback table.
Twelve cases in each strict/shipping build cover IRQ/FIQ, three banks and two
RAM/register fill patterns:
the source clears, service ends, the nesting counter balances and the original
registers/CPSR return intact. The callback is the original clear helper, not
the normal message processor. The FIQ dispatcher executes its original `MUL`
at `0x7e44` to select the callback. These are
bounded interrupt-path results, not a complete IOP boot or message exchange.

The original outgoing signal helper also returns in sixteen cases per build,
covering the three AP banks, IRQ/FIQ selection, two fill patterns and the
source3 wrapper. It sets actual AP pending bits without changing the payload
or advancing the AP CPU. Before this connection, the same cases stopped on
the unmapped store at `0x7ea0`. Cache maintenance and normal queue processing
remain outside this witness.

## IOP execution foundation

An optional version1 IOP controller connects the partial ARMv4T core to
actual shared DRAM. It requires an explicit reset pulse and firmware-window
programming before RUN. The original target driver supplies the protocol:
`+0x100` requests hold/run, `+0x110` supplies the physical firmware base,
`+0x114` supplies the inverse power-of-two window mask, and `+0x118` supplies
the controller's self address. The bounded model maps IOP low addresses to
that physical buffer. It does not copy firmware or synthesize message replies.

The original kernel opens raw gates `0x2e` and `0x31` before pulsing bit31
at `0xbf100130`. Only that reset bit, with an attached controller, resets
and holds the IOP CPU. Both gates must be fully open for controller access
or execution; their individual clock roles and rates remain unestablished.
The initial register state is unavailable until reset. Unknown bits, widths,
live remapping, out-of-RAM windows and unsupported MMIO are refused. The
post-reset control request `0x10` is accepted only while freshly held with
no window programmed; its additional live effects are not implemented.

Each explicit board service call executes at most one IOP instruction.
Reads and AP execution do not implicitly advance it. Hold preserves CPU
state; a permitted reset invalidates the window without erasing shared RAM.
Reset after optional cache initialization is refused as described above. STOP reflects
an actual hold request, never an unsupported instruction or missing register.
IOP failures preserve their own PC/access diagnostics separately from the AP.
Successful backing stores invalidate the AP exclusive monitor, including the prefix
of a multi-store that later fails. Board reset/free detach the borrowed
component; service retains no RAM or owner pointer. No controller is attached
by default, and no AP/IOP clock ratio is inferred.

With no optional cache selected, the bounded original kernel segment
`0x8065f7d0..0x8065f840` executes 32
instructions with an explicit version1 caller object, image buffer and absent
optional callback. Its five controller writes and one status read configure
and start the real IOP core. The subsequent unmodified firmware entry passes
eight strict/shipping cases with four RAM fill patterns, reaching the same
88 unique original instructions and 65,668 successful byte stores as the
isolated prefix. The first unsupported cache write preserves registers and
retirement, leaves RUN set, and does not fault the AP. This bounded sequence
does not establish execution of the whole driver, reset helper or boot chain.

A separate whole-kernel diagnostic, still conditional on the earlier single
audio-write exception and inherited board inputs, lets the original kernel
reset and start this controller. At one supplied IOP instruction per diagnostic
tick, the IOP retires 2,286 instructions and writes 57,184 bytes before the AP
stops at `PC=0x8065f97a`, writing `8` to unimplemented `0xbf300018`. Its actual
firmware base is `0x40a1f000`, window mask `0x3ffc0000`, and RUN remains set.
This is the next host-to-IOP interrupt boundary, reached before the IOP cache
stop at this unmeasured schedule. With the audio exception disabled and IOP
unattached, the guarded kernel trace remains exactly unchanged. These are
distinct diagnostics; neither establishes a connected bootloader/kernel boot.

The prelinked `IOP_s5l8920x_firmware` bundle contains a 164 KiB firmware
image. An isolated host probe executes its unmodified A32 entry, establishes
banked stacks and verifies the guest's BSS clearing. The probe stops at
`PC=0x8000`, writing `1` to the unavailable register `0x86300020`, after
2,646 retired instructions. The tested initial register/RAM patterns reach
the same boundary. This is separate from kernel boot and does not establish
the IOP's exact CPU implementation or its controller's semantics.

`ARM_ARCH_V4T` is an explicitly selected, partial, little-endian A32 profile
for this investigation. Its reset and status-register rules follow the
[ARM7TDMI TRM, DDI0210C](https://documentation-service.arm.com/static/5f4786a179ff4c392c0ff819),
sections 1.4 and 2.2-2.10. It does not select an ARM7TDMI revision for the
S5L8920. The profile supports data processing, MRS/MSR, B/BL/BX, byte/word
and block transfers, IRQ/FIQ entry and exception return. LDR/LDM into PC
retain ARM state; BX and exception returns select state architecturally.
CPSR has NZCV, I/F/T and mode fields, without ARMv6 A/Q/GE/endianness bits.
Writes changing reserved PSR bits or CPSR.T through MSR are refused.

The profile also supports A32 `MUL`/`MLA`, following
[ARM DDI0406C.b, A8.8.100 and A8.8.114](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
It refuses PC operands, a destination equal to the first encoded source, and
nonzero reserved addend bits in `MUL`. The result is the low 32 bits of the
product plus the optional addend. The S forms update N/Z and preserve V;
this functional model retains C for ARMv4's architecturally unknown carry
result. Instruction retirement counts do not model multiplier cycle timing.

The A32 `UMULL`, `UMLAL`, `SMULL` and `SMLAL` forms use the existing
64-bit arithmetic, following
[ARM DDI0100I, A4.1.76/87/128/129](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a).
Accumulation wraps at 64 bits; S forms set N from bit63 and Z from the
complete result. The model retains ARMv4's unpredictable C/V values.
PC operands and equal destination registers are refused. This partial
profile also conservatively refuses a first source equal to either
destination: the manual relaxes that older restriction for known
implementations, but this profile does not identify one. Second-source
overlap is supported; ARMv6 and ARMv7 behavior is unchanged.

An isolated original IOP routine at `0x6e00` multiplies its 64-bit argument
by24. Before long-multiply support it stopped at `UMULL` at `0x6e0c` after
three instructions. Eight explicit arguments across all16 flag states now
return the correct wrapped result after all six original instructions,
without data access or device responses. This is original-code execution
evidence, not whole-kernel progress or a multiplier timing measurement.

The profile implements A32 `STRH`, `LDRH`, `LDRSB` and `LDRSH` using the
existing memory-transfer paths, following
[ARM DDI0100I, A4.1.28-30, A4.1.104 and A5.3](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a).
The six immediate/register offset, pre-indexed and post-indexed forms retain
ARMv4's operand restrictions. In particular, register-offset writeback with
the same base and offset register is refused on ARMv4T, while the existing
ARMv6 and later behavior is preserved. Odd halfword addresses, PC data or
offset operands, PC writeback, base/data writeback overlap and reserved
encodings are refused before data access. A PC base without writeback uses
the original instruction address plus eight. Loads zero- or sign-extend the
actual bytes, stores write only the low halfword, and failed host accesses
preserve registers and retirement. This does not introduce a physical abort
or timing model. Doubleword transfers remain unavailable on ARMv4T.

The original IOP `STRH r2,[r5,r0]` regression failed before this change and
now performs a two-byte store. Architectural cases cover all six addressing
forms, sign boundaries, aliases, failed conditions and bus refusals. IOP
regressions exercise shared RAM, private cache stores and real clean writeback;
timer and FMI halfword accesses still fail at their word-only device boundary.

Thumb execution, swap, doubleword transfers, SWI and
coprocessors remain unavailable in this profile. Later ARM extensions stop
before side effects; failed conditions still skip instructions. Unsupported
instructions return `ARM_UNDEFINED` to the host without retirement. No CPU
ID, CP15/MMU, cache-controller replies or mailbox
responses are supplied. Existing shared decoder restrictions also apply;
this is not complete ARMv4T conformance. Native engines and the legacy
snapshot format continue to reject this profile. CPU reset's zero values
for otherwise indeterminate state are host choices, not hardware evidence.

## AES key-disable state

`s5l8920_aes_key_disable_configure` supplies a known initial 16-bit latch at
`0x87800000`. With configured raw CDMA gate `0x25` fully open, aligned word
reads return that state and writes set the requested bits without clearing
earlier disables. Repeating the same configuration preserves guest writes;
changing the initial value is refused. Functional board reset restores the
supplied initial word and gate state. Free/init discards both. No initial
zero, fuse value or hardware key is assumed.

The target's original CDMAAES initialization queries bits 0..7 through
`0x808a236c`, then stores inverted availability bytes. Its disable path
`0x808a2ef6` calls the single-bit writer `0x808a2380`; other cached key states
remain unchanged. The original iBoot writer`0x4ff01cb0` and LLB writer
`0x84000bf4` construct a 16-bit mask from their two arguments. This supports
the bounded disable latch. The target bootloader opens raw DMA gate `0x25`
for crypto operations; its channel-enable helper returns immediately.
An additional AES clock gate from another driver is not assumed.

Missing initial state, closed/intermediate/unconfigured gates, unsupported
bits, misaligned accesses and non-word widths refuse atomically. A latched
bus fault prevents further effects. Without an attached CDMA component,
other AES/CDMA addresses remain unmapped.
The latch alone does not provide usable keys or cryptographic acceleration.

The kernel's key query was reached only in an explicitly conditional
diagnostic that accepted one unimplemented audio write. The production
audio stop at `0x84304000` remains unresolved; this register support does not
establish a later genuine boot stage.

## Polled DMA/AES data path

An explicitly attached `s5l8920_cdma_t` supplies context1 at `0x87801000`
and channels1/2 at `0x87001000`/`0x87002000`. The original iBoot-1537.9.55
command at `0x4ff01cdc` programs four IV words, 128/192/256-bit software
keys, the context, and a 32-byte data descriptor plus terminal for each
channel. Feed flags `0x30103` select AES and restart CBC chaining; drain
flags are `0x103`. Data length must match both programmed byte counts and
be a nonzero multiple of16. Only bounded RAM views are used, including the
explicitly selected low RAM alias. Partial buffer overlap refuses; equal
backing buffers support in-place encryption and decryption.

The original channel reset and polled control words are supported. GO
validates descriptors before publishing state; malformed chains, incomplete
keys/IVs and unavailable RAM refuse without output. Both channels must start
before work can proceed. Each explicit `s5l8920_cdma_board_service` call
transforms at most one16-byte block using the existing AES implementation.
Remaining counts and addresses reflect actual bytes consumed. Running clears
and completion latches only after all output has been written. Completion is
write-one-to-clear. Reads do not process data. The configured raw gate25
must be fully open for MMIO; a closed gate pauses service, intermediate or
unknown gate state refuses. A latched board fault also prevents work.

The service quantum is a functional scheduling choice, not measured hardware
latency. No interrupt mode, peripheral FIFO, scatter/gather, live channel
reset, hold/abort or other AES context is accepted. The kernel's distinct
control words remain refused. In particular, its M2M setup at `0x808a472a`
uses `0x88`, whereas the bootloader programs `0x80`; the polled path does
not infer interrupt wiring from another emulator. Key/IV register reads
also remain refused. Functional board reset detaches the borrowed component;
the caller resets and reattaches it explicitly. No DMA is attached by default.

Hardware key selectors refuse. The original command's type`0x200` queries
disable bit1 and type`0x201` queries bit2; unavailable keys are not substituted.
The unchanged command, including its real clock/cache helpers and final key
erasure, has been exercised with36 public known-answer cases covering all
three sizes, both directions, in-place buffers, a zero/null IV and four-block
CBC. Four additional cases verify disabled-key returns and refusal of an
enabled but unavailable hardware key. The CBC vectors are from
[NIST SP800-38A, Appendix F.2](https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf).
The matching public platform driver at
[openiBoot866562f, CDMA](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8920/cdma.c)
corroborates the register sequence; implementation is independent.
These are isolated original-firmware routine tests with explicit scheduling,
not a connected boot chain or physical-device observation. The genuine
kernel stop at the unknown audio write`0x84304000=7` remains unchanged.

## Display clock setup

The prepared kernel reads AppleSamsungMIPIDSI's complete32-word register
snapshot and consumes the panel identity produced by the original iBoot
query and device-tree routines under the explicit inputs below. With the SWI
controller attached and the original audio-reference property produced, the
whole-kernel diagnostic stops in AppleS5L8920XAudioComplex at PC `0x8078b4d6`,
writing `7` to unavailable register `0x84304000`, after283,830,458 retired
instructions. It has not reached launchd, SpringBoard or displayed pixels.

This run transplants the controller state produced by the original iBoot
initialization body, enables its physical gate, and supplies explicit idle
observations and a reload-latch timer view. Passive PHY/tail/port read values
are assumed zero; TIMEOUT, INTMSK and MEMACCHR use stated related-family values.
One system and one independent PHY cycle are supplied per existing diagnostic
timebase tick. These are functional inputs, not a measured complete bootloader
handoff. The kernel reads all32 words without refusal and disables raw gate19
through its original provider method before reaching the panel-property guard.

The earlier run stopped at the first DSIM read after282,534,650 instructions
and transferred both explicitly empty NVRAM banks. The new scenario reaches
the display driver earlier and transfers only the first4096-byte bank portion
before its stop; instruction totals and unrelated driver completion are not
monotonic progress measures. The earlier prepared boot, storage and clock
assumptions still apply. Neither run establishes complete NVRAM initialization.

An independent DSIM clock component implements the idle clock-control fields,
PLL band/P/M/S programming, a system-clock stability timer, and exact rational
PLL output frequency. It requires explicit initial idle observations and a
continuously present reference clock. No board reset values or clock routing
are inferred. Polling cannot advance the timer. Live PLL retuning, non-idle
output-clock requests, and unsupported registers refuse without state changes.
After enabling the PLL, timer-register readback remains unavailable by default:
the evidence does not distinguish a live counter from a reload latch. The full
controller can explicitly select either view for a modeled input scenario;
neither choice is asserted as measured target behavior.

The register fields and timer behavior are corroborated by Samsung's
[S5PC100 manual, sections 3.2.20-21](https://www.manualslib.com/manual/1231975/Samsung-S5pc100.html?page=788)
and its [P/M/S equation](https://www.manualslib.com/manual/1231975/Samsung-S5pc100.html?page=789),
and the matching target initialization sequence. The manual describes related
IP; its analog operating limits and reset values are not asserted for S5L8920.
The component models digital configuration and timer behavior, not analog lock
acquisition or reference-clock loss.

The unchanged iBoot prefix `0x4ff095b4..0x4ff09676`, including its original
clock-gate helper, programs `CLKCTRL=10000004`, `PLLTMR=300000` and
`PLLCTRL=06831572`. It uses the original display table at `0x4ff2b244`:
320x480, 24-bit color, P=12, M=343, S=1. With an explicitly supplied 24MHz
reference, the rational output is 343MHz. The table selects one data lane;
the later initializer first enables two lanes and then narrows to that value.
The kernel tree's two-lane property is not substituted for this firmware input.

The controller now also supports software and functional reset from stop state,
programmed display configuration, FIFO initialization, latched PLL/reset causes
with write-one-to-clear acknowledgement, and two data lanes entering/exiting
ULPS. Reset behavior follows the register groups described in Samsung's
[reset register reference](https://www.manualslib.com/manual/1231975/Samsung-S5pc100.html?page=774);
the [escape requests](https://www.manualslib.com/manual/1231975/Samsung-S5pc100.html?page=779)
remain asserted until the guest clears them. Changed reset values stay unknown
until programmed or explicitly supplied as reset inputs. Functional
reset preserves supported configuration and clears escape requests. Reset
completion requires a stable internal PLL and supplied system cycles. Lane
transitions require supplied PHY cycles and their enabled clocks.

Reset, stop, entry, exit and wakeup durations are explicit functional-model
inputs. Gated lanes retain their internal state; this is an inference from the
original firmware's temporary gating during ULPS reconfiguration, not a target
measurement. The precise PHY divider, clock-domain phase and analog timing
remain unverified. Initial observations require a released, idle controller;
active-image, HS-clock, packet, BTA and remote-reset operations still refuse.

An isolated firmware test starts after the logger call with a prepared caller
frame and executes the unchanged initializer body through its epilogue. It
includes the real gate helper and both complete 1000us delays, with original
timebase reads and unsigned64 division. The body returns zero after356,572
instructions with one supplied cycle per instruction in each clock domain;
a coarser independent schedule returns after11,774. Both produce the original
one-lane configuration and ready-global store. Neither substitutes a call,
instruction or successful return. Independent missing-system, missing-PHY and
missing-timebase cases remain waiting in their corresponding poll or delay.

An explicitly attached controller now receives aligned word accesses through
the board's checked bus at `0x89000000`, with raw physical gate `0x19` as used
by the original iBoot helper. The caller supplies and owns the configured
controller and its clock inputs. A closed gate pauses both supplied clock
domains and refuses MMIO. Missing attachment retains the unmapped bus behavior;
unsupported widths, registers and aperture tails remain checked failures.
Board reset/free detach the peer without changing its state. IRQ wiring and
automatic clock-source/divider conversion remain unavailable.

The unchanged body/dependency witness also runs through this board connection,
with the same return counts and missing-clock waits. Before attachment it stops
after48 retired instructions at its first DSIM write, `0x4ff0961c`, targeting
`0x89000008`. Board tests cover gate pauses during PLL and ULPS transitions,
first-failure preservation, and a real CPU load that halts without consuming a
refusal value and then retries a supported register through a warmed fetch.

SWRST now reads the pending request and clears when modeled reset release
completes. Reads cannot complete it. This self-clearing command is a functional
inference from the original firmware's release wait without a clearing write
and a [later Samsung DSIM implementation](https://android.googlesource.com/kernel/google-modules/display/+/661047c0c1c4939aa81fcc822bb21dc5a484f913/samsung/cal_9845/dsim_reg.c).
That later generation has a different register map; it is supporting family
evidence, not proof of the exact older readback.

Optional idle observations cover TIMEOUT, INTMSK, write-only port read responses,
empty RXFIFO data, memory characteristics and PHY/additional words through7c.
They must be supplied before guest writes; missing words stay unavailable.
They cannot override status, event, reset, clock-ready or programmed display
values. TIMEOUT follows guest writes and its supplied reset value. Other
observations are fixed across the supported idle control operations. Those
inputs do not enable writes; the optional packet link separately owns PKTHDR
and populated RXFIFO. An empty RXFIFO response requires known emptiness.
The caller explicitly chooses any otherwise unspecified bus response and timer
view. These inputs do not establish physical reset values or implement packet,
analog tuning, interrupt-mask or panel operations.

A bounded test first executes the original initializer, then the kernel's
unchanged32-word snapshot loop with a prepared object and MMU. Before these
reads were supported, it stopped after9 retired instructions at SWRST04.
With explicit idle observations it completes194 instructions and stores all32
words, with no peripheral mutation from reading. Separate cases use reload
versus remaining timer values and varied passive PHY/tail words. This proves
the snapshot loop under those inputs, not the complete driver or boot chain.
The logger/prologue, complete panel startup and displayed image remain open.

### Command packets and the panel query

The controller now has an optional LP short-command link. PKTHDR writes queue
supported short packets; an independent escape-clock input advances their
explicit transmission interval. An external consumer must actually accept each
packet before its FIFO entry drains. An absent consumer leaves the original
guest poll pending. FIFO full/empty flags follow occupancy. RXFIFO reads consume
one received word; status polling neither consumes packets nor advances time.
Capacities and transition durations are supplied functional inputs, not measured
S5L8920 constants. The board's physical gate pauses the new escape-clock input.

HS clock requests separately transition through supplied PHY cycles. Automatic
read-command bus turnaround waits `2 + STOPstate_Cnt` escape cycles, then waits
for an explicit peer direction change. The programmed TIMEOUT fields bound
turnaround and receive silence. Zero timeout counts are unavailable because
their target semantics are not established. These fields, receive causes and
FIFO flags follow the related-family [escape-mode description](https://www.manualslib.com/manual/1231975/Samsung-S5pc100.html?page=779),
[timeout register](https://www.manualslib.com/manual/1231975/Samsung-S5pc100.html?page=776)
and [FIFO status register](https://www.manualslib.com/manual/1231975/Samsung-S5pc100.html?page=787),
corroborated by the actual target's polling instructions. No interrupt is wired.

The receive boundary accepts an explicitly supplied, already decoded packet.
It preserves the supplied header word and stores little-endian payload words,
with zero padding in the final partial word. Long response length must match its
header; channel, capacity and response type are checked. ACK/error reports and
decoder-supplied ECC/CRC failures produce their respective causes. Wire-level
encoding and integrity checking remain the caller's responsibility. No panel
response is supplied implicitly. Long TX packets, HS packet data, forced BTA,
TE, active scanout and a complete panel device remain unsupported.

A bounded firmware witness executes the unchanged original initializer, short
write helper, HS-ready helper, Pinot B1 query and ID decoder with a prepared
caller. The previous library refuses the first packet store at `0x4ff09952`.
With explicit link inputs, the original NOP write returns after43 instructions,
the HS helper after16, and the query/decoder segment after300. A declared test
reply produces ID `0x00e50486` through the guest's original bit packing. This is
an explicit virtual reply based on a supported variant lead, not a measured
physical panel or an automatically populated kernel device-tree property.

A19-byte reply is fully drained while the caller stores only its15-byte
capacity. Missing PHY/escape clocks or a missing consumer leave the original
polls pending; absent direction changes, missing replies and error reports
reach original failure logging with ID0. The subsequent full-kernel witness
uses the stateful panel and original handoff described below.

### Panel reset, commands and firmware handoff

An optional Pinot panel now receives the controller's transmitted short
packets. Its identity bytes, channel and nonzero timing intervals are explicit
inputs. Initial power is off and reset is asserted. The caller supplies stable
power and elapsed time; no PMU rail state or physical calibration is inferred.
A powered reset-low pulse must meet its configured minimum, followed by the
release interval. Power loss, reset assertion and software reset cancel pending
replies. Unavailable panels and other channels do not answer; the transmitter
can still drain its packet. Only an accepted B1 read starts a delayed response
through the controller's actual turnaround and RX FIFO paths. FIFO backpressure
retains it; transaction timeout prevents it leaking into a later read.

Supported commands include NOP, software reset, sleep in/out and display
off/on. Sleep transitions require elapsed time. Display on requires completed
sleep exit, and sleep entry requires display off. Repeated levels or pending
sleep commands do not restart deadlines. Unsupported commands refuse without
partially consuming the controller queue. These are control states, not pixels
or evidence of a working display.

The board connects the panel reset to GPIO40, which the original helper reaches
using identifier `0x500` at `0x830000a0`. Attachment requires an explicitly
programmed output and packet controller. GPIO writes drive reset without
advancing time; unknown drive modes and conflicting flash chip-select ownership
refuse atomically. Raw DSIM gate19 controls link service. Board reset/free
detach the borrowed devices without changing them.

A seven-case firmware witness executes the original initializer, both actual
GPIO reset helpers, short-command/HS helpers, query/decoder and complete LCD
device-tree producer at `0x4ff0662c`. It includes the original property lookup,
string comparison and ARM memory copy. Caller frames, parser root, stable
power and independent clocks are prepared inputs; no guest instruction or
return is replaced. The declared variant decodes to `0x00e50486` and is present
in this exact iBoot's variant table at `0x4ff29bf8` with mask `0x00ffffff`.
This establishes a supported virtual variant, not a measured physical identity.

The query takes300 instructions and the complete handoff733. All57,856 bytes
of the matching device tree are compared: only the expected `lcd-panel-id`
and15-byte `raw-panel-id` contents change. No-power, insufficient-reset and
missing-recovery scenarios reach original error logging without identity.
Zero identity is rejected without modifying the tree. A missing ID property
allows only the raw bytes to be copied; an absent parser root returns zero
without changing either property, so a zero return alone is not success.
Original short helpers also drive sleep/display state transitions.

The initial witness guard omitted the memory-copy function's final `BX LR` at
`0x4ff22ecc`; including that original instruction completed the witness. The
failed run is retained. The full Pinot entry, PMU power coupling, calibration
commands, long packet transmission, active scanout and a connected boot chain
remain unverified or unimplemented.

A subsequent prepared kernel run uses the original-produced controller,
panel, GPIO and matching device-tree state. Only the four ID bytes and15 raw
identity bytes lose their diagnostic read guards; unrelated guards and the
raw field's trailing byte remain. Independent link clocks, stable power and
panel time are explicit inputs, retaining the earlier boot assumptions. The
kernel loads `0x00e50486` at `0x8093f860..0x8093f862`. Its later zero check and
panel-start return were not observed before the SWI stop. A register-only
observation repeat has the same remaining complete trace. This proves actual
identity consumption, not completed panel startup or a connected boot chain.

## SWI foreground request transport

The matching kernel's AppleSamsungSWI setup writes STR-delay at `0x89100024`
and control at `0x89100000`. Its device-tree logical gate `0x35` maps to raw
physical gate `0x0e`: observation around the original provider call shows
only that gate changing from0 to15. The tree supplies divider12, delay8000,
four command bits, current command1 and voltage command10. The original
arithmetic programs delay16000 and control `0x00000b03`.

An optional controller now supports those programming words, bounded15-bit
primary/secondary data and foreground command1/3. The caller explicitly
supplies an initially powered idle controller, nonbusy status readbacks and
a nonzero eligibility interval in divided NCLK cycles. These are functional
inputs, not inferred silicon reset values or measured serial timing. A
foreground request captures its data, command, control and STR-delay. Busy
persists until an external receiver accepts that exact request after its
supplied interval. Polling cannot advance time or complete a request; elapsed
time alone cannot clear busy. Reset cancels pending work and preserves the
sequence counter so stale acceptance cannot consume a subsequent request.

The board's checked word accesses use raw gate `0x0e`; a closed gate refuses
MMIO and pauses the explicit clock input. Missing attachment remains unmapped.
Unsupported widths, registers and aperture tails still fail. Board reset/free
detach the borrowed controller without changing it. Secondary activation and
arbitration, IRQ handling, wire serialization, STR-delay/mode interpretation
and regulator/backlight effects remain unavailable. A qualified receiver must
implement those relevant semantics before acknowledging a real transfer.

A bounded witness executes unchanged original setup arithmetic, including its
ARM division helper, through board MMIO. Before attachment it stops at the
first STR-delay write after81 instruction attempts. With the component it
reaches the setup boundary after87 attempts. The original foreground helper
returns after35 instructions with packed word `0x0da5` and command3. A declared
diagnostic receiver can accept it after924 supplied NCLK cycles, using divider12
and the explicit77-cycle interval; this is not physical timing or a PMU model.
The original busy test then returns idle, and its idle wait completes.

Without clocks acceptance fails. Without a receiver even arbitrarily many
cycles leave busy asserted, and the original wait reaches its actual time
helper. The original secondary submission refuses at `0x808ad6a2`; a closed
gate refuses the setup write. Prepared objects, MMU and tree scalars are
explicit witness inputs; no instruction or return is replaced. In the prepared
whole-kernel continuation, the original provider opens raw gate `0x0e`, both
setup writes complete, and the provider closes the gate. No SWI request or
transfer occurs before the later audio-complex stop. This proves setup with
the explicit controller inputs, not communication with a regulator.

## Accelerometer configuration

The matching tree identifies `i2c0/accelerometer` at address `0x1d` with
`accelerometer,lis331dl` and `accelerometer,lis302dl` compatibility strings.
The original kernel uses the AppleLIS302DL-compatible probe at `0x80acfa54`:
it reads register `0x0f`, checks the transfer result, then programs interrupt
configuration. Its call stack was observed at the pending read. It does not
compare the returned ID byte. LIS331DLH is a different driver personality.

An explicitly initialized LIS331DL now supports its `0x3b` identification byte,
the three control registers, and both wake configuration/threshold/duration
sets. SDO selects address `0x1c` or `0x1d`; subaddress bit7 controls incrementing.
The documented cold configuration is `07/00/00`, with wake configuration zero.
This is an explicit powered, calibrated device state, not inferred board state.
The register behavior follows STMicroelectronics
[AN2960, sections4 through8](https://datasheet.octopart.com/LIS331DLTR-STMicroelectronics-datasheet-66494901.pdf).
This copy is hosted by a distributor; the document is ST's June2009 revision1.

Setting BOOT starts a calibration reload with a caller-supplied nonzero duration.
Only elapsed device time clears BOOT; polling cannot complete it. User control
registers survive the reload. Configuration writes during reload refuse, as do
reserved accesses and unsupported status, acceleration, filter and interrupt
source reads. No acceleration samples, calibration coefficients or sensor
interrupts are fabricated. Motion conversion and interrupt generation remain
unimplemented; this is identification/configuration support, not a full sensor.

`s5l8920_lis331dl_service` services a matching pending controller request through
the existing FIFO/completion/IRQ path. It stages the device update and leaves
unsupported requests pending atomically. It does not attach a default sensor,
advance device time or establish I2C wire timing. The caller owns the external
device; controller reset/free leaves it unchanged. Existing machine defaults
are unaffected.

With the endpoint explicitly supplied, the prepared kernel completes the ID
read and writes `22=c0`, `22=c0`, `30=00`. It advances from247,423,500 to
249,751,450 retired instructions, then requests a byte from address `0x1e`,
subaddress `0xc0`, at PC `0x8078c1e2`. That request remains pending without
ACK or data. The diagnostic retains the earlier prepared boot inputs and
assumed clock schedule; the configured1ms calibration reload is not exercised
by this four-transfer sequence. No full boot or sensor-data claim follows.

## Compass configuration and calibration availability

The next observed request belongs to the AppleAKM8973S probe at `80811624`.
Its actual stack contains helper return `808113b5` and probe return `80811681`.
The matching device tree selects `compass,akm8973s` at address `0x1e`.
Its initializer enters EEPROM mode, checks status, reads `66/67/68`, returns
to power-down, and copies those bytes into the gain registers.

The explicit AK8973 model follows AKM's
[MS0561-E-01, sections5 through9](https://media.digikey.com/pdf/Data%20Sheets/AKM%20Semiconductor%20Inc.%20PDFs/AK8973.pdf).
It supports reset, status, configuration, bank wrapping and supplied EEPROM
bytes. Power-down and EEPROM startup waits advance only with device time.
Gain writes discard upper bits as specified in section5.3.3 note8. Reset
preserves supplied calibration. Unknown EEPROM, measurement, programming and
test operations remain unavailable. Output registers contain reset values only.

This family reference and matching driver do not prove every AK8973S detail.
Device initialization explicitly assumes a qualified reset; reset wiring,
physical calibration, sensor conversion and interrupt generation remain open.
`s5l8920_ak8973_service` completes supported requests through the controller's
existing FIFO/status/IRQ path; refused transactions leave both states intact.
No default attachment or scheduler changes the existing iPhone OS 3 machine.

With an explicitly reset device and no supplied EEPROM bytes, the prepared
kernel completes `C0` read, `E0=02` write, and another `C0` read. At 249,829,225
retired instructions, PC `8078c908`, it requests `1e/66/1`. Calibration remains
unknown and 268,542ns of startup wait remain under the existing assumed clock
schedule. This request stays pending. These three transfers establish further
driver execution, not completed calibration, connected boot or SpringBoard.

## DART programming and page translation

The matching device tree places two `dart,s5l8920x` controllers at
`0xbfe00000` and `0xbff00000`. The original AppleH2PDART initializer programs
sixteen segment-table addresses through offset `8`. Its mapper allocates
64 KiB of table storage, writes 32-bit entries for 4 KiB pages, and returns
device addresses in `0x3c000000..0x3fffffff`. Entry bit0 marks validity;
bits12..27 select a physical page in `0x40000000..0x4fffffff`.

Each controller now retains those indexed segment writes and bounded config
fields at offset `0x0c`. The uncached translation API walks the actual
little-endian guest RAM tables, including independently placed segment
tables. It checks enable state, unknown/invalid segments, invalid or unsupported
PTEs, table backing, target backing, and page boundaries. Failure preserves
the caller's output. Results are host diagnostics; hardware error-status
encoding, counters, interrupt delivery and peripheral DMA clients remain
unimplemented. The observed `0x702` flush command has no cache work in this
uncached model; no cache topology or completion timing is claimed.

Initial config and command-window observations must be supplied explicitly;
there are no power-on defaults. Initial config must disable translation.
Config reads retain the programmed fields. Writes to the command or table
window invalidate that window's read observation: subsequent reads remain
guarded because their values are not established. In particular, the model
does not return a last-written command as guessed readback. Indexed data,
error status, miss counts and unsupported commands also remain guarded.
Functional reset restores supplied initial observations and invalidates every
segment; free/init discards them. The two banks are independent.

The original complete initializer passed 54 cases with production writes and
config reads, using fifteen explicitly supplied post-write table-port
observations per case. These observations are a private fixture input, not
production behavior. Four direct-bus original-code cases verify the cold
config-read stop and the next unknown table-port read after one segment write.
Twenty-one original map/dummy-page/unmap cases produce entries that the
production walker translates and then refuses after unmapping. These block
fixtures use explicit objects/registers; they do not execute IOKit allocation.
Host tests cover every device page, both banks, boundaries and rejected state.
This adds functional translation, not a completed DART driver or boot.

The strict host suite passes 80 tests and the shipping configuration passes
75. The existing prepared-kernel trace and all three bootloader validation
traces are unchanged. With no supplied DART observations, the farther
prepared-kernel diagnostic still stops after 216,561,265 instructions on the
config read at `0xbfe0000c`; its checked failure now identifies unavailable
register state. That diagnostic retains unmeasured GPIO/clock/handoff inputs
and is not a complete bootloader-to-kernel execution.
Supplying three explicit, unmeasured zero initial observations for each DART
allows the original initializer to program its first segment. It then stops
29 instructions later, at `0x808b0e7c`, on the unknown post-write table-port
read at `0xbfe00008`. No such initial values are production defaults.

## DRAM controller configuration

The register layout and initialization commands at `0xbfc00000` match the
[ARM PL340 manual, DDI0331E](https://documentation-service.arm.com/static/5e8e330588295d1e18d39313?token=).
The exact integration/revision is not established. The configuration model
covers words at offsets `0x0c..0x50`, sixteen QoS words at `0x100..0x13c`,
and the two observed chip configurations at `0x200/204`. It supports the
bootloader's 32-bit LPDDR configuration, documented field widths, and reserved
encoding checks. LPDDR forces the CAS half-cycle bit to zero.

Functional reset enters Config and invalidates programmed words; target
tie-off/reset register values are not supplied. Configuration reads require
prior writes. Reads and writes require Config or Low_power state. Unsupported
bridge widths, alignment, fields and encodings fail without changing state.
Controller status, direct commands and state transitions remain refused;
unimplemented register offsets and the separate PHY remain unmapped.

The matching LLB initializer at `0x840033c0` computes the **15-bit** refresh
period at `0xbfc00010` using the original clock getters and division routines.
Explicit 200 MHz/divisor1 inputs produce `0x617`. The kernel's startup cache
keeps only bits0..13; that mask does not reduce the hardware register width.
Its isolated rescale block divides the cache and writes through the original
virtual register accessor. Full LLB configuration starts at `0x840031c8`.

This implements the configuration phase only. The existing host RAM backing
remains independent of DRAM initialization; no PHY readiness, refresh traffic,
mode-register execution, suspend/resume, controller-start success or complete
bootloader handoff is established. Completing those behaviors requires the
controller commands, PHY and memory timing to be connected.

## Audio NCO configuration

Aligned word accesses at `0x84300014/18/1c` retain the audio NCO control and
two coefficients. Control accepts the observed values `0` and `0xd00`.
Access requires configured raw audio gate `0x18` with all four low bits set.
Missing, closed or intermediate gate state refuses the transaction without
changing programming. Closing and reopening the gate preserves coefficients;
no clock edge or frequency is inferred from opening it.
Coefficient writes establish independent full-width values; reads require
prior programming. Control/status reads, other control values and unsupported
widths or alignments fail without altering configuration. Functional SoC reset
invalidates programming; free/init clears it. No reset values, reference
frequency, generated clocks, readiness, DMA or PCM output are inferred.
The shared audio control at `0x84304000` remains unimplemented.

The matching kernel's `AppleS5L8920XAudioComplex` setter at `0x8078b7a0`
doubles the requested value into coefficient A, subtracts its reference word
into B, and writes control `0xd00`. Both operations wrap at 32 bits, so
unsigned B greater than A is valid. The getter at `0x8078b740` reads both
coefficients and uses the original division helpers to recover the configured
value. The whole setter rejects a zero request before programming; entering
the internal programming block with zero tests arithmetic only.

The original setter and getter bracket their register accesses with the same
provider gate calls used by AudioComplex startup. A whole-kernel observation
around the startup call at `0x8078b4cc/ce` establishes that logical device-tree
gate `0x37` changes raw gate `0x18` from `0` to `0xf`; the other51 gates are
unchanged. Removing only the two observer lines reproduces the preceding
complete trace, including the same unsupported shared-control write.

Ninety-five isolated original-kernel cases passed against these production
registers, including wrapped coefficients, the original division routines and
three withheld writes. The normal cases explicitly open raw gate `0x18`;
missing or closed gate cases stop at the original first coefficient store,
PC `0x8078b7cc`, without programming a word. Objects and block entry points
are explicit; complete IOKit methods and gate callbacks are outside this
witness. All eighteen
connected LLB scenarios passed, preserving pending transfers when responses
are unavailable. With the existing explicit zero saved-state inputs, LLB
advances three instructions to 1,192,354 steps, PC `0x8400989c`, where the
write of `7` to `0x84304000` still fails. This is configuration support, not
audio output or a complete bootloader/kernel handoff.

The audio-reference handoff is separately executed using the original iBoot
BSS clear, clock calculation, node/property lookup, index15 getter and store.
The previously supplied PLL/selectors and explicit24MHz reference yield
162MHz at `audio-complex/ncoref-frequency`. Five cases cover normal execution,
missing property/root, a changed PLL multiplier and a disabled PLL. Only the
four property bytes change. The prepared kernel then loads that value at
`0x8078b404` and stores it in its audio object before reaching `0x84304000`.
This is an original-firmware calculation from diagnostic clock inputs, not a
physical frequency measurement or a connected bootloader chain.

Strict host tests passed 79/79 and shipping tests 74/74. The separate
prepared-kernel trace remains unchanged: 182,680,916 steps, stopped at
`0x807887ba` on an unprepared GPIO pin 0 read at `0x83000000`.

## D1755 saved software state

Registers `0x60..0x63` now retain the matching firmware's saved flags, boot
stage, counters and reason byte. They support exact byte reads/writes at
I2C0/address `0x74`. Each read requires an explicit initial byte or completed
guest programming. Initial configuration is immutable and idempotent; it
cannot reload guest writes or replace a previously unconfigured written byte.
Functional SoC reset retains these external PMU bytes; free/init clears them.
There are no inferred initial values, automatic counters, watchdog expiry or
reset effects. The separate `0x6f` endpoint is unchanged; reset control `0x5f`,
RTC bytes `0x64..0x67`, scratch `0x6d` and GP bytes `0x70..0x73` are outside
this service. Unsupported requests remain pending without changing state.

Original LLB helpers `0x84001c70/0x84001ce0` passed 6,190 isolated calls
against the model with actual CPU I2C exceptions. These cover all byte values,
unknown reads, cache hits/misses, slot 1/9 aliasing at `0x60`, invalid indices,
withheld responses and NACK/cache divergence. A rejected write leaves device
state unchanged even though the original unverified helper returns zero and
updates its cache optimistically. The prior implementation leaves all eight
tested read/write requests to these four bytes pending.

Eighteen connected original-reset scenarios passed. The real caller at
`0x84000894` reads the previous stage from `0x61`, copies nonzero reasons to
`0x63`, and updates a saturating nibble counter in `0x62`; equal cached results
skip the write. Later code stores stage `0x10` and reads flags from `0x60`.
Before NCO support, execution with four explicit zero initial bytes reached
a checked unmapped write of zero to physical `0x84300014`, PC `0x84009892`, after
1,192,351 steps. The matching device tree places that address in the
audio-complex/AMC range. Missing initial bytes or withheld transactions remain
pending at their specific requests. These results do not establish audio
behavior, bootloader/kernel handoff, physical saved-state defaults or full boot.

## D1755 LDO and pin configuration

The configuration service stages the matching board's LDO suspend image with
an exact two-byte write of `00 40` at `0x12`. The original kernel obtains
`ldo-suspend-defaults` from the supplied device tree and limits its four-byte
property to two bytes before calling the real I2C write path. This image is
stored separately from the live enable and voltage fields. It has no inferred
initial value. Repeated programming is idempotent; functional SoC reset retains
the external PMU programming, while free/init clears it.

Partial or different suspend images remain refused. Reads also remain refused
after programming: retaining the requested bytes does not establish hardware
readback. Applying this configuration during sleep, regulator transitions and
physical power effects are not implemented. A successful transfer uses the
existing I2C completion and interrupt path without creating PMU events.

Exact byte transactions now retain eleven LDO settings at `0x17..0x21`,
selector fields at `0x22`, and enable fields at `0x10/0x11`. Their masks come
from the matching kernel's twelve-row table at `0x80af1f90`: the twelfth rail
has an enable bit but no voltage setting. Known initial bytes are explicit,
immutable and idempotent. Field updates preserve all other bits. The complete
bytes written by the original LLB tables at `0x840115c4/0x840115d4` can also
establish state at `0x10/0x11/0x17..0x21`; other full-byte changes remain
unimplemented. There is no inferred complete image or initial value for `0x22`.
Reads remain pending until their byte is known.

The existing configuration service also supports independent pin bytes
`0x50..0x57`. Exact byte writes establish their retained configuration, including
the LLB's `0x50=0x11`. The kernel's eight-byte reads and writes starting at `0x50`
are also supported. Reads require every requested byte to have been programmed;
a complete image write commits all eight bytes after successful I2C completion.
Partial and shifted bursts and neighboring registers remain unimplemented.
These bytes are separate from
the packed selectors at `0x59..0x5b` and the explicit live status at `0x05..0x08`.
Neither LDO nor pin configuration creates electrical pin samples, voltage
readiness, settling, power transitions or spontaneous events. Functional SoC
reset retains the external PMU state; free/init clears it.

The original GPIO-default routine at `0x80ae6bbc` reads the independent status
image and all eight pin bytes, applies the device tree's eight mask/value pairs,
then writes the complete pin image. The matching activation property does not
use the conditional status-bit merge; the quiesce property uses status bits
for pins 4 and 6. Supporting these transfers supplies no initial status or pin
values. Unknown inputs still stop the original request. This is configuration
support, not evidence of physical pin behavior or completed kernel startup.

The complete original kernel LDO method at `0x80ae8a74` passed 3,522 calls
against the production service, exercising the rail table, flags, enables,
preserved bits, bounds and missing responses. It executes the original mutex
and ARM division helpers. Its lower bound is unsigned and its upper bound
signed: some large unsigned arguments therefore reach masked programming.
The model retains those raw field encodings without inventing physical
voltages or changing firmware arithmetic. The original pin method at
`0x80ae8c28` passed 1,124 calls with prior explicit byte programming, varied
masks/values, invalid indices and withheld responses. Its OR value is not
limited by its clear-mask argument. Packed-selector arguments are zero in
that witness. Both kernel fixtures prepare objects/MMU/time and execute the
original polling transport with CPU IRQs masked.

Connected original LLB reset execution now completes all fourteen later
write/readback checks through production LDO and pin services, after the
earlier regulator sequence. With explicit boot-state `0x80`, zero trim field
and initial control `0x23=0`, the complete PMU initializer returns at
1,162,251 steps, followed by the saved-state operations described above.
This progression uses unchanged firmware and actual CPU I2C exceptions.
It does not establish a bootloader/kernel handoff, physical PMU behavior or
complete boot. The prepared kernel's separate GPIO pin 0 stop is unchanged.

## D1755 voltage programming and readback

Exact byte transactions at I2C0/address `0x74` retain voltage configuration
registers `0x14`, `0x23`, and `0x2c..0x2f`. Reads require explicit initial
state or supported completed guest programming. Initial configuration is
immutable and idempotent; it cannot reload or replace guest writes. The voltage
bytes update their five-bit setting while preserving known upper bits. The
bootloader's complete writes with zero upper bits are also accepted at `0x14`
and `0x2f`. Other upper-field changes remain refused. Control `0x23` requires
an initial byte and supports retaining or setting bits 6/7; clearing them or
changing other bits remains unimplemented. Unsupported shapes, neighboring
registers and stale transaction tokens leave state unchanged. Functional SoC
reset retains the external PMU domain and cancels controller transactions;
free/init clears it. No analog voltage, readiness, settling or power transition
is supplied by these configuration operations.

The unchanged LLB helper at `0x84001d44` programs `0x14`, writes zero to
`0x2f`, and reads/modifies/writes `0x23` with bit 6 set. It verifies all three
writes. Its integer range is 725 through 1500; it rounds the encoded setting
upward. The complete kernel method at `0x80ae6c68` instead rounds downward,
preserves the upper three bits, and addresses `0x14/0x2c/0x2d/0x2e` for its
four indices. Initialization reads `0x14/0x2d/0x2e/0x2f` and separately sets
control bit 7. Those differing offsets are present in the original firmware;
the emulator preserves distinct addresses and does not infer preset aliases.

Private original-code witnesses passed 792 LLB helper calls and 1,307 kernel
method calls against the production service. They exercise voltage bounds,
upper-bit preservation, argument modes, missing initial state and withheld
transactions. LLB uses actual CPU I2C exceptions after reset initialization;
the kernel fixture supplies objects/MMU/time and uses original polling
transport with CPU IRQs masked and a zero delay argument. Invalid-index
logging and nonzero delay execution are not covered. Separate finite-response
experiments show the LLB helper can return zero despite failed verification;
its return value alone is not evidence that programming succeeded.

Connected original-reset execution with explicit boot-state, CHIPID word 3
and control `0x23` inputs now completes all seven regulator transfers and
three successful readback checks. A supplied zero trim field reaches that
return after 993,014 steps, then writes register `0x17`, the first entry in
the later configuration table supported above. Missing initial control
remains pending at its `0x23` read. The other boot-state branch still requires
explicit ADC conversion inputs. The separate prepared kernel remains at its
unprogrammed GPIO pin 0 stop; no connected kernel boot or SpringBoard is proven.

## D1755 software boot-state byte

Register `0x6f` retains a software boot-state byte used by the matching LLB
and kernel. Exact byte reads require an explicit initial value or a completed
guest byte write. Writes replace all eight bits; reads retain them. Repeating
initial configuration cannot undo guest programming, and initial configuration
after an unconfigured guest write is refused. Neighboring registers and other
transfer sizes remain unsupported. Functional SoC reset retains the external
PMU byte and cancels controller transactions; free/init invalidates it. This
logical retention policy does not establish physical power sequencing or
invent an erased value, autonomous flag changes, or power transitions.

The LLB's complete general-purpose helpers pass 190 finite-observation cases,
covering all ten logical indices, their cache behavior, invalid indices and
I2C failures. Slots 1 and 9 alias register `0x60`; slot 0 maps to `0x6f`.
These mappings do not enable the other registers. A separate 1,546-case
witness executes the original slot-0 helpers against production storage,
covering all 256 byte values through actual writes and uncached reads with
real CPU I2C interrupts. The firmware returns success for an unverified write
even after NACK and retains an optimistic cached byte. The device model keeps
its actual stored byte unchanged on NACK; invalidating the firmware cache
exposes that difference through the original read helper.

Twenty connected original-reset cases exercise the boot-state decisions,
including missing reads/writes and explicit NACKs. For supplied byte `0x00`,
the decision returns at instruction 946,247 and the firmware starts ADC
channel 2; no analog completion is invented. For supplied `0x80`, the
firmware clears the flag and reaches the still-unsupplied identification
word at `0xbf50000c`, stopping at `0x84008570` after 952,192 instructions.
For `0xaf`, two original writes retain `0x0f` and reach that same stop after
958,028 instructions. These are explicit input cases, not discovered
power-on defaults. The old-library witness retains the original `0x6f` read
pending. Neither these paths nor the separate kernel trace prove full boot.

## D1755 events, status and interrupt masks

The event endpoint models explicitly initialized event latches at registers
`0x01–0x04`, independent supplied status at `0x05–0x08`, and guest-programmed
interrupt masks at `0x09–0x0c`. New explicit causes accumulate even while
masked. The observed byte read at `0x02` consumes that byte only; the four-byte
read at `0x01` consumes all four returned bytes. Repeated reads cannot reload
consumed events. Status reads retain the separate supplied status image.
Unsupported access shapes remain pending without changing device state.

The matching iBoot pin-level getter at `0x4ff061c0` reads one byte at `0x07`
and returns the selected bit for pin indices 0 through 7. This exact byte read
shares bits 16 through 23 of the existing four-byte status read at `0x05`.
It requires explicitly supplied status, preserves event latches and masks,
and does not derive electrical levels from pin configuration. Writes and
other partial status-read shapes remain unsupported.

Read-clear is a logical model inferred from the matching kernel's complete
event handler, its saved wake-event merge, and its low-level parent interrupt.
The original handler performs one event read and no event-register write
acknowledgement, including for unregistered events. The matching device tree
specifies GPIO pin 157 with flag 1; the original GPIO driver maps that flag to
low-level mode and acknowledges the GPIO latch after its callback. These
consumers establish the software contract; physical byte-phase and propagation
timing have not been measured.

Once initial events and every mask byte are known, unmasked pending events
drive GPIO 157 low. Consuming or masking them releases the pin, while the
GPIO's own pending latch still requires its normal acknowledgement. Initial
events, status and masks are never invented. Explicit ADC completion also
raises register `0x02` bit 5 when the event domain is configured. CPU execution,
polling and time advancement cannot complete a conversion. Functional SoC
reset preserves the external PMU domain and cancels controller transactions;
free/init invalidates the supplied state.

The unchanged connected LLB passes 38 cases through the production endpoint,
covering every event bit, status decisions and withheld service. With supplied
causes/status, its original cache helper returns after 934,495, 940,371 or
940,376 instructions and reaches a one-byte read at `0x6f` for general-purpose
slot 0. Fourteen or fifteen actual CPU interrupts execute the I2C completion
path. The LLB witness does not program PMU masks or invent their reset values.

A separate 37-case witness executes the complete original kernel event
handler and polling transport against the model. It verifies event indexing,
saved wake-event merging and the original ADC callback in idle state. Explicit
unmask programming and GPIO configuration verify that the handler's event read
releases the pin while retaining the GPIO latch until acknowledgement. CPU
interrupts are masked in this prepared kernel context and client interrupt
records are unregistered; full driver initialization and a PMU-triggered CPU
exception through that kernel driver have not been demonstrated. Both old-library
witnesses leave the original event read pending. Full boot remains unproven.

## D1755 configuration and complete kernel field updates

The configuration endpoint supports the original LLB's `0x2a` programming at
register `0x24` and its independently verified readback. This is the only
supported value for that register: other values, including the kernel's
power-related bit-7 update, remain pending. It is not an assumed reset value
or a power-transition model.

Registers `0x59–0x5b` hold eight packed three-bit configuration fields. Matching
kernel code at `0x80ae8c28` masks and replaces an indexed field while preserving
its neighbors. The endpoint supports the observed individual byte accesses
and three-byte transfers starting at `0x59`. Reads require every requested byte
to have been programmed. Electrical routing and GPIO level effects remain
unmodeled. Functional SoC reset preserves this external PMU programming while
cancelling I2C transactions; free/init invalidates it.

The connected, unchanged LLB completes all six initial PMU write/read
checks at instruction 928,600, with thirteen actual CPU interrupts. It then
waits on a four-byte read of event/status registers starting at `0x01` when
event service is withheld.
The first observed `0x02` byte in this configuration-only witness is supplied
explicitly. The event model above has separate supporting consumers.

A separate witness executes the complete original packed-field helper,
including its real uncontended kernel mutex acquire/release and original
PMU/ARMIIC/N88 polling transport. It passes 2,051 cases: four initial patterns,
eight field indices, eight masks and eight input values, two rejected indices,
and withheld configuration. Fields crossing byte boundaries retain neighboring
bits. Missing service leaves the original request pending. CPU, stack, objects,
MMU and transport-lock ownership are prepared explicitly; no firmware code is
patched. The separate per-pin register update is masked off in this witness.
This is not complete kernel driver initialization or a full boot.

## D1755 ADC requests and supplied conversion results

The ADC endpoint accepts exact one-byte control accesses at I2C0 address
`0x74`, register `0x30`, and two-byte result reads at `0x31`. Guest programming
establishes control; there is no assumed initial value or analog input. Bit 4
starts a conversion and stays busy until a matching explicit completion supplies
both raw result bytes. The original consumers decode `(low & 3) | (high << 2)`;
the unused upper bits of the low byte are retained exactly as supplied.
The request exposes the channel and mode through the original control byte.
Bit 6 and unrelated registers remain unsupported.

Busy conversions cannot be replaced by another start. Clearing start cancels
the request; accepted control programming invalidates the previous result.
Tokens reject stale/duplicate completions and never wrap. Functional SoC reset
preserves this external PMU domain while cancelling I2C traffic. This is an
explicit reset policy, not measured power sequencing. No conversion duration,
voltage or calibration is inferred. Configuring the event domain above enables
the completion event and its mask-controlled GPIO interrupt.

The unchanged connected LLB now completes control write/read verification at
instruction 881,952, using five actual CPU interrupts across the initial PMU
observation and modeled transactions. Its next pending request is a one-byte
write to register `0x24` when configuration service is withheld. No conversion
result is supplied in this trace.

A separate witness calls the complete original ADC routine at `0x84001e10`
from prepared context after the real bootloader setup. All 16 channel values
return the supplied results through the original I2C interrupt path, including
zero, maximum and arbitrary raw result bits. Channel 3 executes its original
`0xa3` preparation, delay and `0xb3` start. Completion follows zero, one or two
busy polls. Withheld completion reaches the original timeout formatter and
failure return, preserving the output canary; withheld time never returns.
Firmware code is unchanged. The witness supplies a logical clock schedule,
not measured timing, and does not represent connected boot continuation.

The matching kernel uses the same result format but waits on a PMU interrupt
instead of polling the control byte. That event-register and interrupt path,
the subsequent PMU initialization registers and complete boot remain missing.

## I2C pin initialization and D1755 control readback

With explicit live levels on GPIO pins 32, 33, 36 and 37, the unchanged LLB
completes I2C initialization after 852,802 instructions. Its original sequence
configures buses 0 and 2, alternates each clock pin between input and low output
over 19 writes, selects peripheral mode and enables interrupts 19 and 17.
The matching device tree confirms
these pin assignments. These pins start with input enabled: an inactive GPIO
observation cannot replace the missing live level. Both supplied levels pass;
no pull-up, electrical bus timing or attached slave is inferred.

The next request reads one byte from D1755 register `0x02`. A private witness
supplies that response explicitly and runs the actual CPU interrupt vector,
original I2C callback and return. The byte reaches the bootloader's cache;
a supplied NACK produces the original failure return. Register `0x02` event
and power-on behavior remain unmodeled.

The control endpoint implements exact one-byte accesses to I2C0 address
`0x74`, register `0x0d`. `s5l8920_pmu_control_configure` requires an explicit
initial byte. `s5l8920_pmu_control_service` completes only the specified active
request. Writes may change bit 4, as used by the matching kernel, while every
other supplied bit is preserved. Unsupported field changes, lengths, registers
and targets remain pending. Power-transition bits, analog effects and readiness
are unavailable. Identical repeated configuration preserves guest programming;
functional SoC reset preserves the external PMU state and cancels I2C traffic.
This reset policy is explicit, not a measurement of backup-power behavior.

The original LLB writes zero, separately reads it back and verifies equality
at instruction 870,290. Three actual CPU interrupts handle the initial observed
byte and two modeled control transactions. The next request writes `0x80` to
PMU register `0x30` and remains pending when ADC service is withheld.
Withheld control input or an unsupported
change retains the earlier request. A separate 16-case kernel witness executes
the original bit-4 update with eight initial bytes, including repeated calls
after SoC reset, through the complete PMU/ARMIIC/N88 polling transport. It uses
prepared mid-function context and already-owned locks; it does not execute
complete kernel driver initialization. All firmware code remains unchanged.

All 79 strict and 74 shipping tests pass. Inherited firmware checks retain
their earlier input boundaries. The separate prepared kernel trace is byte
identical at its 182,680,916-step GPIO stop. A connected bootloader-to-kernel
handoff and SpringBoard remain unproven.

## Five UART ports and original interrupt delivery

The matching device tree and LLB register table identify five UART banks at
`0x82500000` through `0x82900000`, each with a 4 KiB aperture and interrupt
sources 24 through 20. Each bank has independent state and host input APIs;
the existing UART0 APIs remain wrappers for bank 0. Initial divisor values
must be supplied explicitly before a read or written by the guest. Functional
reset restores only supplied divisors and clears traffic and CTS observations.

The UART model supports the firmware's 8N1 and 8N2 frames, 16-byte FIFOs and
single-byte holding registers when FIFO mode is disabled. A second stop bit
takes an additional bit period. CTS gates the start of each frame; withdrawing
it preserves the frame already transmitting. Missing CTS cannot start an
automatic-flow-control transmission. UART0's no-modem capability permits its
original initializer to leave modem control unwritten.

Completed error-free receive frames and explicit receiver idle-timeout events
latch separate interrupt causes. Enables and write-one-to-clear acknowledgements
control delivery without consuming held bytes. The original interrupt handler's
status writeback ignores live read-only bits. Error status is zero under the
error-free input contract; erroneous frames remain unsupported. Timeout duration,
receive-line sampling, DMA and modem-status registers are not modeled.

With explicit divisor inputs, the unchanged LLB initializes all five ports,
then stops after 637,176 steps at Thumb `0x84001030`, reading GPIO pin 33 at
`0x83000084` while preparing I2C pins. The console ends in non-FIFO mode with
receive interrupts enabled. The UART4 frame uses two stop bits and a different
divisor. Withheld divisor input preserves the earlier UART read failure.

A host diagnostic supplies 70 UART0 frames after this initialization. Each
causes an actual emulated CPU interrupt through the original page tables,
vector, wrapper, dispatcher and registered callback, then restores the
interrupted general registers, status and VFP state. The original firmware
stores 64 bytes and drops six when its software buffer is full. A separate
30-call witness checks receive and timeout dispatch, ring storage, semaphore
updates and acknowledgement on all five ports. No firmware code is patched.

The firmware's clock-gate indices differ from the device tree's indices.
Source cycles continue to come explicitly from the caller after external
gating; no physical clock hookup or receive-timeout threshold is inferred.
These UART witnesses establish bounded host execution, not a complete iOS boot.
All 79 strict and 74 shipping tests pass. Earlier firmware checks preserve
their input boundaries; the separate prepared kernel trace remains unchanged
at its 182,680,916-step GPIO read failure.

## USB control requests and original bootloader shutdown

`s5l8920_usb_control_configure` supplies each supported control word explicitly:
controller clock gates at `0x86100e00`, and PHY power, clock selection, reset
request, configuration and tuning at `0x86000000` plus offsets 0, 4, 8,
`0x1c` and `0x44`. The matching device tree identifies both register ranges.
[OpeniBoot's S5L8920 PHY definitions](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8920/includes/hardware/usbphy.h)
and its initialization/shutdown sequence corroborate the original firmware
accesses. The matching kernel's PHY driver uses the same control words;
its software power states do not establish hardware readiness.

Reads require explicit configuration. Aligned word writes preserve unknown
bits; tuning changes accept only the observed `0xe3f` field pattern.
Functional reset restores supplied words. These are logical control requests:
no analog lock, power transition completion, cable state or transfer engine
is modeled. Status, FIFO and endpoint accesses refuse in every control state.

Unchanged LLB, iBoot and iBSS functions pass 72 complete initialization,
repeated-initialization, shutdown and reinitialization calls across six clock
frequencies. Their original time conversion and delay loops execute using an
explicit source-clock schedule. Thirty negative calls verify that each missing
control, frozen time and unsupported power-field change prevents completion.
The old core has 36 expected register refusals. Unit checks also cover widths,
unknown bits, functional reset, instruction retry and state preservation.

With the earlier explicit board inputs and six zero initial USB control words,
the connected original LLB completes its shutdown sequence and stops after
243,328 steps at Thumb `0x84004478`, reading UART0's unprogrammed divisor word
at `0x82500028`. Its executable bytes remain unchanged. Without USB inputs,
the earlier 214,677-step stop remains; the diagnostic now identifies a refused
register in the recognized USB range instead of an unmapped address.
This is a host execution witness, not a complete bootloader/kernel handoff
or proof of working USB transfers.

All 79 strict and 74 shipping tests pass. Earlier firmware checks preserve
their input boundaries, and the separate prepared kernel trace remains
byte-identical at its 182,680,916-step GPIO refusal.

## Firmware-selected RAM boot mapping

The word at `0xbf100000` now requires an explicit initial value through
`s5l8920_miu_configure`. Matching LLB, iBoot and iBSS routines preserve its
upper 30 bits and write selection 1 or 2 into the low two bits. Their epoch
checks and mismatch paths execute unchanged. Selection 2 maps installed RAM
at physical zero, as independently identified by
[OpeniBoot's S5L8920 entry sequence](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/arch-arm/entry.sx)
and the matching iBoot kernel handoff described below.

Guest writes select the real shared RAM window, invalidating translation and
host-pointer caches and the exclusive monitor when the mapping changes.
Other selections remove the RAM window; their low boot sources remain
unimplemented and accesses stop explicitly. Upper-bit changes, written modes
0 and 3, incorrect widths and unconfigured accesses refuse. No reset value,
readiness status, transition delay or ROM/SRAM contents are invented.
Functional reset restores the supplied initial word and its supported mapping.
Once configured, MIU control owns the selection; the direct-handoff API cannot
override it. Repeating the same initial configuration preserves guest writes.

The private original-code witness passes 2,304 cases across all initial modes,
each upper bit, selector arguments and matched/mismatched epoch checks. The
old core has 1,920 expected register refusals. Successful original calls also
verify RAM alias coherence and refusal of unimplemented low boot sources.
Unit checks cover reset, invalid writes, instruction retry, and cached data
and instruction accesses across a guest mapping change.

With the earlier explicit clock, GPIO and POWERID inputs plus MIU initial
words 0, 1, 2, 3 or `0xfffffffc`, the unchanged LLB passes its original MIU
initialization. Each run next
stops after 214,677 steps at Thumb `0x84004ba0`, reading `0x86100e00`.
The MIU routine performs two reads and one write, selecting 1. This is a
host diagnostic with bounded SRAM supplied by the fixture; it does not prove
the non-RAM boot alias or a complete bootloader-to-kernel handoff.

All 79 strict and 74 shipping tests pass. Inherited firmware checks retain
their earlier stops without new MIU input. The separately prepared kernel
trace remains byte-identical at its 182,680,916-step GPIO refusal.

## Inactive GPIO readback and board identification

Matching LLB, iBoot and iBSS GPIO helpers read a pin's configuration before
enabling input. The original table programs pin 15 to `0x0d1e`, with input
enable clear. These consumers establish control-field readback, but do not
establish the inactive data bit's physical behavior.

`s5l8920_gpio_inactive_readback` supplies that bit explicitly for an already
programmed interrupt-off pin whose input enable is clear. Word reads combine
the retained control fields and the supplied bit. No value is inferred from
a live pin sample or a guest data-bit write. Accepted configuration writes,
live sample events and functional reset invalidate the observation; repeated
reads retain it. Invalid calls and refused accesses preserve state. This is
a bounded logical input policy, not a physical disabled-sampler or latch model.

The original three-image helpers pass 17,664 complete calls across all 368
pins, eight modes and both inactive-data values, with opposing live samples
and guest-written data bits. Missing inactive data remains a checked failure.
The unchanged LLB also completes its board-identification routine under five
explicit input variants. It enables eight pins, waits using supplied timebase
ticks, reads the straps, restores the original configurations and publishes
the independently checked POWERID cache. Inactive readback values do not
determine the sampled board identity.

Without MIU configuration, with one explicit timebase tick per retired
instruction, these LLB runs stop
after 213,980 steps at Thumb `0x8400860a`, reading `0xbf100000`. Missing
inactive observations retain the earlier 212,219-step stop; missing live
samples stop at 213,616 steps on pin 12. A frozen timebase stays in the
original delay at the one-million-step limit. These are diagnostic inputs,
not measured strap levels or hardware timing. The next register's low two
bits are accessed by the original `miu_init` routine after an epoch check;
its supported RAM selection is described above.
The separate kernel GPIO stop and the missing complete boot remain unchanged.

## Explicit POWERID state

The aligned word at `0xbf100158` requires an explicit initial value through
`s5l8920_powerid_configure`. Guest writes retain the upper three cache bytes
and flags 0 and 1. Changes to unknown bits 2 through 7 refuse atomically;
unconfigured accesses, other widths and misalignment also refuse. Identical
configuration preserves guest writes, while conflicting host input refuses.
Functional reset restores the supplied initial word and free/init clears it.
These are logical policies, not measured silicon reset behavior. No identity,
GPIO samples, entropy, power behavior or relationships between fields are generated.

The matching LLB and iBSS initialize the high byte to the larger of four and
the seven-bit epoch field in CHIPID0. The matching LLB, iBoot and iBSS read
bits 16 through 23, store two GPIO-derived cache bytes with flag 0, and set
flag 1 after accessing separate nonce-cache words. Those separate words and
nonce production remain unmodeled. Their epoch guards retain the original
mismatch path and still encounter a checked refusal on the unimplemented
`0xbf100000` control register when the epoch matches.

The private original-code witness passes 8,704 cases across 128 epochs and
four raw initial-word patterns: complete epoch initializers and byte getters,
bounded cache/flag fragments with explicit register inputs, and both guard
paths. This includes 384 refused attempts to clear unknown low bits. Fragment
checks do not establish GPIO sampling, nonce production or fatal-handler execution.

With an explicit POWERID input, no inactive GPIO observations, and the existing
clock/SRAM assumptions, the
unchanged LLB entry advances to 212,219 steps, then stops at Thumb `0x84001030`
reading GPIO pin 15 at `0x8300003c`. The original initialization table has
already programmed all 368 pins; the refused read is in a later GPIO
configuration helper's read-modify-write operation. No external samples
are supplied. The separate kernel GPIO diagnostic and the missing connected
bootloader/kernel handoff remain unchanged.

## PLL programming with explicit reference clocks

The three PLL words at `0xbf100004..0xbf10000c` require independent disabled
initial values, reference frequencies and nonzero settling intervals through
`s5l8920_pll_configure`. No initial configuration is installed by default.
Programming supports enable bit 0, shift bits 1 through 3, multiplier bits
8 through 15, divisor bits 20 through 25 and the observed bit-30 control.
An enabled word requires bit 30 and nonzero multiplier/divisor fields.
Bit 17 is read-only status; guest writes cannot set it. Unsupported fields,
widths, alignment and unconfigured accesses remain checked failures.

The matching LLB disables all three PLLs, programs `0x40609601`,
`0x40605103` and `0x40606403`, then polls bit 17 for each. Matching iBoot's
reader at `0x4ff13c48` establishes the wider six-bit divisor and three-bit
shift fields. The pinned S5L8920 source corroborates the programming and
polling sequence but uses narrower masks; those masks are not imported.

Settling is an explicit logical timing assumption, not measured acquisition
time: every accepted enabled write starts the supplied interval, and only
`s5l8920_pll_reference_clock` with a nonzero configured reference frequency
can finish it. CPU steps, reads and board timebase ticks do not advance it.
Disabling cancels settling. Identical configuration preserves guest state;
conflicting inputs refuse. Functional reset restores the supplied disabled
words and free/init invalidates them. Host events preserve CPU state and
latched diagnostics.

`s5l8920_pll_rate` exposes the settled rational output rate, using a 64-bit
numerator and 32-bit denominator; disabled output is zero, and pending or
absent output refuses without modifying the caller's outputs. This does
not model analog lock, jitter or waveforms, and PLL outputs are not yet
connected to CPU/peripheral scheduling.

The unchanged original LLB completes clock setup, publishes the expected
28 frequencies and executes all 368 GPIO initialization writes. Four input
variants, using explicit 24 MHz reference clocks, 32-cycle settling and one
reference cycle per retired instruction and no POWERID input, stop after
83,532 steps at Thumb `0x840085d4`, reading `0xbf100158`. Increasing
settling to 97 cycles produces more original polling and reaches the same
stop after 83,721 steps. With frozen or absent reference clocks, the original
first PLL poll remains busy at the 100,000-step observation limit.
These schedules are diagnostic assumptions, not physical timing measurements.

The 12 original iBoot reader/math/device-tree-copy cases now use actual
board reads for all three PLLs and 25 selectors, retaining the independent
frequency oracle. No firmware instructions are changed. The separate kernel
diagnostic still stops at unprepared GPIO; a connected bootloader/kernel
handoff and complete boot remain unproven.

## Bounded clock-selector programming

The 25 aligned selector words at `0xbf100010..0xbf100070` retain accepted
guest programming. Reads before a guest write remain checked failures.
Ordinary selectors accept bits 0 through 11; selector 10 also accepts
bits 16 through 23, and selector 15 accepts bits 0 through 19. Unknown
bits, other widths and misalignment refuse atomically. Functional reset
invalidates all selector programming, and free/init clears it.

The matching LLB writes the initial table at `0x84011a7c`, which agrees
with the pinned S5L8920 source, and later selects final divisors from
`0x84011a18`. The original iBoot reader at `0x4ff13b40` consumes these
fields. The matching kernel performance controller saves all 25 words at
`0x80788e3c..0x80788e8a` and restores them with selected low-byte changes
at `0x8078a006..0x8078a056`. This supports retaining a bounded configuration
image. Firmware-written bits 11 and 19 are retained without interpreting
them as ready, busy or lock indicators. Physical selector update timing
and output waveforms remain unmodeled; PLL behavior is described above.

With the same explicit chip-ID and gate inputs described below, all four
original LLB entry runs initially completed 52 gate writes and 25 selector writes.
Before PLL support, they stopped at `0xbf100004` at Thumb `0x84008916`, after
19,099 steps and 19,098 retired instructions. Code bytes remain unchanged.
The selector checkpoint's private iBoot witness passed 12 reader/math/device-tree-copy cases
using actual board selector reads, two initial-memory patterns and six copy
bounds, with three explicit private PLL register inputs. The PLL model above
replaces those inputs with actual board reads and preserves the 28-word oracle.
The separate full kernel GPIO stop remains unchanged.

## Bounded clock-gate programming

The 52 aligned gate words at `0xbf100078..0xbf100144` require independent
initial inputs through `s5l8920_clock_gate_configure`. Guest writes can set
the low nibble to `0xf` or `0`. Gate zero also retains bit 4 as CPU idle
control. The attached IOP/FMI gates retain their separately modeled bit-31
reset behavior. Other upper-bit changes, unconfigured accesses, mixed modes,
other widths and misalignment remain checked failures. Reapplying an identical
initial input preserves guest programming; replacing it fails. Functional reset restores the supplied
initial words, and free/init invalidates them.

IOP board service lends access to these same 52 gate words for one instruction
at a time. It accepts only aligned words and shares the AP's existing checked
write semantics without calling the AP bus or changing its failure latch.
The standalone `s5l8920_iop_service_with_gates` entry point requires an explicit
capability; earlier standalone entry points leave the aperture unavailable.
All transient bindings are cleared after execution or failure. Endpoint
reentry, unsupported reset domains and IOP self-reset during execution refuse.
Gate accesses do not advance time, service NAND chips or count as RAM writes.

The matching IOP reset dispatcher at `0xc01c` maps bank zero/one to gates
`0x26`/`0x27`. Its original helper at `0x8b28` reads the gate at `0x8b58`,
asserts bit 31, calls the delay at `0xa174`, and clears bit 31. These writes
drive the existing attached FMI reset model: assertion cancels controller
commands and buffered data, preserves diagnostic counters, and requires
reprogramming after release. It does not reset a separately owned NAND chip.
The delay reads the supplied 64-bit counter through `0x6dc0` and divides by
24 through `0x6dfc`. Reads and CPU execution do not supply those ticks.
This models the observed register path; physical clock/reset latency remains
unmeasured and no chip response or FTL readiness follows from its completion.

The matching kernel method at `0x80789e10` reads offset `0x78`, sets bit 4
when its third argument is one, clears it otherwise, and writes the resulting
word through the original accessor at `0x8078a094`. Its AppleARMPlatform
caller forwards the boolean through a virtual method. The later symbolized
driver names the equivalent operation `cpuIdleUpdate`; that later name is a
correspondence lead, not a target hardware specification. A bounded witness
executes the original target method and both accessors with explicit caller
objects and gate inputs. The original store regression failed before support.

This is retained control programming, with actual readback and reset to the
supplied initial value. It does not itself execute WFI, reset the CPU, advance
time or inject an interrupt. The existing functional WFI implementation remains
independent: early completion is permitted by
[ARM DDI0406C.b, B1.8.14](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Physical clock stopping, power transitions and idle latency are unmodeled.
Tests cover both control directions, all other gate/upper-bit boundaries,
invalid low-nibble combinations, atomic rejection, first-fault retention,
initial-input idempotence, reset/free and functional WFI in both control states.

The matching LLB initializer and iBoot gate helper agree with the pinned
[S5L8920 clock implementation](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8920/clock.c)
on the enable/disable masks. The original iBoot helper's upper address bound
also establishes the 52-word extent. Generic bit-31 reset pulses outside the
attached IOP/FMI models remain unsupported. Retained clock configuration does
not establish clock signals, source readiness, physical timing or power-on values.

The private witness passes 848 original iBoot helper cases: 832 valid calls
across all gates, four raw input patterns and four on/off arguments, plus
12 upper-bound returns and four preserved failures from wrapped addresses.
With explicit initial gate words of either `0` or `0xf`, and either tested
chip-ID bit-8 input, the unchanged original LLB completes all 52 gate writes.
It selects gates 0, 24, 36, 37, 44 and 49 and clears the others. All four runs
initially stopped at the first dynamic-clock selector write: `0xb00` to `0xbf100070`
at Thumb `0x84008904`, after 18,756 steps and 18,755 retired instructions.
The selector implementation above advances these runs to the first PLL read.
The initial inputs remain diagnostic configurations, not measured reset
state. Leaving gate inputs absent preserves the earlier 17,032-step stop;
the full kernel's separate GPIO stop also remains unchanged.

## Explicit identification inputs

Four aligned words at `0xbf500000..0xbf50000c` can each be supplied through
`s5l8920_chipid_configure`. An unconfigured word refuses reads. Identical
configuration is idempotent; changing a configured value fails. Functional
reset retains these fixed inputs, while free/init invalidates them. Guest
writes, byte/halfword accesses and misaligned words remain checked failures.
No revision, unique identifier, reserved-bit value, CPU identity or device-tree
property is inferred from configuring another word.

Matching LLB and iBoot consume offsets 0, 8 and 12. The pinned
[S5L8920 chip-ID definitions](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8920/includes/hardware/chipid.h)
also identify offset 4 as the SPI clock-selection input. These observations
establish read locations and consumers, not measured fuse values or fuse
programming behavior. The private witness passes 2,176 original LLB/iBoot
getter calls over 128 explicit input sets, including all 64 revision pairs
with and without unrelated bits, epoch fields, the permuted identifier and
the raw two-word pair.

The unchanged LLB entry still stops at `0x840084ca` when word 0 is absent.
Supplying either `0` or `0x100` exercises both alternatives of its first bit-8
consumer. The original code selects ACTLR.WFINOP and disables ACTLR.L2EN;
neither input is asserted to be a physical sample. After extending only the
private SRAM fixture for the LLB's explicit 4,096-entry page table at
`0x84034000`, both runs build the table, enable the MMU and reach the next
checked read: clock-gate register `0xbf100078` at Thumb `0x84008868`, after
17,032 steps and 17,031 retired instructions. The fixture ends at `0x84038000`;
this does not establish physical SRAM capacity or complete LLB execution.
Other identification words stay unconfigured in these entry runs. The full
kernel's separate unprepared-GPIO stop remains unchanged.

## Partial GPIO configuration, polling and interrupts

The matching device tree describes 46 ports of eight pins at `0x83000000`,
inside a 4 KiB aperture. Aligned word accesses now support explicitly
programmed digital input/output controls: `0x210`/`0x212`, data bit 0, and
pull selections `0`, `0x80` or `0x100`. Matching iBoot helpers at
`0x4ff02008..0x4ff02132` establish these polling forms;
[the pinned S5L8920 OpeniBoot source](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8920/gpio.c)
corroborates them. Its separate older interrupt-controller definitions are
excluded because they conflict with the matching kernel's register layout.

`s5l8920_gpio_input` supplies a digital sample for a selected pin. Input
reads require a supplied sample; ordinary input-enabled GPIO output reads
reflect the programmed level. Peripheral-selected output reads use the
supplied pad sample instead.
Input writes and pull settings do not fabricate external levels. Functional
reset preserves samples but invalidates all pin programming. No power-on or
bootloader pin configuration is assumed. Unsupported widths, field
combinations and unknown register bits retain checked failures before
mutating device state.

Drive field `0xc00` is retained with these controls. Peripheral selectors
`0x20`, `0x40` and `0x60` can be stored with masked input, output or
interrupt-off mode, preserving either input-enable setting. Enabled reads
use explicitly supplied pad samples; a retained output-mode bit does not
make the GPIO software latch the peripheral's signal source. Disabled
peripheral reads remain guarded, including interrupt-off mode. This models
configuration and explicit digital samples, without peripheral signal
generation, routing or analog drive behavior.

Interrupt-off mode `0xe` requires mask `0x10`. With no peripheral selector,
its disabled-input reads accept the separate inactive-data observation
described above. That observation API rejects peripheral-selected pins.
Every accepted configuration write invalidates the prior observation.
Neither a pull setting nor a written data bit establishes a disabled
sampler's readback value.

The matching kernel's selector requests 2, 3 and 4 change only bits `0x60`,
preserving the low mode, DATA, input enable, interrupt mask, pull and drive
fields. In 1,800 isolated original-driver cases, the previous model refused
1,440 output/off cases; all now pass. These execute the original post-lock
dispatch and virtual register store with synthetic objects and mappings,
without executing locking or establishing peripheral traffic. Host tests
cover sampled and unavailable readback, opposing output latch/sample values,
pending-cause preservation, interrupt suppression, rejected fields and reset.
Peripheral selections combined with GPIO interrupt modes remain refused.

Validation passes all 80 strict and 75 shipping tests. The normal LLB,
two finite audio-prefix scenarios and canonical prepared-kernel traces stay
unchanged. With the preceding diagnostic's same harness and explicit inputs,
including its 30 unmeasured DART post-write observations, the kernel passes
the GPIO31 selector write and reaches 226,950,746 instructions. It then stops
at PC `0x8061caa6` on an unmapped word write of zero to `0x82000000`.
This retains the diagnostic's GPIO, clock, gate, DMC and handoff assumptions;
it establishes neither a connected bootloader handoff nor a complete boot.

Masked output mode also accepts input-disabled controls `0x12`/`0x13`,
retaining pull and drive fields without a peripheral selector. The matching
kernel's output configuration body at `0x807886e4..0x80788768` changes mode
and level while preserving bit `0x200`; its getter explicitly selects input
mode before reading DATA. Programming an input-disabled output clears any
previous inactive observation, and DATA reads remain guarded. The off-mode
observation API does not supply readback for outputs. This models the
programmed output latch, without establishing an electrical pad level.

The first 224 pins also support high/low level, rising/falling edge and
either-edge interrupt modes (`0x204`, `0x206`, `0x208`, `0x20a`, `0x20c`).
Control bit `0x10` masks delivery; seven status words at offsets
`0x800..0x818` acknowledge selected causes by writing one bits. The matching
kernel's configuration, mask and dispatcher routines establish the mode
fields, masking and acknowledgement order. A
[later Apple GPIO driver](https://kernel.googlesource.com/pub/scm/linux/kernel/git/jikos/trivial/+/1ff2fc02862d52e18fd3daabcfe840ec27e920a8/drivers/pinctrl/pinctrl-apple-gpio.c)
corroborates the trigger polarities and drive/input-enable field names;
that is a cross-generation inference,
not a measurement of N88. Its different masking and parent-group layout
are excluded.

This logical model latches events while masked and suppresses parent source
94 until unmasked. An initial sample cannot create an edge; active levels
relatch after acknowledgement. Programming a level mode evaluates the supplied
sample, while configuration changes cannot create edges. External source 94
is combined with the GPIO cause, and reset clears internal pending state.
These sampling/latching choices do not establish physical timing, debounce
or analog behavior.

The private firmware witness passes 14,352 original iBoot polling calls,
including all pins, pull selections and both levels. With explicitly
prepared pin state and a synthetic object/vtable, the original kernel
initialization loop reads and writes all 368 controls and fills its shadow
array. The initial polling implementation stopped at the following
interrupt-status clear at physical `0x83000800`. Interrupt support now allows
all seven clears and passes 1,120 original kernel configuration, unmask and
full-dispatcher cases. A synthetic client callback verifies its arguments
and observes edge acknowledgement before dispatch and level acknowledgement
after dispatch. This is bounded driver execution, not an executed bootloader
or complete GPIO controller.

The matching restore initializer at `0x84000ef4` writes 368 halfwords from
its pin table as word controls. That table exactly matches iBoot's table.
The original wrapper and initializer now complete all 368 writes on the
checked board in 1,850 instructions; the earlier controller refused the
first control, `0xd1e`. This witness inspects stored configuration without
inventing input samples or reading disabled samplers. Another 26,496
original iBoot selector calls preserve drive and pull fields while reads
reflect explicit samples. The matching normal LLB contains the same table
at `0x840112b0`. Its early startup calls wrapper `0x84008194`, which invokes
initializer `0x84001078`. That original wrapper and initializer also complete
all 368 writes on the checked board in 1,850 instructions. This establishes
the table and its initialization consumer in normal-boot firmware; the
isolated call does not establish the later iBoot/kernel handoff or disabled
input readback. The table is not installed as a board default.

The separately prepared kernel diagnostic now reaches GPIO after its
chip-revision, platform clocks, USB frequency, backlight calibration,
product identifier and VIC geometry handoff checks. These retain explicit
configuration assumptions, including an unmeasured B5 selection, generic
CPU identity, nominal clock schedule and zero backlight calibration.
Without prepared GPIO state, the current stop is still the first control
read at Thumb `0x807887ba`, physical `0x83000000`, after 182,680,916 steps.
Recognizing that address changes its diagnostic from unmapped to unprepared;
this GPIO implementation does not advance that full trace or establish boot.

All 79 strict and 74 shipping tests pass, including checked CPU load/store
retries and all pin/pull/sample combinations. The existing 276 firmware cases
pass and the canonical 61,650-step ECC-guard trace is unchanged. The full
182,680,916-step trace matches the GPIO polling checkpoint, including the
unprepared-pin failure and preserved post-stop bus diagnostics.

## SPI programming and explicit word link

The matching device tree describes SPI controllers at `0x82000000`,
`0x82100000` and `0x82200000`, each with a `0x1000`-byte aperture. SPI0 has
a NOR-flash child, SPI1 has the N88 multitouch device, and SPI2 uses a
separate baseband driver. The first two match `AppleSamsungSPIController`.
These device descriptions do not supply NOR contents or calibration data.

The separate S5L8920 component accepts the matching driver's stopped-state
initialization: control offset `0` receives zero, pin offset `0x0c` receives
zero, and status offset `8` acknowledges `0x0040000f`. Control zero establishes
a stopped controller and does not reset pin programming. The pin latch accepts
the observed software CS bit `1` after a known control command. Partial event
acknowledgements retain which causes have been cleared; writing zero clears
none. Both original service routines write the full status word back, including
the read-only FIFO levels at bits `6..15`. Acknowledgements ignore those level
bits and clear only selected event causes. They neither change FIFO contents
nor establish FIFO observations. Unknown status fields reject the whole write.
Programming alone generates no pin traffic or peripheral responses. Interrupt
service requires the explicit word-link contract described below.

While stopped, the component also stages configuration at offset `4`, the
clock-divider word at `0x30`, and word-delay programming at `0x38`. It retains
the two timing words independently without guessing register-width masks or
effective clock rates. Configuration accepts the fields emitted by the driver,
with master encodings `0/3`, mode encodings `0/1/2`, and word-size encodings
`0/1/2`; unknown fields and encodings are refused before changing state. This
staging does not validate timing, activate DMA, or enable serial traffic.
Readback remains guarded for all three registers. Repeating stop preserves
their programming; reset invalidates it.

Control bits `2/3` reset the TX/RX FIFOs independently, establishing an empty
16-entry buffer and invalidating the corresponding count programming. Bit `0`
records an armed request when configuration, nonzero divider, word delay,
pin and both FIFOs are known. Only manual master operation with mode `0/1`
can arm; DMA, automatic transmit and interrupt enables remain guarded.
Nonzero control commands have no inferred self-clearing readback or event
effects. Stopping preserves queued data and programming.

TX/RX counts at `0x4c/0x34` retain independent raw requests after their FIFOs
are known. PIO writes at `0x10` queue raw words; reads at `0x20` consume known
received words. Full TX and empty RX refuse without changing state. There is
no implicit received-data producer or clock advance, so programming and FIFO
access never imply serial progress or count completion. With no word link,
armed configuration changes are limited to selecting PIO mode; timing changes
require stopping.

Without a configured link, status reads remain refused: FIFO knowledge alone
does not establish all event causes. Unsupported fields and non-word accesses
remain guarded before state changes. Component reset invalidates all
programming rather than supplying a silicon reset image. The controller
gaps, additional banks and legacy address
aliases remain unmapped; no S5L8900 SPI behavior is imported.

Original version-1 driver helpers decode five-bit FIFO levels at status bits
`6..10` and `11..15`, and compute free space using capacity 16. A fixture
checks these helpers in 3,468 cases. Its separate initialization fixture uses
synthetic objects and mappings, starts after the provider's clock callback,
and ends before event-source enable. Thus it establishes the original three
register stores, not a complete power transition, serial transfer or boot.
The pinned public S5L8920 implementation corroborates register locations;
its larger FIFO limit is not adopted. A newer Apple SPI implementation has
additional FIFO/interrupt registers and cannot establish N88 interrupt causes.

The strict 80-test and shipping 75-test suites pass with this component. Full
boot checks preserve the canonical prepared-kernel trace and all three LLB
traces. With the same explicit diagnostic inputs, the farther kernel run
advances from the reset/start stop at 226,951,206 to 226,951,489 instructions and
stops at `0x8061caa6`, writing interrupt-enable configuration `0x002041b8`
to SPI0 at `0x82000004`. Reset, counts, chip selection and initial FIFO
loading now execute through the component. Those inputs still include
unmeasured GPIO, clock, DART and boot-state observations. This is a bounded
initialization result; complete boot, serial transfers and hardware validation
remain unproven.

Separate original-code fixtures cover the kernel request validator and timing
calculations, plus 1,536 iBoot interrupt-service and 16,464 kernel PIO-service
cases. Service fixtures supply finite, explicit status and received data;
their acknowledgements pass through the production component while transfer
and configuration writes are recorded. They verify acknowledgement handling
without establishing hardware event generation, FIFO thresholds, IRQ delivery
or serial exchange. The original iBoot chip-select helpers also execute
against the production pin latch. None of these fixtures is a full boot run.

A further configuration fixture varies the divider and delay source clocks
independently. The original methods produce separate ceiling-divided timing
words, and 72 register-prefix cases pass through the production component,
stopping before the original reset/start write. The kernel's request validator
accepts products up to a computed divider of 2,048; the matching iBoot warns
at 1,024 and still writes the value. These are software behaviors and do not
establish a silicon register-width mask.

A further 147 original PIO preparation cases execute through production reset,
count programming, software chip select and initial FIFO priming. The driver
writes control `0x0d` once, programs both counts to the larger buffer length,
queues at most 16 real transmit bytes and then enables interrupts. Production
stops at that interrupt-enable write before changing the configuration. A
separate witness records only that final write and reaches the subsequent
wait boundary. Both use synthetic objects and explicit clocks/PIN input;
zero-length cases are sequence probes, not completed transactions. They
establish neither event thresholds nor serial exchange or interrupt delivery.

An explicit error-free word backend now supplies serial progress. Before
control programming, its caller must provide TX-low (`0..15`) and RX-high
(`1..16`) thresholds. These define a functional level model; they are not
asserted silicon reset thresholds or measured event-edge timing. Status then
combines actual FIFO levels, RX/TX completion causes at bits `0/22`, and
threshold cause bit `1`. Completion causes latch until W1C. An active threshold
level persists through acknowledgement. Configuration enables `7/8/21` route
those causes to VIC sources `29/28/27`, ORed with external inputs. IRQ and FIQ
selection use the existing interrupt fabric.

The caller supplies complete SCK periods after external clock selection and
division: each word needs `8/16/32` periods. Word-delay reference cycles are
supplied separately, preserving the driver's independent timing domains.
This API does not guess a CPU-clock conversion, divider width or physical
source frequency. It exchanges decoded error-free words; the device backend
owns electrical phase, line sampling and received values. Unknown bit-13 wire
ordering and exchange with software CS deasserted remain guarded.

Completed words decrement the programmed counts, return actual transmitted
words, and queue only explicitly supplied receive data. Missing peer input,
receive FIFO overflow, or insufficient output storage refuses the whole clock
call without changing state or output. Partial words retain their remaining
periods; stopping pauses them. Midword reset, count, timing, base configuration
and CS changes refuse. Component reset discards the link and in-flight state.
There is no automatic serial progress from MMIO reads or writes.

With explicit peer words, thresholds and divided SCK periods, 147 original
request/setup and PIO-service cases complete through production FIFO, status,
acknowledgement and configuration handling: 261 service calls exchange 2,748
words, including original dummy transmit bytes and receive discards. Missing
peer input refuses before advancing the request. The fixture still enters
the original routines directly with synthetic objects; it does not execute
their event wait or IRQ dispatcher. No flash identity, status reply, NOR image,
board clock integration or full boot is established by this word-link result.
The boot checks leave the link unconfigured and retain the exact prior kernel
and LLB traces.

## Explicit serial flash device

The optional `sst25vf080b` component implements a candidate 1 MiB serial NOR.
The original flash driver's `BF258E` branch selects matching geometry; its
status and identification helpers emit `05 FF` and `9F FF FF FF`. Other chip
IDs are supported by that driver, so this does not identify the fitted N88 chip.

Device semantics follow [Microchip DS20005045D](https://ww1.microchip.com/downloads/en/DeviceDoc/20005045D.pdf),
sections 4.3/4.4 and tables 4-2/4-3/7-1. Commands cover status/ID and streaming
reads, write authorization, status protection, byte/AAI programming and
sector/block/chip erase. The cold status is `1C` from table 4-2; BP3 is reserved
in the protection table. `WP#` qualifies status lock-down. EWSR authorizes the
next WRSR only. Programs require erased bytes; protected operations are ignored.
AAI advances in aligned pairs without wrapping; its limit clears write
permission, while WRDI exits the mode. WRDI does not cancel a pending
program. Hardware AAI ready/busy output can be sampled without shifting a byte.

Initialization borrows an explicit mutable image; it never creates, clears,
loads or persists one. The owner supplies isolated storage and nonzero
program/erase durations within the specified limits. Elapsed nanoseconds are
separate from SPI clocks. The device waits through the 100 us power-up interval;
polling cannot finish an operation. Image changes occur when its busy interval
expires. Status-latch writes occur at CE rising without an invented busy delay.
This is a decoded-byte interface for qualified mode-0/3 traffic. HOLD changes
must already be qualified at SCK low. Analog edges, partial-byte aborts and
power-loss damage are outside this interface.

The separate `s5l8920_spi_flash_clock` adapter exchanges completed 8-bit words
through production FIFOs and the device command parser. An explicit known-bit
bias resolves undriven MISO; unknown received bits refuse the whole operation.
Both states remain unchanged if any byte, FIFO capacity or input condition
fails. Chip selection, word-delay clocks, flash time and board interrupt refresh
remain explicit caller responsibilities. The board does not attach this part
or invent a target NOR image by default.

Host tests cover protection boundaries, authorization, timing, storage bounds,
command refusal and adapter rollback. With an isolated test image and explicit
pins/bias/clocks, 150 executions of the original PIO setup/service routines
complete 330 services and 618 exchanged bytes. They include status and ID reads,
wrapped reads, programming, busy polling, erase and AAI continuation. The fixture
does not run the event wait or interrupt dispatcher, and supplies no target NOR
contents or automatic board clock scheduling. This addition does not advance
the last verified full-kernel boot stops or establish physical-device behavior.

The board can now bind an initialized, caller-owned chip to an explicitly
configured GPIO output and clock gate before SPI control programming. GPIO
output edges drive CE through the flash command model; a refused command commit
preserves both GPIO and flash state. Shared chip/CS wiring, peripheral/input
pad modes, and midword selection changes refuse. Reset and free detach the
connection without changing the borrowed chip or image. Direct peer-word input
is refused on an attached port so it cannot bypass the connected flash.

An additional clock API accepts undivided PCLK or NCLK cycles, selected by
configuration bit 14 and divided by the raw programmed divider. Fractional
progress starts an active word and is retained across stop/resume and gated
input. Idle and unselected-source cycles do not earn future progress. Direct
SCK-period input refuses while fractional source progress exists. Gate nibble
zero pauses source input, and `F` permits it. Flash CE is independent of the
controller's software CS latch: an unselected chip supplies undriven bits,
resolved only by explicit receive bias. Completed transfers refresh the real
VIC IRQ/FIQ lines without changing CPU registers or a latched bus failure.
Word-delay cycles and flash elapsed time still require separate ordered inputs;
no source frequency or conversion from retired CPU instructions is inferred.

A read-only observer at the retained diagnostic kernel stop identifies the
driver's GPIO selection path, with `internal-cs` false, GPIO 148 driven low,
divider 2, zero word delay and gate 9 enabled. Its cached NCLK is 24 MHz. The
cached 100 MHz delay clock does not establish the live PCLK rate, because the
current selector word differs from the earlier captured input. Gate 9 remains
explicit wiring; no mapping from device-tree logical gate 21 is invented.

Focused tests cover fractional division, independent clock selection and delay,
stop/gate preservation, GPIO command commits, missing data, rollback and reset
ownership. The original 150 PIO transactions also pass through board source
clocks, flash state and interrupt refresh. That fixture retains its original
internal-CS path and supplies separate host GPIO edges; it does not execute the
kernel's GPIO callback or event dispatcher. Automatic scheduling and actual
target NOR contents remain unresolved. No flash is attached by default.
The strict 82-test and shipping 77-test configurations pass. With the same
explicit boot inputs, all three LLB traces and both prepared-kernel traces
match their prior results exactly; this connection alone does not advance boot.

An explicit `sst25vf080b_init_unbacked` option represents the candidate chip
with its entire array unavailable. It allocates no storage and returns no
invented erased bytes. Identification, status, authorization, power-up and pin
behavior remain modeled. READ/fast-READ accepts command/address bytes but
refuses at the first array-data byte. A program or erase that would modify
storage refuses at CE rising; known protected or unauthorized no-ops still
complete without storage access. Refusal preserves the flash, controller and
GPIO states. The ordinary image-backed initializer still rejects missing or
incorrectly sized images; accidental absence does not select this mode.

This permits bounded driver discovery without supplying synthetic target
contents. It does not identify the physical part or establish its warm state,
and it cannot complete any firmware operation requiring unknown NOR data.

`sst25vf080b_init_partial` also accepts a caller-owned 1 MiB image with a
copied availability bitmap for its 256 sectors. Each set bit supplies all bytes
of one 4 KiB sector. Initialization fills no storage. Normal and fast reads stop
at the first unavailable byte, including across sector boundaries and address
wrap. Byte and AAI programming require known erased bytes; an unknown backing
byte containing `FF` is insufficient. A completed sector, block or chip erase
establishes known `FF` contents only for its actual extent, when BUSY expires.
Protected and unauthorized erases establish nothing. The existing complete
image initializer supplies every sector; the unbacked option supplies none.
The SPI adapter preserves both peers when a transfer would cross into unknown
data. The caller remains responsible for persistence and image isolation.

The timing diagnostic now reaches a NOR read at `0x0fc000`. The original
`AppleARMCHRPNVRAM` call stack and matching device tree identify two 8 KiB banks
at `0x0fc000` and `0x0fe000`. Original bank-validation code checks signature
`5A`, a folded header checksum, and Adler32 over bytes `0x14..0x1fff`, returning
the stored generation. Its 36-case isolated witness passes valid-generation
and corruption cases, but also shows that this validator does not validate
inner partition structure. These are synthetic witness inputs, not supplied
boot storage. No NVRAM image, factory data or default flash attachment is
created by the partial-availability API.

The flash target passes 13,577 checks, and the full strict 84-test and shipping
79-test configurations pass. The prepared kernel with no array supplied
reproduces its previous trace exactly, stopping at the same unknown NOR read.

With this explicit candidate attached to GPIO 148/gate 9, a diagnostic kernel
run exchanges eight bytes for status, JEDEC identification and another status
request. Transfers drive the production IRQ path during guest execution. It
advances from 226,951,489 to 247,423,479 instructions and stops at
`0x8078c1e0`, where I2C0 refuses a request-start write of `4` to `0x83200024`.
This run supplies one NCLK cycle per diagnostic timebase tick, with a 24 MHz
elapsed-time conversion, known high MISO bias and cold chip state. These are
explicit unmeasured inputs, alongside the earlier prepared boot assumptions;
they are not a measured CPU clock ratio or a complete bootloader handoff.
Read-only bus observation confirms the guest consumes `FF 1C`, `FF BF 25 8E`
and `FF 1C`, with chip-select stores from the original GPIO accessor. At the
new stop, I2C0 requests one byte from address `1D`, subaddress `0F`; auxiliary
register `14` has not been initialized, so the prepared-field guard refuses.
No NOR array bytes are supplied. The 82-test strict and 77-test shipping
configurations pass; complete boot and physical-device behavior remain unproven.

## Bounded I2C requests and explicit responses

Three I2C apertures at `0x83200000`, `0x83300000` and `0x83400000` use
interrupt sources 19, 18 and 17. The matching iBoot table describes all three;
the device tree instantiates banks 0 and 2. The original bootloader uses
byte accesses, while `AppleS5L8920XI2CController` uses aligned words at the
same selectors. Both forms now share the checked controller. Halfwords,
other byte lanes, unknown selectors and unverified readback remain refused.

A transfer requires an explicit seven-bit address, one-byte subaddress,
zero auxiliary register, length up to 128 bytes and an observed nonzero
control value. Writes require all staged bytes. Starting a command exposes
a stable request through `s5l8920_i2c_request`; it produces no automatic
ACK, receive data or completion. `s5l8920_i2c_complete` accepts a response
only for the matching active request. Successful reads require exactly the
requested bytes. Errors and successful writes carry no receive bytes.
Sequence numbers prevent delayed responses from completing a new request
after a functional reset.

The matching consumers treat status `0x10` as completion and `0x20` as an
error, and acknowledge causes through offset `0xc`. Acknowledgement leaves
unread receive bytes intact; FIFO reads do not acknowledge interrupts.
The observed control values `0x30` and `0xf0` permit logical delivery, and
zero suppresses it. Internal causes are combined with external VIC levels.
These scheduling and delivery choices are an explicit model, not measured
clock phase, arbitration, electrical ACK timing or a complete register
specification. No PMU or sensor register values are supplied by this change.

The private witness executes original initialization writes for all three
banks, and 54 original request/response cases. These cover the iBoot request
function, response fragment and return tail, plus kernel request, status,
receive-loop and acknowledgement fragments. It supplies synthetic objects,
page mappings, call frames and explicit endpoint responses. The kernel
fixture explicitly prepares auxiliary register `0x14` as zero because the
kernel does not write it; iBoot's request function does.

A further 102 cases execute the complete original kernel request method and
polling path through its return, including the real status, receive,
acknowledgement, statistics, interrupt-state and delay routines. The delay
uses the original interval conversion and timebase callbacks. Explicit
24 MHz configuration and one tick per 25 retired instructions supply time;
this is the existing unmeasured nominal clock schedule. CPU interrupts are
masked, and CPU-data context, zero wake offset and scheduler threshold are
prepared explicitly. Access and code-range guards refuse unexpected paths.

These cases cover all three banks, one- and 128-byte reads/writes, success
and error, immediate and delayed responses, and enabled/disabled statistics.
With no response, the original method remains busy after three polls. With
the clock frozen, it stays in its original delay loop without reading I2C
status. No instruction or function return is replaced. Provider clocks,
scheduler waits, interrupt exception delivery and complete driver
initialization remain unexecuted. The separate full kernel diagnostic still
stops at unprepared GPIO state.

## Explicit D1755 clock endpoint

The optional PMU RTC endpoint handles only I2C0 address `0x74`, with exact
four-byte little-endian reads at `0x4c` (raw counter) and `0x64` (offset),
and four-byte offset writes at `0x64`. The matching `AppleD1755PMU` and
`AppleD1755PMURTC` consumers establish these transfers. Older PMU layouts
are excluded. No chip revision, battery, charger, regulator, alarm or
interrupt register values are supplied.

`s5l8920_pmu_rtc_configure` requires explicit counter and offset values.
`s5l8920_pmu_rtc_advance` adds raw units modulo 32 bits; no physical rate
or epoch is inferred. Neither CPU execution nor the board timebase advances
this counter. `s5l8920_pmu_rtc_service` completes only the specified active
request and samples the counter at that call. Unsupported requests remain
pending. Reconfiguration is refused while I2C0 has an active request,
pending status or unread data. Functional reset preserves this configured
clock domain while cancelling the I2C request; free/init invalidates it.
This is an explicit logical reset policy, not measured backup-power behavior.

The private witness runs 72 original PMU/RTC calls through the original
ARMIIC device/controller wrappers and complete N88 I2C polling path.
It verifies repeated reads until the counter agrees, rollover, adjusted
time, offset writes and cached no-op setters. The fixture prepares objects,
MMU/CPU context, already-owned locks, masked interrupts and the nominal
clock schedule; complete driver initialization is not executed. Before the
endpoint was implemented, the first register `0x4c` request remained pending
after 5,000 instructions. The full kernel GPIO boundary is unchanged.

## Explicit generic CPU configuration

The CPU API also provides an **explicit generic Cortex-A8 r3p2 configuration**
for implementation testing. `arm_reset_cortex_a8` validates the revision and
independent L1 instruction/data sizes (16 or 32 KiB) and L2 size (absent,
128, 256, 512 or 1024 KiB) before changing CPU state. Its identity registers
come from [ARM DDI0344K](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde),
sections 3.2.2–6, 3.2.21, 3.2.23 and 3.2.24. This configuration is not
selected by the S5L8920 machine or by ordinary `arm_reset_profile` calls.
It does not establish the 3GS CPU revision or cache geometry.

The selected generic core implements privileged MIDR (including its documented
aliases), CTR, TCMTR, TLBTR, MPIDR, CLIDR, CCSIDR and Secure CSSELR accesses.
CSSELR selects the actual configured cache geometry; its architecturally
unknown reset value is chosen as zero. Reserved upper write bits are refused,
and reserved low selections return the zero CCSIDR encoding documented by
table 3-42. Writes to read-only IDs, User accesses, PC operands, board silicon
ID and unaudited feature-bank selectors remain refused. ARM and Thumb use the
same checked path. Cache maintenance remains synchronous over coherent memory;
this adds no cache timing, ECC faults or Nonsecure transitions. ARM1176
snapshots exclude and clear this inactive state without changing format v32.

Host validation passed all 78 strict and 73 shipping tests, plus the existing
24 timebase and 192 interrupt-controller firmware cases. Old and new snapshot
executables cross-load the same byte-identical v32 save. A separately labelled
kernel diagnostic selects r3p2, 32 KiB L1I/L1D, 256 KiB L2 and no ECC: its
initial run reached Thumb `ADDW` at `0x80021b34` after 76,172 steps.
This is a generic configuration hypothesis. The original N88
diagnostic retains its entire 61,650-step trace and ECC configuration guard.

Thumb `ADDW`/`SUBW` now implement the plain 12-bit immediate, including the
SP forms and `ADR` aliases, for Cortex-A8 and Swift. `ADR` uses the aligned
architectural Thumb PC; arithmetic wraps at 32 bits and preserves flags.
Invalid destinations are refused before changing registers. See
[DDI0406C.b, A8.8.4/9/12/221/225](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
Tests cover all 4,096 immediates over six arithmetic boundaries, every register
pair, PC alignment and wrap, IT conditions, neighboring encodings, and split
instruction-fetch faults. ARM1176 retains its legacy Thumb halfword behavior.
All 78 strict and 73 shipping tests pass with this decoder.

The labelled generic diagnostic now reaches the original console initializer's
first UART write after 293,151 steps: ARM `STR r5,[r0]` at `0x8027c3dc`, writing
`3` through virtual `0xc0000000` to physical `0x82500000`. Its RAM-only bus
refuses this access. A separate guard confirms that this trace does not use
the inherited, unaudited CP14 debug-register responses. Full 256 MiB heap and
file-backed RAM produce identical traces; the file-backed variant also exactly
reproduces the preceding configuration checkpoint's trace with its archived
library. These host diagnostics establish neither the N88 CPU configuration
nor a complete kernel boot. The canonical ECC guard remains unchanged.

## Cortex-A8 address translation queries

Privileged current-state `ATS1CPR`, `ATS1CPW`, `ATS1CUR` and `ATS1CUW` now
report through the Secure Physical Address Register (PAR). These operations
query privileged or User read/write permission without accessing the target
data. The existing MMU walk caches the physical address and PAR attributes
together, including memory type, cache policy, shareability, NS and supersection
information. Guest translation, domain, access-flag and permission failures
update PAR without changing the ordinary fault registers or taking an exception.
An unavailable host page-table access retains the checked-bus halt and leaves
PAR unchanged. See [DDI0344K, tables 3-78/79 and pages 3-71 through 3-74](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde).

PAR supports privileged reads and writes; its unknown reset value is chosen
as zero. Nonzero reserved or unused write fields, PC operands and User access
are refused. Unsupported translation attributes, TEX remapping, big-endian
walks and extended physical addresses cannot produce a successful PAR result.
Other-state translations and security-state transitions remain unavailable.
ARM1176 and Swift retain their existing CP15 behavior.

All 79 strict and 74 shipping tests pass. New regressions exercise ARM/Thumb,
all four queries, descriptor formats and attributes, permission/fault priority,
warm mappings and invalidation, checked-bus retry, condition suppression and
instruction-fetch failures. Legacy v32 snapshots remain byte-identical and
cross-load in old and new executables despite the appended, unserialized A8
state. The original 61,650-step guarded diagnostic is unchanged.

A separate generic-CPU diagnostic now routes MMIO through the partial S5L8920
board and prepares the matching iBoot's PRAM range (`0x4fffc000`, 16 KiB), with
explicitly assumed zero initial contents. It advances past `ATS1CPR` to a
checked write at ARM `0x8008973c` after 307,108 steps. A read-only observation
trace confirms that virtual `0x40000000` has no first-level descriptor, so ATS
returns the section-translation failure `PAR=0x0b`. The guest subsequently
maps physical zero at virtual `0xc009b000` and attempts to copy its vector
instructions there. Without an explicitly prepared boot mapping the board
refuses that physical write. The subsequent handoff investigation below
identifies the missing preparation; this remains a partial host diagnostic.

## Inherited RAM boot mapping

The matching iBoot's normal kernel handoff passes stage 3 through
`0x4ff00a24` and `0x4ff13fa4`. The platform callback at `0x4ff1348c` selects
the call at `0x4ff134d6`, passing 1 to `0x4ff13acc`. That setter preserves
the other bits of `0xbf100000`, sets bits [1:0] to 2, and reads the register
back. The caller subsequently jumps to the kernel entry with the boot arguments.
This is a connected static path, not an executed bootloader result.
[OpeniBoot's S5L8920 entry sequence](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/arch-arm/entry.sx)
independently identifies this selection as physical RAM at zero.

`s5l8920_set_ram_boot_window` now prepares that inherited RAM selection for
a direct kernel handoff. Low addresses share the existing storage at
`0x40000000`; the modeled extent is bounded to the installed 256 MiB.
That extent follows the RAM-selection interpretation and known RAM geometry;
it is not a measurement of the complete hardware remap aperture. All bus
widths, host pointers and host loads use the same checked decoder. Changing
the selection invalidates translation and host-pointer caches and clears the
exclusive monitor, while preserving registers, RAM and latched bus failures.
Without MIU configuration, initialization and functional reset remove the
explicit preparation.
The newer explicitly configured MIU control described above connects guest
selection 2 to this window. Unknown upper-bit changes, ROM/SRAM backing and
physical reset sequencing remain unmodeled; without initial MIU input,
accesses to `0xbf100000` still stop explicitly.

A frozen private witness executes the original kernel vector-copy routine
twice, copying `0x90` and `0xf60` bytes while preserving the 16-byte sleep-token
gap. Before implementation it stops at the first physical-zero write after
25 retired instructions. Both copies pass on the first implementation in
49 and 526 instructions, with exact data, register state and access counts.
Unit regressions cover alias coherence, bounds, reset, retained diagnostics,
warm instruction/data pointers, MMU and host-pointer variants, and partial
stores at the window boundary.

All 79 strict and 74 shipping tests pass, as do the 276 existing prepared
timebase, UART and interrupt-controller calls. The original guarded
61,650-step diagnostic retains its complete trace.

With this preparation enabled, the labelled generic-CPU kernel diagnostic
advances to 367,777 steps: Thumb `STR.W r0,[r4,#0x208]` at `0x8027b4e6`
attempts to write `0x7fffffff` to timer deadline register `0xbf100208`.
That register was the next unsupported operation. The variant without the mapping exactly
preserves its previous 307,108-step trace; boot-argument, device-tree and
CP14 guards remain active in both variants. These results do not establish
the actual N88 CPU configuration, a complete kernel boot or SpringBoard.

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

## Timebase counter and deadline timer

The matching tree's `pmgr` node has device type `timer`; its first register
range maps to `0xbf100000`, size `0x2000`. The original platform initializer
maps that range and passes base+`0x200` to the CPU callback installer at
`0x800885a8`. Its original callback table at `0x802ee7f8` includes the ARM
time reader at `0x800895a4`. `_mach_absolute_time` reaches it through an
original Thumb entry, interworking veneer and per-CPU callback dispatch.
The reader samples high, low, high, retries when the high words differ,
then adds the per-CPU wake-time offset.

The board implements independent word reads at `0xbf100200/204` from one
64-bit counter. Callers supply source ticks through
`s5l8920_timebase_clock`; counter arithmetic wraps modulo 2^64. Reads and
CPU execution do not advance time. Reset to zero is a functional starting
point, not a measured power-on phase. Physical clock frequency and gating
remain unimplemented. The initial counter implementation left deadline
programming unavailable; the deadline model below now handles that operation.
Other PMGR selectors and unsupported widths/alignment retain checked stops.

A private fixture frozen before implementation starts at the unchanged public
entry. On checkpoint 82 it stops at the first high-word read, `0x800895a8`,
after 10 retired instructions and 11 attempts. The expected full register,
FP, flags, monitor, RAM and access counts match; the original baseline and
its seven artifact identities are preserved. All 24 subsequent cases passed
on the first run, with the fixture and runner unchanged: stable reads, host
ticks between reads, low-word rollover, 64-bit wrap and wake-offset arithmetic.
Returns take 20 instructions, or 25 when the original retry loop runs. The
full strict and shipping host suites pass 78 and 73 tests respectively.
The 326 existing prepared UART/VIC cases also pass, and the complete
61,650-step canonical ECC-guard trace is unchanged.
This is a prepared function contract, not
timer initialization, FIQ delivery, kernel boot or physical validation.

The deadline model implements word programming at `0xbf100208` and the
enable/acknowledge sequence at `0xbf100220`, with its latched event routed to
VIC source 6. Matching iBoot's selected timer table at `0x4ff298dc` identifies
both addresses and the source. Its complete deadline routine at `0x4ff04c1c`
samples the timebase, writes `0xffffffff`, writes control `3`, and replaces
the count with a clamped relative interval. A null callback writes control `2`.
The original kernel initializer writes `0x7fffffff`, then control `3` and `1`;
its hardware callbacks read or replace the count without another start write.

Supplied timebase ticks now decrement an enabled interval and latch one event
at expiry. Control bit 0 runs or stops the counter; bit 1 acknowledges the
event. Replacing the interval, stopping it or enabling it does not acknowledge
an existing cause. An expired interval requires a new count to fire again.
External source 6 is ORed with the timer cause, and reset preserves that
external input while clearing the timer configuration. This is a functional
one-shot interpretation of the firmware sequences. Logical expiry follows
the supplied interval; a zero interval waits for the next supplied tick.
It does not establish physical bus/clock phase. Unprogrammed and post-expiry
count reads, control reads, other control bits and AP access to the second timer remain
refused because their observable semantics are not established.

A frozen private fixture executes eight original iBoot calls, including a
null callback, one-tick and ordinary intervals, low-word carry, large-distance
clamping and past/equal deadlines. The retained baseline stops at the predicted
first device writes. All calls pass unchanged on the first implementation,
returning in 11 or 48 instructions with the expected full register state,
stack, data-access counts and programming order. No ticks occur during these
calls. Separate component tests exercise countdown, wrap, reprogramming,
acknowledgement, external input sharing and checked-bus failures. Synthetic
guest vectors establish IRQ/FIQ entry, device acknowledgement, rearming,
VIC completion and exception return without advancing time through CPU steps.

All 79 strict and 74 shipping tests pass, together with the 276 existing
prepared timebase/UART/VIC calls, both original vector copies, and the complete
unchanged 61,650-step canonical diagnostic trace.

The guarded generic-CPU diagnostic now advances from the deadline write to
370,765 steps, where Thumb `LDRB r1,[r0,r4]` at `0x8027b586` first reads the
unprepared `/device-tree/chosen/random-seed` property at physical `0x4110264c`.
The original tree contains a two-byte placeholder; it is not bootloader-supplied
entropy. The archived preceding library and its own board header reproduce
the entire prior 367,777-step trace with the same inputs. The new diagnostic
still uses the explicit generic CPU, zero-PRAM and inherited-RAM assumptions;
no source clocks, completed kernel boot or physical result are claimed.

## Early UART and transmit interrupts

UART0 has a separate checked word-access model at `0x82500000`, within the
matching device tree's 4 KiB aperture. Its initial 8N1 polled model has been
extended to the five-port bootloader behavior described above, including
8N2, non-FIFO holding registers, CTS and receive-event interrupts. Unsupported
registers, widths, modes, DMA and fractional offsets still stop without
committing the access. The iPhone OS 3 UART remains separate.

The matching kernel's original ARM initializer at `0x8027c3c0` writes ULCON
`3`, UCON `0x405` (or `5` for an explicit zero clock property), UMCON `0`,
UBRDIV through its real baud helper, UFCON `3`, and UMCON `1`. Its ready
callbacks test UTRSTAT transmitter-empty bit 2 and receive-ready bit 0;
data accesses use word UTXH/URXH. This differs from modern Apple's FIFO-full
transmit polling and its initializer's reset of both FIFOs. The original
N88 baud helper reads the 64-bit field at `gPEClockFrequencyInfo+0x90`,
consistent with `fix_frequency_hz` in the nearby public
[XNU 2050 header](https://github.com/apple-oss-distributions/xnu/blob/xnu-2050.48.11/pexpert/pexpert/pexpert.h).
That identifies a clock input, not its actual board frequency.

Register fields and the `(16 - sample_field) * (divider + 1)` clock ratio
are corroborated by Apple's
[UART register definitions](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.41.3/pexpert/pexpert/arm/apple_uart_regs.h)
and [serial implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.41.3/pexpert/arm/pe_serial.c).
The model's functional contract has a 16-byte FIFO in each direction and a
separate transmit shift register. Callers explicitly supply selected input
clock cycles; an 8N1 frame completes after ten bit periods. CPU instructions
and status reads do not drain it. Completed bytes are delivered in order;
insufficient host output capacity refuses the entire clock advance for retry.
Receive input consists of already completed, error-free host frames. It does
not model receive-line sampling or overrun errors.

Functional reset empties this component, but does not claim N88 hardware
register defaults or the state handed over by iBoot. Configuration reads
fail until the corresponding register is programmed or its supported initial
input is supplied. Status/data require all configuration words except modem
control on UART0, which has no modem pins. FIFO reset commands self-clear and affect
queued bytes, while a transmitting frame continues. Live status follows FIFO
and shift state; receive and zero-threshold transmit events stay latched until
W1C. Those transition/timing rules are the explicit component abstraction,
not a physical N88 timing measurement. Receive-line timing, automatic timeout
duration, erroneous frames, clock gating, scheduler source-clock conversion,
other formats and complete driver configuration remain unfinished.

A private pre-implementation oracle executed the unchanged 260-byte
initializer/baud pair, its original 76-byte software divider, function table
and clock-pointer literal. All 54 prepared combinations of clock input,
optional clock/sample properties and 16-bit divider override matched the
predicted instruction trace, six captured writes, full register/FP/flag/monitor
state, exact data access counts, 32-byte frame and whole-RAM hash. The first
capture failure was retained: its expected CPSR omitted the negative flag
from comparing a high mapped pointer with zero. The corrected oracle passed
before UART implementation. Capture establishes the CPU/control-flow oracle,
not device operation. A separate actual-board baseline stopped at the first
ULCON write after seven retirements and verified the complete partial state.
Prepared clock inputs, including an intentionally synthetic 64-bit case,
do not establish a physical clock or a bootloader handoff.

The first run of that frozen oracle against the actual new board device
passed all 54 calls. A separate witness, written after device implementation,
executes the unchanged ARM sender/receiver and their four callbacks. Its six
calls cover uninitialized, empty, ready and busy paths, with complete expected
instruction traces and register/frame checks. The busy sender polls twice
before explicit selected-clock input completes a preceding frame; its new
byte completes only after another 2,080 input cycles for the prepared divisor.
This demonstrates directly entered original routines using the component,
not a running kernel console or an integrated scheduler.

The polled UART checkpoint passed all 78 strict and 73 shipping tests, including clock-source selection,
frame boundaries, FIFO limits/order, atomic output-capacity retry, receive
gates, reset commands and checked CPU access retry. The final rebuilt library
also passes the 54 initialization calls, six polled I/O calls, and all 192
earlier board-VIC cases. The entire canonical 61,650-step kernel-entry trace
is unchanged and still stops at the unestablished L2 parity/ECC configuration.
No CPU implementation, boot guard or legacy machine behavior was changed.

Transmit interrupt delivery uses UCON bit 13 to gate the existing latched
UTRSTAT bit 5 cause. The enable can change during a frame without changing
its remaining time or clearing pending status. The matching device tree
routes UART0 to source 24; the board ORs this cause with any externally held
input on that source before updating the PL192 and CPU IRQ/FIQ lines.
Board-owned UART clock and receive entry points refresh that wiring before
returning, so an elapsed frame does not require a later MMIO read to expose
its interrupt. Reset clears UART causes while preserving external input levels.

The unchanged Thumb method at `0x8083c26c` independently identifies the
transmit enable and software status-mask bits. Its 120-byte body clears the
old enables, constructs the requested mask, writes UCON, and optionally calls
an external interrupt-source method. A private fixture prepares software
masks that take its original return path before that external call; this is
not proof of real driver registration. Before transmit support, two disable
calls returned after 34 instructions and four enable calls stopped at
`0x8083c2b8` after 24 retirements. Every predicted register, FP, flag, monitor,
UART-state, RAM-hash and access-count check passed on the first baseline.
The full and partial oracles were frozen before implementation. The matching
handler at `0x8083c10c` also confirms masked status acknowledgment by writing
its selected UTRSTAT causes back to that register; its external callbacks
remain separate work.

The first run after implementation completed all six original setter calls,
each in 34 instructions, with every full-state check passing and the frozen
fixture unchanged. The focused component and board tests also passed on their
first run. They cover masking during transmission, atomic clock-output retry,
immediate time-driven IRQ/FIQ delivery, shared externally held source 24,
reset, and guest acknowledgment/EOI/exception return through explicitly
synthetic vectors. Those vectors do not establish the N88 kernel's interrupt
entry or registered serial callback.

The final transmit-interrupt checkpoint passes all 78 strict and 73 shipping
tests, the six original setter calls, 54 unchanged initialization calls, six
unchanged polled I/O calls, and 192 unchanged board-VIC cases. The entire
canonical 61,650-step kernel-entry trace remains identical at its guarded
L2 parity/ECC boundary. CPU implementation and firmware guards are unchanged.

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
and MVFR reads remain explicit capability stops in the instruction-only A8
profile. Explicit generic r3p2 configuration supplies FPSID `0x410330c3`,
MVFR0 `0x11110222`, and MVFR1 `0x00011111` from DDI0344K Table 13-5.
These identify the selected generic core, not a measured S5L8920 revision
or complete instruction support. Privilege and CPACR checks apply with
FPEXC.EN either set or clear. MVFR writes and reserved selectors remain
refused. These rules use
[DDI0344K, section 13.4](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde)
and [DDI0406C.b, B9.3.21/22](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).
The latter defines FPSID serialization but no valid MVFR VMSR selector.

Configured identity regressions fail on the old implementation and pass with
these reads; all 79 strict and 74 shipping tests pass. The original guarded
61,650-step trace and 276 prepared peripheral calls retain their results.
With the existing generic CPU and nominal clock assumptions, an explicitly
prepared empty serial/NVRAM handoff and unchanged CPU cell-count metadata,
the diagnostic previously stopped at `VMRS r0,MVFR1` (`0x80088d8c`) after
154,636,539 instructions. The new reads advance it to 157,855,234 instructions,
where Thumb `0x80787a60` reads the guarded `arm-io/chip-revision` property
at physical `0x4110367c`. This remains a partial host initialization trace.

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
enters the guest Data Abort handler. Standard unaligned accesses with A=0
additionally support Normal memory, including transfers across nonadjacent
physical pages. Each access checks the current permissions and memory type;
Device and Strongly-ordered memory stop before the rejected data access.
Unaligned words use ascending byte accesses; word-aligned halves of a
misaligned 64-bit element retain word accesses. The original base survives
an abort or checked host-bus failure. Completed store bytes remain in memory,
while a load publishes only completed elements. This retained register prefix
is an implementation choice within the architecture's UNKNOWN abort state.
Tests cover every unaligned page split, partial data and page-walk failures,
retry, access controls, aliases, both instruction states and host RAM paths.
8/16-bit elements and big-endian accesses remain unsupported. See
[DDI0406C.b, A3.2.1, A7.7.1, A8.8.320/404 and B1.9.8](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

VLD2/VST2 multiple-structure forms additionally interleave or deinterleave
32-bit elements into adjacent or spaced pairs of D registers, or four
consecutive D registers. They retain naturally aligned accesses and the same
writeback rules; standard unaligned VLD2/VST2 is not implemented.
Two-register forms reject a 32-byte alignment assertion. Each completed
word publishes to its own interleaved register lane. Other element widths,
VLD2/VST2 single-lane/replicate forms and the remaining structure families are not
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

VLD1 single-element loads additionally copy one naturally aligned 32-bit
word into either lane of any D register, preserving the other lane and all
FPSCR fields. Both ARM and Thumb encodings support no writeback, a four-byte
increment, or addition of the original offset register, including base/offset
aliases. The lane and base publish only after the word has completed.
Explicit four-byte alignment failures, or standard failures with SCTLR.A
set, enter Data Abort before translation. Standard unaligned accesses with
A=0 remain unsupported. Invalid sizes, index/alignment fields and R15 bases
stop before coprocessor access checks; valid unavailable instructions enter
guest Undefined. Big-endian execution and 8/16-bit lane transfers remain
unsupported. The separate broadcast form is described below. See
[DDI0406C.b, A8.8.321 and B1.9.8](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests exercise all D registers, both lanes, base/offset combinations and
alignment forms, raw FP classes, whole register/RAM preservation, access
denial, IT conditions and adjacent unsupported structures. They also check
single-word User loads at a page end through Normal, Device and Strongly-ordered
mappings, translation/permission/alignment faults, split Thumb fetch faults,
checked host failures followed by retry, and guest enable/return retry with
exactly one data read. The first run found a wrong physical input address in
the new MMU test, an older refusal test whose encoding now selects a valid
lane load, and two new assertions that overlooked the existing legacy D31
preload-hint alias. Those test assumptions were corrected; the implementation
was unchanged. ARM1176 and Swift retain their existing behavior.

The unchanged 1,168-byte ARM `vDSP_dotpr` routine at
`0x30857a5c..0x30857eec` completes 3,680 prepared calls. Before lane-load
support, 3,456 calls already passed; the first additional unit-stride call
with sixteen elements and both inputs four bytes past a 16-byte boundary
stopped after fifteen instructions at `0x30857a94`, on
`VLD1.32 {d6[0]}, [r0]!`, before any input read. The fixture, helper and
runner hashes were unchanged for the first successful run. The initial
3,456 calls cover unit counts below sixteen and three signed nonunit stride
pairs through 65, with four pointer-alignment tuples, four small-integer
patterns and all four FZ/DN combinations. Another 224 calls cover selected
unit counts 16, 17 and 18 whose alignment prefix branches to the remaining
scalar/block path before reading the CPU-family commpage. No identity
value or override is supplied.

All operands, products and partial sums in these calls are exact F32
integers under nearest-even rounding and LEN/STRIDE zero. Independent
closed-form sums, checked against twenty integer anchors, verify the output.
The fixture checks the entire FP bank, including the unstored upper lane
that the final odd-element VMLA updates from the preceding pair's retained
inputs. It also checks all general registers, flags, exclusive and return
state, whole RAM, instruction counts and exact per-byte fetch/input/output,
eight-byte frame and eight-byte argument accesses. These prepared calls do
not establish other large unit-stride paths, fractional or special inputs,
process launch, board boot, timing or physical behavior.

After lane-load support, all 77 strict and 72 shipping tests pass. The
max/min, F64 dot-product, NEON integer, VFP integer, precision, square-root,
division, unit F64 ramp, F32 ramp and F64 remainder fixtures retain their
6,912, 3,456, 6,144, 4,096, 4,096, 640, 1,536, 336, 1,824 and 112 passing
calls. The complete guarded kernel-entry trace remains identical through
the 61,650-step L2 ECC stop.

VLD1 single-element broadcasts load one naturally aligned 32-bit word into
all lanes of one or two adjacent D registers. Odd starting registers are
valid for the two-register form; a pair starting at D31 is refused. Both
ARM and Thumb forms retain the same access, alignment and writeback rules
as lane loads. Immediate writeback is four bytes even with two destination
registers. The original offset register is used when it aliases the base.
Both destinations and the base publish only after the single read completes;
the raw value, including NaNs and subnormals, is copied without changing
FPSCR. Unsupported sizes and invalid register shapes stop before access
checks. The decoder separates this allocation from lane transfers and
legacy preload hints. See
[DDI0406C.b, A8.8.322 and B1.9.8](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Broadcast tests cover every destination and base/offset combination,
one/two registers, both alignment encodings, all raw FP classes, access
denials, invalid sizes and overflow, endianness refusal, IT conditions and
legacy behavior. The existing lane fault tests also exercise broadcasts:
User page-end Normal, Device and Strongly-ordered reads, read-only mappings,
translation/permission/alignment faults, split Thumb instruction fetches,
checked host failures and guest enable/return retry. They verify that both
registers consume exactly one word and preserve uncompleted state.

The broadcast fixture uses the original 134-byte Thumb `_vvrecf` short-count
body at `0x308668a0..0x30866926`. Its frozen baseline on the preceding core
passes all 256 empty calls, then stops in the first one-element call at
`0x308668e6`, `VLD1.32 {d16[]}, [r1:32]`, after twelve steps and 34 fetched
bytes. The count has been read once; no input or output has been accessed.
The fixture verifies the entire partial FP/general-register, flag, monitor
and RAM state at that stop. With broadcast support, the first run completes
all 4,096 calls using the unchanged fixture, runner and oracle-helper hashes.
The matrix covers all counts zero through fifteen, all sixteen word-aligned
input/output pairs and all sixteen
RMode/FZ/DN combinations, with 32 raw inputs and four initial D19 pairs.
A frozen Python helper supplies only six pure Fraction functions, selected
from its syntax tree without executing its build or file-handling code.
The independent five-stage oracle has 45 rows and twelve raw result/flag
anchors, including thirteen refined intermediate values reused as unstored
upper-lane inputs. The two-element tail deliberately reads its first input
and writes its first output twice; the one-element tail still computes
both unstored upper lanes. These behaviors are included in the full FP and
per-byte access oracle. All general registers, flags, FPSCR, monitor,
return state, whole RAM and exact instruction counts also match. No
CPU-family input or override is supplied. These are approximate instruction
pipeline results, not correctly rounded reciprocal guarantees. They do not
establish aliases, larger counts, process launch, board boot or physical
behavior. The focused tests passed on their first run without production,
test or oracle corrections. All 77 strict and 72 shipping tests pass.
All fourteen prior firmware fixtures also pass, bringing the prepared-call
total across fifteen routines/fixture groups to 66,880. The entire canonical
kernel-entry trace remains identical through its 61,650-step ECC guard.

VST1 single-element stores now use the same checked address, alignment and
writeback rules for either 32-bit lane of any D register. They preserve the
entire FP register bank, FPSCR, ARM flags and the existing monitor state.
Stores use guest write permissions and mark alignment, translation and
permission Data Aborts as writes. A failed transfer leaves the base unchanged;
checked host write failures retain their existing halt-and-retry behavior.
This follows
[DDI0406C.b, A8.8.405 and B1.9.8](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

The lane matrices now exercise both directions, including every register,
lane, writeback and alignment combination; raw NaNs, invalid encodings,
access denials, IT conditions and legacy behavior; and page-end accesses
through all three implemented memory types. Read-only User mappings allow
loads while refusing stores with WnR set. Checked writes, split Thumb fetches
and guest enable/return retries verify one completed store, unchanged FP
state and preservation of uncompleted memory and base updates.

Original ARM `vDSP_vclip` unit-stride paths complete 18,432 prepared calls.
The exact 1,044-byte image at `0x30841c9c..0x308420b0` ends before the
nonunit-stride path; it is a bounded image, not the complete function.
Before stores were supported, 3,072 empty calls passed and the first
nonempty call stopped after 26 instructions at `0x3084209c`, on
`VST1.32 {d0[0]}, [r10]!`. It had read four input bytes, eight bound bytes
and twelve argument bytes and written a twelve-byte frame, with no output.
All four intermediate SIMD events already matched. The first focused build
and tests passed, and the frozen fixture and runner required no changes for
the first successful firmware run.

Counts zero through three cover all sixteen input/output word-alignment
pairs; eight larger counts from four through 31 use four equal low-address
alignments so block transfers are naturally aligned. Both positive and
negative unit strides, six raw bound pairs and all sixteen guest rounding,
FZ and DN combinations exercise 28 input classes. The selected short paths
never reach the CPU-family lookup; no identity value is prepared. Bounds
include infinities, NaNs and subnormals, checking the original instruction
sequence even for inputs outside ordinary finite clipping use.

An independent exact-rational oracle supplies 168 rows with separate maximum
and minimum results and exception flags, checked against eighteen raw
two-stage anchors. Every SIMD operation checks the complete FP bank and
FPSCR/CPSR, including iterative changes to the unstored upper lane. Final
checks cover all general registers, flags, monitor and return state, whole
RAM, instruction counts and exact per-byte fetch/input/bound/output/frame
and argument accesses. Empty calls read only the four-byte count argument.
These calls do not establish nonunit strides, counts of 32 or more, aliases,
standard unaligned block transfers, process launch, board boot, timing or
physical behavior.

After lane-store support, all 77 strict and 72 shipping tests pass. The F32
dot-product, max/min, F64 dot-product, NEON integer, VFP integer, precision,
square-root, division, unit F64 ramp, F32 ramp and F64 remainder fixtures
retain their 3,680, 6,912, 3,456, 6,144, 4,096, 4,096, 640, 1,536, 336,
1,824 and 112 passing calls. The complete guarded kernel-entry trace remains
identical through the 61,650-step L2 ECC stop.

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

NEON VREV16, VREV32 and VREV64 reverse whole elements within halfwords,
words and doublewords. Both ARM and Thumb encodings cover all legal D/Q
registers, including in-place reversal. Reserved element/block sizes and odd
Q operands stop before access checks. The operation preserves core flags,
FPSCR, FPEXC and host FP state. An independent byte-array oracle and six raw
result anchors cover every register pair, both fetch paths, access denial and
IT execution/skips. The first test-only run recorded 31,740 failures; the
first implementation passed the VFP suite without corrections. See
[DDI0406C.b, A8.8.386](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

NEON VTRN transposes 8-, 16- or 32-bit elements across two D or Q operands,
staging both complete results before updating either operand. It preserves
ARM flags, FPSCR and the host FP environment. Reserved widths and odd Q
operands stop before access checks. Identical operands have an architecturally
UNKNOWN result; that case remains an explicit capability stop after valid
access checks. Tests cover every register pairing, an independent byte-array
oracle, permission and enable checks, IT execution, neighboring encodings,
split fetch faults, checked-bus retry and guest enable/exception-return retry.
See [DDI0406C.b, A8.8.420](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

NEON integer VADD and VSUB implement independent modular arithmetic in
8-, 16-, 32- and 64-bit lanes across the full D/Q bank. ARM and Thumb share
unsigned arithmetic, with all source reads completed before destination
writes. Core flags, FPSCR and the host FP environment are preserved. Odd
Q operands stop before access checks; valid denied accesses enter the guest
Undefined handler, and Thumb honors IT conditions. An independent bytewise
carry/borrow oracle checks wraparound, boundaries, register roles and aliases,
both fetch paths and access controls. The test-only baseline recorded 17,280
failures; the first implementation passed the full VFP suite. See
[DDI0406C.b, A8.8.282/414](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

NEON VSHL immediate and signed/unsigned VSHR support 8-, 16-, 32- and
64-bit lanes in ARM and Thumb. Left shifts truncate; right shifts either
zero-fill or replicate the original sign bit, including shifts by the full
lane width. Unsigned host arithmetic avoids signed-shift assumptions and
explicitly handles the 64-bit boundary. All source reads precede register
writes; status and host FP state are preserved. Odd Q operands stop before
access checks, while related modified-immediate encodings retain their
existing decoder. An independent bit-mapping oracle covers every legal shift
amount, register roles and aliases, access control and IT execution/skips.
The test-only baseline recorded 40,620 failures. Four older transpose-neighbor
assertions now check the valid `VSHR.U64` result and access fault explicitly.
See [DDI0406C.b, A8.8.395/398](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

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

Advanced SIMD VMAX/VMIN.F32 cover both instruction sets and every D/Q
register. Both operands are unpacked with standard FZ/default-NaN controls
before selection. Quiet NaNs produce the positive default NaN; signaling
NaNs additionally set IOC, and flushed subnormals set IDC even when the
other operand is a NaN. Maximum chooses positive zero and minimum negative
zero when their signs differ. Finite selection uses integer ordering and
does not introduce rounding exceptions or use host floating-point state.
Results from all active lanes are staged before publication, preserving
overlapping sources. Invalid sizes and odd Q operands are refused before
coprocessor availability is considered. This follows
[DDI0406C.b, A2.7.8 and A8.8.335](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover all destination/source register triples, both operations/ISAs,
D/Q widths, all 512 rounding/FZ/DN/LEN/STRIDE control combinations and ignored
trap/AHP fields. Nineteen raw anchors verify an independent native-comparison
oracle in both operand orders. All pairs of 22 classes and 1,024 random
four-lane pairs exercise sixteen guest controls and four host rounding modes
with pending host exceptions. Further checks cover cumulative flags/VMRS,
access denial, invalid encodings, IT conditions, legacy and adjacent
allocations, checked host fetch failures, split-page User faults, and
guest enable/return retries. The first run exposed four refusals incorrectly
reclassified as guest lazy-enable traps: unsupported neighboring ARM SIMD
instructions inherited the legacy broad SIMD mask while FPEXC was disabled.
Cortex-A8 now keeps those missing operations as capability stops regardless
of availability; supported operations still report explicit guest access
faults. Twenty-seven older neighbor assertions were updated to require this
capability-stop behavior. ARM1176 and Swift behavior is unchanged. The numerical
implementation, raw anchors and independent oracle required no corrections.

Original ARM `vDSP_vmax` and `vDSP_vmin` scalar paths complete 6,912 prepared
calls. Their exact 376-byte images start at `0x308582d0` and `0x308593d0`;
these are bounded scalar images, not the entire functions. Before support,
256 empty-input calls passed and the first nonempty call stopped after
seventeen steps at `0x30858438`, `VMAX.F32 d2, d1, d0`, after four bytes from
each input, twenty stack writes and twelve argument bytes, with no output.
The frozen fixture and runner were unchanged for the first successful run.
Unit strides cover nine counts from zero through 15; three signed nonunit
stride tuples additionally cover 16, 17, 31, 32, 33 and 65. Four word-alignment
tuples and sixteen guest rounding/FZ/DN combinations exercise twenty raw
input pairs. An exact-rational oracle supplies forty main and eight upper
lane rows, checked against twenty raw anchors in both operand orders.

The scalar routines also compute an unstored upper lane from S1 and S3.
Four prepared upper-lane pairs tied to alignment exercise signed zero,
signaling NaN, subnormal/infinity and quiet-NaN/subnormal cases. Both result
lanes and cumulative FPSCR/CPSR are checked after every SIMD operation.
The fixture checks all FP/general registers, flags, monitor and return state,
the entire RAM image, instruction counts and exact per-byte fetch/input/output,
twenty-byte frame and twelve-byte argument accesses. Empty calls read only
the four-byte count argument. No CPU-family input is prepared or read. These
results do not cover large unit-stride/vector paths, source/output aliases,
process launch, board boot, timing or physical behavior.

After max/min support and the refusal-path correction, all 77 strict and
72 shipping tests pass. The existing dot-product, NEON integer, VFP integer,
precision, square-root, division, unit F64 ramp, F32 ramp and F64 remainder
fixtures retain their 3,456, 6,144, 4,096, 4,096, 640, 1,536, 336, 1,824 and
112 passing calls. The complete guarded kernel-entry trace remains identical
through the 61,650-step L2 ECC stop.

Advanced SIMD VCGE.F32 register comparisons cover both instruction sets and
every D/Q register. Each lane produces all ones for greater-than-or-equal
and zero otherwise. Both operands are unpacked with standard FP controls:
subnormals become signed zero and set IDC, including when the other operand
is a NaN. Either kind of NaN produces false and sets IOC. Positive and
negative zero compare equal. Finite and infinite values use integer ordering,
without host FP state or rounding. All results are staged before publication
to preserve overlapping sources, and lane exceptions accumulate without
changing FPSCR.NZCV or ARM flags. Reserved sizes and odd Q operands stop
before coprocessor access checks. Integer, immediate-zero, absolute and
other comparison operations remain unsupported. This follows
[DDI0406C.b, A2.7.8 and A8.8.293](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover all D/Q register triples and aliases, all 512 guest rounding,
FZ/DN/LEN/STRIDE combinations and ignored trap/AHP fields. Twenty raw anchors
check an independent native-comparison oracle in both operand orders. All
pairs of 22 classes and 1,024 random four-lane inputs exercise sixteen guest
controls, four host rounding modes and pending host exceptions. Additional
checks cover cumulative flags and VMRS, access denial, invalid encodings,
IT conditions, legacy and neighboring operations, checked host fetch failures,
split-page User translation/XN/AP faults, and guest enable/return retries.

The unchanged 468-byte ARM unit-stride portion of `vDSP_vthres`, at
`0x3082b060..0x3082b234`, completes 8,064 prepared calls. Before comparison
support, the first 1,536 empty calls returned and the first nonempty call
stopped after twenty instructions at `0x3082b218`, on `VCGE.F32 d4, d2, d0`,
before any output write. The C fixture, shared storage helper and Python
runner were unchanged for the first successful run. An independent
exact-rational oracle supplies 168 rows, checked against twenty raw anchors
in both operand orders and four retained-subnormal identities. The comparison
flushes subnormal operands, but the following VAND masks the original input:
a true comparison can therefore preserve a raw subnormal in the output.

Calls cover counts zero through fifteen, sixteen source/output word-alignment
pairs for short calls, four matching alignment pairs for block paths, six
raw thresholds, twenty-eight input classes and all sixteen guest rounding,
FZ/DN controls. The fixture checks the full FP bank and partial FPSCR/CPSR
after every VDUP, VCGE and VAND, including the repeatedly transformed,
unstored S5 lane. It also checks every general register, the return state,
exclusive monitor, whole RAM and exact instruction/input/threshold/output,
four-byte frame and eight-byte argument access counts. Empty calls read
only the four-byte count. No CPU-family input is prepared or read. Coverage
excludes nonunit strides, counts of sixteen or more, source/output aliases,
unaligned block accesses, process launch, board boot, timing and physical
behavior. The first build and all three focused tests passed without
production, test, fixture or oracle corrections.

After comparison support, all 77 strict and 72 shipping tests pass. The
existing clipping, F32 dot-product, max/min, F64 dot-product, NEON integer,
VFP integer, precision, square-root, division, unit F64 ramp, F32 ramp and
F64 remainder fixtures retain their 18,432, 3,680, 6,912, 3,456, 6,144,
4,096, 4,096, 640, 1,536, 336, 1,824 and 112 passing calls. The entire
guarded kernel-entry trace remains identical through the 61,650-step L2
ECC stop.

Advanced SIMD VRECPE.F32 and VRECPS.F32 cover both instruction sets and
every D/Q register. The estimate uses the architectural 256 input buckets
and integer division to generate its eight-bit fraction. Standard FP
controls flush input subnormals with IDC, return signed infinity with DZC
for zeros, return signed zero for infinities, and flush finite magnitudes
at least 2^126 to signed zero with UFC. Estimates do not raise IXC. NaNs
produce the default NaN, with IOC for signaling inputs.

The reciprocal step unpacks both inputs before NaN processing. Infinity
times zero contributes a positive-zero product without IOC, including
when a subnormal input flushes to zero and raises IDC. Otherwise the step
rounds a multiplication and then subtracts that rounded result from 2.0;
it is not fused. Both stages use the existing integer FP arithmetic and
standard rounding/FZ/DN controls, accumulating exceptions without changing
FPSCR.NZCV or ARM flags. Guest rounding, FZ/DN, LEN/STRIDE and trap enables
do not select the operation. Both instructions stage all destination lanes
before publication and reject reserved sizes or odd Q operands before
access checks. U32 estimates remain unsupported; reciprocal-square-root
forms are described below. This follows
[DDI0406C.b, A2.7.8 and A8.8.384/385](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover every D/Q register pair or triple and alias, all 512 guest
control combinations and ignored trap/AHP fields, seventeen estimate
anchors and twelve reciprocal-step anchors in both orders. The latter
include an exact zero after product rounding whose fused counterpart
would be nonzero. Independent native references check both ends of all
256 estimate buckets at nine boundary exponents with both signs, all
pairs of twenty-two FP classes and 1,024 random four-lane inputs. Both
operations run under sixteen guest FP controls and four host rounding
modes with pending host exceptions. Further checks cover cumulative
flags and VMRS, access denial, invalid sizes and Q operands, IT conditions,
legacy profiles, unsupported neighbors, checked host fetch failures,
split-page User translation/XN/AP faults and guest enable/return retries.

The first focused run passed the CPU and checked-bus tests. Four older
MACC-neighbor assertions still expected the newly supported VRECPS encoding
to stop. Those test expectations were updated; the reciprocal production
code and frozen firmware fixture and oracle were unchanged.

The original 2,096-byte ARM `vDSP_vdiv` body at
`0x30821afc..0x3082232c` completes 3,456 prepared calls. Before reciprocal
support, 256 empty calls returned and the first nonempty call stopped
after twenty instructions at `0x308222ec`, on `VRECPE.F32 d2, d0`.
Its complete partial register, flag, memory and access state matched the
independent baseline: four bytes from each input, an 84-byte frame write,
twelve argument bytes and no output. The same C fixture and Python runner
passed on the first run after the implementation, with unchanged hashes.

An exact-rational oracle supplies forty denominator/numerator rows with
all six separately rounded stages: estimate, reciprocal step, multiply,
second step, numerator multiply and correction multiply. Seventeen estimate
anchors, twelve bidirectional step anchors and every estimate bucket's
endpoints at three exponents and both signs check the oracle. Calls use
positive unit strides for nine lengths from zero through fifteen and three
signed nonunit stride tuples for those lengths plus 16, 17, 31, 32, 33 and
65. Four alignment tuples and all sixteen guest rounding/FZ/DN controls
exercise finite boundaries, infinities, both kinds of NaN, signed zeros
and subnormals. Four seeds cover the unstored S1/S13 lanes.

The fixture checks the full FP bank and partial FPSCR/CPSR after each
arithmetic stage, including input prefetches interleaved with calculation.
It accounts for software pipelining that can leave four scalar elements
after the vector loop. Final checks cover all general registers, return
state, exclusive monitor, whole RAM and exact instruction/input/output,
84-byte frame and twelve-byte argument accesses. Empty calls read only
the four-byte count argument. No CPU-family input is prepared or read.
These are results of the firmware's approximate reciprocal pipeline;
they do not establish correctly rounded division, aliases, negative unit
strides, large unit-stride paths, unaligned block accesses, process launch,
board boot, timing or physical behavior.

After reciprocal support, all 77 strict and 72 shipping tests pass. The
existing threshold, clipping, F32 dot-product, max/min, F64 dot-product,
NEON integer, VFP integer, precision, square-root, division, unit F64 ramp,
F32 ramp and F64 remainder fixtures retain their 8,064, 18,432, 3,680,
6,912, 3,456, 6,144, 4,096, 4,096, 640, 1,536, 336, 1,824 and 112
passing calls. The complete guarded kernel-entry trace remains identical
through the 61,650-step L2 ECC stop.

Advanced SIMD VRSQRTE.F32, VRSQRTS.F32 and VCEQ.F32 immediate zero cover
both instruction sets and every D/Q register. The square-root estimate
uses exact integer squared-midpoint comparisons for the architectural
128 buckets at each exponent parity. It returns signed infinity with DZC
for signed zero, including flushed subnormals with IDC; positive infinity
returns zero, and negative nonzero inputs return the default NaN with IOC.
Quiet NaNs return the default NaN without IOC; signaling NaNs raise IOC.
Finite estimates do not raise IXC. The refinement step rounds its product
first, then rounds the exact halved difference `(3-product)/2` once.
The shared integer add/subtract routine adjusts the exponent before final
rounding; existing callers retain their original behavior. Zero times
infinity has the special result 1.5 without IOC, while input flushing can
still raise IDC.

Immediate equality with zero returns an all-ones lane for either signed
zero or a flushed subnormal. Other inputs return zero. Its quiet-NaN
behavior differs from the signaling GE comparison: only signaling NaNs
raise IOC. All three operations use standard FP controls and accumulate
exceptions without changing the guest's other FPSCR fields or ARM flags.
Destination lanes are staged, and invalid sizes or odd Q operands stop
before access checks. U32 estimates and other immediate comparisons remain
unsupported. See
[DDI0406C.b, A2.7.8 and A8.8.292/391/392](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover all register pairs/triples and aliases, all 512 guest control
combinations, independent native arithmetic references, special-value and
rounding-boundary anchors, and all 65,024 positive-normal estimate bucket
endpoints across 254 exponents. Representative boundary exponents
also pass through both decoders. All pairs of twenty-two FP classes and
1,024 random four-lane inputs run under sixteen guest controls and four
host rounding modes with pending exceptions. Access, encoding, IT, legacy,
neighbor refusal, cumulative flags, VMRS, checked fetch failures, User
split-page translation/XN/AP faults and guest enable/return retry matrices
include all three operations.

The first focused run passed the CPU and checked-bus tests and all new
arithmetic checks. Four older sign-operation neighbor assertions still
expected the newly supported VCEQ encoding to stop. Those expectations
were updated; production code and the frozen firmware fixture and oracle
were unchanged.

The frozen firmware fixture covers the original short-count Thumb bodies
`_vvrsqrtf` at `0x30866b80..0x30866c26` (166 bytes) and `_vvsqrtf` at
`0x308669bc..0x30866a7a` (190 bytes). It prepares 8,192 calls: both routines,
counts zero through fifteen, all sixteen word-aligned input/output pairs
and sixteen guest rounding/FZ/DN combinations. Before implementation, 512
empty calls returned; the first nonempty call in each routine stopped
after twenty instructions, at `0x30866bf2` on VRSQRTE and `0x30866a3a` on
VCEQ immediate zero. Both complete partial states matched the baseline:
62 fetched bytes, four input bytes, four count bytes, two raw FP events
and no output. The first run after implementation completed all 8,192
calls with every check passing; the harness, runner and both oracle helpers
retained their pre-implementation hashes.

The independent exact-rational oracle checks all nine or twelve arithmetic
and bitwise stages, including the different estimate/zero-comparison order
in the square-root tail. Its 101 rows include retained upper-lane values
from preceding blocks and four initial seed pairs. Decimal and rational
square inequalities independently verify all estimate bucket endpoints.
The fixture checks the full FP bank and flags at each stage, all general
registers, return state, monitor, whole RAM and exact instruction/data
access counts, including duplicated accesses in two-element tails. No
stack frame, external call or CPU-family input is involved. These bounds
do not establish large-count paths, aliases, unaligned inputs, correctly
rounded mathematical square roots, process launch, boot or physical behavior.
For example, the original reciprocal-square-root pipeline returns the
default NaN for signed zero with IOC and DZC; the square-root pipeline
returns positive zero with the same exceptions. These are the observed
instruction-pipeline results, not scalar libm guarantees.

All 77 strict and 72 shipping tests pass. Fifteen earlier firmware fixture
groups retain their 66,880 passing calls, for 75,072 calls across sixteen
groups with the new routines. The complete canonical kernel-entry trace
remains identical through its 61,650-step L2 ECC guard.

VZIP and VUZP rearrange raw 8- or 16-bit elements in D registers and
8-, 16- or 32-bit elements in Q registers, in both ARM and Thumb. Both
destinations are staged before publication. All FP status, core flags and
the exclusive monitor remain unchanged. Encoded D32, reserved sizes and
odd Q operands stop before access checks. D32 assembler spellings are
VTRN aliases. Identical operands have architecturally UNKNOWN results;
the interpreter explicitly stops after checking access, matching VTRN's
policy. The source is
[DDI0406C.b, A8.8.422–423](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Tests cover all legal register pairs, byte-label result anchors, both
operand orders, the upper register bank, all sixteen rounding/FZ/DN
combinations and four host rounding modes with a pending exception.
Access denial, UNKNOWN operands, invalid encodings, IT skipping, legacy
profiles, checked fetch failures, split User instruction pages and guest
enable/return retries exercise both instructions and every legal width.
The existing VTRN bit-8 negative cases remain invalid encoded D32 VZIP.

The private firmware fixture contains the original ARM `vDSP_ctoz`
body at `0x3083df3c..0x3083e610` (1,748 bytes) and `vDSP_ztoc` at
`0x3083e61c..0x3083ed90` (1,908 bytes), with their 24 literal bytes.
It prepares 8,448 calls: both directions, every count from zero through
65, four raw bit patterns and sixteen FP control combinations. Arrays
are separate and 16-byte aligned, with interleaved stride two and split
stride one, following Apple's [ctoz](https://developer.apple.com/documentation/accelerate/vdsp_ctoz)
and [ztoc](https://developer.apple.com/documentation/accelerate/vdsp_ztoc)
contracts. Expected FP states come from original array positions at each
load and permutation; the full bank is checked after every instruction.
The fixture also checks all general registers, flags, monitor, return,
whole RAM, exact executed PCs, per-byte fetch/data counts, the 36-byte
frame, eight-byte descriptor and four-byte count argument.

Large-count paths retain the original entry and family branch and receive
an explicit prepared `CPUFAMILY_ARM_13` value at `0xffff1080`. Apple's
public [Cortex-A8 family mapping](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.41.3/osfmk/arm/cpuid.c)
and [family constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.41.3/osfmk/mach/machine.h)
justify that ABI input independently of CPU revision. This modern source
does not establish actual N88 commpage initialization, MIDR, reset state
or ECC configuration. No alternate family is supplied to force a path.

The first baseline exposed an incorrect LR-preservation expectation in
the fixture: the scalar four-element LDM overwrites LR, and the epilogue
restores the saved return address into PC. That failed fixture was archived
before correcting the expectation from the original disassembly. The
corrected pre-implementation baseline passed 1,536 calls, then stopped at
VUZP at `0x3083e09c` for count eight and VZIP at `0x3083e714` for count
sixteen. Their independently predicted full partial states matched:
33/31 instructions, 132/124 fetched bytes, 32/80 input bytes, no output,
36 frame writes, eight descriptor bytes, four count bytes, four family
bytes, eight literal bytes and one/five FP events. The corrected harness,
runner and four primary-source files were frozen before implementation.
The focused tests passed on their first run. The first frozen firmware
run passed 6,272 calls, then found a transient FP mismatch at `ztoc`
count 32 even though final RAM, general registers, flags and access counts
matched. Its complete artifacts and uncommitted source were preserved.
The original disassembly showed two independent reference events reversed:
VLD1 at `0x3083e738` precedes VZIP at `0x3083e73c`. Correcting that
reference order required no emulator change. The corrected fixture again
reproduced both complete baseline stops against the preserved old library.
With that reference correction, all 8,448 calls pass on the unchanged
implementation, including the intermediate FP states.
Other strides, alignments, aliases, families, process launch, boot and
physical behavior remain outside this fixture's scope.

All 77 strict and 72 shipping tests pass. The sixteen earlier firmware
fixture groups retain all 75,072 passing calls, giving 83,520 across
seventeen groups with these conversions. The full canonical kernel-entry
trace remains identical through the 61,650-step L2 ECC configuration guard.

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
Thumb SMMUL, SMMLA and SMMLS now implement signed high-word multiplication,
including all three rounded variants, for Cortex-A8 and Swift. They share
portable arithmetic with the existing A32 forms: the signed product is exact,
while accumulation and rounding wrap modulo 64 bits before extracting the high
word. This removes negative signed shifts and signed-overflow hazards from
the A32 calculation. Thumb forbids SP operands; A32 permits them. Both reject
PC operands except the add form's no-accumulator alias. These instructions
preserve NZCV, Q and GE. See
[DDI0406C.b, A8.8.184 through A8.8.186](https://documentation-service.arm.com/static/5f8dc043f86e16515cdbbc92).

Regressions use an independent shift/add product and separate high-word
carry/borrow oracle. They cover sign extremes, halfway rounding, accumulation
wrap, register overlaps, every register role, conditions and IT state, reserved
encodings, and noncontiguous instruction fetches with translation/permission
faults. ARM1176 keeps its existing A32 operations and legacy Thumb decoding.
Other Thumb DSP multiply and divide encodings remain unsupported.
All 79 strict and 74 shipping tests pass with this change.
The 276 prepared timebase, UART and interrupt-controller firmware calls pass,
and the original guarded 61,650-step diagnostic retains its complete trace.

A private generic-CPU diagnostic prepares the matching device tree's two-byte
`chosen/random-seed` property with the explicit test value `0xa53c`. Static
inspection shows that matching iBoot fills its existing property length using
timebase sampling and SHA-1; the diagnostic does not execute that entropy
producer or claim a random seed. It observes the original kernel reading both
bytes and wiping them. The same generic CPU/no-ECC, zero-PRAM and inherited-RAM
assumptions remain explicit, and all other unprepared property guards remain.
With a 20-million-instruction budget, the baseline stops at `0x800311fc`,
Thumb `SMMUL r0,r0,r1` (`fb50 f001`), after 17,884,017 steps. The first new
implementation reaches that budget without an instruction failure. Extending
only the budget to 50 million reaches a guarded read of CPU0's zero-valued
`reg` property at physical `0x41103164`, PC `0x802411be`, after 20,170,011
steps. No timer ticks are supplied. This is further host diagnostic progress,
not a validated N88 CPU configuration, complete boot or physical-device result.

A subsequent private handoff experiment retains the supplied zero-valued
CPU0, memory and PWM vibrator `reg` properties as explicit unchanged-property
assumptions. The original kernel formats their registry locations as `0`;
this does not establish normal bootloader output or memory geometry. For VRAM,
61 original matching iBoot instructions derive base `0x4fe3a000` and size
`0x1c2000` from a prepared active 320x480 display configuration. That witness
executes the range producer, but does not initialize display hardware or run
the complete bootloader. Its first run had an incorrect expected instruction
address; correcting that oracle required no emulator change.

The same diagnostic retains the VIC, GPIO and PMU's supplied zero
`#address-cells` values as interrupt-specifier metadata. The actual kernel
consumer takes its zero-cell branch for all three. Only those exact properties
are admitted; all other unprepared fields retain their guards. A 50-million
instruction run exhausts its budget, and extending only that limit to 100
million reaches `VREV32.8 q4,q12` at `0x8063e170` after 94,289,747 steps.
The new implementation executes all four consecutive reversals and reaches
the next unsupported instruction, `VADD.I32 q12,q4,q15` at `0x8063e180`,
after 94,289,751 steps. The frozen reference reproduces the earlier complete
trace. Generic CPU/no-ECC, test-seed, zero-PRAM and inherited-RAM assumptions
remain explicit; no source clocks or physical evidence are supplied.

For the reversal change, all 74 shipping tests and 78 of 79 strict tests pass.
The strict failure is the existing 512 MiB machine-allocation test under low
Windows commit headroom. A targeted retry and the unchanged parent emulator
library reproduce the same allocation failure; it remains a local validation
limitation. All 276 prepared timebase, UART and interrupt-controller calls
pass, and the original 61,650-step guarded trace remains byte-identical.

The subsequent integer VADD/VSUB implementation executes all four additions
and advances the same diagnostic to `VSHL.I32 q12,q8,#1` at `0x8063e1dc`,
after 94,289,774 steps. A frozen old-library run exactly reproduces the
previous bounded trace, register state and guarded-access counters. The
diagnostic's optional RAM read pointer excludes the entire boot-argument
and device-tree regions; writes and guarded accesses retain their callbacks.
All handoff observers still pass. This establishes instruction progress
under the existing assumptions, without adding clocks or a complete boot.

For integer addition/subtraction, all 79 strict and 74 shipping tests pass,
including the unchanged large-allocation test. The preceding checkpoint's
host allocation failure remains recorded above as a historical result.
All 276 prepared firmware calls pass again, and the complete canonical
61,650-step guarded trace remains byte-identical.

With immediate shifts implemented, the unchanged prepared diagnostic advances
48,767 further steps to `VLD1.32 {d24-d27},[r5]!` at `0x8063e168`, after
94,338,541 steps. This later call supplies the byte-unaligned source address
`0x80656815`; the vector-load implementation at that checkpoint explicitly
refused that case. The old-library reference reproduces the entire previous bounded
trace, and all handoff observers still pass. This is further instruction
progress under the same assumptions, without a complete boot or validated
cryptographic result.

For the shift change, all 79 strict and 74 shipping tests pass, as do all
276 prepared firmware calls. The canonical 61,650-step guarded trace remains
byte-identical. The first completed focused run passed the new shift cases;
updating four historical unsupported-neighbor expectations completed the
full VFP suite without changing the implementation.

With standard unaligned VLD1/VST1 implemented, the old-library reference
reproduces the entire preceding bounded trace. The new library passes that
load and reaches the diagnostic's configured 100-million-step limit at
`0x8009706e` after 99,999,999 steps, without another interpreter refusal.
All seven handoff observers and the seed consumption/wipe observer pass.
The firmware, metadata, CPU assumptions and guards are unchanged. This is
kernel execution progress, without a completed boot or cryptographic result.
All 79 strict tests, 74 shipping tests and 276 prepared firmware calls pass;
the canonical 61,650-step guarded trace remains byte-identical.

Extending only that probe's budget to 200 million steps reaches the next
budget boundary at `0x80089790`, after 199,999,999 steps. The same library
and handoff observers pass, with no new interpreter or bus refusal. The
last trace is in a memory-copy routine; the earlier budget boundary was
in a hash-related finalization routine. These samples do not establish
forward boot progress or rule out a repeated guest loop.

Read-only observations subsequently identify a timed reseeding loop: all
1,174 checks return zero time and all 1,174 take the backedge while the
board's source clock is frozen. A separate diagnostic supplies one existing
timebase tick per 25 successful abstract instruction cycles. This explicitly
assumes the nominal 24 MHz / 600 MHz ratio and one instruction per cycle;
it does not measure hardware timing or add a public machine scheduler.
The guest then exits both observed timed loops. Zero supplied ticks reproduce
the complete earlier 100-million-step trace, including its bounded state and
access counters after removing the read-only observer lines.

The clocked diagnostic exposes an A32 return bug: `MOV pc,lr` at `0x8063ca8c`
returns to `0x8063a3cd`, but the old interpreter keeps ARM state. It decodes
the subsequent Thumb bytes as ARM, including a spurious SVC at `0x8063a3d8`,
and the guest panics with `fleh_swi: took SWI from kernel mode`.
Cortex-A8 and Swift now apply ARMv7's `ALUWritePC` interworking to ordinary
A32 data-processing results written to PC. Odd targets select Thumb; word
aligned targets select ARM; the unpredictable low-bit pattern `10` is
refused before changing PC or state. ARM1176 keeps its existing alignment,
and exception returns still select state from SPSR. See
[DDI0406C.b, A2.3.1](https://documentation-service.arm.com/static/5f8daeb7f86e16515cdb8c4e).
Regressions cover all result-producing operations, conditions, immediate and
register operands, carry inputs, both fetch paths, real Thumb-to-ARM calls
and returns, shifted aliases, invalid register shifts and SPSR returns.

All 79 strict and 74 shipping tests pass, along with the 276 prepared firmware
calls and the unchanged canonical guarded trace. Frozen references reproduce
both previous unclocked and clocked results exactly. The fixed unclocked
probe retains its complete earlier trace; the fixed clocked probe passes
the bad-return path and reaches Thumb `0x8063961a` in multiword arithmetic
at its 100-million-step limit. Its handoff and seed observers still pass.
This establishes execution beyond the former panic under the explicit
clock and CPU assumptions, without a completed boot or cryptographic result.

Extending only the fixed clocked probe's budget to 200 million steps reaches
a concrete device-tree guard after 146,879,806 steps: ARM `0x80089b54`
reads the unprepared zero-valued property at physical `0x4110030c`.
The same handoff and seed observers pass. This later stop requires evidence
for that property's handoff value before lifting its guard; it is neither
a completed kernel boot nor a reason to remove the remaining guards.

That property is the root `serial-number`, containing 32 zero bytes in the
supplied tree. Matching iBoot looks up `SrNm` in its configuration records;
an absent record leaves the property untouched. Executing its original writer,
lookup and copy code verifies absent, unrelated, inline and external-record
cases. An explicitly labelled missing-record diagnostic retains those 32
zero bytes. The real guest string routine returns length zero, and execution
reaches the next guard after 147,011,967 steps: the 8 KiB
`/chosen/nvram-proxy-data` property at physical `0x4110052c`.

`s5l8920_build_empty_nvram_proxy` provides an opt-in empty-variable handoff
image for this target. It writes exactly 8 KiB, with generation 1, the matching
iBoot bank header, a `common` partition with `0x7f0` empty variable bytes, and
the remaining free partition. It computes each header checksum and the body
Adler-32 using portable byte accesses. Null pointers or incorrect lengths fail
without writing. The function allocates no memory, installs no device-tree
property and changes no board state or defaults; it does not implement NOR
hardware or persistent variable updates.

The format follows iBoot-1537.9.55's serializer at `0x4ff11994`, header checksum
at `0x4ff11aa4`, body checksum at `0x4ff183d0` and common-partition allocation
path at `0x4ff11496`. This is an explicit empty emulated configuration, not
measured physical provisioning. The entire iBoot serializer has not been
executed. A separate host construction agrees with an independent Adler-32
calculation; the matching guest copies all 8 KiB exactly and its real parser
returns common offset `0x30`, length `0x7f0`. Under the same generic CPU,
clock and other handoff assumptions, that diagnostic advances to 154,432,660
steps, where Thumb `0x8024207c` reads guarded `/cpus/#size-cells` at physical
`0x41102fa4`. These results establish handoff consumption, without a complete
kernel boot, SpringBoard or physical-device result.

For the C NVRAM builder, focused tests compare the entire independently
constructed image and check alignment, canaries, repeated initialization and
unchanged buffers on rejection. All 79 strict and 74 shipping tests pass, as
do the 276 prepared firmware calls and the canonical 61,650-step trace.
Frozen old-library and new-builder runs both reproduce the complete
154,432,660-step diagnostic trace, including the copy/parser observations,
bounded register state and guarded-access counters.

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
  32/64-bit NEON VLD1/VST1 and 32-bit VLD2/VST2 memory forms, 32-bit VLD1/VST1 lane transfers,
  32-bit VLD1 broadcasts to one or two D registers,
  register Boolean operations, VEXT, VREV16/32/64, 8/16/32-bit VTRN, D8/D16 and Q8/Q16/Q32 VZIP/VUZP, F32 VABS/VNEG,
  immediate constants, core-register VDUP, 8/16/32/64-bit integer VADD/VSUB,
  immediate VSHL and signed/unsigned VSHR,
  register VMUL/VADD/VSUB/VMLA/VMLS.F32,
  F32 VMUL/VMLA/VMLS by scalar, VMAX/VMIN.F32, VCGE.F32 register comparisons,
  VRECPE/VRECPS/VRSQRTE/VRSQRTS.F32, VCEQ.F32 immediate zero,
  and F32/signed/unsigned 32-bit NEON conversion
  described above.
  Other upper-bank VFP arithmetic, the remaining NEON families, and full
  context-switch semantics remain to implement. Remaining shared lower-bank
  arithmetic derives from VFP11 and requires a Cortex-A8 semantic audit.
- The partial S5L8920 fabric supplies a bounded UART with transmit interrupts but still lacks
  remaining UART interrupt/DMA/error behavior, clocks, storage, graphics, input
  and power devices. Build those components from the N88 firmware requirements.
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

The matching 10B500 LLB is now independently available for startup analysis.
A fresh bounded read of the manifest-selected Apple IPSW member matches the
retained 84,420-byte encrypted component, SHA-256
`130160fc58689e98b9bf9a636db1834913f4ce7795a346282fe869813862f597`.
The matching published key produces 81,920 bytes, SHA-256
`98c53b339223e38c5bc07003c531d84c0bfc2fa3319920591175d07270c430a5`.
ARM vectors, relocation base `0x84000000`, the `n88ap` and `1537.9.55`
identifiers, and the matching GPIO table validate its plaintext structure.
The earlier candidate key from 10B329 failed plaintext validation and remains
excluded. Firmware and key material are kept outside source control.

A separate private diagnostic executes the unchanged LLB from its reset
entry. Its explicit SRAM fixture covers the image and the data/BSS/stack
envelope through `0x84034000`; that bound is not a measured SRAM capacity.
The first unsupported access is a word read at physical `0xbf500000`,
Thumb `0x840084ca`, after 1,473 steps and 1,472 retired instructions.
Executable bytes remain unchanged. This identifies the chip-ID interface
as the next LLB dependency without supplying a revision value, skipping
instructions or claiming execution of BootROM or a complete bootloader.

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
cross-checked against both firmware files. The matching normal LLB's
initializer at `0x840086a8` programs the same PLL constants; its 25-word
selector table at `0x84011a18` exactly matches the restore table. This
corroborates the selected configuration in normal-boot firmware, without
establishing physical PLL behavior or execution past its lock polling.

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
