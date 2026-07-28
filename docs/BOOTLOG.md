# Anatomy of a boot

An annotated walk through one real boot of Apple's iPhone OS 3.1.3 kernel on
S5LBox: from the reset vector to the last driver that starts, with the emulated
device behind each stage.

> **The stage-by-stage narrative is historical; the frontier note is current.**
> The original run below ended at a missing VFP `VLDMIA`. Current real firmware
> has crossed that wall and the later `0xe1630381` ARMv5TE multiply stop. A resume
> chain then reached the 2.9 B configured cap and wrote checkpoints at 2.4, 2.7
> and 2.85 B, with no panic or emulator undefined stop. A diagnostic continuation
> reached 2,944,340,624 instructions and stopped on ARMv6 `UXTB16`
> (`0xe6cf3073`) in user mode. The complete paired-extend implementation then
> replayed through that stop, wrote a 2.97 B checkpoint and reached a clean
> 2.98 B cap. Free pages dipped to 97 and ended at 214 against a target of 250.
> The strongest current SpringBoard evidence is run22: a fresh display-enabled
> 128 MiB cold boot of exact diagnostic commit
> `40209b27cb10d01c552398ff918ee613c4908ed0`. Run20 had already validated the
> mixer+SDO correction through 4 TV-out frames, IRQ 30's shipped filter/action,
> the close-gate wake, exact PID 20 `IOServiceClose` return, and a 320x480
> `startWindowServer` return. It then stopped at 1,937,979,818 on valid VFP11
> `FMDHR` / `VMOV.32 d7[1], r4` in libm `_fmod+0x1a8`.
>
> Run21 cleared that exact stop and exited **0** at the configured
> **2,500,000,000-instruction cap**, **562,020,182 instructions beyond** run20.
> SpringBoard again entered `applicationDidFinishLaunching:` at
> 1,923,358,329, returned false from `SBTetherController isTethered` at
> 1,924,647,850, and continued through debugging/demo preferences,
> lock-button, and platform-controller initialization. It entered
> `SBTelephonyManager -init` after the singleton call at 1,965,837,070. PID 20's
> last exact user instruction was 1,966,242,080, then its thread switched out
> at 1,966,246,193 inside a shared-cache `mach_msg` before the telephony call
> returned. Post-run resolution proves that `_CTTelephonyCenterGetDefault`
> creates a CTServerConnection, successfully looks up literal
> `com.apple.commcenter`, receives port name **0x4f07**, and blocks in its
> initial generated handshake. The request ID is **0x0054b557**, with send size
> **0x834** and receive size **0x30**.
>
> Run22 then proved that the copied-in request reaches destination port object
> `0xc0d705a0`, whose mqueue is saturated at **`msgcount=qlimit=5`**. The
> trace then recorded the exact queue-full and `fullwaiters` PCs before the
> SpringBoard sender blocked and did not resume before the clean
> **2,100,000,000-instruction cap**. At that commit this did not prove five
> linked messages, because `msgcount` includes reserved/in-flight slots; those
> route PCs were adjacent candidates rather than a fail-closed binding because
> the old recorder omitted the decisive `r8` kmsg; and the run22 PID-1 owner
> print was rejected because that decoder did not first distinguish the port's
> active-receiver/in-transit/timestamp union.
>
> **Run23 resolved all three.** With the hardened probes it walked the queue to
> **five genuinely linked** messages with `reserved-or-in-flight=0` — every one
> the same `0x0054b557` handshake with a distinct reply port — **BOUND** both
> route PCs to the exact `r4=mqueue`/`r8=kmsg` pair, and decoded the receive
> right **authoritatively** to **launchd, PID 1**. It also found that
> AppleBaseband enabled its reset event source and its callback **never fired**,
> so no delivered baseband notification explains the saturation. Why CommCenter
> — alive, never exited, PID 24 — has not taken its own port is the open
> question. No dequeue, reply, or permanent deadlock is inferred.
>
> This is later control flow, **not a rendered home screen**. `UIController`
> remained at 0 hits, live scanout recorded 0 mutations, and the PPM remained
> the seed-only 8x16 block with 0 changed pixels in both runs. The original
> hashes remained unchanged, external-md failures were 0, the guest-free low was
> 50.63 MiB, and the run22 and run23 directories occupy 447.42 and 447.43 MiB on
> F:. Exact run22 source passed
> all eight hosted jobs in
> [core run 30106957804](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30106957804).
> Earlier exact-commit hosted
> [core run 30095081111](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081111)
> and
> [unsigned iOS run 30095081184](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081184)
> passed for `debec04`. Later test-only `0670ab8` also passed hosted core/iOS
> runs with VFP 469/0. Latest hosted test-only `657e8d8` passes VFP 488/0 locally and
> hosted
> [core run 30097023293](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023293)
> plus
> [unsigned iOS run 30097023356](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023356).
> The installable app still runs a synthetic guest through CoreGraphics and
> has no real-boot session, touch, audio or guest networking.

Everything here is from actual historical runs. The command below is the recipe,
not a promise of byte-identical current output: the stopping point and log are
tied to that run's source/configuration, and current output may differ. Supply
your own IPSW-derived inputs (see [BOOT_CHAIN.md](BOOT_CHAIN.md)):

```sh
build/core/bootkernel firmware/kernel.macho \
    -d firmware/devicetree.bin \
    -c "debug=0x8 serial=1 nand-enable-adm=0" \
    -r firmware/rootfs.img -R 512 \
    -n 400000000
```

Three parts of that command line are not preferences:

- **`-R 512`.** `arm_vm_init` hardcodes `virtual_avail = 0xe0000000`, so the
  kernel's physical-linear window is exactly 512 MiB. Advertising more makes
  `zone_virtual_addr` stop early-outing and index a `pv_head_table` that is
  still all zeros during `zone_bootstrap` — a null-zone dereference at ~34 M
  instructions that looks nothing like a RAM-size problem. The physical window
  is exactly `[0x08000000, 0x28000000)` and NOR begins at `0x28000000`; current
  machine initialization rejects the historical 768 MiB configurations before
  execution.
- **`nand-enable-adm=0`.** `AppleS5L8900XADMFMC::start` polls a NAND/DMA ready bit
  that never sets here and panics. The driver's own `probe()` honours this
  boot-arg, so it simply never matches. We boot from a RAM disk; we do not need
  NAND.
- **`-r firmware/rootfs.img`.** The real, decrypted 413 MiB
  (433,274,880-byte) HFSX root filesystem
  from the IPSW, published through `/chosen/memory-map` as `RAMDisk` with `rd=md0`
  appended to the command line.

The loader does not retain a second image-sized host buffer. It opens and sizes
the source, proves the kernel/device-tree/boot-args/RAM-disk/framebuffer ranges
are contained and pairwise disjoint, allocates guest DRAM once, and streams the
rootfs directly into its final range through the retained handle. Source
metadata is checked around the transfer. The roughly 445 MiB grown RAM disk
therefore lives inside the 512 MiB guest allocation instead of beside it.

Three more workarounds are applied automatically and echoed in the run header —
the IORTC 30-second wait patched to zero, the `mbx` node's `compatible` string
broken so the PowerVR driver does not match, and the `sha1` nub un-matched so
`IOCryptoAcceleratorFamily` never installs its hardware-SHA-1 hook (`-S` keeps it,
`-g` keeps MBX, `-K` disables the kernel patch). They are printed rather than
silent on purpose: every one of them is a lie we are telling the guest, and a lie
you cannot see is a lie you will spend a day rediscovering.

Instruction indices are counts of retired guest instructions since reset. They
are deterministic for a given build and inputs, and they move as the emulator
changes — treat them as a map, not a fingerprint. Sections marked *(historical)*
were measured on an earlier, shorter, root-less boot and are kept because the
narrative depends on them.

---

## Stage 0 — what stands in for iBoot

We do not run iBoot to hand off to the kernel. `bootkernel` recreates a subset of
the handoff inputs: loaded segments, boot arguments, device-tree properties and
RAM-disk metadata. This deliberate simplification gets to XNU quickly, but it is
not equivalent to executing iBoot, and the guest can observe the synthesized and
patched inputs described above.

The kernelcache is a real, decrypted, LZSS-expanded ARMv6 Mach-O. Its segments
are placed at their own preferred addresses, physical = virtual − 0xb8000000:

```
  __TEXT           vm 0xc0008000 -> pa 0x08008000   2,117,632 bytes
  __DATA           vm 0xc020d000 -> pa 0x0820d000      98,304
  __HIB            vm 0xc0000000 -> pa 0x08000000      20,480
  __KLD            vm 0xc0260000 -> pa 0x08260000       4,096
  __PRELINK_TEXT   vm 0xc02cd000 -> pa 0x082cd000   5,013,504   ← every kext
  __PRELINK_INFO   vm 0xc0795000 -> pa 0x08795000     245,760
  __LINKEDIT       vm 0xc0261000 -> pa 0x08261000     439,716
  entry            vm 0xc0069040 -> pa 0x08069040
```

`__PRELINK_TEXT` is the whole point of a *kernelcache*: every kernel extension
is already linked into the image. That is why Apple's drivers can start in a run
with no filesystem — they are already in memory before the first instruction.

Then we build the two structures iBoot owns.

**The device tree**, Apple's own format, taken from the same IPSW and patched in
place. iBoot fills in the frequencies at runtime because they depend on the
board; a tree with zeros in them makes the kernel divide by zero or wait
forever, so we write the S5L8900 values:

```
  /device-tree    clock-frequency       -> 103,000,000
  /cpus/cpu0      timebase-frequency    ->   6,000,000
  /cpus/cpu0      clock-frequency       -> 412,000,000
  /cpus/cpu0      bus-frequency         -> 103,000,000
  /cpus/cpu0      peripheral-frequency  ->  51,500,000
  /cpus/cpu0      fixed-frequency       ->  24,000,000
  /memory         reg                   -> {0x08000000, 0x08000000}
```

Every patch is same-length and in place, because this format has no relocation
table: each offset is implicit in the byte stream, so changing a length would
mean rewriting everything after it.

**`boot_args`**, 0x138 bytes at physical 0x087db000, with the kernel entered at
`r0 = &boot_args`. Two fields in it are load-bearing and were established by
experiment rather than assumption:

- `Version` (offset 2) must be **6**. `pe_identify_machine()` does
  `ldrh r3,[r0,#2]; cmp r3,#6` and panics `pe_identify_machine: Epoch Mismatch`
  on anything else.
- `topOfKernelData` (offset 0x10) must be **physical**, not virtual. The kernel
  uses it directly as the base for the page tables it is about to build; passing
  the virtual form put TTBR0 at 0xc07dc018 and the first MMU walk read unmapped
  memory.

---

## Stage 1 — reset to virtual memory

| instr | what |
|---|---|
| 0 | `__start`, MMU off, running from physical 0x08069040 |
| 44,312 | `_PE_init_platform` |
| 44,777 | `_DTInit` — the kernel takes the device tree we handed it |
| 44,790 | `_pe_identify_machine` — the epoch check above |
| 44,798 | `_pe_arm_get_soc_base_phys` — asks the tree where the SoC lives |
| 49,023 | first `_DTGetProperty` (858 calls before the run ends) |
| 56,743 | `_PE_parse_boot_argn` — reads the command line, 67 calls |
| 58,575 | **`_arm_vm_init`** — builds page tables and enables the MMU |

From here the kernel is executing out of its own translation regime at
0xc0000000, in tables it wrote itself, walked by our MMU out of guest RAM through
the ordinary bus — exactly as hardware would.

**And not out of TTBR0 alone.** Eleven instructions into `__start` the kernel
writes `TTBCR.N = 2`, which on ARMv6 splits translation: virtual addresses below
2^(32−N) come from TTBR0, everything above from TTBR1. Kernel text
(0xc0008000–0xc020d000) and the 0xffff0000 vector page therefore live in **TTBR1**,
and `set_mmu_ttb` thereafter rewrites TTBR0 *alone* as it switches user pmaps.
Walking TTBR0 unconditionally survives this whole stage — both registers start at
the same base — and dies at the first `pmap_switch` to a user pmap, hundreds of
millions of instructions later, as an unexplained prefetch-abort storm at
0xffff000c. That was the single longest-lived bug in this project.

This stage is unforgiving in a specific way: everything before `_arm_vm_init` is
physical, everything after is virtual, and a wrong address in `boot_args` does
not fail here. It fails later, somewhere that looks unrelated.

---

## Stage 2 — finding the serial port

The kernel does not know where the UART is. It asks the device tree, and then it
maps it.

| instr | what |
|---|---|
| 127,579 | `_PE_init_kprintf` |
| 129,675 | `_serial_init` |
| 131,527 | asks `pe_arm_get_soc_base_phys` for the SoC base |
| 157,003 | finds the `uart0` node in the tree |
| 157,472 | `_ml_io_map` — maps the register page into kernel space |
| 158,663 | `serial_init` returns 1: **a working console** |
| 160,727 | `_switch_to_serial_console` |
| 163,340 | `_PE_initialize_console` |
| 163,351 | `_initialize_screen` |

`/device-tree/arm-io/uart0` has `reg = {0x4c00000, 0x1000}`, an offset from the
SoC base of 0x38000000 — physical **0x3cc00000**, which is
`core/src/soc/uart.c`. The kernel then programs it exactly as it would program
silicon:

```
  WRITE off 0x00 (ULCON)  = 0x00000003   8 data bits
  WRITE off 0x04 (UCON)   = 0x00000405
  WRITE off 0x28 (UBRDIV) = 0x00010019   the divisor for its baud rate
  READ  off 0x10 (UTRSTAT)= 0x00000006   "transmitter ready"
  WRITE off 0x20 (UTXH)   = 0x00000069   'i'
```

That poll of UTRSTAT before every byte is why our UART reports the transmitter
permanently ready: a device that never says "ready" is a device the guest spins
on forever, and it will not tell you why.

---

## Stage 3 — the first byte

| instr | what |
|---|---|
| 7,895,270 | `_printf` entered, from `_PE_init_iokit+0x1a` |
| 7,895,357 | first `_uart_putc` — the first byte this project ever emitted |

The format string, recovered from `r0`, is `"iBoot version: %s\n"`. The argument
is empty because we did not run iBoot to leave a version string behind, so the
line comes out as `iBoot version: ` — 15 characters that took a real timer to
earn. Before the timer block was right, `mach_absolute_time()` read zero forever
and the kernel never got here at all.

Note the nearly eight million instructions between the console being *ready*
(163,340) and the console being *used* (7,895,270). That gap is the VM system and the
zone allocator coming up: `_zcram`, `_zone_page_alloc`, `_kernel_memory_allocate`,
`_vm_page_grab`. The kernel does an enormous amount of work before it says
anything at all, which is exactly why "no output" is such a poor diagnostic and
why this project instruments call paths instead.

---

## Stage 4 — the heartbeat

| instr | what |
|---|---|
| 241,995 | **FIQ #0** — the first interrupt ever taken by this machine |
| 245,911 | `_machine_startup` |
| 249,822 | `_kernel_bootstrap` |

The interrupt path is worth spelling out because three separate components have
to agree, and each was wrong at some point:

- `_pe_arm_init_interrupts` programs **timer 4** at offsets 0xA0–0xAF in the
  block at physical 0x3e200000 (`/device-tree/arm-io/timer`,
  `core/src/soc/timer.c`), and routes **VIC line 7 to FIQ** — not IRQ.
- `_s5l8900x_set_decrementer` writes the next deadline to 0xA8.
- `_fleh_fiq_s5l8900x` acknowledges by writing `0x00030000` to the latch. Our
  acknowledge mask has to be exactly that: latch any bit the handler's write
  does not clear and the line stays asserted, the handler re-enters immediately,
  and the boot hangs with no diagnostic at all.

Separately, `_s5l8900x_get_timebase` reads 0x080/0x084 as a **free-running
64-bit counter** — this is `mach_absolute_time()`. It must count whether or not
any timer is armed; gating it on timer 4's enable bit reads zero through all of
early boot.

The steady-state cadence, from the log:

```
  FIQ #1  @instr 13,991,177
  FIQ #2  @instr 18,106,370   gap 4,115,193   t4_count 59,930
  FIQ #3  @instr 22,221,564   gap 4,115,194   t4_count 59,930
```

59,930 ticks at 6 MHz is 9.99 ms; 4,115,193 instructions at 412 MHz is 9.99 ms.
The kernel's 100 Hz scheduler tick and the emulator's instruction budget agree,
which is the whole content of the timebase-ratio fix: feed the timer at the
guest's real cpu:timebase ratio (one tick per ~68 instructions) instead of 1:1.
Before that fix this same boot took **1,939,179** FIQs and spent 65.9% of its
instructions inside the handler, because the kernel could never service a
deadline before the next one had already passed. The 400 M-instruction boot now
takes **385** FIQs — 38,235 instructions, 0.0% of the run, longest single entry
344 instructions.

---

## Stage 5 — IOKit, and Apple's drivers meeting our hardware

This is the part that makes the whole exercise real. IOKit walks the device tree
we supplied, matches the prelinked kexts against the nodes it finds, and starts
them. They then program registers — and what is on the other side of those
registers is C in this repository.

The console output, in order, with what each driver is actually talking to:

| Apple's kext says | device-tree node | physical | on our side |
|---|---|---|---|
| `AppleS5L8900XIO::start: chip-revision: EVT0` | `/arm-io` | 0x38000000 | the SoC nub itself |
| `AppleARMPL192VIC::start: _vicBaseAddress = 0xe38ed000` | `/arm-io/vic` | 0x38e00000 | `soc/vic.c` — 2 reads, 21 writes |
| `AppleS5L8900XEdgeIC::start: 0xe38e6000` | `/arm-io/edgeic` | 0x38e02000 | **not modelled** — 2 writes, counted |
| `AppleS5L8900XGPIOIC::start: 0xe38f5000` | GPIO IC | 0x39a00080 | shares the power page; GPIO proper at 0x3e400000, 27 writes, not modelled |
| `AppleS5L8900XPowerController::start: 0xe38fd000` | `/arm-io/power` | 0x39a00000 | `soc/power.c` — 7 reads, 21 writes |
| `AppleS5L8900XClockController: Dynamic Performance State Management Enabled with max state 3` | `/arm-io/clkrstgen` | 0x3c500000 | **not modelled** — 1 read, 6 writes |
| `AppleARMPL080DMAC::start: dmac0 / dmac1` | `/arm-io/dmac0`, `dmac1` | 0x38200000, 0x39900000 | mapped, no register traffic yet |
| `AppleS5L8900XADM::start: mapped I/O registers at 0xe9915000/0x38800000` | `/arm-io/adm` | 0x38800000 | mapped, no register traffic yet |
| `AppleS5L8900XSDIO::start(): SDIO Revision 8900X` … `registers @ paddr 0x38d00000` | `/arm-io/sdio` | 0x38d00000 | **not modelled** — 1 read, 6 writes |
| `AppleS5L8900XSPIController::start: spi0 / spi1` | `/arm-io/spi0`, `spi1` | 0x3c300000, 0x3ce00000 | mapped, no register traffic yet |
| `AppleS5L8900XUSBPhy::start registers at 0xea942000` | `/arm-io/otgphyctrl` | 0x3c400000 | mapped, no register traffic yet |
| `AppleS5L8900XI2CController::start: i2c0 / i2c1` | `/arm-io/i2c0`, `i2c1` | 0x3c600000, 0x3c900000 | **not modelled in this historical run** — i2c0 had 7 reads and 7 writes; both controllers and the PCF50635 are now modelled |
| `AppleS5L8900XTimer::start: 0xea94a000` | `/arm-io/timer` | 0x3e200000 | `soc/timer.c` — 6,447 reads, 254 writes |
| `AppleS5L8900XWatchDogTimer` + `AppleARMWatchDogTimer installing handlePEHaltRestart handler` | `/arm-io/wdt` | 0x3e300000 | mapped, no register traffic yet |
| `AppleS5L8900XI2SController::start: i2s0 / i2s1` | `/arm-io/i2s0`, `i2s1` | 0x3ca00000, 0x3cd00000 | mapped, no register traffic yet |
| `AppleMPVDDriver::init / ::start` | `/arm-io/mpvd` | 0x39600000 | video decoder; mapped only |
| `AppleMBXDevice(0xc0bf4800): Init` | `/arm-io/mbx` | 0x3b000000 | the 2D/3D block; mapped only |
| `ApplePCF50635PMU::start: pmu _pmuIICNub` | on i2c0 | via 0x3c600000 | the PMU speaks I²C; consistent with the i2c0 traffic above |
| `AppleMicron2020::start()` / `Registering IOCameraSensor service.` | camera | — | the sensor, over I²C |
| `AppleBaseband: Could not find mux function` | `/arm-io/spi2` | 0x3d200000 | the full modem is not modeled; M5 now requires only a faithful graceful no-modem path if this absence proves causal |

**That table is the 200 M-instruction boot *(historical)*, and it is now the
short version.** Honouring `TTBCR.N`/`TTBR1` in the MMU started the rest of the
tree — the boot had been stopping at the first `pmap_switch` to a user pmap,
which deleted kernel text and the vector page from a walk that only ever
consulted TTBR0, and stormed on prefetch aborts at 0xffff000c forever. What the
same UART carries now, in order and unedited apart from elision:

```
BSD root: md0, major 2, minor 0
AppleBaseband::start(0xc07c4a00): baseband
AppleS5L8900XIO::start: chip-revision: EVT0
AppleARMPL192VIC::start / AppleS5L8900XEdgeIC::start / AppleS5L8900XGPIOIC::start
AppleS5L8900XPowerController::start / AppleS5L8900XClockController::start
AppleARMPL080DMAC::start: dmac0 / dmac1
AppleS5L8900XADM::start: mapped I/O registers at 0xe316a000/0x38800000
AppleS5L8900XWatchDogTimer::start / AppleARMWatchDogTimer installing handler
IOSDIOController::init(): IOSDIOFamily-24.7 Dec 18 2009 01:49:48
AppleS5L8900XSDIO::start(): SDIO Revision 8900X
AppleS5L8900XI2CController::start: i2c0 / i2c1
AppleMPVDDriver::init / ::start
AppleS5L8900XI2SController::start: i2s0 / i2s1
AppleS5L8900XSPIController::start: spi0 / spi1
AppleS5L8900XUSBPhy::start registers at 0xe9fca000
AppleS5L8900XTimer::start: _timerBaseAddress: 0xe9fd2000
com.apple.AppleFSCompressionTypeZlib load succeeded
virtual bool AppleMobileFileIntegrity::start(IOService*): built Dec 21 2009 08:27:49
L2TP domain init / PPTP domain init
Jettisoning kext bootstrap segment.
AppleMicron2020::start() / Registering IOCameraSensor service.
ApplePCF50635PMU::start: pmu _pmuIICNub: 0xda970e80
AppleARMPL080DMAC::_initDMAChannel: index: 0..7  (twelve channel inits)
AppleS5L8900XSerial: Identified Serial Port on ARM Device=uart0/uart1/uart3/uart4
AppleSerialMultiplexer: mux::start: created new mux (18) for spi-baseband
AppleMultitouchZ2SPI: successfully started
AppleMultitouchZ2SPI: using DMA for bootloading
IOSDIOController::enumerateSlot(): Searching for SDIO device in slot: 0
[0.567924666]: AppleS5L8900XSDIO::sendCommand(): Timeout waiting for CMDRDY
IOSDIOController::enumerateSlot(): CMD5 failed with SDIO device on slot 0
AppleS5L8900XSDIO::enumerateCards(): Unable to enumerate SDIO device
```

4,595 bytes from unmodified Apple kernel and kext code. Three lines are worth
naming: `AppleMobileFileIntegrity` — the code-signing enforcer — starts cleanly;
`AppleMultitouchZ2SPI` reaches its bootloading/DMA request, which proves execution
of that driver path but not a usable touch device; and SDIO reaches the mapped
command/poll path before timing out. The timeout does not establish working card,
interrupt, timing or error semantics.

Two things in the historical table are worth dwelling on, and both still hold.

**"Mapped, no register traffic yet" is the honest state, not a summary.** Those
drivers matched, ran their `start()`, called `ml_io_map` to get a virtual window
onto their registers, printed the address — and then did not touch it in the
remainder of this run. That is a real observation about how far each driver
gets, and it comes from counting every non-RAM access rather than from reading
the log's prose.

**"Not modelled" is visible, not silent.** In this run 19 reads and 772 writes
went to 10 pages nothing answers for. They are counted, attributed to a PC, and
reported by page. That is the entire reason the power controller was findable:
one page absorbed 3,887,707 reads because a driver was polling a status bit that
would never change, and it showed up as a line in a report instead of as a boot
that mysteriously took forever.

The complete list of physical pages the kernel touched outside RAM — **22** of
them now, in the 400-million-instruction boot with the root filesystem. The
right-hand column is the *first PC* that reached each page, and this is the
report that changed most in the last session: it used to bottom out at a bare
address, and now it names the kext.

```
  0x3cc00000  r=4595   w=4601   uart0      _PE_init_kprintf+0x9c
  0x3e200000  r=87963  w=1304   timer      _pe_arm_init_interrupts+0xba
  0x38e00000  r=88     w=292    vic0       _pe_arm_init_interrupts+0xf8
  0x38d00000  r=10003  w=10     sdio       AppleS5L8900XSDIO+0x118c
  0x3d000000  r=3      w=702    pke        AppleS5L8900XCrypto+0x3930
  0x39a00000  r=18     w=38     power      AppleS5L8900X+0x24e8
  0x3e400000  r=0      w=49     gpio       AppleS5L8900X+0x24d0
  0x3c500000  r=1      w=19     clkrstgen  AppleS5L8900X+0x5e54
  0x38e01000  r=0      w=16     vic1       AppleARMPL192VIC+0x1290
  0x38e02000  r=0      w=3      edgeic     AppleS5L8900X+0x56a0
  0x3c600000  r=7      w=7      i2c0       AppleS5L8900X+0x3e2c
  0x3c900000  r=7      w=7      i2c1       AppleS5L8900X+0x3e2c
  0x3c300000  r=0      w=13     spi0       AppleS5L8900X+0x4978
  0x3ce00000  r=0      w=19     spi1       AppleS5L8900X+0x4978
  0x3cc04000  r=8      w=15     uart1      AppleS5L8900XSerial+0x20f8
  0x3cc0c000  r=10     w=17     uart3      AppleS5L8900XSerial+0x20f8
  0x3cc10000  r=8      w=15     uart4      AppleS5L8900XSerial+0x20f8
  0x38c00000  r=0      w=42     (crypto)   AppleS5L8900XCrypto+0x14c8
  0x38000000  r=6      w=11     (arm-io)   AppleS5L8900XCrypto+0x2b24
  0x38200000  r=1      w=2      dmac0      AppleARMPL080DMAC+0x1d08
  0x38100000  r=2      w=0      unidentified — no device-tree node at this offset
  0x00000000  r=1      w=0      _PE_create_console reading the framebuffer base
```

`0x38d00000` is the interesting one: 10,003 of its 10,013 accesses are
`IOSDIOController::enumerateSlot` polling for a CMD5 response from a card that
does not exist. It times out and reports so — a device correctly failing is not
the same thing as a device that hangs, and the report distinguishes them.

For addresses inside a kext, the resolver can go as far as `<bundle-id>+0xNNNN`
and no further, and that is a hard limit rather than a to-do: the kernelcache
builder strips each prelinked kext's `LC_SYMTAB`, so there are no per-kext
function names to find. None of the kernel's 11,430 symbols fall inside
`__PRELINK_TEXT`. See [debugging.md](debugging.md).

---

## Stage 6 — BSD, and the root filesystem

| instr | what |
|---|---|
| 64,567,734 | **`_bsd_init`** |
| 81,654,150 | first `_kprintf` / `_serial_putc` |
| 116,573,687 | first data abort: FSR 0x07, DFAR 0xea110000, in `IOBufferMemoryDescriptor::initWithPhysicalMask` |
| ~73.5 M *(measured separately)* | `_IOFindBSDRoot` → `_mdevadd` → **`BSD root: md0, major 2, minor 0`** |

That last line is the one M4 was for. The kernel walked its own storage stack,
found the RAM disk we published in `/chosen/memory-map`, and mounted a real
413 MiB HFSX volume out of Apple's own IPSW as its root.

Getting there needed two things that were not obvious. In this historical run,
`bsd_init` called `IOKitInitializeTime`, which did
`waitForService(resourceMatching("IORTC"), &{tv_sec = 30})`; the PMU/RTC was not
modelled and `IORTC` was not published. Both I2C controllers and a PCF50635 are
now modelled, but direct unpatched `IORTC` publication remains unverified.
Thirty seconds of guest time is a very long silence.
Patching that timeout to zero reaches `IOFindBSDRoot`. And the 512 MiB `memSize`
cap above is what stops early VM init from faulting once a 413 MiB disk is
actually present.

Where the instructions actually go, sampled every 1,024 instructions:

```
   17.0%  OSDictionary::getObject(OSSymbol const*)
    5.2%  _mac_file_label_init
    3.5%  _lck_mtx_unlock
    3.3%  OSMetaClass::checkMetaCast
    3.0%  _strncmp
    2.6%  OSObject::taggedRelease
    2.3%  _lck_mtx_lock
    2.3%  OSSymbolPool::findSymbol
```

That profile is a portrait of IOKit matching: string interning, dictionary
lookups and metaclass casts, over and over, as the registry is built and every
prelinked personality is compared against every node. It is what a healthy 2009
kernel bringing up its driver stack looks like, and it is the strongest single
piece of evidence that the emulation is not merely *not crashing* but doing the
right work.

Note the report's own warning in this run: 77,858 samples were dropped because
the profiler's function table filled at 1,024 entries, and it says so rather than
quietly printing a plausible-looking distribution. An earlier version of this
profiler silently dropped everything past 64 entries and printed identical output
at 200 M and 400 M instructions — which looked exactly like coverage.

`_panic` and `_Debugger` are never reached. 48 distinct abort sites remain, all
FSR 0x07 on a marching sequence of kernel virtual addresses in
`IOBufferMemoryDescriptor::initWithPhysicalMask` and the kernel's own
`_fleh_dataabt`; the kernel takes them and continues. "Survivable and
unexplained" is on the list of things to explain rather than a thing to be
pleased about.

---

## Stage 7 — pid 1, and where the historical pre-VFP run stopped

`bsdinit_task` runs, `load_init_program` opens `/sbin/launchd` off the volume we
just mounted, and `execve` gets remarkably far.

| instr | what |
|---|---|
| 230,812,220 | `_bsdinit_task` |
| 230,864,582 | `_load_init_program` |
| 230,895,729 | `_execve` |
| 230,968,564 | `_mac_vnode_check_exec` — the MAC policy is consulted and allows it |
| 231,010,531 | `_grade_binary` (3 calls) — the armv6 Mach-O is graded, not rejected |
| 231,011,045 | `_load_machfile` — launchd is mapped |
| 231,049,078 | `_ubc_cs_blob_add` (2 calls) — its signature blobs are registered |
| 232,201,298 | `_cs_validate_page` (15 calls), `cs_validate:hashing` (15) — and `bad_hash` / `no_hash_exit` **never reached**: every page validates |
| 233,031,366 | **`_fleh_swi`** (24) — the first system call instruction pid 1 ever executes |
| 233,347,392 | `_mach_msg_overwrite_trap` (12) |
| 234,013,919 | **`_unix_syscall`** (5) |
| 234,731,379 | `_fleh_undef` (1) → `_sleh_undef` → `_vfp_trap` |
| **234,731,493** | **the emulator stops: UNDEFINED INSTRUCTION, `0xecb10a20`, lr `_vfp_trap+0x38`** |

**pid 1 executed user-mode code and made system calls.** `_panic` was never
reached in this run. It did not end on a guest panic — the emulator stopped on
an instruction it did not then implement.

XNU does not leave VFP enabled: `_init_vfp` grants CP10/CP11 access once, and
after that the gate is `FPEXC.EN` alone, cleared per thread. So the first VFP
instruction any thread executes is *supposed* to take an Undefined exception,
which `_fleh_undef` → `_sleh_undef` → `_vfp_trap` → `_vfp_switch` turns into
"enable VFP and re-run it". That whole path worked in this run. What stopped
this run was the instruction `_vfp_switch` itself uses: `0xecb10a20` decodes as
`VLDMIA r1!, {s0-s31}`, the load-multiple that restores a thread's VFP register
file. The interpreter correctly returned `ARM_UNDEFINED` rather than guessing.
That VFP family is implemented and covered by regression tests now, so this is
no longer the current blocker.

Keep the scale honest: in this trace, five BSD system calls and twelve Mach
traps was not a userland. Nothing had been logged by userspace and no daemon had
started. Later work added the CLCD/panel path; it did not turn this historical
trace, or the app's synthetic demo, into SpringBoard.

Three walls on this exact path are worth recording, because each looked like a
completely different problem from its symptom:

- The kernel first reached this code and **livelocked on ~2.8 million identical
  data aborts** at `_copyout+0x40`, one every ~395 instructions, because we never
  set `DFSR.WnR`. XNU therefore repaired every write fault as a read fault,
  forever. The very first unprivileged write the kernel performs is the `copyout`
  of the string `"/sbin/launchd"`.
- Then `execve` returned **errno 86, `EBADARCH`**, on a disk where all 385 ARM
  Mach-Os are cputype 12 / cpusubtype 6. The disk was fine; we were returning
  zero for `ID_ISAR1`, so the kernel's Jazelle probe failed, its architecture
  field stayed 0xF, and `cpu_subtype` fell through to `CPU_SUBTYPE_ARM_ALL` —
  which `grade_binary`'s jump table does not cover.
- Then **launchd's first text page failed its signature**, and it spun
  `cs_invalid_page` → `psignal` ~95,000 times. This looked exactly like a corrupt
  disk image, and it was not. `cs_validate_page` hashes exactly 4096 bytes, and
  `SHA1UpdateUsePhysicalAddress` routes exactly-4096-byte buffers to a **hardware**
  SHA-1 engine whenever `_performSHA1WithinKernelOnly` is non-NULL — a hook
  installed by `IOCryptoAcceleratorFamily`, which matched in our boot. The engine
  at 0x38000000 is not modelled, so six reads came back as whatever the stub
  returned and `SHA1Final` emitted that.

  Two private, untracked historical verifications exonerated the image *before*
  anything was changed — a UDIF verifier over all 7 `blkx` tables and every
  per-`blkx` CRC32, and an HFSX reader that reported code-directory page hashes
  for all 155 signed Mach-Os and 6,731 code pages on the volume. They reported
  zero mismatches, launchd 46/46 and dyld 56/56. The verifier tools and outputs
  are not present in the public tree, so this is recorded evidence rather than
  a reproducible current check.

  **The clinching evidence was timing.** `SHA1Transform` costs ~2,262 Thumb
  instructions per 64-byte block, so hashing 4 KB in software must cost ~145,000
  instructions. The observed `SHA1Init` → verdict interval was **14,329** — 10.1x
  too few. Software SHA-1 provably never ran, which located the bug without
  disassembling anything further.

### Stage 8 — the fstab wall, and why launchd was halting the machine

Once launchd ran, the boot ended in a *clean guest reboot*: `_halt_all_cpus`,
with `_panic` never reached. That is not a kernel fault, it is launchd giving
up. The console said:

```
Running fsck on the boot volume...
/dev/disk0s1: No such file or directory
/dev/disk0s1: CAN'T CHECK FILE SYSTEM.
/dev/disk0s1 (hfs) EXITED WITH SIGNAL 8
fsck failed!
```

The guest's `/private/etc/fstab` is 76 bytes and reads:

```
/dev/disk0s1 / hfs ro 0 1
/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2
```

**Neither device can exist on this machine.** `disk0` on an iPhone1,2 is
published by `AppleNANDFTL` (raw NAND → a linear logical space) and cut into
`disk0s1`/`disk0s2` by **`IOFlashPartitionScheme`**, which fails its probe
unless the provider carries a `boot-from-nand` property and then validates a
*magic* and a *major version* on an on-media partition table:

```
IOFlashPartitionScheme::%s: ERROR: magic on partition table, 0x%08X, doesn't match expected value, 0x%08X
IOFlashPartitionScheme::%s: ERROR: major version on partition table, 0x%08X, does not match driver, 0x%08X
```

Both that table and the FTL's own on-media format are undocumented, so per the
project rule we do not synthesise them. We boot the system volume as the RAM
disk `md0` instead, and there is no `disk0` of any kind.

Be precise about *which* piece is undocumented, because it is easy to conclude
too much here. Partitioning in general is fine: `IOStorageFamily` in this
kernelcache carries built-in **`IOGUIDPartitionScheme`** and
**`IOFDiskPartitionScheme`** personalities (provider `IOMedia`,
`IOPropertyMatch { Whole = true }`, and an FDisk content table that maps type
`0xAF` to `Apple_HFS`), so any block device that does get published can be cut
up with an ordinary GPT or MBR. What is missing is anything that would publish
that `IOMedia` **during the launchd bootstrap** — there is no
`IOUSBMassStorageClass` and no SCSI stack in this kernelcache, and the
kernel-side DiskImages entry point `di_root_image()` is called solely by
`imageboot`/`netboot`, neither of which is compiled in. The DiskImages stack
itself is alive and does have userland clients on the system volume —
`/usr/libexec/mobile_image_mounter` and `/usr/libexec/debug_image_mount` both
name `IOHDIXController` and `hdik-unique-identifier` — but they are lockdownd
services that attach a `.dimage` on host request, onto `/Developer`, long
after fstab has been read. The kernel also has exactly one memory device:
`IOFindBSDRoot` reads a single `RAMDisk` property under a static `didRam`
guard, so there is no `md1` to be had either.

Disassembling `launchctl`'s `_bootstrap_cmd` (launchd-321) settles exactly what
launchd wants, and the distinction turns out to be the whole answer — **fsck is
fatal, mount is not**:

```c
statfs("/", &sfs);
if (sfs.f_flags & MNT_RDONLY) {          /* xnu mounts EVERY root MNT_RDONLY */
    if (!is_safeboot()) fputs("Running fsck on the boot volume...\n", stdout);
    if (fwexec(fsck -p) == -1 && fwexec(fsck -fy) == -1) {
        fputs("fsck failed!\n", stdout);
        reboot(RB_HALT);                 /* <-- the halt. unconditional. */
    }
    path_check("/etc/fstab") ? fwexec(mount -vat nonfs)
                             : fwexec(mount -uw /);
    /* ... every failure from here on is only _log_launchctl_bug() ... */
}
```

Three facts fall out of that, each checked against the binaries on our own
rootfs rather than assumed:

- `fwexec()` returns −1 unless the child exits with status 0, so `/sbin/fsck`
  really must succeed. `/sbin/fsck` is the BSD wrapper: it reads `/etc/fstab`
  and checks every entry with a nonzero pass number, so it inherits whatever
  the file says. Deleting the file does not help — it has
  `"Can't open checklist file: %s"` and exits non-zero.
- The `MNT_RDONLY` test is on `struct statfs.f_flags` at **offset 0x40**, which
  identifies it as the 64-bit-inode `struct statfs`. If `/` is already
  read-write, launchd skips this entire block.
- `mount(8)`'s `ismounted()` compares **both** `f_mntfromname` and
  `f_mntonname`, and xnu's root mount has `f_mntfromname == "root_device"` (set
  in `vfs_rootmountalloc_internal`, which is also where the unconditional
  `MNT_RDONLY` comes from). So an fstab entry for `/` is *not* skipped as
  already-mounted, and with `update` in its options it becomes a genuine
  `MNT_UPDATE` remount.

So the VM rewrites the record in the **loaded copy** of the RAM disk to name
the device it actually provides (`tools/bootkernel.c`, `rd_rewrite_fstab`; the
image on disk is never touched, and the patch refuses unless the stock 76 bytes
appear exactly once):

```
/dev/md0 / hfs rw,update 0 1
```

`pass 1` keeps Apple's own `fsck_hfs` in the loop rather than skipping the
check the hardware would have done — and it costs nothing, because the volume
carries `kHFSVolumeUnmounted` and `fsck_hfs -p` quick-exits on a clean volume.
The result:

```
Running fsck on the boot volume...
/dev/md0 on / (hfs, local, noatime)
launchctl: Couldn't stat("/etc/mach_init.d"): No such file or directory
```

`mount -v` prints `read-only` when it means it; its absence is the proof that
`/` is now mounted **read-write** — which on hardware is what `disk0s2` would
have supplied for `/private/var`. launchd clears the whole crucial-path table
(`/tmp`, `/var/tmp`, `/var/folders`, `/var/db/launchd.db` all already exist on
the volume) and goes on to load LaunchDaemons.

Two residual complaints on that path are cosmetic and worth naming so nobody
chases them:

- `Bug: launchctl.c:3793 ... sysctl(nbmib, 2, ...) == 0` — `nbmib` is
  `{CTL_KERN, 40}` = `KERN_NETBOOT`, a node this kernelcache does not register
  because its NFS client is compiled out. The failure path branches to the
  *same* instruction as the `netbooting == 0` success path, so it changes
  nothing.
- `Bug: launchctl.c:3094 ... value != NULL` — the `boot-args` property lookup
  on `IODeviceTree:/options`, used only to decide whether the boot is verbose.

### The volume had zero free blocks

`freeBlocks == 0` in the volume header and every one of the 105,780 bits in the
allocation bitmap set: Apple ships the system dmg sized exactly to its contents,
because on hardware `/` is `disk0s1` and stays read-only forever while
everything writable lives on `disk0s2` — a volume the restore `newfs`es and
which this machine does not have. So `/` was writable and nothing could be
*allocated* on it.

`bootkernel --grow <MB>` (default **32**, `0` disables) grows the volume in the
**loaded copy** of the RAM disk, so `firmware/rootfs.img` stays as it came out
of the IPSW. HFS+ is specified in Apple's TN1150 and the image is a bare,
unencrypted volume, so this is the documented layout applied rather than a
guess at one — four edits, in `rd_grow_volume()`:

1. `volumeHeader.totalBlocks += n`;
2. `volumeHeader.freeBlocks` **recounted from the bitmap**, never derived from
   the growth, so the header cannot end up describing a bitmap that was not
   written;
3. the allocation block holding the **reserved tail** moves: the old one is
   freed, the new one allocated;
4. the alternate volume header is rewritten at `totalBlocks × blockSize − 1024`.

Step 3 is the one with a trap in it. TN1150 puts the alternate volume header
1024 bytes before the end of the volume and reserves the final 512; the block
containing that tail is marked in use and belongs to no file. Walking the
catalog and the extents overflow file over this image accounts for 105,778 of
the 105,780 in-use blocks, and the two left over are exactly block 0 (boot
blocks + primary header) and block 105,779 (the alternate) — and no file
*could* own that last block, because extents are whole allocation blocks, so an
owner would own the alternate header's bytes and be scribbled on at every
flush. When the tail moves, that block becomes ordinary free space.

The 16 KiB allocation file is the ceiling: 16384 × 8 = 131,072 bits, a 512 MiB
volume at this block size. `--grow` clamps there and says so. (The headroom
exists because `newfs_hfs` rounded 105,780 bits up to a whole 4-block clump;
TN1150 allows a bitmap larger than the volume needs and requires only that the
surplus bits be zero, which they are.)

Rebuilding the bitmap from scratch — a full catalog walk, which is what
`fsck_hfs` phase 5 does — reproduces the on-disk bitmap exactly on all 113,971
blocks of the grown volume, and `freeBlocks` matches the recount: 8191 blocks,
32.00 MiB.

**What the guest says about it.** `fsck_hfs -p` accepts the grown volume and
launchd goes on to remount `/` read-write, which is the kernel's own HFS mount
code independently accepting the new `totalBlocks` against the new device size
(`md0`'s size comes from the same grown buffer, so the volume exactly fills the
device — which is where `fsck_hfs` looks for the alternate header). But
`fsck_hfs -p` **quick-exits on a volume carrying `kHFSVolumeUnmounted`**, so
that is a weaker check than it sounds.

In the historical run, forcing the full one — clearing the clean bit so preen
had to do the real scan — exposed a CPU gap rather than a volume defect:

```
Running fsck on the boot volume...
=== ABNORMAL STOP: UNDEFINED INSTRUCTION ===
  pc 0x00013130  cpsr 0x60000010 (User, ARM)
```

`0x13130` is `fsck_hfs+0x12130` (its `__TEXT` is at `0x1000`), and the word
there is `0xe1660385` — **`SMULBB r6, r3, r5`**. At that commit the interpreter
trapped the DSP-multiply space deliberately. Current source implements and tests
the complete related ARMv5TE set (`SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and
`SMLAWy`), so this exact instruction gap is closed. The forced-dirty full-check
scenario has not yet been rerun end to end, so the honest current claim is
"opcode implemented", not "full fsck completed".

So the volume is checked where it actually lives instead. Snapshotting the
machine at 2.9 G instructions and reading the volume header and allocation
bitmap straight out of guest DRAM, at the RAM disk's own physical address:

```
LIVE GUEST VOLUME HEADER (guest DRAM, 2.9 G instructions in)
  signature HX  version 5  blockSize 4096
  totalBlocks 113971   freeBlocks 8191   nextAllocation 105779
  attributes  0x00000000
live bitmap: 105780 of 113971 blocks in use, 8191 free
bits set in the newly-added range 105779..113970: [113970]
```

Every number is the one written into the image, unchanged after billions of
instructions of guest execution — and the one bit set in the new range is the
new alternate volume header's block, exactly where TN1150 puts it. The
interesting field is `attributes`, which the image carries as `0x00000100`
(`kHFSVolumeUnmounted`) and the guest has **cleared**: that is the kernel's own
`hfs_mountfs` marking the volume mounted and flushing the header back. Writes
to `md0` reach the volume, and the volume they reach is the grown one.

`freeBlocks` is still the full 8191, so nothing had been allocated yet at that
point — though HFS+ updates the on-disk header lazily, so that is evidence of
"no sync since a write" as much as "no write".

**The free space did not, by itself, change the historical comparison.** Run
out to 3 billion instructions with and without `--grow`, the console was
identical line for line.
`_execve` stays at 11 either way — but that number was never measuring what it
looked like it was measuring:

```
mDNSResponder[14] syscall_builtin_profile: mDNSResponder (seatbelt)
mDNSResponder[14] Builtin profile: mDNSResponder (seatbelt)
```

**A LaunchDaemon is running, as pid 14, in both runs.** launchd starts its jobs
with `posix_spawn`, not `execve`, so the `_execve` probe never sees one; the 11
hits are `load_init_program` plus the `fwexec()` helpers of launchd's own
bootstrap. Nothing was stuck at 11. Both runs simply need ~3 G instructions
rather than 800 M to get there, and both then stall in the same place.

### Where the free space has to come from

Growing the volume comes straight out of the guest's free page pool, because
the RAM disk is static memory below `topOfKernelData`: 90.93 MiB before,
58.93 MiB after, at the documented `-R 512`.

`topOfKernelData` is a single **line**, not a list — everything below it is
static — so a RAM disk placed *below* the kernel image is exactly as protected
as one placed above it, and stops pushing that line up by the size of the root
filesystem. `-Y` does that, and needs `-V` to open a gap under the kernel
(`phys = vmaddr − virt_base + phys_base`). Measured:

| flags | RAM disk | free page pool | volume free | reaches |
|---|---|---|---|---|
| `-R 512` (documented boot command) | 445 MiB | 58.93 MiB | 32 MiB | launchd, daemons |
| `-V 0xa4000000 -R 768 -Y` (historical; now rejected) | 445 MiB | 312.14 MiB | 32 MiB | `BSD root: md0`, then idle |
| `-V 0xa0000000 -R 768 -Y --grow 100` (historical; now rejected) | 512 MiB | 248.14 MiB | 98 MiB | `BSD root: md0`, then idle |

The two 768 MiB rows are retained as historical observations from older source,
not valid current configurations. The current machine constructor rejects a RAM
aperture that overlaps a decoded device. NOR begins at `0x28000000`, so SDRAM
starting at `0x08000000` has a maximum non-overlapping size of 512 MiB. The
allocation file also caps this volume layout at 512 MiB.

**The last column was the historical point.** In those older runs both `-V` rows
reached `BSD root: md0` and then went idle without reaching
`_load_init_program`, while the documented 512 MiB run reached it at about 225 M
instructions and execed launchd. A `gVirtBase` below the kernel's compiled-in
`VM_MIN_KERNEL_ADDRESS` (`0xc0000000`) was already unusable; current source also
rejects the overlapping 768 MiB physical map before boot. The old 59-versus-312
MiB comparison illustrates the memory pressure that motivated `-Y`, not an
available current configuration.

---

## Stage 9 — current snapshot-resume frontier

Two interpreter changes make the current instruction counts different from the
older narrative without making them less meaningful.

First, XNU's exact ARM1176 wait-for-interrupt instruction no longer retires in a
host loop while nothing happens. The CP15 WFI callback advances the timer and
CLCD only to the earliest enabled VIC edge that can wake the processor, while
the CPU's retired-instruction count remains unchanged. If no future event is
known it falls back safely. Snapshot triggers therefore remain absolute retired
instruction positions rather than a mixture of work and fabricated idle spins.

Second, the next exact user-mode stop after VFP was decoded rather than patched
as a one-off:

```text
pc       0x33dba604
encoding 0xe1630381
decode   SMULBB r3, r1, r3
```

That belongs to the ARMv5TE signed DSP multiply group. Current source implements
the full related family — `SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and `SMLAWy`
— including top/bottom half selection, signed word-by-halfword truncation,
sticky-Q overflow behavior, legal aliases and fail-closed invalid forms. This
cleared `0xe1630381`; it was not replaced with a hard-coded result.

**Measured current continuation:** the first three intervals below ended at
their configured cap. The fourth stopped fail-closed on the named user-mode
instruction rather than guessing its semantics; the fifth replayed through the
fix to its cap. `_panic` and `Debugger` were not reached in any interval.

| restore → cap | checkpoint written | new evidence | free-page report |
|---|---|---|---|
| 2.2 B → 2.45 B | 2.4 B | crossed `0xe1630381`; `launchd` and `mDNSResponder` alive | 2,004 pages / 7.83 MiB; low 1,999 |
| 2.4 B → 2.8 B | 2.7 B | one new `_execve`, first at 2,605,595,575; `systemShutdown false` | 542 pages / 2.12 MiB; low 539 |
| 2.7 B → 2.9 B | 2.85 B | no panic or undefined stop | 317 pages / 1.24 MiB; target 250; low 301 at 2,886,008,832 |
| 2.85 B → 2,944,340,624 | none | stopped on `0xe6cf3073`, ARMv6 `UXTB16 r3, r3`, in user mode | 253 pages / 0.99 MiB; target 250; low 97 at 2,934,505,472 |
| 2.85 B → 2.98 B, paired-extend fix | 2.97 B | cleared `0xe6cf3073`; status `OK`; 2 `_load_machfile`, 400 code validations, 4,266 SWIs, 3,373 Unix syscalls | 214 pages / 0.84 MiB; target 250; low 97 at 2,934,505,472 |

The 58.93 MiB figure above is the earlier post-layout pool, not the amount left
after this much userspace activity. Direct streaming removed a second host-side
copy but the roughly 445 MiB RAM disk remains pinned guest memory below
`topOfKernelData`. The latest run went 153 pages below the guest's free target,
recovered to three pages above it before the former opcode stop, then ended 36
pages below target at 2.98 B. That movement suggests XNU's reclamation path is
active, but the available headroom is still unsafe for the app. The storage
audit also proved that setting md physical mode and adding an external bus
aperture is insufficient: this kernel's `_bcopy_phys` only applies the normal
DRAM direct-map delta. The narrowly scoped writable md-strategy bridge, its
locked file adapter, an exact full-image/ARMv6/LC_UUID/site-gated 7E18 patch
manifest, and a bounded immutable-source HFS work-image provisioner now exist
under unit tests. `bootkernel --external-md` now installs the cold-boot chain:
it exact-gates the original kernel, device tree, and rootfs; creates a no-replace
writable work image; publishes md0 through a synthetic address outside the
128 MiB DRAM aperture; and installs only the two audited strategy-copy exits.
### 2026-07-22: first 128 MiB external-md real-firmware run

Commit `d9d9e40` was run cold to a 400,000,000 retired-instruction cap with the
documented exact 7E18 inputs and default 32 MiB growth. All three identity gates
passed, and the create-only work image was 466,825,216 bytes with SHA-256
`4fb9b51eaca0f52fdba8d2a7909b57eab7e8d5c6e67112f277f501a8af76cc61`.
The immutable source hashes were identical before and after the run.

The guest reported `BSD root: md0, major 2, minor 0`, first entered
`_load_init_program` at 235,856,815, first entered `_execve` at 235,888,017,
and printed `*** launchd[1] has started up. ***` followed by
`Running fsck on the boot volume...`. At the cap the machine reported status
`OK`: no `_panic`, `_Debugger`, raw-mdevrw guard, undefined emulator stop, or
bridge failure. The bridge completed 6,695 reads (27,397,632 bytes), zero writes,
and zero failures. The zero writes mean the write exit is still unit-tested only;
the bounded run ended just after fsck began.

The memory hypothesis is now measured. The guest advertised 128 MiB, began with
a 120.14 MiB post-layout free pool, and ended with 21,826 free pages (85.26 MiB),
well above its 406-page target. The last live host sample used roughly 62 MiB of
resident memory. This is not directly comparable to the later 2.98 B direct-RAM
age, but it removes the old 445 MiB static guest allocation and avoids the former
near-zero headroom at the same architectural boundary. Snapshot backing
identity/overlay state remains future work, and full NAND is the
higher-fidelity, much larger route.

### 2026-07-22: exact first raw `/dev/rmd0` boundary

A second create-only cold run extended the same guarded strategy path until the
first raw-character read. It stopped intentionally before executing `_mdevrw`
at **402,741,536** retired instructions, with no panic, undefined instruction,
or bridge failure. At the boundary:

```text
pc  c0073f94  _mdevrw entry
lr  c009920d  _spec_read+0x118
r0  09000000  /dev/rmd0
r1  ea967ef0  struct uio *
r2  00000000
```

The exact XNU32 `uio` held one 32 KiB iovec, offset zero, read direction,
segment 5, and residual `0x8000`. Before that call the strategy bridge completed
6,715 reads (27,479,552 bytes), zero writes, and zero failures. The guest had
21,187 free pages (82.76 MiB); the run's low was 21,186 pages. The immutable
firmware inputs and create-only work image were unchanged by the guard stop.

Commit `b0ec58c` replaced the audited `_mdevrw` prologue with
`svc #0xe3; bx lr` under the same exact 7E18 transaction and installed the first
bounded raw-uio bridge. Host tests covered reads, writes, XNU partial-iovec
updates, all user segment variants, the `0xc0000000` user ceiling, TTBR0/TTBR1,
legacy 1 KiB AP subpages, malformed metadata, aliases, and partial backend
failures. The next two cold runs tested that implementation rather than
assuming those host-only mappings were enough.

### 2026-07-23: run03 crossed the raw guard but fsck failed

Run03 reached `launchd` and fsck and continued to the configured
**420,000,000** retired-instruction cap, so the old `_mdevrw` guard was no longer
the boundary. The guest nevertheless reported that `/dev/rmd0 (hfs)` exited
with signal 8 (`SIGFPE`), and fsck did not complete. This was progress past the
old stop, not a successful boot: no SpringBoard frame was captured.

### 2026-07-23: run04 isolated both raw-I/O mismatches

Run04 added a per-request diagnostic and reproduced the fsck path through
405,000,000 instructions. The first failure was the offset-zero raw read:

```text
seg=5  rw=0  resid=32768  offset=0
fault=0x01001000/pa=0x00000000/fsr=0x00000807
```

This is a read from `/dev/rmd0`, but it requires a write into the user buffer.
The first page was resident and the next page needed XNU's native write-side
demand-page/COW handling; host-side page-table inspection cannot manufacture
that fault correctly.

The second failure was another 32 KiB read:

```text
offset=0x1bd30000  resid=32768  media_end=0x1bd33000
```

Only 12 KiB is inside the work image; the remaining 20 KiB is in the adjacent
allocation tail. The same two request shapes recurred in fsck's `-p` and `-fy`
passes. The closest public XNU `_mdevrw` has no logical EOF bounds check and
calls `uiomove64` once, so treating this request as an immediate `EINVAL` was
also incompatible.

The correction changes the exact four-byte patch to
`svc #0xe3; svc #0xe4` and adds `ARM_SVC_REDIRECTED`. Resident requests can
still use the bounded direct path. A translation fault redirects to the exact
Thumb `_uiomove64` entry at `0xc0128d14`, backed by one of four 128 KiB slots
keyed by the entry kernel SP and reserved below `topOfKernelData`; native XNU
then handles demand paging/COW and returns through the second SVC. A
zero-initialized, coherent 128 KiB in-memory tail preserves write-then-read
behavior beyond the media without growing either the immutable rootfs or its
work image.

### 2026-07-23: run05 cleared raw I/O and progressed through fsck

Run05 used a fresh work image and reached its **430,000,000** retired-instruction
cap with exit status 0. The serial sequence included `launchd`, then:

```text
Running fsck on the boot volume...
/dev/md0 on / (hfs, local, noatime)
```

The raw bridge completed two reads and no writes. Both reads took the native
path: two redirects, two checked completions, zero pending continuations, and
zero raw guest errors. Of the 65,536 raw bytes, 45,056 came from media and
20,480 came from the coherent guard tail. The aggregate external-md counters
were 6,901 reads (28,295,168 bytes), one 512-byte write, and zero failures;
6,899 reads and the write used the strategy bridge.

The lowest observed free-page count was 20,820 pages (81.33 MiB) at instruction
425,852,928. `_execve` recorded 11 hits and `_load_machfile` recorded 6. The
work image remained exactly 466,825,216 bytes. The authenticated kernel, device
tree, and rootfs hashes were unchanged: exact kernel patches still touched only
the loaded guest-RAM copy, and filesystem edits remained confined to the
separate work image.

The latest matching hosted checks at `ea92fca` also completed successfully:
[`core-tests` run 30009684129](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30009684129)
and
[`ios-build` run 30009684054](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30009684054).
Those workflows validate the public build and tests; run05 through run08 are
the separate private-firmware runtime evidence.

### 2026-07-23: run06 sustained the cold path to 1 B

Run06 used another fresh work image and extended the external-md cold path to
its **1,000,000,000** retired-instruction cap with exit status 0 and empty
stderr. It retained the launchd, fsck, and root-mount sequence from run05, then
printed:

```text
mDNSResponder[14] syscall_builtin_profile: mDNSResponder (seatbelt)
mDNSResponder[14] Builtin profile: mDNSResponder (seatbelt)
systemShutdown false
```

The external bridge completed 10,004 reads (40,994,304 bytes), 27 writes
(107,008 bytes), and zero failures. Strategy I/O accounted for 10,002 reads and
all 27 writes. Raw I/O remained two reads and no writes, with zero guest errors:
two native redirects, two checked completions, and zero pending continuations.
Those raw reads again split into 45,056 media bytes and 20,480 coherent-guard
bytes.

The lowest observed free-page count was 17,221 pages (67.27 MiB) at instruction
980,615,168. `_execve` remained at 11 hits while `_load_machfile` advanced to
25. The work image stayed exactly 466,825,216 bytes and the authenticated
kernel, device tree, and rootfs hashes remained unchanged.

### 2026-07-23: run07 reached a clean 2 B in userspace

Run07 used a fresh 128 MiB external-md work image and reached its
**2,000,000,000** retired-instruction cap. The exit file contained 0, stdout was
234,838 bytes, and stderr was empty. The serial record retained `launchd`, the
fsck and `/dev/md0` root-mount sequence, both `mDNSResponder[14]` Seatbelt
lines, and `systemShutdown false`.

The final PC was `0x3145ad4c` in USR mode with `CPSR 0x20000010`.
731,259,769 instructions, 36.6% of the run, retired in USR mode. The reached
probes were:

```text
_execve                    12
_load_machfile             32
_thread_bootstrap_return   92620
_unix_syscall              58166
```

The external bridge completed 12,782 reads (52,372,992 bytes), 82 writes
(325,120 bytes), and zero failures. Strategy I/O accounted for 12,780 reads and
all 82 writes. Raw I/O remained two reads and no writes, with zero guest errors:
two native redirects, two checked completions, and zero pending continuations.
Those raw reads consumed 45,056 media bytes and 20,480 coherent-guard bytes;
neither the raw media path nor the guard recorded a write.

The run ended with 13,000 free pages (50.78 MiB). Its low was 12,983 pages
(50.71 MiB) at instruction 1,836,056,576. The work image stayed exactly
466,825,216 bytes, and the source kernel, device-tree, and rootfs hashes were
unchanged.

This run cannot answer the display question. The framebuffer was disabled, and
CLCD status, interrupt mask, and scanning were all zero. Therefore run07 is
absolutely not evidence that SpringBoard started or that the real display path
works.

### 2026-07-23: corrected the CLCD handoff before a display run

The model had assigned the wrong meaning to one saved-register range.
`0x0d8..0x0ec` was labeled as panel timing, but openiBoot's S5L8900 LCD setup
uses those words as per-window auxiliary configuration pairs; `0x0e8` is also
the update word used by `AppleH1CLCD`. The actual timing registers are
`VIDTCON0..3` at `0x20c..0x218`.

The N82 seed now represents an iBoot-compatible 320x480 handoff:

```text
VIDCON0   00000441
VIDCON1   00000008
VIDTCON0  00030303
VIDTCON1  000e0e0f
VIDTCON2  013f01df
VIDTCON3  00000001
```

Those production values select the 54 MHz display clock divided by five,
enable scanout, preserve inverted-VCLK polarity, encode the N82 porch/sync
timing and 320x480 active area, and supply the final handoff word. `VIDTCON2`
is not globally fixed: it is derived from the requested geometry, and the
production 320x480 request yields `0x013f01df`. The initial `0x0d8`, `0x0e0`,
and `0x0e8` window-configuration words are `0x00001000`, with their paired
auxiliary words zero.

The model also no longer equates an enabled window with a running controller.
Frames and CLCD-derived wake events advance only when start state, `CLCD_CTRL`
global enable, and `VIDCON0` bit 0 are all active; host publication observes
the same invariant.

Run08 subsequently exercised this corrected seed. The result below is a
display-path diagnostic, not SpringBoard proof.

### 2026-07-23: run08 executed display bundles without CLCD MMIO

Run08 used a fresh external-md work image, 128 MiB of guest RAM, and
`-F -H 0x38900000` for framebuffer seeding plus exact CLCD-page tracing. The harness reached its
**600,000,000** retired-instruction cap and reported `stopped ... OK`. Final PC
was `0xc017056c` (`_SHA1Init+0xc4`) and stderr was empty. The host wrapper's
exit-marker file was accidentally empty, so this run has no captured OS process
exit status.

Exact instruction-entry PC coverage reported:

```text
AppleH1DisplayDrivers  hits 675  first 126211220  last 201032245
AppleMerlotLCD         hits 409  first 209372737  last 211410011
CLCD MMIO page         0 accesses
```

The first two lines prove only that the CPU reached PCs inside both executable
bundle ranges. They do not prove retirement or that `AppleH1CLCD::start`
completed. With no guest access to the CLCD page, seeded configuration remained
intact while guest-time ticking advanced IRQ status and the frame counter:

```text
irq-status 1  mask 0  scanning 1  CLCD_CTRL 0x41
VIDCON0 0x441  VIDCON1 0x8  active window 0  running 1  frames 386
```

The capture was nonblack only technically: 128 white pixels formed one 8x16
block at the top-left, every other pixel was black, and only 384 RGB bytes were
nonzero. This is neither recognizable UI nor evidence of guest-driven scanout.

The lifecycle ring retained 70 events with zero pathname-copy failures.
`launchd`, fsck, and the `/dev/md0` root mount were present; service spawns
progressed through `/usr/sbin/notifyd` at instruction 586,776,479. Exact
SpringBoard path attempts were zero. User mode retired 44,274,420 instructions
(7.4%), and free pages reached a low of 19,260 (75.23 MiB).

The external bridge completed 8,059 reads (33,034,752 bytes), 16 writes
(61,952 bytes), and zero failures. Raw I/O completed two redirects and two
completions with zero pending continuations and zero guest errors. The source
kernel, device-tree, and rootfs hashes remained unchanged.

The correct conclusion is narrow: the CPU reached PCs inside both bundle ranges
and the corrected seed survived. There is no proof of instruction retirement,
a successful `AppleH1CLCD` start, SpringBoard, or a useful display path. Zero
MMIO is important evidence, but it does not alone identify the exact blocker.
Run09 supplied the longer bounded lifecycle/display run described next.

### 2026-07-23: run09 reached the SpringBoard launch request, not SpringBoard

Run09 used a fresh display-enabled 128 MiB external-md work image and ran to a
**2,000,000,000** retired-instruction cap. The harness reported
`stopped ... OK` and stderr was empty. Its wrapper did not provide an OS process
exit marker, so this record does not claim a host exit code. User mode retired
729,934,906 instructions (36.5%). The free-page low was 12,976 pages
(50.69 MiB) at instruction 1,829,371,904.

The process-lifecycle ring retained 120 events. At instruction 635,280,837 it
recorded one exact stock SpringBoard pathname in a `posix_spawn` attempt. This
is a launch request only: the current probe does not establish the syscall
return value, a child process, SpringBoard instruction execution, or a frame.
One separate, later pathname-copy operation failed; it was unrelated to the
captured SpringBoard pathname.

The longer interval did not establish a guest-driven display path:

```text
AppleH1DisplayDrivers  hits 687  first 126211220  last 1571737384
AppleMerlotLCD         hits 409  first 209372737  last 211410011
SPI0                   13 early platform writes
CLCD MMIO page         0 recorded accesses
```

Only six late two-instruction callbacks account for the H1 range's extension
past run08. Merlot made no later entry observation. SPI0 traffic never advanced
beyond its 13 early platform writes, so there is no observed Merlot panel
transaction. Seeded scanout remained live and reached 589 frames, but the final
PPM was byte-identical to run08: exactly 128 white pixels in one 8x16 block at
the top-left and every other pixel black.

The external bridge completed 12,798 reads (52,438,528 bytes), 82 writes
(325,120 bytes), and zero failures. The source kernel, device-tree, and rootfs
hashes remained unchanged. Run09 is therefore a meaningful move from “no
SpringBoard pathname seen” to “launchd requested the exact stock pathname.”
It is not evidence that SpringBoard reached user mode or rendered. The next
diagnostic must capture the spawn return/outcome and any child lifetime while
retaining display evidence.

### 2026-07-24: run11 exposed the SETEXEC launch shape

Run11 used direct OS-level stdout/stderr files so the report survived wrapper
timeouts. It reached its 700,000,000-instruction cap with `OK` and empty stderr,
repeated the exact SpringBoard `posix_spawn` request at 635,280,837, and then
recorded BTServer at 637,448,889. The external image completed 8,754 reads and
24 writes with zero failures. The PPM remained byte-identical to run09's seeded
8x16 white block on black.

The old raw-return probe remained pending and the vfork child probe saw no
`_thread_resume`. Matching-era launchd forks once per job, sets
`POSIX_SPAWN_SETEXEC`, and calls `posix_spawn` with no PID output; run11's 19
forks followed by 19 service spawns strongly predict the same shape. Exact
disassembly of the shipped xnu-1357.5.30 kernel confirms what flag `0x0040`
would do: bypass `_vfork`/`_vfork_return`/`_thread_resume`, exec-replace the
launchd child on success, and return an errno to the old wrapper on failure.
Run11 did not read the attribute flag, so the two absences remain neither
failure nor success evidence in that run; run15 later confirmed SETEXEC.

Run11 also recorded a later `_exit1(proc=e0381ca8)`, but its older trace did not
capture the SpringBoard and BTServer entry proc/PID identities. The exit is
therefore deliberately unattributed.

The probe added after run11 closed that evidence gap without changing guest behavior. It
decodes the 32-bit spawn descriptor and flag `0x0040` from guest memory,
requires the exact `exec_activate_image` and `_load_machfile` path with no
vfork-family hit, and samples `r0` at the exact shipped-kernel
`_posix_spawn` result epilogue. After `r0=0`, it requires a successfully stepped
user instruction with the same re-walked task, uthread, proc, and PID. Demand
fetch faults and transiently unreadable identity defer the claim; the first
attributed `_exit1` closes lifetime tracking.

### 2026-07-24: run15 proved stock SpringBoard application-code execution

Run15 populated the exact probe in a fresh display-enabled 128 MiB external-md
cold boot. It reached 2,000,000,000 retired instructions with harness status
`OK` and empty stderr. The SpringBoard request at 635,280,837 carried live flag
`0x0040` (`POSIX_SPAWN_SETEXEC`); exact `exec_activate_image` and
`_load_machfile` phases followed, with no vfork-family phase, and the exact
shipped-kernel result epilogue returned `r0=0` at 636,108,374. The first
identity-validated replacement-process instruction retired at 636,114,681.

The trace revalidated task/proc/PID `c2d8c000/e03820c0/20` and target
thread/uthread `e0324aa8/c0b9b130`. Through instruction 1,851,355,734 that
address-space key accumulated 37,134,545 committed user instructions:
10,039,939 in dyld, 27,093,301 in the shared cache, and 1,305 in the low image.
The first low-image instruction was PC `0x000034e8` at 1,519,973,164.

A read-only HFSX walk resolves the stock path
`/System/Library/CoreServices/SpringBoard.app/SpringBoard` to rootfs offset
`0x0db8f000`, logical size `0x123d30`, and SHA-256
`b2ca875c968da1917ffe577b708c730c8e59ddfaa3c94efd5510fa57b2d1539d`.
Its 32-bit ARMv6 Mach-O names `0x34e8` both as `LC_UNIXTHREAD.pc` and exported
`start`; all 291 embedded SHA-1 code-page hashes verify. Later run15 PCs resolve
through the image's Objective-C metadata to `SBTetherController` methods,
including code referenced by SpringBoard lifecycle handling. The last low PC
`0xdc068` is only the common dyld lazy-binding helper, not a crash site.

The exact process took 882 traced traps and never entered `_exit1`. At the cap,
its target thread was switched out with an open, validated `mach_msg` SVC
episode; that is a normal blocking state, not evidence of death. The external
bridge completed 12,798 reads (52,438,528 bytes), 82 writes (325,120 bytes),
and zero failures, including two checked raw redirects and completions. Source
kernel, device-tree, and rootfs hashes remained unchanged.

The display boundary did not move with process execution. There were zero
exact-process or live-scanout framebuffer mutations, no guest-driven CLCD
handoff, and the captured PPM remained the seed-only 8x16 white block on black.
Run15 therefore proves stock SpringBoard executable entry and subsequent
SpringBoard application-code execution. It does not prove `UIApplicationMain`
was directly observed, UI readiness, or rendering.

### 2026-07-24: run16 completed the PMU/I2C and display-driver startup path

Run16 exercised the new S5L I2C/PCF50635 model from commit `3963d22` in a fresh
display-enabled 128 MiB external-md cold boot capped at
**250,000,000** retired instructions. The harness reported `OK`, the host
process exit status was 0, and stderr was empty. This was deliberately an early
hardware-startup run: no user-mode instruction retired before the cap, so it
cannot by itself say anything about the later SpringBoard process.

The exact PMU checkpoints closed the earlier GPIO-property uncertainty.
`InterruptControllerName` returned the non-null object `r0=0xc0ac1980`; the
PMU start-failure PC was never reached, while the pre-I2C parent-publication
checkpoint and first I2C call were both reached. The controller then entered
and completed its wait path 44 times. Its final live counters were:

```text
i2c0  starts 57  tx 88  rx 17  NAK 2
```

The PCF50635-backed serial path proceeded through the PMU's converter reads and
reported `DOWN0`, `DOWN1`, and `DOWN2` at 625 mV. These are live guest-driver
transactions, not injected console strings, but they are not accurate regulator
values: 13 of 17 register reads targeted currently unknown registers, whose
deterministic zero placeholders produce 625 mV in the guest's conversion.

Offline control-flow correlation of retained instruction-entry observations
established that both Merlot `start` calls returned true and
`AppleH1CLCD::start_hardware` returned true. The wider trace recorded 10,803 H1
observations and 948 Merlot observations. Unlike run15, the guest also exercised
the CLCD page: 795 reads and 32 writes, ending with `CLCD_CTRL=0x01110041` and
interrupt mask `0x00003f01`.

These successful observed return paths are not a rendered SpringBoard. The
framebuffer capture remained the seed-only 8x16 white block on black, and the
run ended before user mode. The panel ID consumed by Merlot (`0x00a5c22b`) was
supplied by the synthesized `-F` device-tree handoff; it is not evidence of
panel discovery. The run also retained the existing IORTC wait patch in the
loaded guest-RAM kernel copy. PMU progress is encouraging IORTC-path evidence,
but it does not prove an unpatched IORTC registration or justify removing that
patch. No supported CLI switch currently disables only IORTC; `-K` disables the
entire patch table and is rejected by external-md, so that experiment first
needs a one-patch diagnostic option or a clearly identified targeted build.

The exact-gated source kernel, device tree, and rootfs remained untouched;
runtime patches affected only guest RAM and filesystem writes remained in the
fresh work image. Run17 below performed the full 2 B experiment that combined
this display startup with identity-validated stock SpringBoard execution.

### 2026-07-24: run17 reached UIKit's local window-server framebuffer path

Run17 exercised commit `0bc18ea` in a fresh display-enabled 128 MiB
external-md cold boot capped at **2,000,000,000** retired instructions. The
harness stopped `OK`, the host process exit status was 0, and stderr was empty.
The SpringBoard pathname appeared at instruction 608,801,884 with
`POSIX_SPAWN_SETEXEC`; the exact shipped-kernel result epilogue returned `r0=0`
at 609,608,299, and the first revalidated replacement-process instruction
retired at 609,722,091.

The trace bound task/proc/PID `c2e06000/e03820c0/20` to 36,379,165 committed
user instructions through 1,873,358,082: 10,021,910 in dyld, 26,356,193 in the
shared cache, and 1,062 in the low image. It recorded 840 exact-identity traps
and no exact-process `_exit1`. The first low-image instruction was the stock
SpringBoard entry `0x34e8` at 1,463,032,885.

The 1,062 low-image instructions are not evidence that SpringBoard stalled in
its executable. Import and instruction resolution identifies the stock BLX at
`0x381e` as the `UIApplicationMain` call, and the exact trace retained its
transition from post-step PC `0x3820` into stub `0xb1208` at 1,828,280,095.
`UIApplicationMain` normally does not return; UIKit and the other shared-cache
frameworks therefore account for most later execution.

Only two later callbacks entered the SpringBoard image:
`+[SpringBoard registerForSystemEvents]` at `0x3940` and
`+[SpringBoard rendersLocally]` at `0x3944`. Both are two-instruction methods
that returned true. Static shared-cache resolution places those callbacks in
`UIApplicationMain` and
`+[UIApplication _startWindowServerIfNecessary]`. After `rendersLocally`,
UIKit obtains the local CAWindowServer, configures renderer flags, enumerates
its displays, and queries the first display's bounds.

Run17 reached that path. Its last retained user exception before the final IOKit
messages was at `0x3110d024`,
`IOMobileFramebufferGetDisplaySize+0x18`, called from
`CA::WindowServer::IOMFBDisplay::update_framebuffer+0xbc` in QuartzCore. The
instruction is `vldr s15, [r0, #0xa8]`; the undefined-instruction exception was
the ordinary lazy VFP-enable trap. It returned to the same instruction and is
not evidence of a crash or bad memory access. The framebuffer routine then uses
`IOConnectCallScalarMethod` selector 8 with two scalar outputs.

Several immediately following `mach_msg` episodes returned transport result
`r0=0`. The final exact-target episode began at 1,873,358,082 with message ID
2816 and switched the target out at 1,873,362,063. Apple's IOKit MIG definition
maps ID 2816 to `io_service_close`; its 24-byte request and 44-byte reply sizes
also match the observed arguments. H1 driver code was observed during the open
episode, whose kernel path reached `_wait_queue_assert_wait` and
`_thread_block_reason`, but the task-local remote port `0x3e07` does not by
itself identify the service. The request remained unresolved through the 2 B
cap. That is a strong reason to probe the H1 framebuffer user-client lifecycle;
it is not yet proof of a deadlock or of which object was being closed.

SpringBoard's `applicationDidFinishLaunching:` implementation at `0xa6f4` was
not reached. Neither the exact SBTetherController scalar-call site nor its
post-call continuation retired. The earlier tether hypothesis is therefore
excluded as the blocker encountered by run17: this execution had not yet
entered the delegate method that initiates it.

The hardware-side path remained active. I2C0 recorded 109 starts,
`AppleH1DisplayDrivers` accumulated 19,562 observations through the terminal
episode, and `AppleMerlotLCD` accumulated 1,164. The CLCD page received 797
reads and 34 writes and ended scanning with `CLCD_CTRL=0x01110041`, but the
exact-process and general live-scanout mutation counts were both zero. The final
PPM is byte-identical to run16's seed-only frame.

The console warning `IOSurface: buffer allocation size is zero` is relevant but
not temporally tied to SpringBoard's late wait: it also appears in run16 before
any userspace instruction. Treat it as an earlier H1/IOSurface startup clue,
not proof that a particular late surface allocation or close failed.

The next trace should preserve the exact process identity while recording every
IOMobileFramebuffer `io_service_open`, scalar selector, return IOReturn, and
`io_service_close`, with task-local port provenance. It should narrowly trace
the H1 client-close and reply path and capture the IOSurface width, height,
bytes-per-row, allocation size, caller, and return at the zero-size warning. A
device-model fix belongs after that trace distinguishes a missing reply or
invalid surface contract from an ordinary process wait; forcing `mach_msg` to
return or fabricating framebuffer pixels would destroy the evidence.

### 2026-07-24: run18 proved the optional TV-out swap/close dependency

Run18 exercised the new exact UI, Mach, IOMFB, and late H1 checkpoints from
commit `9bab56c05514f6cd3dbb5fa96b229a0c7b5146a0`. It was a fresh display-enabled
128 MiB external-md cold boot with profile window
`1,750,000,000..2,500,000,000`. The harness stopped normally at the
**2,500,000,000**-instruction cap with `OK`; stderr was empty, `_panic` and
`_Debugger` were not reached, and the external bridge completed 12,015 reads
and 173 writes with zero failures.

The manifest recorded the same exact-gated original inputs still present in
`firmware/`:

```text
kernel.macho    0d8cdb339d37cf37a1db2638fff79272ecd63a17764bf7666efa1618725df70c
devicetree.bin  4867c95fedf544bda2ecaa2626ae14c01a60d7771dc53ffe6fd3a6aac8b8ba57
rootfs.img      c3251e7f092c939d5818e92086cb47680981cfb03731de7b55d238c942eb5e82
```

The source firmware was not rewritten. Kernel compatibility patches and
iBoot-style device-tree edits existed only in the guest-RAM copies. Rootfs
growth, fstab changes, and guest writes were confined to the fresh
466,825,216-byte run18 work image.

The SpringBoard timing through the display boundary was deterministic. SETEXEC
returned `r0=0` at 609,608,299; the first revalidated replacement instruction
retired at 609,722,091. The exact process called `UIApplicationMain` at
1,828,280,094, called `registerForSystemEvents`, returned
`rendersLocally == YES`, and entered CAWindowServer display detection. The
primary H1CLCD IOMFB open, geometry update, layer-surface lookup and constructor
all returned; QuartzCore's first display server construction also returned.

Display detection then opened a second IOMFB object. Run18 observed its 720x480
geometry and TV-out setter calls. Static vtable and selector resolution identify
that optional object as `AppleH1TVOut`. Its selector-3 path never assigns the
generic IOMFB surface-ID field, so zero is the expected surface ID. The resulting
zero `CoreSurfaceBufferLookup` is not evidence that the primary 320x480 CLCD
surface failed.

The second object entered the exact IOMFB finalizer at 1,873,357,991 and called
`IOServiceClose` at 1,873,358,007. The resulting ID-2816 Mach episode reached
`_wait_queue_assert_wait` at 1,873,361,179 and switched the exact SpringBoard
thread out at 1,873,362,063. `IOServiceClose` did not return. Other guest
userspace continued to the normal 2.5 B cap, so this is a per-thread swap wait,
not a whole-emulator deadlock.

The shipped driver closes the causal chain. Closing with queued or active TV-out
work sleeps on the swap gate. The TV-out IRQ 30 filter/action clears that work
and wakes the same gate. The 7E18 device tree maps its control, mixer and SDO
registers to `0x39100000`, `0x39200000`, and `0x39300000`; run18 sent all three
pages to the unmapped path:

```text
0x39100000  86 reads / 201 writes
0x39200000  105 reads / 45 writes
0x39300000  94 reads / 181 writes
VIC0 line 30 enabled, raw/pending never asserted
```

Because the pages were unmapped, run18 could not retain their final control
state or generate SDO pending/IRQ 30. The contemporaneous interpretation that
all three bit-0 run states would be active was a hypothesis, not a measured
fact; run19 later disproved that aggregate-gate assumption. Missing TV-out
register/VSYNC/IRQ semantics still explain run18's exact close wait. They were
not proved to be the only remaining blocker after the thread wakes.

The initial post-run18 model provided byte-lane-safe storage for the three
pages, independent stopped/ready handshakes, SDO VSYNC pending as W1C with its
mask bit, and a 60 Hz level on VIC0 IRQ 30. It incorrectly treated every
bank's bit 0 as one aggregate timing gate. Run19 established that control `+0`
is conditional programming state and that the persistent timing eligibility is
mixer+SDO. The model must not fabricate an IOSurface, framebuffer, TV signal,
cable/hotplug state, IRQ 38, or pixels.

Run18 also exposed two independent safety issues outside that wait. Its
historical Boot_Video address `0x0ff6a000` lay above physical
`topOfKernelData 0x0885c000`, so XNU could treat the scanout pages as free.
Current planning instead reserves framebuffer
`0x0885c000..0x088f2000`, advances TOKD to the 16 KiB-aligned `0x088f4000`,
and requires `0x11000` bytes of bootstrap headroom. CLCD handoff validation now
checks AppleH1CLCD's page-rounded `stride * height` allocation and rejects
32-bit multiplication, rounding, or physical-end overflow without changing
controller state. Both changes postdate run18; run19 validated the corrected
placement and active CLCD window as recorded below, but not rendered pixels.

### 2026-07-24: run19 validated layout and exposed the TV-out timing-gate bug

Run19 was the first real-firmware boot of exact source commit
`afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9`. Its local preflight passed 3/3,
and the exact commit already had green hosted core run
[30088519878](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30088519878)
and unsigned-iOS run
[30088519892](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30088519892).
The fresh 128 MiB external-md boot ran from
`2026-07-24T11:12:00.8559935Z` to
`2026-07-24T11:30:23.3127833Z`, exited 0 at the
**2,500,000,000**-instruction cap with `OK`, and wrote 922,889 bytes to stdout
and zero bytes to stderr. The bridge reported zero failures and its raw-native
path completed two redirects and two completions with nothing pending.

The exact-gated original inputs were rehashed after the run and remained:

```text
kernel.macho    0d8cdb339d37cf37a1db2638fff79272ecd63a17764bf7666efa1618725df70c
devicetree.bin  4867c95fedf544bda2ecaa2626ae14c01a60d7771dc53ffe6fd3a6aac8b8ba57
rootfs.img      c3251e7f092c939d5818e92086cb47680981cfb03731de7b55d238c942eb5e82
```

The 466,825,216-byte work image was a fresh copy. Firmware compatibility edits
remained confined to loaded kernel/device-tree copies, and filesystem writes
remained in that work image. The corrected external-md layout was exact and
non-overlapping:

```text
boot_args       0x087db000
raw bounce      0x087dc000..0x0885c000
Boot_Video      0x0885c000..0x088f2000  (320x480x4)
topOfKernelData 0x088f4000
```

The SpringBoard path repeated rather than advanced:

```text
SETEXEC result epilogue, r0=0                         599,023,341
first identity-validated replacement instruction     599,119,560
SpringBoard UIApplicationMain call                 1,849,444,535
[SpringBoard rendersLocally] returns YES           1,869,087,332
QuartzCore detectDisplays entry                    1,870,899,597
primary QuartzCore new-server return               1,881,846,583
optional IOMFB finalizer                            1,887,341,013
IOServiceClose call                                 1,887,341,029
Mach episode 2305 begins, message ID 2816           1,887,341,104
wait_queue_assert_wait                              1,887,344,201
SpringBoard thread switches out                     1,887,345,137
```

From run18's finalizer through `_wait_queue_assert_wait`, every corresponding
checkpoint is exactly **13,983,022** instructions later in run19; the final
switch is 13,983,074 later. This is scheduling drift, not downstream progress.
`IOServiceClose` return, close-after-gated-work, the close epilogue,
`GSSetMainScreenInfo`, and `applicationDidFinishLaunching:` all remained at
zero hits. Generic kernel `io-service-close-return` hits belonged to other
requests; the exact ID-2816 correlation stayed open and unobserved.

The three TV-out pages were now mapped. Their final first-word state was
control/mixer/SDO `0/5/1`; SDO pending/mask was `0/0`. The model consequently
reported `running=0`, phase 0, zero TV-out frames, no raw IRQ 30, and zero
filter/action/completion-dispatch hits. The driver did not fail to receive an
IRQ while all three gates were active: the model's all-three condition never
became true.

Capstone disassembly resolves the discrepancy. Object `+0x200` is SDO,
`+0x204` is mixer, and `+0x208` is the control/coefficient block. Start writes
SDO `+0=1` and mixer `+0=5`; per-source programming writes control `+0=1` only
when a source exists and explicitly writes zero on the no-source path. The
shipped IRQ filter reads SDO and mixer state, not control `+0`. Control still
has a real independent shutdown handshake: it is written zero and polled for
ready bit 1. Run19 returned `0x2` immediately and eliminated run18's
`TVOUT SHUT DOWN PROBLEM` warnings. The surgical correction is therefore to
retain control ready semantics while making mixer+SDO, not all three banks,
the VSYNC timing predicate. The corrected local tree passes 23/23 Release
tests, with SoC 5,504/0 and snapshot 469/0. Exact correction commit `590d224`
also passed hosted
[core run 30091220128](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220128)
and [unsigned iOS run 30091220122](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220122).
Run20 supplied the fresh firmware result: the correction completed the exact
TV-out chain as recorded below. That runtime success is not a render claim.

CLCD independently validated the corrected primary-display handoff. Window 0
was active at framebuffer `0x0885c000`, 320x480, stride 1280, and the controller
was scanning/running with 662 frames. Rendering did not advance: the final
460,815-byte PPM had SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`
and was byte-identical to run18's seed. It contained 153,472 black pixels and
128 white pixels in the original 8x16 corner block, no other colors, zero
changed pixels, and zero observed live-scanout writes.

### 2026-07-24: run20 cleared TV-out and reached the launch delegate, not a frame

Run20 booted exact source commit
`590d2248af4d7e5e92ec7bbd1be079c3bb415542`, the hosted-green TV-out
correction. It did not reach the configured cap: the harness exited **9** at
**1,937,979,818** instructions after fail-closing on a user-mode instruction.
The external-md bridge reported zero failures. The guest-free low-water mark
was 13,250 pages (**51.76 MiB**) at instruction 1,937,571,840, and the retained
run directory measured **447.18 MiB on F:**.

The original source inputs were rehashed and remained:

```text
kernel.macho    0d8cdb339d37cf37a1db2638fff79272ecd63a17764bf7666efa1618725df70c
devicetree.bin  4867c95fedf544bda2ecaa2626ae14c01a60d7771dc53ffe6fd3a6aac8b8ba57
rootfs.img      c3251e7f092c939d5818e92086cb47680981cfb03731de7b55d238c942eb5e82
```

The TV-out correction worked under the shipped path. The model counted 4
frames; VIC0 IRQ 30 entered the shipped filter and action; the close sleep gate
returned; and the exact PID 20 user return carried `r0=0`:

```text
TV-out IRQ 30 filter entry                         1,894,168,651
TV-out IRQ action entry                            1,894,171,336
IOMFB close sleep-gate return                      1,894,175,066
IOMFB close epilogue                               1,915,251,328
IOServiceClose PID 20 user return, r0=0            1,915,263,517
UIKit startWindowServer return                     1,919,831,289
SpringBoard applicationDidFinishLaunching: entry  1,923,358,329
```

The primary display decoded as 320x480, stride 1280. SpringBoard advanced past
the old optional-TV-out wait into `applicationDidFinishLaunching:`, but its
`UIController` call had zero hits. The PPM remained byte-identical to the
run18/run19 seed: 153,472 black pixels, 128 white pixels in the original 8x16
block, no other colors, and **0 changed pixels**. The live observer also
recorded zero RGB-visible scanout writes. SpringBoard is therefore **not
rendered**.

The terminal word was `0xEE274B10` at userspace PC `0x33acca88`, resolved under
PID 20 to libm `_fmod+0x1a8`. The committed decoder rejected it as Advanced
SIMD, but VFP11 defines it as `FMDHR d7, r4`; a modern disassembler may print
`VMOV.32 d7[1], r4`. This is a VFPv2 transfer into the high word of `d7`, not
NEON.

The decoder correction handles the low/high 32-bit transfers for `d0..d15`
while preserving fail-closed NEON/reserved cases. The full local Release suite
passes 23/23; targeted binaries pass at VFP **452/0**, ARM **810/0**, and JIT
**347/0**, including architectural guest-Undefined handling for denied
user-mode FPEXC access.

### 2026-07-24: run21 cleared `_fmod`, reached the cap, and still drew no pixels

Run21 used exact source commit
`debec04ff9b0faa469d5ad2ee7d75d1bf3b53b1a`. It exited **0** at the configured
**2,500,000,000-instruction cap**, crossing the run20 failure coordinate by
**562,020,182 instructions**. The exact libm `_fmod+0x1a8`
`FMDHR` / `VMOV.32 d7[1], r4` path therefore ran successfully in the original
firmware. This validates the decoder fix on the reached firmware path; it does
not claim complete VFP support.

SpringBoard again entered `applicationDidFinishLaunching:` at
**1,923,358,329**. `-[SBTetherController isTethered]` returned from `0x967ba`
to `0xa72c` at **1,924,647,850** and took the false branch
`0xa730 -> 0xa74c`. SpringBoard continued through
`loadDebuggingAndDemoPrefs`, `_initLockButtonBearTrap`, and
`SBPlatformController`, so tether detection is not the current frontier.

The trace next reached `+[SBTelephonyManager sharedTelephonyManager]` at
**1,965,837,070** and entered `-init` at `0x28240`. The last exact-attributed
PID 20 user instruction occurred at **1,966,242,080**. Its thread switched out
at **1,966,246,193** within a shared-cache `mach_msg`, before return to
`0xa77d`, while the rest of the guest continued to the cap.

Post-run shared-cache resolution identifies
`_CTTelephonyCenterGetDefault` creating a CTServerConnection. Its bootstrap
lookup for the literal `com.apple.commcenter` succeeds and returns port name
**0x4f07**. The initial generated handshake enters at **0x30a1177c**, then
calls `mach_msg` at **0x30a117e0** with request ID **0x0054b557**, send size
**0x834**, and receive size **0x30**. SpringBoard blocks before the return at
**0x30a117e4**. The generated stub does not initialize `msgh_size` or
`reserved`, so the observed header size **6** is stale stock stack state, not
emulator corruption.

The destination and call boundary were therefore exact, while a successful
reply, deadlock, queue-full condition, and baseband causality remained
unproved. That requirement led to run22's saturated-queue result below; its
active owner, linked entries, service receiver, and baseband correlation are
the next trace boundary.

The rendering result remained negative. `UIController` had **0 hits**,
live scanout had **0 mutations**, and the PPM was byte-identical to the seed,
SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`,
with **0 changed pixels**. SpringBoard is **not rendered**.

The immutable inputs reverified unchanged:

```text
kernel.macho    0d8cdb339d37cf37a1db2638fff79272ecd63a17764bf7666efa1618725df70c
devicetree.bin  4867c95fedf544bda2ecaa2626ae14c01a60d7771dc53ffe6fd3a6aac8b8ba57
rootfs.img      c3251e7f092c939d5818e92086cb47680981cfb03731de7b55d238c942eb5e82
```

The external-md bridge reported **0 failures**, guest free memory bottomed at
**50.63 MiB**, and the retained run directory occupies **447.27 MiB on F:**.
Hosted
[core run 30095081111](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081111)
and
[unsigned iOS run 30095081184](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081184)
passed the exact `debec04` source used by run21.

The next two commits changed tests only and must not be treated as run21 source.
`0670ab8cbf6b9febbfe059b17ffdeb755ee0133a` passed hosted
[core run 30096115501](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30096115501)
and
[unsigned iOS run 30096115527](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30096115527),
with VFP **469/0**. Latest hosted `657e8d8f2f42d09c573a4012a618e0f896307bdf`
expands helper-sequence coverage to VFP **488/0 locally** and passed hosted
[core run 30097023293](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023293)
plus
[unsigned iOS run 30097023356](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023356).

### 2026-07-25: run22 proved the saturated CommCenter send path

Run22 used exact source commit
`40209b27cb10d01c552398ff918ee613c4908ed0`, with the copied executable pinned
by SHA-256
`D3B9C9BF543409C9EDC95C1A1233B000D0B6C42AF5C132B69B47C19207196614`.
The 128 MiB display-enabled cold run exited **0** at its
**2,100,000,000-instruction cap** with empty stderr.

The copied-in kernel request retained ID `0x0054b557` and destination port
object `0xc0d705a0`. At **1,966,245,348**, `_ipc_mqueue_send` received mqueue
`0xc0d705b8` with `msgcount=5`, `qlimit=5`, `seqno=0`, and
`fullwaiters=0`. The trace then recorded the full-queue and pre-store
`fullwaiters=1` PCs at **1,966,245,373** and **1,966,245,387**,
`_thread_block_reason` at **1,966,245,550**, and the SpringBoard switch-out at
**1,966,246,193**. The sender did not resume before the cap.

This establishes a saturated queue at entry and a later blocked sender. The
old route recorder omitted `r8`, however, so the two exact PCs are adjacent
candidates rather than a fail-closed binding to the same kmsg. It also does
not establish five linked messages because `msgcount` counts reserved/in-flight
slots. The committed report's PID-1 owner is not authoritative: it interpreted
the port's `+0x3c` union without first validating `ip_receiver_name`, so an
in-transit destination or inactive timestamp could be mistaken for a receiver
space. CommCenter's last scheduled PID 24 worker repeatedly issued Mach ID
`1000` before a semaphore wait, consistent with a periodic
`clock_get_time` worker. Its destination was not resolved and the old counter
did not require SEND, so that is a candidate, not proof about the service
receiver.

The hardened next probe therefore requires the active receiver-name
discriminator, validates the copied-in kernel header, walks reciprocal mqueue
links, retains wait state per CommCenter thread, and correlates the exact
AppleBaseband reset-detect notification route before changing hardware
behavior.

SpringBoard remained unrendered: zero `UIController` hits, zero live-scanout
mutations, and zero changed pixels. The PPM retained seed SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`.
Immutable input hashes remained unchanged, external-md failures were zero,
guest-free memory bottomed at **50.63 MiB**, and the retained directory
occupies **447.42 MiB on F:**. Hosted
[core run 30106957804](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30106957804)
passed all eight jobs for the exact source.

### 2026-07-25: pre-Run23 probes passed their exact startup gates

This is a tooling checkpoint, not a boot result. Exact diagnostic commit
`5a40c5eec5bbf7c4b7d8909d0c1f364bc078338a` carries trace-only observers that
make run22's missing distinctions explicit:
authoritative receive-right ownership versus the port union; linked circular
kmsgs versus reserved/in-flight `msgcount`; exact route kmsg binding;
per-thread trap/semaphore/block/schedule episodes; and one nested-frame-safe,
live-port AppleBaseband reset-to-CommCenter notification chain.

The wait episode wording is deliberately bounded: an ordered block and
switch-out with no later execution proves “no resume observed.” It does not yet
prove that an off-CPU thread remained on its wait queue at the final cap,
because another thread could wake it without it being scheduled again.

The observers fail closed on unreadable or contradictory fields, incomplete
queue topology, mismatched route registers, resumed or reused threads,
sequence/ring overflow, restored history, notifier teardown, and stale port
pointers. They read guest state only: they do not toggle reset GPIO, change
SPI/baseband behavior, fabricate a message, edit a queue, or alter the source
firmware.

A final integration audit found that the first AppleBaseband draft could join a
linked handler from one retained reset event to an unlinked send/route from
another through aggregate counters. It also found repeated-send inheritance and
a later failed candidate overwriting displayed bound-kmsg evidence. The final
observer requires one still-retained matching dispatch with `messageClients`
evidence, rejects repeated sends, captures candidates locally, and includes
negative startup self-checks for all three cases.

With all mutable compiler state on F:, the integrated tree passed strict GCC
syntax, the `bootkernel` target build, whitespace validation, adversarial
startup self-checks, and an exact 7E18 zero-instruction run. That run printed:

```text
CommCenter ownership probe: VALIDATED
CommCenter wait probe: VALIDATED
AppleBaseband reset/notification probe: VALIDATED
stopped after 0 instructions: OK
```

The abbreviated lines above omit the report's decoded offsets and causal path
description; they do not imply that any runtime event occurred. Run23 must
cold-replay the exact committed binary to the SpringBoard frontier before
these probes can answer the ownership, service-thread, or baseband questions.

The same exact commit passed every job in hosted
[core run 30143448600](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143448600)
and the unsigned-package job in explicitly dispatched
[iOS run 30143455036](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143455036).
Those results validate public build/test/package behavior only. Run23 was not
launched during this handoff.

The earlier checkpoint-continuation chain is stronger evidence for sustained
userspace and snapshot
repeatability. It is **not** evidence that SpringBoard rendered. The bounded
continuations either reached their configured caps or, for the one diagnostic
interval, stopped fail-closed on the named `UXTB16` encoding.

### 2026-07-25: run23 bound the route, walked the queue, and named launchd

Run23 is the exact cold replay the pre-Run23 probes were built for. It used
source commit `777afb4c2350690ecd40cd9e69d12e3967a227cb` — a docs-only
descendant of hosted-green `5a40c5e` whose `CMakeLists.txt`, `core/`, and
`tools/` inputs are byte-identical to it — through the tracked
`tools/run23-cold-replay.ps1` launcher (SHA-256
`FF29B15C09AAEF8EAAC461C5503CC04C4586EA0D52AB08C9B3CA25B174D6596D`). The
copied binary was 625,423 bytes, SHA-256
`978987BE339A7C11A3A3CBB87CBE28DB450518AD3AEB8C7CE5E5A6558ACAD67E`.

It exited **0** at the configured **2,100,000,000-instruction cap**
(`stopped after 2100000000 instructions: OK`) in **1,434.86 seconds**, with
**empty stderr**, no `_panic`, no `_Debugger`, and no undefined-instruction
stop. Launcher postflight passed: the canonical kernel, device tree, and
immutable rootfs re-hashed unchanged, and the work image was exactly
466,825,216 bytes.

#### The queue is now walked, not inferred

Run22 could only say `msgcount=5`. Run23's bounded reciprocal walk resolves it:

```text
mqueue=c0d705b8 containing-port=c0d705a0  port io_bits=80000000 (active type validated)
copied-in kmsg=c3d3c000 header=c3d3c2c4 bits/size=80001211/2104
  destination/reply=c0d705a0/c39e7000 id=0x0054b557  destination/id match=yes/yes
queue fields: msgcount=5 qlimit=5 seqno=0 fullwaiters=0
queue linked-list snapshot: head=c21e3000 readable=yes linked=5 closed=yes
                            consistent=yes truncated=no reserved-or-in-flight=0
  [0] c21e3000 next/prev=c31d7000/c448c000 size=2104 dst=c0d705a0 reply=c2bf6ea0 id=0x0054b557
  [1] c31d7000 next/prev=c3f50000/c21e3000 size=2104 dst=c0d705a0 reply=c34d2630 id=0x0054b557
  [2] c3f50000 next/prev=c3e52000/c31d7000 size=2104 dst=c0d705a0 reply=c2d33d80 id=0x0054b557
  [3] c3e52000 next/prev=c448c000/c3f50000 size=2104 dst=c0d705a0 reply=c31c32d0 id=0x0054b557
  [4] c448c000 next/prev=c21e3000/c3e52000 size=2104 dst=c0d705a0 reply=c31c3cf0 id=0x0054b557
```

So there really are **five linked messages**, not five reserved slots:
`reserved-or-in-flight=0`. All five are the *same* CTServerConnection initial
handshake — identical request ID `0x0054b557` and identical 2,104-byte size —
sent to the same destination port with **five distinct reply ports**. SpringBoard's
own kmsg `c3d3c000` (reply `c39e7000`) is the sixth, and `qlimit` is 5. The
senders behind the other five reply ports were **not** identified by this run.

Both route PCs are now **BOUND**, using the decisive send-path register pair
`r4=mqueue`, `r8=kmsg`, so run22's "adjacent candidates" are retired:

```text
BOUND send: queue-full slow branch     pc=c00147ba @1966245373  r4/r8=c0d705b8/c3d3c000
BOUND send: mark fullwaiters=1 (pre)   pc=c00147d6 @1966245387  r4/r8=c0d705b8/c3d3c000
```

The exact kernel path was `_ipc_kmsg_get` @1,966,242,177, `_ipc_kmsg_copyin`
@1,966,244,206, `_ipc_kmsg_send` @1,966,245,251, `_ipc_mqueue_send`
@1,966,245,348, `_thread_block_reason` @1,966,245,550, and the SpringBoard
thread switched out at **1,966,246,193**. The episode ended
`OPEN/UNRESOLVED`; the receive buffer was never read.

#### The receive right belongs to launchd, and this time that is authoritative

The hardened decoder validates `ip_receiver_name` **first**, then the whole
object graph:

```text
port receiver-name=00001b03; union-authority=validated
receiver chain: space=c0acfe60 active=1 task=c0ad7b10 task-space=c0acfe60 (matches)
                proc=e0381d68 pid=1
decode AUTHORITATIVE
```

Run22's PID-1 print was rejected because it dereferenced the `+0x3c` union
without a discriminator. Run23 reaches the same PID through the discriminator
and the full active-space/task/backpointer/proc chain, so the result now
stands: **launchd holds the receive right for the port SpringBoard is sending
to.** The probe also states the correct caution — launchd routinely pre-creates
and holds service ports, and this does not by itself prove that CommCenter
failed.

#### CommCenter is alive, and it never took the port

The exact pathname-qualified SETEXEC attempt armed at **517,086,676** with
task `c2ca2760`, proc `e037f890`, **PID 24** (discovered, never hardcoded).
Across the run that identity was never invalidated, took **0** signals, and
never entered `_exit1`. It retired **10,975,004** user and **58,986,380**
kernel instructions before the SpringBoard send entry — and **0 of either
afterwards**. Its exact destination-owner correlation is a **MISMATCH**:
the port's receiver is launchd, not CommCenter.

Six CommCenter threads were retained. Three are blocked on the *same*
semaphore `c0b239a0` with distinct nonzero deadlines and the timer field
active — timed waits, continuation `c0026fc5` (`_semaphore_wait_continue`),
wait queue `c0b239a8`:

```text
e038dbb8 (SETEXEC entry thread) deadline=03d2398d last mach trap -31 returned r0=0 @1939557313
e0376000                        deadline=03d825a9 last mach trap -31 returned r0=0 @1966208488
e02e4774                        deadline=040f65a7 last mach id=3206
```

The fourth is the interesting one. Thread **e02f5888** blocked in
`_ipc_mqueue_receive` at **932,507,189** on mqueue `c0dd99f0` (containing port
`c0dd99d8`, task-local name `0x10004001`, options `0x03000006`) and **no resume
was observed** for the remaining ~1.03 billion instructions.

Each of these is classified "last observed unresolved block; no resume
observed." That is deliberately weaker than "still enqueued at the cap": no
final live wait-state reread exists, so an asynchronous wake that left a thread
runnable but unscheduled is not excluded. Two further threads (`e0379bb8`,
`e035d000`) are historical resolved waits and are not terminal evidence.

#### AppleBaseband never fired, so it is not the delivered cause

The read-only causal observer's own frontier line is the headline:

```text
frontier: event-source enable call was entered, but no reset callback was
          observed since trace start
```

Concretely, from a cold-start baseline with complete subscription history:

```text
AppleBaseband object=c0c3a700  setup reset-function hits/nonzero=1/1 value=c0b6b020
event-source result hits/nonzero/committed/enable-call-entries=1/1/1/1 value=c0b6c340
reset callback hits=0        reset read returns=0        changed=0
messageClients dispatch attempts total/high/low=0/0/0
notification handlers=0  send-headers=0  routes=0  retained causal chains=0
```

AppleBaseband found its reset platform function, created and committed an event
source, and enabled it — and then the reset callback never ran. There is
therefore **no** AppleBaseband notification, no Mach send, and no queue route.
The saturated queue is **not** explained by a delivered baseband notification.

The same observer supplies the correlation that matters most. Of 14 IOKit
`registerInterest` wrappers seen, exactly one was accepted, and it is
CommCenter's:

```text
interest[0] service/port/mqueue=c0c3a700/c3c59ab0/c3c59ac8 wrapper=c019ee6a
            thread=e02f5888 registrations/success=1/1 @931584215
            baseline receiver name/space/task/proc/pid=00003503/c0acf7e8/c2ca2760/e037f890/24
            receive entry hits/CommCenter/unreadable=0/0/0
            routes reserve/full/fullwaiters/no-waiter/woken=0/0/0/0/0
```

CommCenter (the same task `c2ca2760`, proc `e037f890`, PID 24) registered
exactly one IOKit interest, on AppleBaseband, at **931,584,215** — and the very
thread that registered it, `e02f5888`, blocked in a Mach receive **923,000
instructions later** and was never seen again. That is a strong temporal and
identity correlation. It is **not** proof that the absent reset notification is
what keeps CommCenter from checking in: the receive that blocked is on port
`c0dd99d8`, not on the interest port `c3c59ab0`, and this run does not
establish whether the two are joined by a port set.

#### The BasebandSPI register window is unmapped

The non-RAM page report names the transport underneath all of this:

```text
0x3d200000  r=4  w=11  first pc 0xc05f2eca  com.apple.driver.BasebandSPI+0x1eca
```

`0x3d200000` is **not** one of the five declared stub windows
(`clkrstgen`, `miu`, `gpio`, `edgeic`, `gpioic`), so it is genuinely unmapped:
reads return zero rather than stored bytes. The traffic is a one-shot, not a
spin:

```text
W 0x000,0x00c,0x038,0x034,0x030,0x008,0x004,0x034,0x004  @933033890..933033922  lr c05a9fd8
R 0x000,0x004,0x008,0x034                                @1757842145..1757842149 lr c05f3425
W 0x004,0x000                                            @1760475736,1760475740  lr c05f4151
```

The driver programs the block just after CommCenter's interest registration,
reads four registers back ~824 M instructions later, writes twice more, and
then never touches it again. Because the driver does not poll it, run23 does
**not** prove this window blocks the boot — but it is an identified peripheral
that the machine answers with zeros, which is exactly the class of gap this
project refuses to leave unnamed.

#### Everything else held, and nothing rendered

The display chain reproduced run20's corrected behaviour exactly: 4 TV-out
frames, one IRQ-30 filter entry and acceptance, one action entry, one
completion dispatch, 8 IOMFB completions, `startWindowServer` returning at
**1,919,831,289**, `GSSetMainScreenInfo` returning at **1,919,831,287**, and
SpringBoard entering `applicationDidFinishLaunching:` at the same
**1,923,358,329** as run20, run21, and run22. CLCD ended scanning/running
`1/1` with **604 frames** on window 0, 320x480, stride 1280, at `0x0885c000`.

`SpringBoard:UIController-call` had **0 hits**. The live observer recorded
**0** overlapping writes, **0** changed writes, **0** changed bytes, and **0**
RGB-visible mutations. The 460,815-byte PPM is byte-identical to the seed:

```text
SHA-256 CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB
```

**SpringBoard is not rendered.**

The external-md bridge completed 12,195 reads (49,968,640 bytes) and 178 writes
(725,504 bytes) with **0 failures**; strategy handled 12,193 reads and all 178
writes; the raw path took 2 reads, 0 writes, 0 guest errors, 2 native redirects,
2 completions, and 0 pending continuations, reading 45,056 media bytes plus
20,480 coherent-guard bytes. Guest free memory bottomed at **12,961 pages
(50.63 MiB)** at instruction 1,957,101,568 and ended at 12,997 pages
(50.77 MiB). The retained evidence directory occupies **447.43 MiB on F:**.

Two diagnostic-integrity caveats belong with all of the above: the per-thread
observer reported **16** exact-hook attribution omissions (first @551,530,083,
last @1,388,875,916) and **50** unreadable classifications with **0** readable
contradictions. Neither overlaps the decisive `_ipc_mqueue_send` episode, whose
route bindings, queue walk, and owner decode are each independently marked
validated or authoritative.

### 2026-07-27: runs 73-75 — the guest runs `pppd`, and uart4 is the wire

Networking step S0. `docs/networking.md` picks PPP over an emulated UART
because both halves already ship — Apple's signed `/usr/sbin/pppd` in userland
and `com.apple.nke.ppp`'s `pppserial` line discipline in the kernel — and
because it needs no new guest kernel code. It is **a temporary workaround
pending real drivers and controllers**, and the code says so in the `--ppp`
help text, in `rootfs_work.h`, and in the plist's own provenance comment.

Three commits are under test: `8a8c75c`, which decodes uart4 (0x3cc10000) as a
transmit-only device window; `42a8c0d`, which rewrites an inert LaunchDaemon
plist into a `pppd` job and watches the wire; and `6dafe28`, which puts
`pppd`'s own stderr on `/dev/console` after run74 proved an exit code was all a
run could otherwise report.

Every run below was launched from a `git archive HEAD` extracted to a fresh
directory, so none of the binaries carry the unrelated GPIO interrupt-controller
work that was uncommitted in the main worktree at the time. Without that
separation the A/B below would have isolated nothing.

#### The success criterion, stated before the result

`7E FF 7D 23 C0 21` on uart4. An HDLC flag (RFC 1662 §4.1), the all-stations
address (§4.2), the UI control byte `0x03` escaped as `7D 23` because ACCOMP is
not yet negotiated and `0x03` is below `0x20`, and protocol `0xC021` = LCP
(RFC 1661 §2). Six bytes and no more, because everything after them varies:
pppd 2.4.2's first Configure-Request carries MRU 1500, ASYNCMAP 0, PCOMP,
ACCOMP and a **random** magic number, so pinning any of it would make the check
fail on every run but one.

**The milestone was not reached.** What follows is what was measured instead,
which is considerably more than nothing.

#### A blocker that is not ours, pinned to the instruction

The first attempt died at instruction 218,331,077 with
`external-md: guest fatal entry reached`. The console ends at
`AppleS5L8900XADMFMC::start: Loading ADM/FMC firmware 'CalmADMFMCFirmware-18'`
followed by `AppleS5L8900XADM::admStart: ADM not ready to start`, and the panic
call site is `0xc04d679c` with the format string `"ADM startup failed"`.

It is not PPP's, and four controls say so:

| build | `--ppp` | stopped at |
|---|---|---|
| working tree | yes | 218,331,077 |
| working tree | no | 218,615,894 |
| clean `HEAD` (42a8c0d) | no | **218,615,894** |
| clean `55ebb98`, no PPP code at all | no | **218,615,894** |

Three different binaries, the same instruction. And **run72 died there too** —
its own log carries `=== _panic entered (call #1) at instruction 218615894 ===`
and none of its four checkpoints were ever written. The fix is the boot
argument `nand-enable-adm=0`, which run71 already carries and which is why
run71 never reaches the ADM path at all. **Every S0 run needs it.** (The ~284k
difference in the first row is the plist rewrite and the longer boot-arguments
string, not a behavioural change.)

One more trap worth a line, because it costs a whole run and looks like
nothing: `-F` invalidates **`firmware/screen.ppm` relative to the process
working directory**, so a run launched with its own working directory exits 1
before booting unless that directory has a `firmware/` subdirectory with that
file in it.

#### run73 (700e6) — the pipeline, proven as far as the exec

In `work/run73-ppp-smoke`. Five things, in order:

```text
ppp        : com.apple.chud.pilotfish.plist @ image+0x00f5f000
boot_args  : cmdline "debug=0x8 serial=1 nand-enable-adm=0 uart4_dma_enable=0 rd=md0"
AppleS5L8900XSerial: Identified Serial Port on ARM Device=uart4 at 0x3cc10000(0xea9d6000)
    #65     @557124470   syscall 244 posix_spawn
             path va 00103d40: complete; path "/usr/sbin/pppd"
```

**launchd spawned our job at instruction 557,124,470.** That single line proves
the hijack survived into the work image, that launchd parsed the rewritten
plist, and that `RunAtLoad` fired — the half of S0 with the most ways to fail
silently.

The bytes were read back out of the published work image and are byte-exact,
and the canonical input is untouched: the stock 530 bytes still sit at offset
`0xf5f000` of `firmware/rootfs.img` hashing
`882ebd0b14088120b03750090ef9b6885a7b3bfbbe286df9ac23ecb431f55312`, and the
work image is exactly 466,825,216 bytes.

**And uart4 was silent for the entire 700e6.** That is not a disappointment,
it is the control that makes the later runs readable: uart4 carries no
`boot-console` property, nothing in the kernel's bring-up writes to it, so any
byte appearing on it later is attributable to the job we installed rather than
to something the machine does anyway.

Before `8a8c75c` the page was not merely unmodelled but **undeclared** —
run59's census recorded `0x3cc10000 r=8 w=15` falling through to the unmapped
path, so every `UTRSTAT` read answered 0, i.e. "transmitter busy". A driver
that waits for room before storing would have waited forever. Decoding the
window is what lets a transmit path terminate at all.

#### run74 (1.2e9) — `pppd` runs for 182 million instructions and exits 1

In `work/run74-ppp`. This is the blocker, and the shape of it is worth more
than the fact of it.

```text
#65  @557124470  syscall 244 posix_spawn   path "/usr/sbin/pppd"
                 task/task-proc/pid c2e151d8/e03832cc/19
#84  @739184188  syscall 1   exit          args a0=00000001
                 task/task-proc/pid c2e151d8/e03832cc/19
#85  @739184282  _exit1 proc=e03832cc rv/status=00000100
```

pid 19 lived **182,059,718 instructions** — so this was not a failed exec;
dyld mapped the 284,608-byte binary and it ran — and then exited **1**. Zero
bytes reached uart4 across the whole 1.2e9.

**The exit code rules out four things.** pppd 2.4.2 has distinct exit codes,
and ONE is `EXIT_FATAL_ERROR`. It is therefore *not*
`EXIT_OPTION_ERROR` (2, the argv failed to parse), *not* `EXIT_NOT_ROOT` (3),
*not* `EXIT_NO_KERNEL_SUPPORT` (4, `ppp_available()` said no), and *not*
`EXIT_OPEN_FAILED` (7, the device could not be opened).

So: **the devfs node exists under the predicted name `/dev/uart.debug`, the
seven-argument command line parsed, the job ran as root, and
`com.apple.nke.ppp` answered.** Four of the nine unknowns S0 was meant to
settle are settled — by an exit code, which is a cheaper instrument than any of
them deserved. What is left is a `fatal()`, and the strings support exactly two
candidates, both present in the image exactly once:
`"Couldn't set tty to PPP discipline: %m"` and
`"Baud rate for %s is 0; need explicit baud rate"`.

#### run75 (850e6) — routing the message that run74 destroyed

`fatal()` writes to stderr, and launchd hands a job with no `StandardErrorPath`
`/dev/null`. run74 could therefore report an exit code and nothing else. The
job now carries `StandardErrorPath = /dev/console`, so `pppd`'s complaint lands
in the same console capture as every other guest message; the key was confirmed
present in the image (5 occurrences of the key name, 7 of `/dev/console`)
rather than assumed to be supported.

Two arguments were spent to afford the 61 bytes inside a 530-byte record whose
slack was 15. `noauth` was determinism rather than necessity — 2.4.2 only sets
`auth_required` when a default route exists, and at this point in the boot
there is none. The explicit `115200` is a real trade, and it is deliberately
self-resolving: without a speed, `set_up_tty()` reads the port's current one
back and `fatal()`s if it is zero, and that fatal is now *the message on the
console*. `AppleOnboardSerial` programs 19200 8N1 at `0xc047244a` during
`start()`, before any tty is opened, and it is the only baud constant in either
serial kext, so the risk was bounded before it was taken.

A cap of 850e6 is sufficient and 1.2e9 is waste: `pppd` is spawned at 557e6 and
dead by 739e6, so everything S0 can observe has happened by then.

**Neither hoped-for outcome happened, and the run is still worth the twelve
minutes.** Two results:

**The failure is invariant to those two arguments.**

| | spawn | `exit(1)` | `pppd` lifetime |
|---|---|---|---|
| run74 (`115200`, `noauth`) | 557,124,470 | 739,184,188 | 182,059,718 |
| run75 (neither) | 557,135,323 | 739,143,287 | 182,007,964 |

51,754 instructions of difference out of 182 million — 0.028%, which is about
what removing two argv entries costs in option parsing. `pppd` fails at the
same place either way, which **retires the `"Baud rate for %s is 0"`
hypothesis**: that fatal fires only in the second row and would have moved the
exit point. It was worth testing and it has been tested.

**And `pppd` printed nothing.** The console is byte-identical to run74's across
all 5,371 bytes both runs produced. So either launchd on 3.1.3 does not honour
`StandardErrorPath`, or `pppd` never acquires a usable stderr, or this build's
`fatal()` logs only through syslog. The honest reading is that pppd's message
is not reachable this way — not that it had nothing to say.

#### Where this leaves S0, and the next instrument

Six of the nine unknowns the milestone was posed to settle are settled, and
none of them by assumption: the plist hijack survives into the work image,
launchd starts the job, AMFI accepts Apple-signed `pppd`, dyld loads it, the
devfs node exists under the predicted name `/dev/uart.debug`, and
`com.apple.nke.ppp` answers. What is left is one `fatal()` inside `pppd`.

The lead is in the lifecycle record and costs one flag:

```text
#84  @739143287  syscall 1 exit   args a0=00000001 a1=0000000c a2=2ffffeec a3=00039c30
                 user pc 33ad7138 (ARM, spsr 00000010)
```

`user pc 33ad7138` is libSystem's `exit` thunk inside the shared cache, so it
identifies nothing. But **r3 = `0x00039c30` is inside `pppd`'s own image** —
the binary is 284,608 = `0x457C0` bytes and loads low — so it is a `pppd` text
address in the neighbourhood of whatever called `exit`. `--call-probe
0x00039c30` captures `pc/lr/sp`, `r0`-`r3` and two stack words there, and `lr`
turns "a fatal() somewhere" into an address to disassemble.

Cheaper still and not yet done: extract `/usr/sbin/pppd` from the image and
disassemble it statically. Both candidate strings —
`"Couldn't set tty to PPP discipline: %m"` and
`"Baud rate for %s is 0; need explicit baud rate"` — are present exactly once,
so their addresses are findable and can be compared against `0x39c30` before
another run is spent.

**Nothing in these three runs is a packet.** uart4 carried zero bytes in all of
them, and the milestone stands unmet and unchanged: `7E FF 7D 23 C0 21`, or it
did not happen.

#### run78 (850e6) — `pppd` says what is wrong, in its own words

The paragraph above is superseded on one point, and it is worth stating which:
`fatal()` does **not** log only through syslog, and launchd does honour the
key. run75 named the wrong stream.

`pppd`'s `error()` and `fatal()` share one emitter at `0x0002245c`. It calls
`syslog()` and then writes the formatted message to `*log_to_fd`. `_log_to_fd`
is at `0x00039c70` with a file image of **1** — stdout — and `nodetach` means
nothing ever lowers it. File descriptor 2 is never written to at all. So
`StandardErrorPath` pointed launchd at a stream `pppd` does not use, and
`/dev/null` consumed the message exactly as before. One key, `StandardOutPath`,
same 530-byte budget (`40ce8e2`), and the console carries:

```text
Wed Dec 31 16:00:06 1969 : set_up_tty, can't set controlling terminal: Inappropriate ioctl for device
Wed Dec 31 16:00:06 1969 : Couldn't set tty to PPP discipline: Inappropriate ioctl for device
```

Two things, not one. `TIOCSETD` with `PPPDISC` is the fatal, and it is the
first of the four `error()` sites in `_tty_establish_ppp` — the one at string
`0x00032a34`, which every later step presupposes. But `TIOCSCTTY` fails
immediately **before** it, and no previous run had visibility into that.

Both fail with `ENOTTY`. That is the whole remaining question, and it is now a
question about the tty layer rather than about `pppd`.

Also corrected by this run: **`0x00039c30` was never a string.** `__cstring`
ends at `0x35d9e`; `0x39c30` is `__DATA,__data`, file image `ffffffff`,
referenced twice — `_die+0x10` tests it and `_main+0xca8` stores
`establish_ppp()`'s return into it. It is **`fd_ppp`**. The run75 note above
proposed probing it as a text address; that would have measured nothing.

`AppleS5L8900XSerial` did attach — `Identified Serial Port on ARM Device=uart4
at 0x3cc10000(0xea9d6000)` — and uart4 still carried **zero bytes**. The
milestone is unchanged.


### 2026-07-28: the un-match fix lands on device, and the black screen is not /vram sizing

**THE FIX WORKS.** The user's second log, same phone, build with
`s5l_bringup_request_t::unmatch`:

```
  [vm] 4 switches are not applied as set: activate, jb-codesign, jb-payload, nat
  ...
  com.apple.launchd 1  *** launchd[1] has started up. ***
  Running fsck on the boot volume...
  /dev/md0 on / (hfs, local, noatime)
  mDNSResponder[14] Builtin profile: mDNSResponder (seatbelt)
  systemShutdown false
```

Nine unapplied switches became four, every `AppleMBX` / `AppleBaseband` /
`AppleSerialMultiplexer` line is gone, and **launchd starts** -- inside the first
410 M instructions, where before it had not appeared by 11.5 G. The device now
matches the desktop line for line.

The sampler correction is confirmed by the same log at the same 409.6 M mark:
`spread over 64 regions, the busiest taking only 13 of 512 samples`. Sixty-four
regions with the hottest at 2.5% is real work. The previous build called the
wedge "NOT spinning" at 2 regions and 60%.

**THE BLACK SCREEN IS A SEPARATE FAULT, and /vram sizing is not it.** Both
machines now reach:

```
  IOSurface warning: buffer allocation failed.  320 x 480 fmt: 42475241 size: 614400 bytes
```

Found by its own message: the string is at `0xc052d1a4`, referenced once from
`com.apple.iokit.IOSurface+0x260c`. The failing function reads:

```
  c05244a0  ldr  r1, [r5, #0x70]        ; the PARENT descriptor
  c05244ac  blx  IOSubMemoryDescriptor::withSubRange   ; 0xc0195088
  c05244b4  str  r0, [r5, #0x24]
  c05244cc  cmp  r3, #0                 ; r3 = [r5,#0x90], the REGION COUNT
  c05244d0  beq  0xc05244f0             ; zero regions -> fall through
  ...
  c05244f0  ldr  r0, [r5, #0x80]        ; the FALLBACK
  c05244fc  ldr  r2, =_page_size
  c052451c  blx  IOBufferMemoryDescriptor::withOptions ; 0xc018c91c
  c0524524  str  r0, [r5, #0x24]
  c0524528  beq  0xc0524598             ; NULL -> the warning
  c05245c0  ldreq r0, =0xe00002be       ; kIOReturnNoMemory
```

So there are TWO paths and both failed. The primary carves the surface out of a
parent memory descriptor with `IOSubMemoryDescriptor::withSubRange` -- that is
the `/vram` path, and it is driven by a loop over `[r5+0x90]` regions. The
fallback is a plain `IOBufferMemoryDescriptor::withOptions(opts, 614400,
page_size)` against the kernel map, and it returned NULL.

**This reframes the question.** "/vram is the pool IOSurface allocates every
surface from" is true only of the primary path, and that path is skipped
entirely when the region count is zero -- which would also explain the earlier
`IOSurface: buffer allocation size is zero`, and why run86 measured THREE
surfaces as strictly worse than two. If the regions are not being enumerated,
the pool's size was never the variable.

run110 probes `0xc05244ac` (the withSubRange call), `0xc05244f0` (the fallback
entry) and `0xc0524598` (the warning) to settle which: if the fallback is
entered without withSubRange ever being reached, the region count is zero and
the fix is upstream of the allocator entirely.

### 2026-07-28: run108/run109 -- the DMAC chain is REFUTED, and the real hang was configuration

Two separate things were wrong today, and measuring both cost less than the
reasoning that produced either.

**THE TWELVE-LINK CHAIN'S TOP THREE LINKS ARE FALSE.** run108 armed call probes
on the registration path and run109 on the return site:

```
  pc 0xc019a35c  registerDMAController     captured 2
  pc 0xc019a2d8  IODMAController::start    captured 2
  pc 0xc019a3c4  getController             captured 11
  pc 0xc018b3fc  IODMAEventSource::init    captured 11

  return of getController (0xc018b42e), 11 captures:
      7 x  r0 c0ce5000     a real controller
      3 x  r0 c0ceac00     the other one
      1 x  r0 00000000     a different call site (lr c019a3db)
```

`AppleARMPL080DMAC` DOES register, twice -- once per controller -- and
`IODMAController::start` is entered from the kext at `lr c070f434` both times.
`getController` then returns a real controller in TEN of eleven calls, and the
two pointers it returns are exactly the `this` values captured entering
`IODMAController::start`. They are the registered PL080 instances.

So "AppleARMPL080DMAC never registers as an IODMAController" is retracted, and
with it "getController returns NULL" and "IODMAEventSource::init fails". The
question of why the SPI controller's `this->0xb0` is zero is OPEN again, and it
is a question about the SPI controller's own path, not the DMAC's.

**HOW THE WRONG ANSWER WAS REACHED**, because the method matters more than the
claim. Three errors compounded:

  1. A vtable slot was read as a call site. `0xc019a35d` appears in the kext at
     `0xc0710390` -- but its neighbours are `IODMAController::completeDMACommand`
     and `notifyDMACommand` among the driver's own overrides. It is the
     inherited vtable, and it proves inheritance, nothing more.
  2. Scanning for direct branches then found nothing, which was read as
     confirmation. But `registerDMAController` is VIRTUAL: C++ dispatches it
     indirectly, so no branch scan can ever see the call.
  3. The XNU core is THUMB. Disassembling `IODMAEventSource::init` as ARM
     produced fluent, entirely fictional output.

Static reading answered none of it. Four probe addresses and one boot did.

**THE HANG THE USER ACTUALLY HAD was not this at all.** An iPhone 17 running the
app burned ~51 M instructions inside `AppleMBX+0xb440`, then reached 11.5 G
without launchd ever starting, while the desktop was at launchd before 1.5 G on
the same kernel, tree and block backend. The difference was configuration, and
the app had been reporting it as a defect all along:

```
  [vm] 9 switches are not applied as set: mbx, sha1, baseband, spi2, usb-otg, ...
```

`bootkernel` un-matches those nubs by default and its own switch help says why:
`/arm-io/mbx` matched means the PowerVR driver busy-polls a reset bit in a
register block this VM does not model; `/arm-io/sha1` matched means
IOCryptoAcceleratorFamily's hardware hook takes every exactly-4096-byte
digest -- the size `cs_validate_page` asks for -- to a register file we do not
model, so launchd's first text page fails its signature and the boot spins on
`cs_invalid_page` HAVING PRINTED NOTHING. That is the reported symptom exactly.

The un-match step existed only as a private helper in `tools/bootkernel.c`, so
the portable core could not do it and the app could not ask. It is core's now,
as `s5l_bringup_request_t::unmatch`.

**THE INSTRUMENT WAS ALSO WRONG, and its own output said so.** The spin sampler
called the wedged device "NOT spinning: work is spread over 2 regions, the
busiest (0xc0783440) taking only 309 of 512 samples". Two regions is 128 bytes
of code holding an entire window -- a loop straddling a 64-byte boundary. The
share test looked at one region only. Fixed: four or fewer distinct regions
across a whole window is a spin however it splits.

**THE NEW FRONTIER is the black screen, and it is not the boot.** run107 ran the
desktop to 12 G and got past everything run106 saw:

```
  IOSurface warning: buffer allocation failed.  320 x 480 fmt: 42475241 size: 614400 bytes
```

`42475241` is 'BGRA' and 614400 is exactly 320x480x4. SpringBoard is asking for
its surface and being refused -- and an earlier line in every run, `IOSurface:
buffer allocation size is zero`, is very likely the same wound seen sooner. The
framebuffer being black is downstream of THAT, not of touch and not of the DMAC.

### 2026-07-28: two links further -- why the SPI controller has no DMA channel

`v[0x360]` refuses because `this->0xb0` is zero, and `0xc05a71b4` is where that
field is set: it is the return of `IODMAEventSource::dmaEventSource(...)`
(`0xc018b4d8`, named in the kernel's own symbol table).

`IODMAEventSource::init` (`0xc018b3fc`, and it is THUMB -- disassembling the
XNU core as ARM produces convincing nonsense) fails in three places, and the
third is the one that matters:

```
    blx  IODMAController::getController      ; 0xc019a3c4
    str  r0, [r4, #0x28]
    cmp  r0, #0
    beq  fail
```

And `getController` opens with:

```
    ldr  r1, ="dma-parent"
    ldr  r3, [r2, #0x9c]        ; getProperty
    blx  r3                     ; provider->getProperty("dma-parent")
    ...
    cmp  r0, #0
    beq  return NULL
```

**`dma-parent` is PRESENT in the shipped device tree**, so that is not the
missing piece -- checked directly, along with `dma-channels` and
`AAPL,phandle`. Past that check it reads the phandle out and looks the
controller up by it at `0xc019a334`, and returns NULL if nothing answers.

So the frontier is one sentence: **`AppleARMPL080DMAC` never registers itself as
an `IODMAController`.** Its `start()` clearly runs -- the console prints
`_dmacBaseAddress` for both instances and `_initDMAChannel` for eight channels
-- but it never gets as far as registering, which is the same boundary as
`DMACConfiguration` never being written.

**The full chain now stands at twelve links, every one measured:**

```
  AppleARMPL080DMAC does not register as an IODMAController
    -> IODMAController::getController returns NULL
    -> IODMAEventSource::init fails
    -> the SPI controller's this->0xb0 stays zero
    -> v[0x360] writes kIOReturnDMAError into the request
    -> ...and returns 0, so the bootloader cannot tell
    -> the ATN_ACK answers 0x1AA1, never 0x4BC1
    -> the Z2 is never programmed
    -> no property reaches userspace
    -> the surface bounds are negative
    -> every contact clamps to one point
    -> a tap changes nothing
```

Nothing in `core/src/soc/` is implicated above the DMAC. The next read is what
`AppleARMPL080DMAC::start` does after `_initDMAChannel` and what stops it
short -- and since the same driver gates audio, one answer may move both.

### 2026-07-28: the whole causal chain, from "no touch" back to the DMA controller

With run106's numbers in hand the remaining gap closed by reading, not by
booting. `v[0x360]` -- the SPI controller entry the firmware sender calls -- is
`0xc05a69a4`, confirmed because that address appears exactly once as a word, at
`0xc05ae554`, which is `0xc05ae1f4 + 0x360`.

Its first act is a refusal:

```
    ldr r3, [r0, #0xb0]     ; the controller's TX DMA channel
    cmp r3, #0
    bne  ...                ; present -> check the RX side
    ldr r3, [r1, #0x20]     ; the request wants TX DMA
    cmp r3, #0
    bne  refuse
  ...same shape for [r4,#0xb4] and [r5,#0x24]...
  refuse:
    ldr r3, =0xe00002d4     ; kIOReturnDMAError
    str r3, [r5, #0x40]     ; into the REQUEST, not the return value
```

`0xe00002d4` is 0x18 past `kIOReturnError`, which is **kIOReturnDMAError**. So
the controller is saying "you asked for DMA and I have no channel for it".

**AND IT RETURNS SUCCESS.** The refusal branches to `0xc05a6e04`, which does
`mov r5, #0` and ends `mov r0, r5`. The error goes in `request->0x40` and the
return value is zero.

That is the whole of run103's 15/15/15. The firmware sender checks only the
return value (`cmp r0,#0` at `0xc0445228`), believes the transfer happened,
performs the ATN_ACK, reads `0x1AA1` where it wanted `0x4BC1`, and retries --
five times per attempt, three attempts. Nothing ever reaches the digitizer,
which is why `spi1 words 176` and `tx_drops 0`.

**The chain, end to end:**

```
  AppleARMPL080DMAC never writes DMACConfiguration      (run104, run106)
        -> setEnabled(true) never runs
        -> the SPI controller holds no DMA channel      (this+0xb0 / +0xb4 zero)
        -> v[0x360] refuses with kIOReturnDMAError
        -> ...and returns 0, so the bootloader cannot tell
        -> the ATN_ACK answers 0x1AA1, never 0x4BC1
        -> the Z2 is never programmed                   (run101, run105)
        -> no property reaches userspace                (run100)
        -> the surface bounds are negative              (run96 snapshot)
        -> every touch lands on the same pixel          (run98)
```

Ten links, every one measured. The bottom four were established days ago and
the top four today, and the join is this function.

**What that means for the model.** Nothing above `AppleARMPL080DMAC` needs
changing. The question is why that driver never enables itself -- the same
question `tools/bootkernel.c` recorded against the AUDIO path and which was
filed there as "a locked phone plays nothing". It is not about audio. It gates
the digitizer too, and through it everything downstream.

Sound was described as downstream of touch. It is more direct than that: both
are downstream of the same unenabled DMA controller.

### 2026-07-28: run106 -- the SPI DMA fix was aimed at the wrong thing

run105 applied the SPI DMA overrun change and the digitizer's counters did not
move: `data 0, acks 0, exec 0, in-hbpp 1`, byte-identical to run101. run106
added an SPI report, and the numbers say the hypothesis behind that fix was
wrong.

```
spi1  words 176  tx-drops 0  rx-underruns 0  rx-overruns 0
      control 00000000  setup 0000101e [no DMA bit]  slaves 1
```

**`tx_drops` is ZERO.** Not one octet was ever discarded at a full transmit
FIFO. The stall that run103/run104 pointed at was never happening, because
nothing was arriving to stall. `words 176` is the probes and command frames and
nothing else -- seven sixteen-octet probes is already 112 of it.

**`setup` bit 0x40 is never set**, so `SPI_SETUP_DMA` -- the flag the whole fix
is gated on -- is never raised by the driver. That bit was INFERRED from one
branch at `0xc05a6c24` and never measured, and this is the measurement.

**WHAT run104 ACTUALLY SHOWED, corrected.** Its `dmac1 items 210 bytes 812340`
was read as "the DMAC is pushing 812 KB into SPI1". Those counters are
per-CONTROLLER, not per-channel. Channel 5 is simply the residue left in the
registers, and its own control word settles it:

```
ch5  src 0bfdd38c  dst 3ce00010  ctrl 84089000  cfg 00000000
```

`dst` is `S5L8900_SPI1_BASE + 0x10`, which is `SPI_TXDATA` -- so the channel is
aimed correctly. But `ctrl & 0xfff` is the transfer size and it is **ZERO**, and
`cfg` has no Enable bit. `DI = 0` and `SI = 1` are right for a FIFO
destination, so this is a channel that was INITIALISED and never given a
transfer. The console's `AppleARMPL080DMAC::_initDMAChannel` lines are exactly
that initialisation.

So the 812,340 octets belong to other channels, and the firmware never reached
the DMAC at all.

**Also measured, and previously only asserted:** `DMACConfiguration (+0x030)` is
NEVER WRITTEN on either controller. An earlier note in `tools/bootkernel.c`
said the same thing about the audio path; it holds for the whole DMAC.

**Where this leaves the bootload.** The chain is: `attemptToBootloadDevice` runs
(run102: 3 captures), `bootloadDevice` runs (3), the preconstructed firmware
sender runs (3), it takes the DMA arm at `0xc04451c4` (run103: 15) and calls
`v[0x360]` (15) -- and the SPI controller then neither arms DMA nor falls back
to PIO, because SPI1 sees 176 octets total and zero drops. The failure is
inside the controller's DMA entry, between `v[0x360]` being called and any
register being written.

`this+0xf4` is what `0xc05a6c24` tests to decide whether to arm DMA, and
`setup` never carrying 0x40 says that test fails. What sets `this+0xf4`, and
what the controller does when it is zero, is the next read -- and it is a
question about `AppleS5L8900XSPIController`, not about the digitizer or the
DMAC.

**The SPI change from run105 is kept**, because it is independently correct: a
shifter does not stall on a full receive FIFO in DMA, and the test pins both
halves. It simply is not what is blocking touch, and it was committed as though
it might be.


### 2026-07-28: run103 -- the firmware goes out over DMA, and the SPI model has none

The last step of the touch diagnosis. run102 put `attemptToBootloadDevice`,
`bootloadDevice` and the preconstructed firmware sender all at three captures
each, so the driver reaches the point of pushing firmware; run101 showed the
device receiving nothing. run103 probed the one branch that can be true of both.

| probe | captures |
|---|---|
| `0xc0445144` the sender | 3 |
| `0xc04451c4` **the DMA path** | **15** |
| `0xc04451ec` **`v[0x360]`, the DMA transfer** | **15** |
| `0xc04451f4` the PIO path | **0** |
| `0xc0445224` `v[0x368]`, the PIO transfer | **0** |
| `0xc0445228` after the transfer | 15 |
| `0xc044525c` **the success branch** | **15** |
| `0xc0445290` the retry increment | 15 |

Fifteen is three bootload attempts times the five retries each makes
(`cmp r8,#5` at `0xc0445294`). **The PIO path is never taken.**

**And the transfer reports SUCCESS.** `0xc0445228` is `cmp r0,#0` and
`0xc044525c` is the `beq` target, and it fires all fifteen times -- so
`v[0x360]` returned zero. What fails is the acknowledgement immediately after
it: the halfword is not `0x4BC1`, so `0xc0445290` increments the retry counter
and the whole thing goes round again.

**The reason is one sentence.** `core/src/soc/spi.c` has no DMA path -- the
only occurrence of the word in the file is a comment -- and drives the attached
slave solely from the TXDATA FIFO write. So a DMA-armed transfer moves no bytes
to the device at all, the SPI controller has nothing to fail on and reports
success, and the digitizer never sees the DATA packet. Its framer is still
waiting for a sixteen-octet probe, so the `1A A1` that follows is answered as a
loopback and the driver reads `0x1AA1` where it wanted `0x4BC1`.

That also explains run101's counters exactly: `data 0, acks 0` with `probes 7`.
A two-octet ATN_ACK starts a sixteen-octet frame the model never completes, so
it is never counted -- and it leaves the framer mid-packet for the next one.

**This corrects a claim `core/src/soc/mtz2.c` carried as settled**: "It is NOT
blocked by DMA ... MTSPIBootloader_Z2 pushes through the ordinary SPI entry
`v[0x368]`." Those three `v[0x368]` sites are real and are all in the
CALIBRATION senders. The firmware sender is a fourth site, `v[0x360]`, and the
flag that selects it is one the guest's console has been printing since run96:
`AppleMultitouchZ2SPI: using DMA for bootloading`.

**So the remaining work for touch is in the SPI controller, not the digitizer.**
The PL080 DMAC is already modelled (`core/src/soc/pl080.c`, 199 assertions) and
the controller arms DMA by setting bit 0x40 when `this+0xf4` is non-zero
(`0xc05a6c24`). What is missing is the join between them: a DMA-armed SPI
transfer has to clock its bytes through the attached slave's `transfer()` the
same way the FIFO path does.


### 2026-07-28: run101 -- the HBPP claim is fixed, and the bootload still sends nothing

The first boot with `core/src/soc/mtz2.c`'s bootloader implemented. Half of it
worked, and the half that did not is now bounded.

**What changed, measured.** The device's own counters at the 3.6e9 cap:

```
hbpp:  probes 7  acks 0  data 0 (0 bytes)  rd 0  wr 0  calib 0  exec 0
pins:  reset-edges 7  reset-bytes 64  in-reset 0  in-hbpp 1
```

Seven probes, all answered yes. run96's six `Could not detect HBPP. Response:
0x00 ...` lines are **gone** -- that was one TRUE and six FALSE from the
one-time claim, and it is now seven TRUE, which is what `finishStarting()` and
`attemptToBootloadDevice()` both need and what a part with no flash actually
says. That specific defect is closed.

**What did not happen.** Not one HBPP packet arrived: no DATA, no ATN_ACK, no
register access, no execute. `in-hbpp 1` at the end -- the part was never
programmed.

**And a consequence worth stating plainly: this run had no touch at all.**
`tap 0 ... NEVER ACCEPTED refused 600000000`. The injection gate now refuses
while the part is a bootloader, which is correct -- an unprogrammed Z2 runs no
firmware that scans a panel, and `deviceReadResultData` at `0xc0441324` would
throw the frame away anyway. But it means the model is, for now, *further* from
a working tap than it was: it used to deliver contacts that all landed on the
same pixel, and it now delivers none. That is a real regression in observable
behaviour and it is not being papered over. It is also not a loss of function,
because a contact that always lands in one place was never a tap.

**Where it stops is NOT yet established, and the console cannot say.** Neither
`attempting to bootload device` nor `not in HBPP, so skipping bootload` appears
-- and the second one did not appear in run96 either, where it certainly ran.
Both use the same level-gated logger, so their absence is evidence about
verbosity and not about control flow. The four `reset-bytes 64` say the dummy
transfers are still landing inside the asserted window, so the pin modelling is
intact.

One hypothesis is already **eliminated**: the device tree carries neither
`fll-mval` nor `cal-dl-addr`, which `0xc04443e4` reads -- but `0xc04443a4`
installs defaults of `0x16e4` and `0x400200` when they are missing, and
`performCalibSeq` then uses `this->0x48` as the value it writes to
`0x10001c04`. A missing property is not fatal, so that is not the blocker.

**The next measurement is the same one that settled run100**: kernel call
probes on `attemptToBootloadDevice` (`0xc04414c4`), `bootloadDevice`
(`0xc0445860`) and its five steps -- `+0x8c` `0xc0444370`, `+0x9c`
`0xc0444a98`, `+0x98` `0xc0444dec`, `+0x88` `0xc04455d0`, `+0x60` `0xc044490c`
-- with a positive control among them. Each must return non-zero or
`bootloadDevice` bails, and the first one is a dispatcher on `this->0x44` that
tail-calls the bootloader vtable's `+0x90` or `+0x94`, neither of which has been
read yet.


### 2026-07-28: run100 — the driver never reads a single report, and the HBPP lie is why

The entry below establishes that no int32 property reaches userspace. run100
establishes **why**, and it is upstream of everything touch-related.

Eight kernel-mode call probes, three of them chosen as **positive controls** —
addresses the guest console already proves were executed, so that a row of
zeroes cannot be confused with a probe that never armed. (run99 ran the same
experiment without controls and its seven zeroes proved nothing; that is why it
is not written up as a result.)

| probe | captures | |
|---|---|---|
| `0xc0442670` `finishStarting` | **1** | control — "detected HBPP. driver will be kept alive" |
| `0xc0441008` `isInHBPP` | **4** | control |
| `0xc04414c4` `attemptToBootloadDevice` | **3** | control — the three retries |
| `0xc043b11c` `publishProperties` | **0** | never entered |
| `0xc04385a8` `getReport` (vtable+0x3f0) | **0** | **not one report is ever read** |
| `0xc0438670` / `0xc04386e0` / `0xc043880c` | **0** | the three publishers |

The controls fired, so the facility works and the driver is alive and probing.
And in two billion instructions **it never issues a single control read of any
report**, and never enters the function that would publish the properties.

**The mechanism, in the driver's own words.** `isInHBPP()` has two callers that
want opposite answers, which `core/src/soc/mtz2.c` documents at length:
`finishStarting()` DETACHES on FALSE, and `attemptToBootloadDevice()` pushes
54,156 bytes of firmware on TRUE. The model resolves this with one monotonic
bit — TRUE once, FALSE forever after — so the driver stays attached and then
skips the bootload. run96's console is that decision, verbatim:

```
AppleMultitouchZ2SPI: successfully started
AppleMultitouchZ2SPI: using DMA for bootloading
AppleMultitouchZ2SPI: detected HBPP. driver will be kept alive
AppleMultitouchZ2SPI:  Could not detect HBPP. Response: 0x00 ...   (x6)
```

A Z2 has no flash. iOS downloads its firmware **on every boot**, which is why
`finishStarting` insists on HBPP: at that moment the part really is an
unprogrammed bootloader. Our device claims to be one and then refuses to be
programmed, so it never runs application firmware — and the driver, correctly,
never interrogates a part that has none.

`mtz2.c` calls this "a bounded, named, single-bit lie that costs three cosmetic
log lines." **That is now measured to be wrong.** It costs every property, and
therefore the surface bounds, and therefore touch — and sound sits downstream
of touch. The file's own note on the alternative is the work item:

> Implementing the HBPP sink — accepting the 54,156 bytes and afterwards
> reporting firmware resident — removes the lie entirely. It is NOT blocked by
> DMA … What blocks it is that the bootloader's own multi-stage protocol is
> unread.

That is now the whole of touch.


### 2026-07-28: the touch bounds read out of a guest — and the lever is **not** the descriptor

run98 left one question standing, and it was the right one to stand on: does
`_mt_DefineSurfaceGrid` back-fill the Sensor Region Descriptor itself, in which
case the descriptor is not what decides where a touch lands? That entry said
**settle this before changing `mtz2.c`**.

It is settled, and the answer is **yes — the back-fill runs, and the descriptor
was never the lever.** No new boot was needed. The evidence is `run96-base/`'s
existing v14 snapshot, which contains raw guest RAM, plus exact disassembly of
the armv6 shared cache already extracted at `work/analysis/dsc_armv6`.

**Finding the structure.** `_alg_InitRowColXYConvert` (0x33d01010) builds two
66-entry tables at fixed offsets inside the grid. Their contents depend only on
constants, so they are a signature: `i*3600/7` for i = -1..64 at `grid+0xc4`,
and `i*5600/11` at `grid+0x40`. Both appear **exactly once** in the snapshot,
0x84 bytes apart — precisely `0xc4 - 0x40`. That fixes the grid, and the call
site at 0x33d002a0 (`add r6, r4, #0x168`) plus the blob copy at 0x33d002bc
(`device + 0xf8`) fix the device object at **VA 0x007e9000**.

**The bounds, read directly:**

| field | value | should be |
|---|---|---|
| `grid+0x148` Xmax | **-434** | 4656 |
| `grid+0x14a` Xmin | -75 | -75 |
| `grid+0x14c` Ymax | **-439** | 7275 |
| `grid+0x14e` Ymin | -75 | -75 |
| `grid+0x150` | -509 = `colTable[-1]` | `colTable[9]` |
| `grid+0x154` | -514 = `rowTable[-1]` | `rowTable[14]` |

Both spans negative. That is run98's pinned normalised 1.0, now observed as
state rather than inferred from a probe count. One correction to run98 while we
are here: index -1 is a **real, deliberately computed table entry** — both loops
start at `mvn r5,#0` — not an out-of-bounds read.

**The fingerprint.** The descriptor `_alg_InitRowColXYConvert` reads is
`grid->0x08`, and in the snapshot it holds `01 00 00 01 00 00 00`. That is not
this model's answer — this model returned eight zero bytes. It is the exact
output of the **back-fill at 0x33d01dd0**, which fires when `blob[7] == 0` and
writes `desc[0]=1`, `desc[3]=1`, `desc[2]=(byte)rows`, `desc[5]=(byte)cols` from
its own arguments. Those arguments are `device+0x20` and `device+0x24`, and both
are zero.

**So the uncommitted fix in the tree could not have worked.** It set
`blob[9]=rows` and `blob[12]=cols` but left `blob[7]` at zero, so the back-fill
would have run anyway and overwritten both with zero. It would have cost a
cold boot to learn nothing.

**And the real fault is further upstream than touch.** `device+0x20` and
`device+0x24` are the **"Sensor Rows"** and **"Sensor Columns"** IORegistry
properties (`_mt_CachePropertiesForDevice`, fetches at 0x33cf7d48 / 0x33cf7d64).
Every int32 property in that object tells the same story:

| property | in the guest | this model publishes |
|---|---|---|
| Multitouch ID | 0 | — |
| Family ID | 0 | 1 |
| bcdVersion | 0 | 0x0100 |
| Sensor Rows | **0** | 15 |
| Sensor Columns | **0** | 10 |
| Sensor Surface Width | **5000** | 4800 |
| Sensor Surface Height | **7500** | 7200 |

5000 and 7500 are not a rounding of 4800 and 7200. They are the literals at
0x33cf8098 and 0x33cf80a0 — **MultitouchSupport's own fallbacks for a property
it could not read**. `_mt_DeviceGetInt32Property` is a plain
`IORegistryEntryCreateCFProperty` on `device+0x08`, so a NULL return means the
key is genuinely absent. **Not one int32 property reaches userspace**, even
though the kernel publishes them from reports 0xD1 and 0xD3 (0xc0438670 and
0xc04386e0) and this model answers both with correct non-zero values.

Report 0xD3's payload layout, from the publisher: `[0]` Endianness, `[1]`
**Sensor Rows**, `[2]` **Sensor Columns**, `[3..4]` bcdVersion big-endian —
which is exactly what `s5l_mtz2_report()` already returns. GET_REPORT_INFO is
not the fault either: the driver tests `record[0]` against zero at 0xc0442db4
and **zero is the accepting value**, which is what the model sends.

**Changed in `mtz2.c` on the strength of this.** The Region Descriptor now
answers with `desc[0]=1, desc[2]=rows, desc[3]=1, desc[5]=cols` — the shape the
back-fill itself produces, which is the only descriptor layout anything in this
system is on record as accepting. A non-zero `desc[0]` suppresses the back-fill,
so a part that describes itself stops depending on the property path. **This has
not been shown to change what a guest does**, and while the property path is
broken it cannot: the descriptor is published by the same mechanism that is
failing.

`to_surface()` now maps the panel onto the bounds the guest actually derives,
`[-75, 4656]` across and `[-75, 7275]` down, instead of onto report 0xD9's
4800×7200. The old mapping ran pixel 319 to 4792, past an Xmax of 4656, so the
right five pixels of the screen were unreachable on top of a ~+6 px offset and a
1.5% stretch. `test_mtz2.c`'s inverse conversion was carrying the same wrong
model and is corrected to the measured one; a new test drives the 0xE7 two-stage
long control read end to end, since the descriptor at 14 bytes is the first
report that does not fit the short form. 46/46 suites pass.

**Still open, and it is now the whole of touch:** why no int32 property reaches
the registry entry userspace queries. That is a question about driver
initialisation, not about geometry, and sound sits downstream of it.


### 2026-07-28: run98 — every touch lands in the same place, and why

**The root cause of the touch failure, measured rather than reasoned.** Six
probes, restored from a fresh v14 snapshot at the lock screen, dragging 12 steps
of 16 pixels each from (62, 432) to (270, 432):

| probe | captures | meaning |
|---|---|---|
| `plugin+0x2344` | **14** | the mask builder was entered, every frame |
| `plugin+0x240c` | **14** | the position compare was reached, every frame |
| `plugin+0x243c` | **0** | **the Position bit was NEVER granted** |
| `plugin+0x2114` | 24 | the slew limiter ran; the flag never latched |
| `0x000417bc` | 12 | SpringBoard discarded the move |

The companion probes are what make this readable: the plugin reaches its
decision fourteen times out of fourteen and answers "did not move" every time.
This is not "never got there", which is the ambiguity that cost three earlier
runs.

#### The measurement that explains it

`plugin+0x2114` was captured 24 times. All twelve first-calls are **byte
identical**:

```text
pc 007d9114  r0 43a00000  r1 00000000  r2 007e0f54  r3 00000000  [sp+0] 3f400000
```

`r0` is `320.0f` and `r1` is `0.0f` — **constant across a 208-pixel drag**.
That is normalised x = 1.0 and y = 1.0, both axes pinned at the surface
maximum. The contact never moves a single pixel, so the 2.0 px dead band at
`plugin+0x21ac` can never be cleared and **no step size could ever have
worked** — which is exactly why 52 px failed identically to 16 px.

#### The cause: eight zero bytes

`_mt_FillMTContactDirectFromBinary` (`0x33cfdac4`) normalises each raw
coordinate as `(v − min) / (max − min)` after clamping through `0x33d00f2c`.
The bounds come from `_alg_InitRowColXYConvert` (`0x33d01010`), which reads the
**second 7-byte record** of the Sensor Region Descriptor — `0x33d01dc8` stores
`blob + 7` as the descriptor pointer:

```text
Xmax = margin + colTable[desc[5] - 1]      colTable[i] = i * 5600 / 11
Ymax = margin + rowTable[desc[2] - 1]      rowTable[i] = i * 3600 / 7
Xmin = colTable[0] - margin        Ymin = rowTable[0] - margin
                                   (both -75; margins are 75, tables start at 0)
```

`core/src/soc/mtz2.c` answers report `0xD0` with **eight zero bytes**, so
`desc[2]` and `desc[5]` are zero and each table is indexed at −1. `Xmax` comes
out **−434** against `Xmin` of **−75**: a *negative span*. The clamp then
returns the maximum for every coordinate unconditionally, on both axes, on every
frame — reproducing the measurement exactly.

The kernel side never reads a field of this blob, which is why eight zeroes
stood for so long. Userspace does.

#### The fix, and why it is not a one-liner

The descriptor must carry the electrode counts the device already reports
through `MTZ2_REPORT_GEOMETRY` — 10 columns, 15 rows — in the two bytes that
index those tables. That makes the span −75..4656 by −75..7275, and
`to_surface()`'s existing 937..4057 normalises to 0.214..0.873: about 16 px per
step, past the 9.45 px slew limit and clear of the 2.0 px dead band. The
Position bit would be granted on the first moving frame.

**But `desc[5]` sits at blob offset 12, so the reply is 14 bytes, and the short
control-read form caps at 11.** `core/tests/test_mtz2.c` catches this
immediately:

```text
Region Descriptor does not fit the short control-read form (14 > 11), so the
driver would use 0xE7 and a longer frame than this model builds
```

So the descriptor change requires the long control-read (report `0xE7`) to exist
first. That is real work, and the change was reverted rather than committed
half-done — the tree is green at 46/46.

**Not established:** what the other five bytes of a region record mean, and
whether `[2]` and `[5]` are literally electrode counts rather than table indices
that happen to equal them for this panel. A one-boot probe at `0x33d010b0` plus
a dump of `grid+0x148..0x14e` would read the four bounds directly and settle it
before the device is changed. (`grid` is `device + 0x168`, from
`33d002a0 add r6, r4, #0x168` in `_mt_InitProcessing`.)

#### A second reader confirmed all of this, and found one thing that may change the fix

Independently re-derived from the dyld shared cache: the base, the store of
`blob + 7` at `0x33d01dc8`, the byte offsets `blob[9]` and `blob[12]`, which
table each feeds, the 75-pixel margins, and the clamp. Three narrowings:

- **`Xmin` above was mis-transcribed** and is corrected in place. The code is
  `table[0] − margin`, not `margin − 75`. Both give −75 here, so no number
  downstream changes.
- The column generator is `(i − grid->0x3c) × 5600 / 11`, not `i × 5600 / 11`.
  The centring term is **zero on this device's path** — `grid->0x3c` is only
  written when `grid->0x20c` is clear, and `_mt_InitM68SurfaceSpecifics` sets
  it — so the arithmetic above is exact, and the projected post-fix
  `Xmax = 4656` survives.
- The clamp returns the **minimum** for `v < min`, not the maximum. "Returns
  the maximum for every coordinate" is broader than the code; it holds across
  the whole realistic input range, which is why it predicts the failure
  correctly, but it is not universal.

**And a live alternative the fix must account for.** `_mt_DefineSurfaceGrid`
back-fills the record itself when `blob[7] == 0`:

```text
33d01dd0  ldrb r3, [r8, #7]        ; blob[7]
33d01dd4  cmp  r3, #0
33d01dd8  bne  #0x33d01e04         ; non-zero -> leave the record alone
33d01df4  strb r1, [r2, #2]        ; desc[2] = (byte)arg1
33d01e00  strb r0, [r2, #5]        ; desc[5] = (byte)arg2
```

So userspace may already intend to supply these two bytes from its own
arguments, and whether that path runs depends on the blob buffer being writable
and on `blob[7]` being zero — neither checkable from the binary alone. If it
does run, the descriptor may not be the right lever at all, and the question
becomes what `arg1`/`arg2` are. **Settle this before changing `mtz2.c`.**

Also worth recording: the platform constants (3600/7, 5600/11, margin 75) are
established for the **M68/default path** of `_alg_InitZephyrPlatformSpecifics`
only. Codes `0x11/0x13/0x20/0x30/0x31/0x60/0x61/0x62/0x70` select different
pitches and margins, and there is no evidence yet of `grid->0x14`'s runtime
value on iPhone1,2.

Also settled for free while pinning the plugin base (`0x007D7000`, five
independent confirmations): the frame timestamp **does** advance at 60 Hz —
0.016, 0.032 … 0.224 s, reassembled from the split `double` in
`forwardContactFrame`'s ABI — and the contact count drops to **0** on liftoff,
so `BreakTouch` is delivered and parsed. Neither was the problem.


### 2026-07-27: run90 — a touch reaches UIKit

**The first time in this project that a synthetic finger has reached the
application.** Not the driver, not the kernel queue, not SpringBoard's vicinity
— `__UIApplicationHandleEvent`, called from GraphicsServices.

run90 restored run85's 3.5e9 snapshot — the machine sitting at the lock screen
— and dragged a contact across the unlock slider from (62, 432) to (270, 432)
in eight steps, with six probes spanning the whole delivery chain.

| probe | mode | captures | meaning |
|---|---|---|---|
| `0xc043d6b8` | kernel | **16** | every event enqueued to userspace |
| `0xc043d560` | kernel | 0 | nobody newly subscribed |
| `0xc043d57c` | kernel | 0 | **nobody unsubscribed** |
| `0x33cfb3ec` | user | **17** | frames crossed into userspace |
| `0x33cfdee0` | user | 9 | reached the MultitouchHID plugin |
| `0x324f6edc` | user | **2** | **reached UIKit** |

The kernel dropped nothing, and the correlation is exact — 8 downs and 8 ups,
every one enqueued ~43,000 instructions after its scheduled moment:

```text
@3520043350  @3523043403  @3526042968  @3529043038
@3532042997  @3535042968  @3538042959  @3541042997     <- the 8 downs
@3544042971  @3547042968  @3550043017  @3553042980
@3556042959  @3559042997  @3562042988  @3565042968     <- the 8 ups
```

Both `__UIApplicationHandleEvent` captures arrive via `lr 335067e4`, the `blx`
inside GraphicsServices' `PurpleEventCallback` — one 643k instructions after
the first finger-down, one 77k after the finger-up.

`0xc043d560` reading zero is not a contradiction: the snapshot restored a
machine whose client had already subscribed before 3.5e9. `0xc043d57c` reading
zero is the load-bearing one — **nobody unsubscribed at any point.**

#### Two theories die here

**"Nothing is listening"** is finished. It was already retracted on the
strength of run77's single enqueue; run90 buries it with 16 enqueues, 17
userspace handles and 2 app deliveries.

**The `UILocked` gate is not blocking delivery to the app.** MultitouchHID
drops frames while that flag is set and it initialises to 1, which was the
leading reason to expect an injected swipe to vanish. Events reached UIKit
anyway. Whatever that gate does here, it is not swallowing the gesture.

#### It did not unlock, and the reason is ours

The final frame is **273,206 of 460,800 non-zero bytes — byte-identical to
run85.** Nothing moved.

UIKit received a **tap**, not a **drag**. The finger-down arrived and the
finger-up arrived; *none of the seven intermediate positions became an
application event*. Slide-to-unlock needs the movement between them, so a tap
on the slider is correctly ignored.

The cause is a defect in our harness, confirmed by reading it rather than
inferred from the funnel — `tools/bootkernel.c`, `touch_tap_step()`:

```c
c.phase    = t->stage == 0u ? MTZ2_PHASE_MAKE_TOUCH : MTZ2_PHASE_BREAK_TOUCH;
c.pressure = t->stage == 0u ? 160u : 0u;
```

with `c.id = 1u` for every tap. **`MTZ2_PHASE_TOUCHING` — value 4, "still
down, possibly moving" — is never emitted by the harness anywhere**, though
`s5l_mtz2_set_contacts()` validates and stores it and the device model has
supported it since the phase encoding was written.

So eight `--touch` points are not a drag and never could be: they are eight
independent *"finger just landed"* reports for the same contact id. A HID stack
that coalesced or discarded seven of them was behaving correctly. No amount of
geometry tuning would have fixed this, and the 16/17/9/2 funnel is what a
correct stack looks like when it is fed a malformed gesture.

The fix is a drag primitive that emits `MAKE_TOUCH`, then `TOUCHING` for every
intermediate report, then `BREAK_TOUCH`, as one gesture on one contact id.

#### What run90 settles for the record

- The device→driver→kernel→userspace→plugin→SpringBoard→GraphicsServices→UIKit
  chain **works end to end.**
- The remaining failure is in **gesture semantics, in our harness** — not in
  the emulator core, not in the kernel, and not in a missing subscriber.
- One caveat on provenance: run90 used run85's archived binary, which predates
  the per-run capture fix, so its frame went to the shared `firmware/screen.ppm`.
  The byte count is identical to run85's, so nothing is ambiguous here, but the
  capture is not independently owned.


### 2026-07-27: run85 — the lock screen

![the lock screen](images/run85-lock-screen.png)

Status bar with "Searching…", the lock glyph and the battery. 4:00, Wednesday
31 December. The Earth wallpaper. **slide to unlock.** iPhone OS 3.1.3,
composited by Apple's own software renderer, on an emulator written from
scratch.

```text
framebuffer: CLCD window 0, 320x480, 273206 of 460800 RGB bytes non-zero
```

against run76's 1,659 on the same activated configuration. 92,145 non-black
pixels — **60% of the screen** — spanning x 0..319, y 3..457, in **44,087
distinct colours**, of which **53,620 pixels are not grey**. run76's frame was
553 grey pixels in a 175x255 corner. This is not a brighter spinner; it is a
different kind of thing.

#### What changed: one constant

`/vram` went from one framebuffer to two. Nothing else.

The display pipeline was never broken, and §23.11 is the measurement that
settled it: 289 VBLANK interrupts, 150 swaps committed, every submitter woken
by `commandWakeup`, hardware-interrupt-driven on a work loop. It was
compositing correctly the entire time. It had ONE surface to composite,
because `IOSurfaceDeviceMemoryRegion::init` hands the region's whole length to
`IORangeAllocator::withRange` (0xc0527c04) and `AppleH1CLCD` asks for
`round_up(1280*480, 0x1000)` = 0x96000, which is exactly what `/vram` held.
The first surface consumed the pool; every later one returned
`kIOReturnNoResources`.

A compositor with one surface can draw a spinner. A wallpaper *and* the chrome
over it needs two.

This is the same bug as `reg = {0,0}` (691b727), one layer further in. That
fix made a first frame possible. This one is why nothing followed it.

#### Still not the home screen, and three other things this does not show

**It is the LOCK screen.** The next step is a swipe, and no tap has yet reached
SpringBoard — run77 proved the driver reads our frames, not that anything
above it does.

**One allocation still fails.** run85's console carries one remaining
`IOSurface warning: buffer allocation failed. 320 x 480 fmt: 42475241 size:
614400 bytes` — `0x42475241` is `'BGRA'`. `AppleH1CLCD`'s layer table has
three entries (layer 0 -> window 0, layer 1 -> window 2, layer 2 -> the video
overlay), so three looked like the real number. **run86 tested that and it is
wrong** — see the next entry. The pool is not what refuses the surface.

**The clock is wrong on purpose-ish.** 4:00 on 31 December 1969 is the RTC
answering with a placeholder. Cosmetic, but it is a modelled device returning
a value nobody has connected to anything.

**"Searching…"** is the status bar correctly reporting no baseband. The
baseband is deliberately hidden from the guest.

#### The capture was nearly lost, and the reason is a live hazard

`firmware/screen.ppm` is a single shared path, and a concurrent short boot
overwrote run85's frame **45 seconds after run85 exited** — run85's log is
stamped 16:35:38, the PPM 16:36:23. The recovered image above came from
run85's own 3.5e9 checkpoint, and it is trustworthy for one specific reason:
its non-zero byte count is **273,206**, matching what run85 reported to the
byte. Without that check the analysis would have been of another run's console
glyph. Any run that writes a PPM while another is running is producing
evidence it does not own.


### 2026-07-27: run86 — three surfaces is *worse* than two

run86 is run85 with `N82_VRAM_SURFACES` at 3 and **nothing else changed**: same
6e9 instruction cap, same flags, same clean build, `/vram reg` widened from
`{0x0885c000,0x0012c000}` to `{0x0885c000,0x001c2000}`. The prediction — written
into the source *before* the run, which is the only reason it is falsifiable —
was that a third surface would clear the last `'BGRA'` failure, because
`AppleH1CLCD`'s layer table has three entries.

It did the opposite.

|                     | run85 (2 surfaces) | run86 (3 surfaces)   |
|---------------------|--------------------|----------------------|
| guest console       | 6,571 bytes        | 17,821 bytes         |
| allocation failures | **1**              | **122** (12,709 B)   |
| non-failure output  | ≈6,480 bytes       | 5,112 bytes          |
| rendered frame      | 273,206 / 460,800  | 273,206 / 460,800    |

The frame is byte-identical. The 122 failure lines account for 12,709 of
run86's 17,821 console bytes — *more* than the entire 11,250-byte increase over
run85 — so stripping them leaves run86 with about 1.4 KB **less** real output
than run85 produced. The third surface is therefore never successfully handed
out. Widening the pool only bought some client the chance to ask again and
fail, apparently once per composite.

Both runs also print exactly one `IOSurface: buffer allocation size is zero`,
so that line is a constant of this configuration and not part of the change.

One of run86's 122 lines is 1,818 bytes rather than 89: a bare `CR` followed by
1,729 `NUL` bytes before the message. That is a serial-console padding artifact,
it is counted above, and it is not otherwise explained here.

#### What this rules out, which is the point

"The pool is too small" is no longer available as the explanation for the
surviving failure. Whatever refuses the next surface is **not short of bytes**.

The open question therefore changes from *how many surfaces* to *which client
is asking*. Static disassembly settled the rest the same afternoon, without a
second boot, and it eliminated every candidate we had:

- **No off-by-one.** The range is *inclusive*. `withRange(L-1)` yields the one
  element `[0, L-1]` — exactly `L` bytes. `IORangeAllocator::init` calls
  `deallocate(0, endOfRange + 1)` at `0xc0194824`, `getFreeCount` sums
  `end + 1 - start` at `0xc019469c`, and the kext itself asserts
  `getFreeCount() == getLength()` for an idle region at `0xc0527a30`. The
  admission test at `0xc0194bb2` gives `N(L) = floor(L / 0x96000)` exactly.
  **Every pool we ever configured was sized correctly.**
- **No fixed free-list capacity.** `withRange` gets capacity `0`
  (`0xc0527bec`); `init` turns that into `capacityIncrement 1` and
  `allocElement` grows the array via `_IOMalloc` at `0xc0194700`.
  Fragmentation and element exhaustion are both out.
- **No retry in the kernel.** `IOSurface::allocate` has exactly one reference
  in the entire kernelcache — vtable slot `+0x70` at `0xc052e048` — reached
  from one guarded, non-looping call site at `0xc0526108`, and
  `IOSurfaceRoot::createSurface` releases and returns `NULL` on failure with
  every branch forward.

So **122 failure lines are 122 distinct `createSurface` calls**, and the only
sites that issue them (`0xc052a914`, `0xc052aae8`, `0xc052ac44`) take an owning
task and a client-supplied dictionary: they are `IOSurfaceRootUserClient`
external methods. **The loop is in userspace**, one `IOConnectCall` per line.
Why that client asked once when one surface was free and 122 times when two
were is *not determined*, and nothing in this kernel encodes it.

#### The correction that matters most: `AppleH1CLCD` was never the caller

We had it backwards, in this document and in the source comment. `AppleH1CLCD`
**does not call `IOSurface::allocate` and structurally cannot emit a "buffer
allocation failed" line.** At `0xc0705f00` it builds a dictionary carrying only
`IOSurfaceIsGlobal` and **no geometry**, so `init` computes size 0, logs
`IOSurface: buffer allocation size is zero` (`0xc0525090`), and the guard at
`0xc05260c0` skips `allocate` altogether — which is exactly why every run,
run85 and run86 alike, prints that line precisely once.

It then sets `size = pitch * height = 0x96000` by hand
(`0xc0706064`–`0xc0706078`), looks the region up **by name** at `0xc07060a0`,
and takes a block directly at `0xc07060bc`. That allocation is **silent**, and
its failure path at `0xc07060c4` falls back to a physical address from its own
mode table (`0xc0706210`). The display therefore gets a scanout buffer whether
or not the pool had room — **which is why run85 and run86 rendered
byte-identical frames.**

One block is consumed before any client sees the pool:

```text
N_client(L) = floor(L / 0x96000) - 1
```

run85 left userspace **one** surface; run86 left it **two**. The "three layers,
so three surfaces" reasoning that motivated run86 was never verified either:
`0xc0705f00` creates exactly one surface per invocation, selecting one of four
parameter sets, with no loop over any layer table.

#### Why the next increment is still the wrong experiment

No region length can be read off statically, because the count is set entirely
by a userspace client. Four probes settle it in a single boot, at PCs where the
operands are live:

| PC | what it proves |
|---|---|
| `0xc0527ae4` | every `/vram` allocation — `r1` size in, `r0` success out, including the silent CLCD one |
| `0xc0526108` | every `IOSurface::allocate`, i.e. every loud client attempt |
| `0xc05244e4` vs `0xc0524528` | pool exhausted, or `withOptions` returned NULL |
| `0xc07060c0` | whether `AppleH1CLCD` got its block or took the fallback |

A caveat that follows from the same read: the `iommu-present` gate at
`0xc05244e4` is only *reached* when the surface has a non-empty region list. A
surface with `[+0x90] == 0` goes straight to `withOptions` at `0xc05244f0`, so
**not every failure line means the pool was exhausted.**

The default is back to **two**, the only configuration measured to produce the
lock screen with a quiet console.

#### A second shared-path hazard, found the same way as the first

The near-loss of `firmware/screen.ppm` is recorded in the previous entry. The
console tee had exactly the same defect, and it was not noticed until it bit:
`uart-console.log` is one fixed relative name, so **run86 overwrote run85's
complete console**.

run85's stream survived only by luck — all 6,571 of its bytes fit the 8 KiB
in-report buffer, so the full text was already inside its own stdout log.
run86's did not fit. 9,630 of its 17,821 bytes lived in that one shared file and
nowhere else, and every number in the table above depends on them.

Under `--external-md` the tee now also resolves to `<work>.uart-console.log`,
unique per run by construction because the harness refuses to reuse a work
image. A run without `--external-md` keeps the shared name, so existing recipes
and `tools/run23-cold-replay.ps1` still find their file where they expect it.

**The general rule, now twice paid for:** any output written to a fixed path is
evidence that belongs to whichever run finished last, not to the run that
produced it.


### 2026-07-27: run80 — the guest transmits PPP. **S0 is met.**

```text
Wed Dec 31 16:00:06 1969 : Using interface ppp0
Wed Dec 31 16:00:06 1969 : Connect: ppp0 <--> /dev/tty.debug

=== UART4 / PPP ===
    UTXH bytes written by the guest   47
    *** MILESTONE: LCP Configure-Request on uart4 ***
        7E FF 7D 23 C0 21 at stream offset 0, instruction 739413052
```

The milestone was stated before any of this was built and it is met exactly,
byte for byte. The whole 47-byte stream is one complete frame:

```text
7e              HDLC flag
ff              all-stations address
7d 23           escaped 0x03, UI control
c0 21           protocol 0xC021, LCP
7d 21           escaped 0x01, Configure-Request
7d 21           identifier 1
7d 20 7d 34     length 0x0014 = 20
7d 22 7d 26  7d 20 7d 20 7d 20 7d 20     option 2, ACCM = 0x00000000
7d 25 7d 26  79 61 f5 7d 3c              option 5, magic number 0x7961f51c
7d 27 7d 22                              option 7, protocol-field compression
7d 28 7d 22                              option 8, address/control compression
51 7d 39        FCS
7e              closing flag
```

Every escape is `0x7D` followed by the byte XOR `0x20`, as RFC 1662 4.2 says,
and the options are the four pppd 2.4.2 asks for by default. `pppd` did not
exit: no `_exit1` was observed for it at all, against run74/75/78 where it died
at 739,184,188. It is sitting in its negotiation loop waiting for a reply,
which is exactly where a peer-less LCP Configure-Request should leave it.

#### What actually fixed it, and what did not

Nothing in the emulator changed. The device model, the device tree and the
kernel are the same bytes that failed four runs in a row. What changed is one
argument: `/dev/uart.debug` became `/dev/tty.debug`.

`/dev/uart.debug` is real, it opens, and `tcgetattr`/`tcsetattr` work on it —
which is why it looked right for so long. It is not a tty.
`AppleOnboardSerialBSDClient::start` registers its cdevsw at `0xc0478060` with
`d_ttys = NULL` and `d_type = 0` rather than `D_TTY`, and the ioctl switch at
`0xc046fd52` has arms for `TIOCGETA` and `TIOCSETA*` but none for `TIOCSETD`
or `TIOCSCTTY`; both fall to `movs r0,#0x19` at `0xc046fe30`, which is ENOTTY.
The path cannot reach `ttioctl` at all — `_ttioctl`'s Thumb pointer
`0xc01368a9` occurs exactly once in the whole 7.9 MB kernelcache, at
`0xc0469430` inside IOSerialFamily, and `AppleOnboardSerial` never references
it — so `linesw[PPPDISC]` was never going to be consulted.

`/dev/tty.debug` is published by `IOSerialBSDClient` and is a real BSD tty.
That it existed was **not** proven when the change was made; the argument was
chosen because pppd probes it for free, exiting 2 with a distinct message if
the node is absent. It was there.

#### The four runs this cost, and why

Worth recording, because three of them measured nothing and each failure was
a different kind:

| run | what it was for | why it told us nothing |
|---|---|---|
| run73 | first `--ppp` boot | died at its 700e6 cap before `connect_tty` |
| run74 | reach the failure | launchd sent the job's output to `/dev/null` |
| run75 | route the message | `StandardErrorPath` — pppd writes to fd **1** |
| run78 | route it correctly | worked: ENOTTY, twice |

run75 is the instructive one. The key was not ignored and launchd was not at
fault; it named a stream `pppd` does not use. `error()` and `fatal()` share one
emitter at `0x0002245c` which writes to `*log_to_fd`, and `_log_to_fd` at
`0x00039c70` has a file image of **1**. Naming the wrong descriptor cost
exactly as much as omitting the key, and looked identical from outside.

The other retraction: `r3 = 0x00039c30` at the exit syscall was read as a text
address and proposed as a probe target. `__cstring` ends at `0x35d9e`;
`0x39c30` is `__DATA,__data`, and it is `fd_ppp`. That probe would have
captured a variable.

**S0 is met and S0 is not networking.** The guest is talking to nobody: there
is no host-side PPP endpoint, so nothing answers the Configure-Request and no
address is negotiated. What is proven is one direction of one link layer.
`docs/networking.md` says what this route is and is not, and it says
temporary.


### 2026-07-27: run77 — the guest reads a touch frame, and accepts it

run71 delivered a report the guest never read. The attention line came up, the
interrupt routed, and then `INTSTAT` group 4 at `0x39a000b0` logged
**1,193,122** read/write-back pairs at ~419-instruction intervals for the rest
of the run, with `length-reads 0` and `data-reads 0`. The GPIO pending latch
was level-sensitive, so the guest's own write-one-to-clear re-asserted the bit
from the still-driven line inside the same store: every acknowledge undid
itself, `IOWorkLoop::signalWorkAvailable` ran every time, and the work-loop
thread that would have issued the SPI read was never scheduled. A livelock, not
a lost interrupt (`5932755` makes the latch edge-triggered).

run77 is the same run with that one change:

```text
tap 0  at 1300000000   (160,240) hold 24000000    down @1300000000 up @1324000000  refused 0
tap 1  at 1550000000   (160,240) hold 24000000    down @1550000000 up @1574000000  refused 0
device: queued 4  length-reads 4  data-reads 4  read 4  refused 0
pins:   reset-edges 5  reset-bytes 48  power-edges 3  power-level 1  in-reset 0  hbpp-answered 1
```

and `INTSTAT` offset `0x0b0` falls to **8 reads, 5 writes** for the whole boot,
four of each landing between instructions 1,300,000,114 and 1,574,001,063 —
one acknowledge per report, which is what the driver's own usage implies (it
writes that group's `INTEN` exactly twice in a boot, so it never masks the line
while servicing it).

The load-bearing probe is `0xc04413e8`, **captured 4**. It is reachable only
via the `beq` after `cmp r5,r0` at `0xc04413b4` — the comparison of the
driver's computed payload checksum against ours — and every capture shows
`r0 == r2` (`0x2a1`, `0x214`, `0x2c3`). **The driver accepted the frames.** The
0xCC frame encoding was derived by reverse-engineering and had never been
confirmed against the real parser; it is confirmed now.

`0xc043d684` (`IODataQueue::enqueue`) captured **1**, at instruction
1,113,171,698 — before either tap, so not ours. The three SpringBoard user
probes stayed at zero, as the manifest predicted they would.

> **Retracted 2026-07-27.** The paragraph that stood here read that single hit
> as "no subscriber yet", and concluded *"nothing in this run had a
> subscriber."* **That is backwards, and it inverted the meaning of the one
> positive measurement in the run.**
>
> `0xc043d684` is referenced by exactly one word in the entire binary
> (`0xc043c3d0`) and called from exactly one instruction — `blxne r3` at
> `0xc043c390` — which is gated on `cmp r0,#0` against the slot pointer **and**
> `tst r3,#1` against the flags. It therefore **cannot execute unless some slot
> simultaneously held a non-null client with bit 0 set.** It fired. So at
> instruction 1,113,171,698 a userspace client *was* subscribed.
>
> "No userspace subscriber exists" has been this project's leading explanation
> for why no tap reaches SpringBoard. It is not supported by this run, and it
> should not be repeated. Candidate explanations that remain live, and that the
> probe set below distinguishes: the client subscribed and then closed or
> unsubscribed (`0xc043d57c` / `0xc043d430`); or `setNotificationPort` was never
> called, so the enqueue succeeded silently with no mach wakeup
> (`0xc043df94`).

What this run does establish is the device half: the driver reads our frames
and accepts their checksums. What it does **not** establish is that a tap
reaches SpringBoard — the final frame is the boot spinner at 1,830 of 460,800
non-zero bytes, and the userspace half is untested.

#### The probe pair that bisects the chain

The full path is now mapped end to end, all ARM mode, read from `LC_SYMTAB`
and disassembly:

```text
kernel IODataQueue -> MultitouchSupport (dequeue) -> _mt_HandleMultitouchFrame
  -> _mt_ProcessPathFrame -> _mt_ForwardBinaryContacts -> MultitouchHID.plugin
  -> IOHIDEventSystem -> SpringBoard -> GSSendEvent -> mach_msg to the app's
     Purple port -> PurpleEventCallback -> _UIApplicationHandleEvent
```

| PC | mode | what it proves |
|---|---|---|
| `0xc043d6b8` | kernel | `enqueue` was called; `r0` = 1 success, 0 queue-full |
| `0x33cfb3ec` | user | `_mt_HandleMultitouchFrame` — a frame crossed into userspace |
| `0xc043d560` | kernel | userspace *enabled* frame delivery (selector 0) |
| `0xc043d57c` | kernel | userspace **un**subscribed |
| `0x324f6edc` | user | `__UIApplicationHandleEvent` — the touch reached the application |

`0x33cfb3ec` is the single best probe: it sits at the kernel/userspace
boundary, so one bit bisects the chain optimally, and it is reached from
**both** dequeue paths so it cannot be missed by guessing the wrong one.
Paired with `0xc043d6b8` the diagnosis is mechanical — kernel fires and user
does not means the loss is precisely in the notification/mach/run-loop leg;
neither fires means the loss is in the kernel, and `0xc043d560` then says
whether anyone had subscribed at all.

Two hazards worth recording before they cost a run:

- **`MultitouchHID.plugin` addresses cannot be probed.** It is `MH_BUNDLE`
  with `__TEXT vmaddr = 0`, loaded at a dyld-chosen base by
  `IOCreatePlugInInterfaceForService`, so its file offsets do not convert to
  absolute VAs. Any list offering them as probe PCs is wrong; use
  `0x33cfdee0` (the `blx` inside `_mt_ForwardBinaryContacts`) instead.
- **`IOKit.framework` appears twice in this cache**, at `0x31464000` and
  `0x32299000` — two complete copies. Probing anything in IOKit means setting
  **both** PCs, because only one copy is bound into a given process.

And a gate that may block the slide-to-unlock experiment outright:
`MultitouchHID` drops every frame while `UILocked` is set, and that flag
**initialises to 1** (`docs/AGENT_HANDOFF.md` §23.4d — repo-recorded, not
re-verified). The probe pair is what tells us whether that is what swallows an
injected swipe, rather than guessing.


### 2026-07-26: runs 58-59 — the guest drew a frame

![the first frame](images/run59-first-frame.png)

The first pixels this project has ever produced. `/device-tree/vram` ships with
`reg = {0,0}`; real iBoot fills it in and we never did. One `dt_set_reg` call in
the block already labelled "device-tree patches (iBoot would have done these)"
turned every measurement around.

**run58-vram**, cold, cap 350e6, seven kernel probes — the cheap half of the
test, because the whole decision chain completes before ~240e6:

```text
dt: /vram reg {0x00000000,0x00000000} -> {0x0885c000,0x00096000}

@178085168  c0527b78  r0 c0bd1d00   waitForService("vram")      nub FOUND
@178085354  c0527b94  r0 c0c3da00   getDeviceMemoryWithIndex(0) range RESOLVED
@240093517  c07060a4  r0 c0b59740   copyMemoryRegionWithName    PurpleGfxMem EXISTS
@240093770  c07060c0  r0 00000001   region->allocate(surface)   SUCCESS
            c0706214            captured 0   <- kIODirectionOut fallback NEVER RAN
```

Both risks flagged before the run cleared. `/vram` is a root-level node rather
than an `/arm-io` child, so whether the platform expert would publish a nub named
`"vram"` for `waitForService` to find was genuinely open: it does. And the region
is sized to exactly one surface, `0x96000`, because that is all that is reserved
below `topOfKernelData`: `allocate` returned 1, so it was enough.

**run59-vram-render**, cold, cap 5e9, exit 0:

```text
live CLCD scanout: 16,092,611 overlapping writes; 4,773,941 changed writes,
                   14,264,987 changed bytes (14,264,987 RGB-visible)
framebuffer: 97,510 of 460,800 RGB bytes non-zero        (every prior run: 384)
wrote firmware/screen.ppm - live CLCD frame is NONBLACK
```

`FAR 0x00621000` does not appear anywhere in the log. `SpringBoard exact-path
attempts: 1`. `CGBlt_fillBytes` was called **61,289** times against 510 in
run57 before it died. `SBUIController:window-created` and `orderFront-call` each
hit once with `nil-bailout` at zero, and `CABackingStoreUpdate` fired 102 times
with its last at **@4,973,034,198** — SpringBoard was still compositing when the
cap stopped it, not limping to a halt.

What is on screen is the **activation** UI, not the home screen: `lockdownd.log`
records `The original activation state is Unactivated`, and SpringBoard draws the
iTunes-connect prompt and emergency-call slider accordingly. That is the correct
frame for this guest to be showing. It is drawn entirely by the guest —
QuartzCore's software compositor, CoreGraphics' rasteriser, our CLCD scanout —
with nothing supplied by the host.

Note the timeline moved. SpringBoard spawns at 579,408,027 here against
3,249,787,435 in run52, because the vram region changes early IOSurface work; do
not compare instruction indices across the fix.

**run54 is the negative control, and it arrived by accident.** It was launched
before the vram fix existed and ran on to its 22e9 cap afterwards, which makes
it the counterfactual the fix would otherwise lack:

| | run54, no fix | run59, with fix |
|---|---:|---:|
| instructions | **22,000,000,000** | 5,000,000,000 |
| SpringBoard launches | **33** (first @584,535,413, last @21,805,188,984) | **1** |
| `SBUIController:orderFront-call` | 1 | 1 |
| `QuartzCore:CATransaction-flush` | 3 | 3 |
| `QuartzCore:CABackingStoreCreate` | 31 | 36 |
| **changed scanout bytes** | **0** | **14,264,987** |
| `postrun_screen_sha256` | `CBAD1C11…` (seed) | NONBLACK |

Four times the runtime, thirty-three SpringBoard generations, the compositor
reaching `CATransaction-flush` and building 31 backing stores — and not one
pixel. So the blank screen was never insufficient runtime, never a timing
artifact, and never a question of patience. One device-tree property separates
these two runs and it is the whole difference. It also confirms the crash loop
was continuous rather than an early-boot transient: 33 generations spread across
21 billion instructions, each dying the same way, which is exactly what a
read-only framebuffer mapping predicts.

`FAR 0x00621000` does not appear in run54's abort table, but that is the
48-entry saturation artifact described above and not evidence of anything — its
binary predates the 65,536-entry table.

One `_exit1 status=0000000a` remains, at @1,595,733,254, pid 32 — before any of
SpringBoard's UI work, and `post-activation entry-proc/PID _psignal/_exit1
entries: 0/0` says it is not on the SpringBoard path. Unidentified, recorded, not
guessed at.

### 2026-07-26: runs 52-56 — the MBX crash loop is broken, and two blockers remain

The `CA_ENABLE_MBX2D=0` fix that runs 41-50 identified from disassembly has now
been run, and it works. It also moved the failure far enough forward to expose a
second, independent problem that had been hidden behind the first.

`run52-fastpath-swrender` is the cold boot of record: 12,000,000,000 retired
instructions, `--ca-software-render`, USB OTG matched, exit 0 with `OK`. It wrote
`run.snapshot-3000000000` (sha256 `99F7087C…`), which runs 55 and 56 restored
from; both therefore inherit the rewritten SpringBoard plist through the snapshot
and record `ca_software_render: false` in their own manifests, which is
provenance rather than a contradiction.

What the fix bought, from run55: `SpringBoard exact-path attempts` fell from
**30 to 1**, `SBUIController:nil-bailout` recorded **0** hits, and
`SBUIController:orderFront-call` recorded its **first hit ever** at
3,470,018,203. SpringBoard constructed its UI controller, created its window and
made it visible. `CATransaction-flush` 2, `CABackingStoreCreate` 31.

Run56 carried the 65,536-entry abort table from `5bef7cb` and caught the fault
the old 48-entry table had been dropping:

```text
@3,641,884,794  DATA  FAR 0x00621000  FSR 0x0f  pc 0x338f64f4
@3,641,885,715  _exception_triage        (vm_fault refused; only hit in the run)
@3,641,911,380  _exit1 status 0x0a       (SIGBUS)
```

`0x338f64f4` is CoreGraphics `_CGSFillDRAM8by1+0x10c`, the instruction
`stm fp!, {r1,r2,r3,r4,r5,r6,r8,sl}`. All eight source registers hold
`0xff000000`: SpringBoard is clearing a surface to opaque black. The backtrace is
29 QuartzCore frames below `IOMobileFramebuffer`, with the four-deep sublayer
recursion visible in it — the entire compositing pipeline runs.

The guest corroborated this itself. `ReportCrash` wrote two reports for two
SpringBoard generations, both `EXC_BAD_ACCESS (SIGBUS)` /
`KERN_PROTECTION_FAILURE at 0x00621000`, and both had to be **carved from
unreferenced disk blocks**: the work image is unjournaled and was never cleanly
unmounted, so the data reached the platter while the catalog insert did not. Any
future search for guest-written evidence should carve as well as walk the
catalog. `/private/var/mobile/Library/Logs/AppleSupport/general.log` shows the
same story — 2 entries on disk behind a stale 146-byte catalog size.

**The destination argument was wrong; nothing overran.** `r9` is not touched
inside the fill loop and `fp` is copied from it once outside, so `r11 == r9`
proves zero iterations completed, and the stack arithmetic agrees exactly
(`r7 = 0x7b2c90` ⇒ entry `sp 0x7b2c98`, less `0x10`, less both pushes = the
reported `0x7b2c40`). CoreGraphics wrote no bytes. Note for anyone reading these
registers later: `lr` is loaded from the per-row width each row, so `lr = 0x500`
means the fill is 1280 bytes *wide*, not that 1280 remain — a reading that looked
compelling and was wrong.

Two hypotheses are excluded rather than merely disfavoured. Our MMU: `ap_permits`
matches DDI0100I for all eight APX:AP encodings, APX is read from bit 15 for
sections and bit 9 for pages, DACR is implemented, and there is **no TLB at all**
— every access re-walks the guest tables, so a stale permission is
architecturally impossible. Copy-on-write: the fault has `n=1`, and a
failed-then-retried upgrade would re-execute the same instruction and climb
without bound. The kernel refused once. For contrast the same instruction had
already filled `0x038c0000`-`0x03955000`, exactly `0x96000` bytes, 54,000
instructions after a kernel IOSurface was created with 320/480/1280/'ARGB'/
`0x96000` — so both the routine and the geometry we supply are correct.

**The second blocker: a surface is created successfully and still nothing
appears.** `AppleH1CLCD::createSurface` fires all five of its checkpoints in
run52 at roughly instruction 238,400,000, with a fully populated descriptor —
`+0x58=0x140`, `+0x5c=0x1e0`, `+0x60=0x500`, `+0x6c='ARGB'`, `+0x74=0x96000` —
and `H1:createSurface-return r0=00000001`, which is success. Yet 12 billion
instructions later run52 still reports `0 changed writes, 0 changed bytes` of
live scanout and `postrun_screen_sha256 CBAD1C11…`, the pre-guest seed. So the
open question is not "why was no surface made" but "why does a successfully
created surface never reach the panel", and the candidates are distinct: the
surface may not sit over `0x0885C000`; the compositor may be drawing into a
different mapping (the fatal composite targeted `0x00621000`, the earlier
successful one `0x038c0000`); or our scanout tracker may be watching the wrong
range, in which case `0 changed bytes` is a measurement failure and not a
rendering one. `CoreSurface:BufferCreate-entry` is `hits=0` in all three runs
including run52, so "the surface does not come from `CoreSurfaceBufferCreate`"
does survive.

**Restore is bit-exact; the run52/run55 disagreement was reporting, twice over.**
An earlier draft of this entry recorded that disagreement as open and asserted
that `createSurface` never ran. Both were wrong, from the same root cause, and
the correction is worth more than the original claim.

The execution is identical: 20 of 20 heartbeats match between run52 (cold) and
run56 (restored) at every 100M from 3.0e9 to 4.9e9, in pc *and* mode; the 21-line
SpringBoard spawn-probe block is byte-identical across all three runs, same TTBR0
`0dfec018`, same thread `e069b554`, same 444,736 pointer-activity entries; and
agreement continues to `_psignal @3,860,606,119` and `syscall 37 kill
@3,862,108,105`, **862 million instructions past the restore point**. That holds
across three different binaries, so the VIC1/WFI refactor in `10c5cd3` was
behaviour-neutral as its commit claimed. `tools/bootkernel.c` is in fact
byte-identical between run52's and run55's builds, so no checkpoint index moved.

Two mechanisms produced the illusion. First, the SpringBoard trace block is
per-generation and `memset`s itself on every respawn
(`tools/bootkernel.c:13695-13697`), keeping only the generation counter: run52's
report describes **generation 6, PID 46**, while run55/run56 describe
**generation 1, PID 36**. Different processes. Nothing warns that generations 1-5
were discarded. Second, the `-n 12000000000` cap truncated generation 6, which
armed at 11,834,133,519 with 165,866,481 instructions of budget. Normalising each
checkpoint to its own block's arm point separates perfectly: everything gen 6
reached is ≤ +150,991,263, everything reading `hits=0` is ≥ +244,691,624. Gen 6
was running the same sequence *faster* than gen 1 and simply ran out of room.

The rule this violates is already written down, in AGENT_HANDOFF §17, added after
the same trap was retracted the first time. This was the third occurrence. Two
consequences follow. Compare the heartbeat pc stream **before** reading any
divergence into differing checkpoint counters — it closes the question in one
step. And a host-side counter is not machine state: `core/include/snapshot.h`
says restored runs start those counters fresh, so a zero in a restored run means
"not seen since the restore point", never "never happened". Reading a restored
run's zero as a whole-run zero is exactly how the `createSurface` error above was
made.

The durable fix belongs in the tool rather than the docs:
`springboard_exec_trace_report()` should print how many generations it discarded
with each one's arm and exit instants, and any block whose counters were not
restored should carry the "restored baseline" banner the AppleBaseband trace
already prints. Not yet implemented.

Two smaller corrections to the same draft. Run56 did **not** die: its emulator
exited 0 and completed all 5e9 instructions; the launcher's exit 99 was a
postflight check failing on the then-missing `firmware/kernel.macho`. And the
distinct-abort tables in run52 and run55 both saturated at 48 entries, so the
fatal fault line appears only in run56 — its absence elsewhere is a table-cap
artifact and not evidence.

### 2026-07-26: runs 41-50 — SpringBoard was never rendering because it is dead

Ten more runs were retained chasing the missing drawing surface down the
CoreAnimation ladder:

```text
run41-uicontroller-return   run46-full-ladder
run42-surface-usbmatched    run47-ladder-fast
run43-surface-deep          run48-past-ctrl
run44-fastpath-render       run49-render
run45-render-ladder         run50-sbui-bisect
```

Every one of them ended with the same framebuffer:

```text
SHA-256 CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB
```

That hash has been identical in every run since run35, and the reason is not
that the boot keeps stopping just short. **Nothing was ever going to draw.**

#### The guest had been writing crash reports the whole time

The guest's own `ReportCrash` had been writing to the rootfs work images all
along. **Thirty-five** SpringBoard crash reports were extracted from them, into
`work\analysis\crashes\` — 32 from run40, 3 from run46. They are byte-identical
in the crashed thread:

```text
Process:         SpringBoard [20]
Path:            /System/Library/CoreServices/SpringBoard.app/SpringBoard
Parent Process:  launchd [1]
OS Version:      iPhone OS 3.1.3 (7E18)

Exception Type:  EXC_BAD_ACCESS (SIGBUS)
Exception Codes: KERN_PROTECTION_FAILURE at 0x00000048
Crashed Thread:  3

  pc: 0x30e1ea50   lr: 0x3123d928   cpsr: 0x60000010   sp: 0x007b75d4
```

Thread 3, symbolicated innermost first, is the CoreAnimation render thread
running off the IOMobileFramebuffer vblank callback:

```text
_mbx2DDisable+0x20
CA::RenderMBX2D::render
render_display
CA::WindowServer::MBXServer::render_update
CA::WindowServer::Server::render_for_time
IOMFBServer::link_callback
IOMobileFramebufferNotifyFunc
IODispatchCalloutFromCFMessage
CFRunLoopRunSpecific
IOMFBServer::link_body
_pthread_body
```

SpringBoard is in a `launchd` crash/respawn loop, dying roughly every **470
million instructions**. One run recorded **30 exec attempts and 29 deaths across
13.7e9 instructions**. Thread 0 spends that time blocked in
`semaphore_signal_trap` under `CA::Render::Context::did_commit` <-
`+[CATransaction flush]` — which is precisely why
`SpringBoard:UIApplicationMain-return` reads `hits=0` in every run, and why
`IOSurface:create-entry` never fires.

#### The faulting instruction stores through a NULL the caller never checked

```text
30e1ea3c  ldr    r3, [r3]          ; r3 = *(0x381200d8) = NULL (MBX2D global ctx)
30e1ea50  strbeq r0, [r3, #0x48]   ; store to 0x00000048   <== FAULT
```

`0x48` is not a wild pointer, it is a structure offset applied to NULL. Under
Darwin a NULL dereference lands in `__PAGEZERO` and reports
`KERN_PROTECTION_FAILURE`, which is delivered as **SIGBUS** rather than SIGSEGV
— so the exception type is not evidence of an alignment problem. An audit
confirmed the interpreter's ARMv6 unaligned-access model is **correct** (SCTLR.U
and SCTLR.A both honoured), and no alignment fault appears among the 2,052
recorded faults. **No CPU bug is involved.**

#### The causal chain, end to end

```text
bootkernel un-matches /arm-io/mbx in the loaded device tree
  -> com.apple.driver.AppleMBX never starts, so no AppleMBXDevice is published
  -> userspace _mbxConnectionOpen calls
     IOServiceGetMatchingServices("AppleMBXDevice"), which SUCCEEDS; then
     IOIteratorNext at 0x30e1fd60 returns MACH_PORT_NULL — an empty iterator,
     not a match failure -> kIOReturnError
  -> _mbx2DCtxInitialize prints "Failed to open connection to MBX", returns NULL
  -> _mbx2DInitialize stores NULL into _mbx2DGlobalContext (0x381200d8),
     and returns 1
  -> CA::WindowServer::MBXServer::MBXServer logs
     "Failed to initialized MBX2D driver (%d)."   (Apple's typo, verbatim)
     but does NOT clear enable_mbx2d, which it set to 1 eighteen instructions
     earlier
  -> the first IOMFB vblank drives RenderMBX2D::render into the NULL context
  -> SIGBUS
  -> launchd respawns SpringBoard (ThrottleInterval 5), and it happens again
```

Every link in that chain is read out of the shipped 7E18 binaries. The one
diagnostic failure worth naming is our own: the emulator faithfully reproduced a
guest process dying and being restarted, and the harness's SpringBoard trace
block reported only the newest generation, so the loop read as forward progress.

#### The fix is an environment variable, and it has not been run

QuartzCore ships a complete CPU software compositor.
`CA::WindowServer::MBXServer::render_update` (`0x3124207c`) is a three-way
fallback:

```text
gles_context()  ->  mbx2d_context()  ->  CA::WindowServer::Server::render_update
```

`mbx2d_context()` (`0x31241a8c`) reads exactly one byte — `enable_mbx2d` at
`0x38190db1` — and returns NULL early at `0x31241aa8` when it is zero. The
fallback then reaches `Server::sw_renderer()` ->
`CARenderOGLNew(_kCARenderSoftwareCallbacks)` -> `CA::OGL::SWContext`, a genuine
CPU rasteriser. `enable_mbx2d` is set from `getenv("CA_ENABLE_MBX2D")`, or
failing that `getenv("LK_ENABLE_MBX2D")`, defaulting to **ENABLED**.

So **`CA_ENABLE_MBX2D=0`** in SpringBoard's launchd environment selects software
rendering and never reaches the NULL store. **No GPU emulation is required.** A
candidate `com.apple.SpringBoard.plist` carrying that variable is staged under
`work\analysis\envvar\`.

This is verified at instruction level from disassembly. **It has not been run.
SpringBoard has still never rendered a frame.**

#### Six retractions

1. **"SpringBoard is progressing normally and merely runs out of instruction
   budget."** FALSE. It is crash-looping; the repeated framebuffer hash is the
   signature of that, not of a frontier that keeps moving.
2. **"Checkpoint restore loses fidelity — cold and restored runs disagree by
   4.6e9 instructions."** FALSE. Restore is **bit-exact**: heartbeat PC streams
   are byte-identical for run35 vs run36d across **27/27** samples, and likewise
   for run37/38, run38/40/41/43, and capA/capB. The apparent disagreement was an
   artifact of the `SPRINGBOARD POST-SETEXEC TRACE` block reporting only the
   **most recent** SpringBoard generation; a longer run prints a later
   generation of the same loop.
3. **"The instruction cap perturbs guest execution."** FALSE — same artifact,
   same evidence.
4. **"`-[UIWindow makeKeyAndVisible]` is never called, so no window is made
   visible."** MISLEADING. That selector has exactly **one** call site in the
   whole 1.19 MB SpringBoard binary, inside `-[SBSyncController
   _delayedBeginReset]`, a restore path never taken at boot. SpringBoard 3.1.3
   uses `-[UIWindow orderFront:]` and `-[UIWindow makeKey:]`, so a zero there is
   the **expected** reading on healthy hardware.
5. **"Un-matching only `/arm-io/usb-otg/usb-device` is insufficient."**
   Understated — it was a complete **no-op**. Driver output census: run46 (child
   un-matched) had **24** `AppleSynopsysOTGDevice` lines; run37 (parent
   un-matched) had **0**. IOKit matching keys off the parent node.
6. **Any claim resting on run39 or run42 "with USB un-matched" is void.** Both
   restored from a snapshot taken with the driver already matched in guest RAM,
   and a device-tree patch cannot affect a restored run.

#### Two corrections to the MBX story itself

- **The MBX register block is at physical `0x3B000000`, not `0x03000000`.** The
  device-tree `reg` value is `{0x03000000, 0x01000000}`, and `/arm-io` `ranges`
  adds `0x38000000`.
- **The "busy-polls a reset bit" account of the original MBX hang is wrong.**
  That poll (`AppleMBXController`, `0xC07799E0`) is gated on `fVariant == 2`
  (s5l8720x) and cannot execute on s5l8900x; the controller's `fRegs` is NULL
  because the node carries only one `reg` pair. The real wedge is
  **`AppleMBXDevice` at `0xC077E8D8`, spinning on physical `0x3B00012C` bit 6,
  with no timeout and no exit.**

The `dt_unmatch("arm-io/mbx")` call itself **stays**. Only its explanation was
wrong — and un-matching the node is what makes the software compositor path
reachable in the first place.

#### What else landed alongside

- `2b08c4d` — LDRD/STRD implemented and CP15 c1 gated on CRm. `test_arm`
  **939 passed** (was 878).
- `a09478e` — the DWC2 configuration registers the OTG driver actually reads are
  now modelled in new `core/src/soc/usbotg.c`: `GHWCFG1=0`,
  `GHWCFG2=0x228de550`, `GHWCFG4=0` read-only, `PCGCCTL` read/write. This
  removes the deterministic panic at instruction **8,728,148,009**. `test_soc`
  **5,621 passed** (was 5,591). Snapshot format **4 -> 5**, which invalidates
  every existing checkpoint. It is **not** reached by a default boot: bootkernel
  still un-matches the USB complex unless `-u` is passed.

### 2026-07-25: run35 reached `UIController`; the frontier is now RSA, not a wait

#### Run34 failed closed on a bug in the checkpoint path

The first checkpoint attempt never wrote a snapshot. It stopped with

```text
external-md sidecar: cannot read ...\rootfs-...-8f01295f77f1.img
snapshot ...\run.snapshot-2400000000: external-md sidecar failed   (exit 4)
```

The cause was in the new code, not the guest: the sidecar copied the work image
by opening it a second time with `fopen`, while the `file_block` adapter already
holds the only handle Windows will grant. The fix copies through the adapter's
own `vm_block.read_at` in 1 MiB chunks, which removes the second handle and is
the stricter choice anyway — it is the same path the guest's own I/O takes, so a
checkpoint cannot disagree with what the guest last wrote. A short read is
treated as failure and the partial file is removed.

This is recorded because it is the failure mode that matters: the run refused to
produce a snapshot it could not stand behind, rather than writing a corrupt one.

#### Run35: the boot was never stuck after `UIController` — it had not got there

Run33 reached its 2.5e9 cap cleanly, with no CPU stop at all, and `UIController`
at 0 hits. The natural reading was that something was blocking. It was not.
`UIController` is simply **past 2.5e9**, and every previous run stopped short of
it. Run35 (cap 5e9, checkpoint at 2.4e9, exit 0, no CPU stop) walked the whole
launch sequence:

```text
UIApplicationMain-call          @3,267,854,042   return hits=0  (run loop, correct)
applicationDidFinishLaunching   @3,321,020,021
isTethered-return               @3,322,116,558   false branch
telephony-shared-call           @3,335,002,459
CTCenterGetDefault-call         @3,335,082,498
CTCenterGetDefault-return       @3,335,312,957   <- telephony SUCCEEDED
telephony-init-return           @3,478,515,451
telephony-shared-return         @3,478,515,454
SpringBoard:UIController-call   @3,478,858,148   hits=1   <- first time ever
```

Telephony did not merely stop blocking after the run30 device-tree fix; it
**completed**, including `CTCenterGetDefault`. The checkpoint sidecars were
written correctly on this run (`run.snapshot-2400000000` 87,457,413 B,
`.mdimage` 466,825,216 B, `.mdstate` 131,248 B).

#### What the remaining time is actually spent on

In the final 200M-instruction window, **99.6%** of samples are userspace and
~40% fall in one 22-instruction loop at `0x3145ad4c..0x3145ada4`. Run logs could
only call that "userspace", because every PC above `0x30000000` lands in one
96 MB shared cache spanning 273 libraries. Extracting the cache from the
retained work image and resolving the address (now `tools/dscmap.py`) names it:

```text
image:  /System/Library/Frameworks/Security.framework/Security  (+0x2bd4c)
symbol: _mulg_common at 3145ac70  (+0xdc)
```

`_mulg_common` is Apple's giant-integer multiply. The disassembly is schoolbook
multiplication on **16-bit limbs** — `mul`, mask against a literal-pool
`0xffff`, carry-propagate, `strh`, bounded by a limb count reloaded from
`[sp,#0x14]`:

```text
3145ad4c  ldrh  sb, [r4, #-2]      3145ad78  ldrh  r5, [r1, #2]!
3145ad58  mul   r0, fp, r3         3145ad84  strh  r2, [r1, #-2]
3145ad5c  mul   lr, sb, sl         3145ad8c  ldr   r2, [sp, #0x14]
3145ad64  and   r3, lr, ip         3145ad94  cmp   r2, r6
3145ad80  add   ip, r3, lr, lsr #16   3145ada4  bne   #0x3145ad4c
```

That is bounded arithmetic, not a spin-wait, and the register trace agrees:
`r1` advances two bytes per iteration and `lr` is used as scratch rather than a
return address. Neighbouring symbols place it in the certificate/key family —
`_SecRSAPrivateKeyRawSign`, `_SecCertificateIsSignedBy`,
`_SecPolicyCreateiPhoneApplicationSigning`, `_SecGenerateSelfSignedCertificate`
— and the kernel side shows `_prngInitialize`, `_SHA1Init`, `_prngOutput`.
`0x33aae484`, the address the episode tracker reports, resolves to `svc #0x80`
in libSystem: an ordinary syscall, so the process is alive and making calls
throughout.

**What this does not show.** No pixel was rendered. The captured frame is still
the seed — `CBAD1C11…`, 384 of 460,800 RGB bytes non-zero, 128 non-black pixels
in the top-left corner, verified by eye and not only by hash. CLCD is confirmed
live and correct around it (`scanning=1`, `frames=1026`, window0 320x480 at
`0x0885c000`, descriptor refreshes 71 → 102), so the display path is ready and
SpringBoard has not drawn into it. Which higher-level operation calls the giant
code, and whether it terminates, are **open**: "RSA-class arithmetic in
Security.framework" is the whole claim, and a bounded inner loop does not by
itself bound the outer computation.

#### The checkpoint finally has a consumer

`bootkernel` has had `--restore` throughout; the launcher never exposed it, so
every iteration paid the full ~28-minute replay to a frontier the checkpoint
already held. `-RestoreFrom` now hashes all three sidecars into the manifest, so
a restored run records the machine state it inherited instead of implying it
cold-booted. Two launches failed closed before one started — a positional-
parameter bug in the manifest write, then a launcher dirty against `HEAD` — both
loudly and before execution.

### 2026-07-25: run30 broke the CommCenter blocker; runs 31-33 clear CPU gaps

#### The fix: stop declaring hardware this machine does not have

Run29 established that the shipped stack, given a baseband that is *declared*
but never answers, times out on SRDY, fails `ASMIOCNEWDLCI` with
`kASMFatalErrorSPI(11)`, and retries forever. Making that failure faster or
cleaner was demonstrably not the answer — the ioctl already failed and
CommCenter already retried, 177 times across 5.7 billion instructions.

A device that never responds is not a device that is absent, and the stock
stack treats them very differently. So the in-memory device tree now un-matches
`/baseband` and `/arm-io/spi2`, exactly as it already did for the MBX GPU and
the SHA-1 engine, telling the guest the truth about this machine. `-B` restores
the old behaviour for anyone modelling the real transport later.

Nothing is fabricated: no reset edge is synthesised, no SRDY asserted, no Mach
message injected, no queue touched, and `firmware/devicetree.bin` on disk is
untouched — the edit is to the loaded copy.

**Run30 result — the blocker is gone:**

```text
_bootstrap_check_in     hits=1  @777,240,124  r1=0x00085ee4 "com.apple.commcenter"
checkin:RETURN(r0=kr)   r0=00000000                     <- KERN_SUCCESS
checkin:SUCCESS-arm     hits=1                          <- MIG server + thread
checkin:FAILURE-arm     hits=0  NEVER CALLED
```

CommCenter owns `com.apple.commcenter`. SpringBoard's telephony singleton,
which had blocked every run since run21, now **enters and returns**:

```text
applicationDidFinishLaunching:  @2,021,677,686
isTethered returned             @2,022,920,037  (false branch)
telephony-shared-call           @2,038,668,276
telephony-shared-entry hits=2   @2,038,895,109 / @2,040,653,293
```

#### Then three CPU-coverage gaps, in the usual shape

Each stop was fail-closed and named itself, and each fix covered the whole
class rather than the single encoding:

| Run | Stop | Reached | Gain |
|---|---|---:|---:|
| 30 | `VCVTR` refused (`0xeefd7a67`) | 2,061,479,415 | blocker cleared |
| 31 | `VCVT.F32.S32` refused (`0xeef84ae7`) | 2,061,479,416 | +1 |
| 32 | `SADD8` undefined (`0xe611ef9e`) | 2,191,848,855 | **+130,369,439** |

Run31's gain of exactly **one instruction** was the useful failure. It proved
that clearing these one encoding per half-hour replay would never end: UIKit
sets a directed rounding mode and then runs whole *sequences* of conversions
and arithmetic under it. So `FPSCR.RMode` was implemented rather than refused —
`vfp_execute()` adopts the mode on the host FPU for the duration of one
instruction and restores it, while the float-to-integer path rounds explicitly
in software. That single change bought 130 million instructions.

The same reasoning produced the ARMv6 parallel add/subtract family: all six
classes (signed, signed-saturating, signed-halving, and the three unsigned
counterparts) across all six lane operations, with GE flags, lane-independent
arithmetic in wider intermediates, and PC operands refused. Implementing only
`SADD8` would have hidden the next stop behind an identical shape.

CLCD descriptor refreshes rose **1 → 7 → 27** across these runs, so the display
path is doing progressively more real work as SpringBoard advances.

**Still no pixels.** `UIController` remains at 0 hits and the PPM is still
byte-identical to the seed in every run above. The CommCenter blocker is gone;
the remaining distance is ordinary reached-path CPU coverage.

### 2026-07-25: run29 named the blocker — SRDY, and the guest said so itself

Run29 is the first replay in this project ever to outlast the guest's own
timeouts: exact commit `cf2f7d1`, a **7,000,000,000-instruction** cap (about 17
guest seconds), **4,679 seconds** of host time, exit **0**, stderr empty,
launcher postflight passed, immutable hashes unchanged, external-md **0
failures**, 0 pending continuations, and the screen still the unchanged seed.

#### The hypothesis it was built to test was wrong

§13.0c predicted that the 2.1e9 cap had merely been stopping part-way through
CommCenter's bounded ten-attempt `SCPreferences` retry, and that a longer run
would let it give up and proceed. It did not.

```text
_bootstrap_check_in   hits=0    STILL NEVER CALLED
checkin:function-entry hits=0   STILL NEVER CALLED
_ioctl                hits=177  @933,155,896 .. @6,601,520,520
_select               hits=10   @966,164,632 .. @6,560,245,824
_IOServiceOpen        hits=6    _IOConnectCallScalarMethod hits=6
```

The ioctls grew from 15 to 177 and the selects from 1 to 10, spread evenly all
the way to 6.6e9. CommCenter also retired **3,235,016 user instructions after**
SpringBoard's send — it is emphatically alive. So the bounded `SCPreferences`
loop was a real but *inner* loop, and not the blocker. The outer retry against
the baseband mux does not terminate.

That correction matters more than the prediction would have: "it will give up
if we wait" is now falsified, and no future frontier claim should assume a
stock timeout resolves without watching it resolve.

#### What the guest printed once the run was long enough

Two console lines appear that no run capped at 2.1e9 could ever have produced:

```text
BasebandSPIIFXProtocolVersion1::handleSRDYTimeoutAction: Exit
AppleSerialMultiplexer: !! mux-ad(err)::bsdIoctl: Fatal error code=kASMFatalErrorSPI(11)
```

That is the guest naming the blocker outright. `BasebandSPIIFXProtocolVersion1`
is the Infineon baseband SPI protocol driver; it waits for **SRDY**, the
modem's slave-ready line — `/arm-io/spi2 function-srdy` GPIO `0x1804`, which by
the decoded encoding is **group 24, bit 4** — and times out. The
AppleSerialMultiplexer then fails the `ASMIOCNEWDLCI` ioctl with
`kASMFatalErrorSPI(11)`, which is exactly the `ioctl(ASMIOCNEWDLCI) failed`
string in CommCenter's own `__cstring`.

So the complete chain, now end-to-end and confirmed by the guest's own
diagnostics rather than inferred:

```text
no modem, no SRDY line, no SPI transfer model
  -> BasebandSPIIFXProtocolVersion1 SRDY timeout
     -> AppleSerialMultiplexer kASMFatalErrorSPI(11)
        -> ioctl(ASMIOCNEWDLCI) fails
           -> CommCenter retries forever, never calls bootstrap_check_in
              -> launchd keeps the com.apple.commcenter receive right
                 -> its queue fills at qlimit=5 with five daemons' handshakes
                    -> SpringBoard's is the sixth and blocks
                       -> applicationDidFinishLaunching: never completes
                          -> UIController never runs -> no pixels
```

#### What this does not settle

It does not establish what a faithful fix is. A clean, prompt failure is
evidently **not** sufficient on its own: the mux already fails the ioctl and
CommCenter retries anyway, 177 times across 5.7 billion instructions. So
"graceful no-modem" cannot simply mean "make the timeout fire faster".

Whether stock CommCenter ever checks in on hardware whose modem is truly absent
is unknown and matters enormously: if it does, there is a path we have not
reached; if it does not, then reaching SpringBoard requires modelling the SPI
transport far enough that the mux comes up and DLCIs are created, with the
modem then reporting no service. That decision needs the
`BasebandSPIIFXProtocolVersion1` SRDY path disassembled before anything is
implemented, and it must not be guessed.

Nothing here is a pixel. `UIController` remained at 0 hits, live scanout at 0
mutations, and the PPM byte-identical to the seed at
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`.

### 2026-07-25: runs 25-28 found the guest was still inside its own timeout

This entry covers a short focused replay (run25, 1.0e9 cap, 582 s), the full
2.1e9 replay of the same probe (run26), and the decisive checkpoint replay
(run28, 1.0e9 cap). Run27 was deliberately killed at ~300M and carries an
`ABORTED` marker; it is not evidence.

#### First, the guest's own binary became readable

The project had never carried a userspace image or symbol map, so every guest
PC below `0x10000000` was an unresolved number. Two read-only helpers fixed
that: `tools/hfsx_extract.py` walks a retained work image's catalog B-tree and
extracts one named file, and `tools/machosyms.py` resolves an address in a
32-bit ARM Mach-O *including lazy and non-lazy stubs* through the indirect
symbol table.

That distinction is the whole point. The tail of `__TEXT` is symbol stubs, so a
PC there means "called an imported function", not "executed its own code", and
only the indirect table says which one.

The stock CommCenter came out at 724,208 bytes, UUID
`b4b87526ae086bb62c982f1078f43f81`, `__TEXT 0x1000..0x9c000`, `__symbolstub1
0x9b718..0x9c000` with 570 four-byte stubs.

#### CommCenter never asks for its port

Run28 watched twelve exact sites for the identity-validated CommCenter
generation:

```text
_bootstrap_check_in         pc=0009bd74 hits=0   NEVER CALLED
checkin:function-entry      pc=0001a99c hits=0   NEVER CALLED
_mach_msg                   pc=0009be04 hits=0   NEVER CALLED
_IOServiceOpen              pc=0009baf8 hits=4   @728317088..@965269818
_IOConnectCallScalarMethod  pc=0009baa4 hits=3   @728341762..@965303169
_ioctl                      pc=0009bdf0 hits=15  @933155896..@965442308
                                        r0-r3=6/c004799a/2ffffa40/16
_select                     pc=0009bed8 hits=1   @966164632 thread=e0379bb8
```

It is not that check-in fails. **CommCenter never enters the function at all.**

An encoding-directed scan of `__text` — needed because a linear disassembly
desyncs on inline data and silently loses call sites — found exactly **one**
call to `_bootstrap_check_in`, the Thumb `BLX` at `0x0001a9be`, inside:

```text
task_get_special_port(task, 4, &bootstrap_port)          ; 0x1a9b4
bootstrap_check_in(bootstrap_port, name, &service_port)  ; 0x1a9be
cmp r0, #0                                               ; 0x1a9c4
  success -> CPCreateMIGServerSource + pthread_create    ; 0x1a9c8..
  failure -> mach_port_deallocate, return 0              ; 0x1a9fe..
```

The name argument resolves to the literal `"com.apple.commcenter"` at
`__cstring 0x00085ee4`. So CommCenter's MIG server source and server thread
exist **only** if that one call succeeds — which is precisely what run24 saw
from the other side: launchd still holding the receive right, six clients
queued, CommCenter alive but never receiving. The function itself is called
from exactly one place, `0x0000cb08`, gated on `bl 0xcc50` returning non-zero.

#### It is asleep, in a bounded retry loop

Run26, the full 2.1e9 replay, gives CommCenter's true last own-image
instruction: `0x0009bf08` at **1,966,338,697** — stub[508], `_sleep`. It was
still executing *after* SpringBoard blocked at 1,966,246,193.

Disassembling one of the four `_sleep` call sites shows the shape:

```text
0000a9ac  blx _SCPreferencesLock          ; wait = 1
0000a9b2  bne 0xa9c0                      ; on failure: CFRelease and bail
0000a9c2  blx _SCNetworkSetCopyCurrent
0000a9c8  cmp r0, #0
0000a9ca  bne 0xaa04                      ; success -> proceed
0000a9d0  blx _SCPreferencesUnlock
0000a9d4  movs r0, #1
0000a9d6  blx _sleep                      ; sleep(1)
0000a9da  cmp r4, #0xa                    ; ten attempts?
0000a9de  b   0xafe8                      ; give up
0000a9e0  b   0xa9a2                      ; else retry
```

**A bounded ten-attempt retry with `sleep(1)` between each.** And CommCenter's
own strings name the rest of the territory it is working through:

```text
0x00088d54  /dev/mux.spi-baseband
0x00091a74  ioctl(ASMIOCNEWDLCI) failed -- status: %d.
0x00091a1c  Setting driver for DLCI %u, dispatcher %p
0x00091bd8  Select exception on DLCI
0x0008cc5c  No response from modem
0x00089d60  Could not validate wireless modem connection
```

That identifies the `0xc004799a` ioctl as **`ASMIOCNEWDLCI`** on
`/dev/mux.spi-baseband`, the AppleSerialMultiplexer node for the mux the
console reports as `created new mux (18) for spi-baseband with adapter
BasebandSPIDevice`.

#### Why every run so far stopped in the middle of a timeout

This is the result that reframes the whole effort, and it is arithmetic rather
than a defect.

Guest time is driven from retired instructions at the real cpu:timebase ratio —
a 412 MHz CPU model against a 6 MHz timebase, about 68.7 instructions per tick.
So **one guest second costs roughly 412 million retired instructions**, and a
single `sleep(1)` is about a fifth of the entire historical 2.1e9 cap.

CommCenter's ten-attempt loop is therefore worth about **4.1 billion
instructions of guest patience on its own** — nearly twice the largest cap ever
run here. Run21 reached 2.5e9; runs 22, 23, 24 and 26 all stopped at 2.1e9.

**No run has ever observed one of the guest's own timeouts expire.** Every one
of them stopped part-way through. A cap that ends at 2.1e9 has not shown that
CommCenter gives up and continues; it has shown that we stopped watching after
about five guest seconds.

#### What this does and does not establish

It establishes that CommCenter never calls `bootstrap_check_in`, that its MIG
server is gated on that call, that it is in a bounded sleeping retry loop, and
that the instruction caps used so far are shorter than the guest's own retry
budget.

It does **not** establish that running longer will make CommCenter check in.
The `SCPreferencesLock`/`SCNetworkSetCopyCurrent` loop is not proved to sit on
the path to `0xcb08`, the `ASMIOCNEWDLCI`/`select` work on
`/dev/mux.spi-baseband` is not proved to be what blocks, and the branch taken
after ten failures has not been followed. Those are questions for a replay long
enough to outlast the guest, which is why the launcher's cap ceiling was raised
from 4e9 to 24e9.

Nothing here is a pixel. `UIController` remained at zero hits and the PPM
remained the seed in every run above.

### 2026-07-25: run24 killed the baseband lead and named the five senders

Run24 is the exact cold replay of commit
`8a08e441fc866ffad866c0c88abc94db9374d527`, which adds two read-only probes
and nothing else. It ran into a fresh `work/run24-portset-senders`, exited
**0** at the 2,100,000,000-instruction cap in **1,625.1 seconds**, stderr
empty, launcher postflight passed, immutable hashes unchanged, work image
exactly 466,825,216 bytes, external-md failures **0**, guest-free low
**50.63 MiB** at 1,957,363,712. Its copied binary was 631,055 bytes, SHA-256
`684A8501C17C792A8966D142F92249250024F4A8846237DC28FFFB39591403CF`.

#### CommCenter is not waiting on a port set, so the baseband lead is dead

Run23 left one decisive question: CommCenter blocks in `_ipc_mqueue_receive`
on an mqueue that is not any AppleBaseband interest port's, so is it waiting
for that port *through a port set*? The answer is no.

```text
CommCenter receives on a NON-interest mqueue:
  commcenter/identity-unreadable=4/0  sets-walked=0  membership-hits=0
newest unmatched receive: mqueue=c2966918 @1760204121 thread=e035d000
  object=c2966900 io-bits=80000000 type=IOT_PORT
  a plain port, not a set: this receive cannot deliver another port's notification
```

Four CommCenter receives were classified and **not one was a port set** —
every candidate resolved as an active `IOT_PORT` against the `+0x18` port
hypothesis, never the `+0x1c` pset hypothesis, so no set walk was ever
entered. The port object `c2966900` also independently corroborates run23's
per-thread dump, where threads `e0379bb8` and `e035d000` carried exactly that
value.

CommCenter therefore never established a receive that *could* deliver an
AppleBaseband notification, so that **delivery route** is closed: synthesizing
a GPIO-75 reset edge would have had nothing on the other end to receive it.

Scope that carefully. It does not show the absent modem is irrelevant.
CommCenter could still be waiting on the modem some other way — the spi2
SRDY/MRDY handshake, or a bounded timeout that never fires. What died is the
specific IOKit-notification hypothesis, which was the most promising lead
after run23. This is decision-tree Case D for that route, by evidence rather
than elimination.

#### The five queued messages belong to five different daemons

Resolving each linked kmsg's reply port through the same validated object
graph the destination owner uses names every blocked client:

```text
[0] kmsg=c21e3000 reply=c2bf6ea0  -> pid 16  space=c0acfac8 task=c2d73760 proc=e03808f0
[1] kmsg=c31d7000 reply=c34d2630  -> pid 18  space=c0acfa10 task=c2d733b0 proc=e03804d8
[2] kmsg=c3f50000 reply=c2d33d80  -> pid 15  space=c0acfb24 task=c2d73938 proc=e0380afc
[3] kmsg=c3e52000 reply=c31c32d0  -> pid 12  space=c0acfc38 task=c0ad7000 proc=e0381120
[4] kmsg=c448c000 reply=c31c3cf0  -> pid 13  space=c0acfbdc task=c2d73ce8 proc=e0380f14
```

All five decodes printed `AUTHORITATIVE`. So five distinct daemons — PIDs 12,
13, 15, 16 and 18 — are each blocked on the *identical* CTServerConnection
handshake `0x0054b557`, and SpringBoard (PID 20) is the sixth against a
`qlimit` of 5.

That reframes the problem. This is not a SpringBoard bug and not a telephony
bug: **CommCenter has never served a single client since boot.** Every
CoreTelephony consumer in the system is queued behind the same silence.

#### What run24 does not prove

It does not prove why CommCenter has not checked in, that PID 12 was the
earliest producer (the linked order is the queue's, and no enqueue timestamps
were captured), that any thread was still enqueued at the cap, or anything
about pixels. `SpringBoard:UIController-call` again had **0** hits, live
scanout recorded **0** mutations, and the PPM is byte-identical to the seed at
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`.

#### What run23 changed, and what it did not

It converted three run22 candidates into results — the route binding, the
linked-versus-reserved queue contents, and the receive-right owner — and it
retired the baseband hypothesis in its delivered form. It did **not** prove why
CommCenter has not checked in, that the missing reset callback is that reason,
that any thread was still enqueued at the cap, or anything at all about pixels.

---

## The screen *(historical — a different boot configuration)*

Everything above is serial output. The kernel also has a graphics console, and
it can use it — but only in a run configured for it. The numbers below were
measured with `v_display = 0` and `serial=1` **dropped** from the command line,
which hands the console to the framebuffer; the standard debugging recipe at the
top of this document keeps serial and therefore paints nothing.

`initialize_screen` was reached from the very first boot that got this far, but
`boot_args.v_display` was non-zero, which makes `vcattach()` return early — so
the graphics console was never acquired and the framebuffer stayed untouched
(0 of 614,400 bytes). Setting `v_display = 0` and dropping `serial=1` from the
command line hands the console to the framebuffer, and the kernel paints its own
boot log into memory we gave it: **61,659 non-zero bytes, 20,553 lit pixels,
313 rows of text**, in XNU's own console font — 40 characters to a 320-pixel
line, so an 8-pixel cell.

Read back off the rendered image, in full, exactly as the kernel drew it
(wrapped by the console at 40 columns, not by this document):

```
iBoot version:
Seatbelt MACF policy initialized
AppleS5L8900XClockController: Dynamic Pe
rformance State Management Enabled with
max state 3
AppleS5L8900XClockController: Turbo Mode
 Supported with ratio 0x00000000 and mas
k 0x00008000
AppleBaseband: Could not find mux functi
on
IOSDIOController::init(): IOSDIOFamily-2
4.7 Dec 18 2009 01:49:48
AppleS5L8900XSDIO::init(): AppleS5L8900X
SDIO-26.0 Dec 18 2009 01:49:39
AppleS5L8900XSDIO::start(): SDIO Revisio
n 8900X
+ AppleMPVDDriver[0xc0bbd800]::init(prop
erties 0xc0bb8080)
+ AppleMPVDDriver[0xc0bbd800]::start(pro
vider 0xc0aae480)
AppleMBXDevice(0xc0bcf800): Init
AppleS5L8900XSDIO: registers @ vaddr 0xe
aa09000, paddr 0x38d00000
AppleMicron2020::start()
Registering IOCameraSensor service.
█
```

Fewer lines than the serial stream carries — the two consoles do not receive the
same messages — and it ends on a live cursor block.

The framebuffer's placement matters more than it looks. It originally sat
immediately after `boot_args`, with an insufficiently aligned
`topOfKernelData`; XNU then built its 16 KiB L1 table at an address different
from the one encoded in TTBR0 and prefetched-aborted at `__start+0x170`, 39,767
instructions in. Moving the framebuffer near the top of DRAM avoided that
alignment symptom but left its pages above `topOfKernelData`, inside XNU's free
page pool. The corrected planner instead places Boot_Video immediately after
the conservative static/raw-bounce reserve, includes its end in
`topOfKernelData`, aligns that line to 16 KiB, and retains at least `0x11000`
bytes for bootstrap allocations. In the exact 128 MiB external-md layout this
means framebuffer `0x0885c000..0x088f2000` and `topOfKernelData 0x088f4000`;
recognized snapshots carrying the old unsafe relationship are rejected.

What was *not active in this recorded run*: `AppleH1CLCD`, the display
controller at `/device-tree/arm-io/clcd` (physical 0x38900000, interrupt 13),
and `AppleMerlotLCD`, which needs a non-zero `lcd-panel-id` at
`/device-tree/arm-io/spi0/lcd0`. The kernel drew into its boot framebuffer but
did not drive the panel path.

To be precise about *why*, since an earlier draft of this section got it wrong:
the CLCD was already modelled — `core/src/soc/clcd.c` has tests — but this trace
left `CLCD_CTRL == 0`. `AppleH1CLCD` is not the component that programs a display
window; it adopts the first enabled window and wraps its pitch, base and geometry
in the IOSurface that becomes the screen. The current model can seed window 0
with the corrected iBoot-compatible N82 timing and capture it only while all
live-scanout gates remain active. It can also supply the Merlot panel identity.
Run08 exercised that combined seed and reached PCs in both bundle code ranges,
but observed no CLCD MMIO and no useful frame; it did not validate successful
driver start. The app's CoreGraphics view can display its synthetic guest's
CLCD buffer, but it is not wired to a shared real-guest session. Touch remains
separate M5 work.

---

## Why this document exists

A boot log is the highest-density evidence an emulator can produce. Every line
above is a claim that some piece of 2009 silicon behaves the way we say it does,
made by software that has no reason to be polite about it. When the kernel says
`SDIO Revision 8900X`, it is because it read a register we implemented and
believed the answer.

The corresponding lesson is in [ROADMAP.md](ROADMAP.md): almost every bug in
this project was invisible until an unrelated fix unlocked the path that exposed
it. A boot log is how you see the path open.

How to read one when it stops making sense — the milestone table, the kext
symbolizer, snapshot/restore, and the bus reports, in the order that has actually
worked five times — is [debugging.md](debugging.md).
