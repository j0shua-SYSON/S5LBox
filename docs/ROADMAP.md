# Roadmap

The guiding rule: **every milestone ends in something observable.** Not "the MMU
is implemented" but "the kernel builds its own page tables, enables the MMU, and
keeps running". Not "drivers work" but "here are the bytes Apple's driver
printed over our UART". If a milestone cannot be phrased as a thing you can
watch happen, it is not a milestone, it is a hope.

Measurements below come from named real-firmware runs and are historical unless
explicitly identified as current. Targets and designs are labelled separately;
the absence of a newer private-firmware trace must not be read as proof that the
old stopping point still applies.

Honest positioning: to the maintainers' knowledge, **no publicly documented
open-source emulator has booted iPhone OS 3.x to SpringBoard.** The closest prior
art found in the project's survey (devos50/qemu-ios) reaches SpringBoard on 1.1
and 2.1.1 using emulated iPod touch hardware. This is project positioning, not a
proof that no private or unindexed implementation exists.

A milestone marked done means its own stated criterion was observed, not that
the machine is complete. Which parts of this emulator run genuine Apple
software, which hardware is not modelled at all, and which is patched,
approximated, or deliberately declared absent to the guest is stated in full in
[README.md, "What this is, and what it is not"](../README.md#what-this-is-and-what-it-is-not).
Read that before reading the table below as a measure of fidelity.

---

## Status at a glance

| | Milestone | Observable completion criterion | State |
|---|---|---|---|
| **M0** | Pipeline online | Core/JIT/test workflows and iOS packaging are configured; the app has historically run a core self-test on the phone | ✅ done; workflow logs are the current CI verdict |
| **M1** | ARMv6 interpreter | ARM + Thumb + VFPv2 on the reached path; unimplemented encodings trap instead of guessing | ✅ run21 cleared the reopened reached-path gate: exact correction commit `debec04` passed hosted core/iOS CI and crossed the run20 libm VFP stop to a clean 2.5 B cap. This is reached-path validation, not architectural completeness |
| **M2** | SoC bring-up | A bare-metal payload prints over the emulated UART; a timer IRQ is taken and returned from | ✅ done and covered by host tests |
| **M3** | Firmware containers + LLB execution | Real IMG3s parse and decrypt; an extracted real Apple LLB payload executes; the kernelcache is extracted | ✅ done; SecureROM/iBoot execution remains future full-chain work |
| **M4** | XNU boots and logs | The kernel reaches `bsd_init`, prints, and Apple's own kexts match and start | ✅ **done** — plus the real root filesystem mounts |
| **M5** | Userspace → SpringBoard | `launchd` runs; the home screen renders and takes a tap | 🔵 **in progress — it renders; it cannot yet be tapped, and the screen it draws is the activation screen.** Two blockers were cleared on 2026-07-26. First, the SpringBoard crash loop: 35 byte-identical `ReportCrash` reports at `_mbx2DDisable+0x20` because un-matching `/arm-io/mbx` left `enable_mbx2d` set with a NULL context. `CA_ENABLE_MBX2D=0` in SpringBoard's launchd environment fixed it — verified at instruction level *and now run*: 30 respawns became 1, and SpringBoard built its UI controller, created its window and called `orderFront` for the first time. Second, and the reason nothing had ever drawn: `/device-tree/vram` ships `reg = {0,0}` and iBoot fills it, so `IOSurfaceRoot` could not publish `PurpleGfxMem`, `AppleH1CLCD` fell back to `withPhysicalAddress(..., kIODirectionOut)`, and `IOSurfaceClient` maps any output-only descriptor `kIOMapReadOnly` — userspace received the framebuffer **read-only** and the compositor faulted on its first store. One `dt_set_reg` call (`691b727`) produced run59: **14,264,987 changed scanout bytes**, 97,510 of 460,800 framebuffer bytes non-zero against 384 in every prior run, and a real frame at `docs/images/run59-first-frame.png`. run54 is the negative control — 22e9 instructions and 33 SpringBoard launches *without* the fix produced zero changed bytes, so this was never a matter of runtime. **Both of those blockers are now cleared, and M5 is still not met** — which is the useful part. Activation is provisioned: the HFS+ catalog writer landed in `33ce6b5`, `-ActivationState = FactoryActivated` plus `-BrickState = false` are written into `/private/var/root/Library/Lockdown/data_ark.plist` in the work image, and it works — the iTunes-connect artwork is gone. What replaced it is the boot spinner, **not** a home screen. run66 gave it 12 billion instructions: `CATransaction-flush` **0** hits, last scanout write at instruction 1,887,035,649, final frame 1,833 of 460,800 non-zero bytes, and the `lockdownd` RSA keygen — the standing "it is just slow" explanation — ran to completion between 8.0e9 and 8.5e9 without changing anything. Touch is built to the driver: `core/src/soc/mtz2.c` and `gpioic.c` landed, and run77 delivered four reports that Apple's `AppleMultitouchZ2SPI` read and whose payload checksums it accepted (probe `0xc04413e8`, `r0 == r2` on all four). No userspace client had subscribed, so **no tap has reached SpringBoard**. What actually remains is one thing, and it is now located rather than guessed: SpringBoard opens the touchscreen's IOKit user client — proven from the MIG reply bytes, `io_service_open_extended(0xb503) -> 0xb603`, with `"Sensor Surface Width"` and nine sibling property names identifying the service — and blocks in the fourth `io_connect_method` on it, at `assert_wait(0xc0d8a358, THREAD_UNINT)`. Whether a delivered frame releases it is under measurement. Full specification, verified addresses and the remaining build order are in handoff **§23.4**; the six retractions from the earlier reading of this milestone are in **§13.0j** and **§23.8**. |
| ~~M5 (historical)~~ | | | Run23 binds both send-path routes to the exact mqueue/kmsg, walks the destination queue to **five linked** identical `0x0054b557` handshakes with **zero** reserved slots against `qlimit=5`, and authoritatively decodes the receive-right owner as **launchd (PID 1)**. AppleBaseband enabled its reset event source but its callback never fired, so no delivered notification explains the saturation. CommCenter (PID 24) is alive, never exited, and its only IOKit interest is on AppleBaseband — registered by the very thread that has been blocked in a Mach receive since 932,507,189. Why the service never checks in is the open question. `UIController` is unreached and the seed-only framebuffer has 0 changed pixels. |
| **D** | Dynarec (parallel) | SpringBoard at interactive frame rates on the phone | 🔵 emitter + ARM/Thumb translator and host execution tests exist (off by default); no code cache or dispatcher calls them |
| **N** | Guest networking (parallel) | The guest resolves a name and fetches a URL | 🔵 **S0 met 2026-07-27 — the guest transmits.** run80: `Connect: ppp0 <--> /dev/tty.debug`, then 47 bytes on uart4 that are one complete LCP Configure-Request — `7E FF 7D 23 C0 21 …`, RFC 1662 framing, the four options `pppd` 2.4.2 asks for by default. `pppd` did not exit; it is waiting for a reply. **Nothing answers**, so no address is negotiated and no packet has been carried: this is one direction of one link layer, not networking. The emulator did not change to achieve it — one argument did, `/dev/uart.debug` → `/dev/tty.debug`, because the former is a real character device that is not a tty (`d_ttys = NULL`, `d_type = 0`, `TIOCSETD` falling to ENOTTY at `0xc046fe30`, and `ttioctl` unreachable from that path). Next is the host endpoint. |
| **A** | Guest audio (first-device track) | Guest PCM reaches the host speaker without blocking the CPU thread | ⚪ priority, not designed or built |
| **P2** | Second machine profile: iPhone 5 / iOS 8.4.1 | An S5L8950X machine boots iOS 8.4.1 to a rendered SpringBoard | ⚪ requested, not started — scoped at the end of this document. **Gated on answering the GPU question first**; everything else is large but ordinary work |

CI builds the portable core on Linux, macOS and Windows, runs strict-warning and
sanitizer jobs, and executes emitted JIT blocks on the arm64 macOS runners. The
iOS workflow proves compile, link, fake-sign and packaging only; it is not an
on-device runtime or real-firmware boot test. Exact assertion totals change with
the suite and optional private firmware, so the workflow log is authoritative.
At `df9dc7b`, `core-tests` run
[`30004015881`](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30004015881)
and `ios-build` run
[`30004015807`](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30004015807)
both completed successfully with the faultable raw bridge.

---

## ✅ M0 — Pipeline online

**Criterion:** CI green, and an `.ipa` that runs real ARM code through our core on
the device.

- Portable C11 core builds and unit-tests locally (MinGW/GCC) and in CI.
- The iOS workflow builds on a macOS runner, fake-signs with `ldid`, and packages
  an `.ipa` artifact — no Apple Developer account is in the CI loop. CI does not
  install or launch that artifact.
- The app's on-device self-test runs ARM instructions through the interpreter and
  drives the emulated timer → VIC → CPU interrupt path. It also reports
  `CS_DEBUGGED` and RWX-mapping preflight hints, but deliberately does not branch
  into generated code during startup. The JIT execution verdict remains an
  unrecorded, opt-in device check.

**Historical device observation:** `r0 = 42`, computed by the interpreter.

---

## ✅ M1 — ARMv6 interpreter

**Criterion:** an ARM1176 interpreter broad enough for the boot path, in which
every unimplemented encoding *traps* rather than executing something plausible.

Validated across the previously reached paths by historical long Apple-code
runs: base ARM and Thumb data processing, branches/interworking, the implemented
single and block-transfer forms, halfword/sign-extending loads, long multiplies,
banked registers and mode switching, the reached exception entry/return paths,
the required CP15 subset, selected ARMv6 extend/reverse media operations, the
ARMv6K exclusive family, an ARM1176 VFPv2 module, and all five related ARMv5TE
signed-DSP multiply families (`SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and
`SMLAWy`). The ARM1176 CP15 wait-for-interrupt form used by XNU also advances
devices directly to the next wake edge without fabricating retired instructions.
This is reached-path coverage, not architectural completeness: LDRD/STRD, other
DSP/media families, the CP14 debug unit and other documented forms still trap
deliberately.

Run20 reopened this milestone at `0xEE274B10`: valid VFP11
`FMDHR d7, r4` / `VMOV.32 d7[1], r4` was incorrectly classified as NEON. The
correction at exact commit `debec04` passed the local Release and focused
strict gates plus hosted core/iOS workflows. Run21 replayed the exact firmware
site and reached its clean 2.5 B cap, 562,020,182 instructions beyond the old
stop. That reached-path gate is restored. Other documented or as-yet-unreached
forms can still trap deliberately.

**The rule that makes this milestone worth anything:** an encoding we have not
implemented returns `ARM_UNDEFINED` and stops the machine. It never falls
through to "close enough". CP15 is the one documented exception — unmodelled
config registers read as zero, because kernels probe that space far too widely
to trap on it.

**Observed:** the CPU suite is green in the reviewed CI run. Its assertion count
is intentionally not frozen here; VFP and edge-case coverage continue to grow.

---

## ✅ M2 — SoC bring-up and UART

**Criterion:** guest code running on the emulated S5L8900 prints text, and an
interrupt is delivered and returned from.

- ARMv6 short-descriptor translation and permission behaviour sufficient for the
  reached path: 1 MB sections, supersections, coarse tables with 64 KB large and
  4 KB small pages, domain/AP/XN checks, and data/prefetch aborts wired into
  execution (DFSR/DFAR, IFSR/IFAR). Page tables are walked out of guest RAM
  through the normal bus. This does not claim every memory attribute or external
  abort source is modelled.
- A system bus with the S5L8900 memory map. Accesses that hit nothing are
  **counted, attributed and reported**, never silently swallowed.
- A Samsung-style UART, a PL190/PL192-style VIC with per-line IRQ/FIQ routing,
  and the real S5L8900 timer block.

**Observed:**

```
S5LBox S5L8900 machine tests
  [guest said] HI
  [timer IRQ -> handler -> return] uart="T", resumed at pc=00000100
```

The machine suite is green in the reviewed CI run; use its log for the current
assertion count.

---

## ✅ M3 — Firmware containers and LLB execution

**Criterion:** real, unmodified Apple firmware out of a real IPSW parses and
decrypts, and an extracted real LLB payload executes in the firmware harness.
This completed milestone is narrower than the full secure-boot chain:
SecureROM is not modelled, iBoot itself has not executed, and SHSH/CERT presence
is recorded without RSA verification. `bootkernel` currently synthesizes an
iBoot-like handoff into XNU. Full SecureROM → iBoot execution remains future
work rather than evidence claimed by M3.

- IMG3 container parser — every tag present in genuine 7E18 firmware
  (`TYPE`/`DATA`/`VERS`/`SEPO`/`BORD`/`KBAG`/`SHSH`/`CERT`) is one we handle.
  Bounds-checked in 64-bit arithmetic, because this is the first component to
  touch a user-supplied file.
- A self-contained AES-128/192/256 validated against the FIPS-197 known-answer
  vectors, plus LZSS for the kernelcache. No OpenSSL; the core keeps its
  zero-dependency property.
- Apple's own device-tree format (not FDT): node and property traversal, path
  lookup, depth-limited and bounds-checked against malicious input.
- Integrated NOR flash with IMG3 scanning, writable and persistent (the shape an
  untethered jailbreak needs). A standalone raw-NAND/storage substrate models
  erased/programmed bit behaviour and persistence in host tests, but it is not
  connected to the S5L8900 machine bus or boot path.

**Observed:** the real version strings come out of the user's own IPSW
(`iBoot-636.66.33`, `EmbeddedDeviceTrees-390.16`), and Apple's **LLB executes
6,668 instructions** of genuine code — switching to Thumb, making real function
calls, touching zero unmapped addresses — before failing a header check on flash
we had not yet populated.

**Deliberately not done: Apple's VFL/FTL.** Mapping logical to physical NAND
pages faithfully requires validating against real firmware behaviour; a
plausible-looking guess would pass our tests and fail silently on a genuine NAND
dump, which is worse than having none. The raw device is provided for an FTL to
sit on later, and the gap is stated in `nand.h` rather than papered over.

---

## ✅ M4 — XNU boots and logs

**Criterion:** the real iPhone OS 3.1.3 kernel initialises, prints to the
console, and Apple's own kernel extensions match against our device tree and
program our emulated peripherals — with no panic.

**That criterion is met, and the milestone is complete.** A
400,000,000-instruction boot of `xnu-1357.5.30~6/RELEASE_ARM_S5L8900X`,
decrypted from a stock IPSW, with the real 413 MiB (433,274,880-byte) root
filesystem attached as a RAM disk. This was measured at `9363283`. A later
historical run made pid 1 progress instead of spinning and halted at instruction
234,731,493 on the VFP encoding recorded under M5; that encoding is implemented
now, so neither count states the current stopping point.

| Measurement | Value |
|---|---|
| `_panic` / `_Debugger` reached | **never** |
| `_bsd_init` reached | instruction 64,567,734 |
| Root filesystem | **`BSD root: md0, major 2, minor 0`** |
| Console output | **4,595 bytes** |
| Distinct functions executed (sampled) | 1,024 (the profiler's table is now the limit, and says so) |
| `_DTGetProperty` calls | 858 — IOKit walking our device tree |
| FIQ entries / cost | 385 / 38,235 instructions (0.0% of the run) |
| `STREX` executed / failed | 2,715,561 / 13 — all retries were observed in `lck_mtx_*`; the trace does not establish their cause or interleaving |
| Exception returns into Thumb | 351, of which 204 land 4-byte aligned (~58% in this recorded run; no hardware distribution is assumed) |
| Non-RAM physical pages touched | 22 |

This table replaces the earlier 200 M-instruction figures (2,177 bytes of
console, 13 device pages, 91 FIQs), which were measured before the root mount and
before the TTBR1 fix. Where the older numbers still appear below, they are
labelled as historical.

Apple's kexts, unmodified, announcing themselves over a UART that exists only as
C in this repository:

```
Darwin Kernel Version 10.0.0d3: Fri Dec 18 01:26:55 PST 2009;
  root:xnu-1357.5.30~6/RELEASE_ARM_S5L8900X

Seatbelt MACF policy initialized
AppleS5L8900XIO::start: chip-revision: EVT0
AppleARMPL192VIC::start: _vicBaseAddress = 0xe38ed000
AppleS5L8900XEdgeIC::start: _edgeicBaseAddress: 0xe38e6000
AppleS5L8900XGPIOIC::start: gpioBaseAddress: 0xe38f5000
AppleS5L8900XPowerController::start: _pcBaseAddress: 0xe38fd000
AppleS5L8900XClockController: Dynamic Performance State Management Enabled
AppleARMPL080DMAC::start: dmac0 / dmac1
AppleS5L8900XADM::start: mapped I/O registers at 0xe9915000/0x38800000
AppleS5L8900XSDIO::start(): SDIO Revision 8900X
AppleS5L8900XSPIController::start: spi0 / spi1
AppleS5L8900XUSBPhy::start registers at 0xea942000
AppleS5L8900XI2CController::start: i2c0 / i2c1
AppleS5L8900XTimer / AppleS5L8900XWatchDogTimer / AppleS5L8900XI2SController
AppleMBXDevice(0xc0bf4800): Init
ApplePCF50635PMU::start / AppleMicron2020::start()
Registering IOCameraSensor service.
```

An annotated walk through this exact boot, stage by stage, with the emulated
device each driver is talking to, is in [BOOTLOG.md](BOOTLOG.md).

### And the first pixels

The kernel now paints its own boot log into the framebuffer. `initialize_screen`
was being reached all along; the blocker was that `boot_args.v_display` was set
non-zero, which makes `vcattach()` return early, so the graphics console was
never acquired. With `v_display = 0` — and `serial=1` dropped from the command
line — the kernel takes the framebuffer for itself: **61,659 of 614,400 bytes
non-zero, 20,553 lit pixels across 313 rows**, verified by rendering the buffer
to an image and reading the text back off it:

```
iBoot version:
Seatbelt MACF policy initialized
AppleS5L8900XClockController: Dynamic Performance State Management Enabled…
AppleS5L8900XSDIO::start(): SDIO Revision 8900X
AppleMBXDevice(0xc0bcf800): Init
AppleMicron2020::start()
Registering IOCameraSensor service.
```

That is XNU's own graphics console, drawn glyph by glyph by the kernel into
memory we handed it. It proves console rendering, not that Apple's display
driver started; the current tree has a tested CLCD model, but the run below did
not reach that kext's code.

The CLCD correction does not change that historical result. It prepared
the controller handoff by treating `0x0d8..0x0ec` as window
configuration rather than panel timing, seeding the actual `VIDTCON0..3`
registers at `0x20c..0x218` with iBoot-compatible N82 320x480 timing, and
requiring every hardware scanout gate before frames or wake edges are produced.
Run08 later exercised the seed, but produced only one 8x16 white block and no
guest CLCD MMIO; it did not validate SpringBoard or successful driver start.

### The five bugs that got us here

Each was found in the last stretch of work, and each is worth naming for *how*
it hid.

1. **The timer block was invented.** Ours was a plausible four-register device
   at 0x00/04/08/0c. The kernel's own symbols said otherwise: a free-running
   64-bit counter at 0x80/0x84 (this is `mach_absolute_time`), timer 4 at
   0xA0–0xAF, and VIC line 7 routed to **FIQ**, not IRQ. Nothing the kernel
   touched existed, so `mach_absolute_time()` read zero forever, every delay
   loop waited on a dead clock, and the boot died silently in a spin rather than
   panicking. Fixing it produced the first console output this project ever
   made: `iBoot version: `.

2. **The framebuffer reserve and `topOfKernelData` were planned separately.**
   The first post-`boot_args` placement left `topOfKernelData` only page-aligned,
   so XNU's 16 KiB L1 table and TTBR0 disagreed and the kernel prefetched-aborted
   39,767 instructions in. The later top-of-DRAM workaround fixed that symptom
   but put Boot_Video above `topOfKernelData`, where XNU could allocate and
   overwrite its pages. The unified planner now places the framebuffer directly
   after the static/raw-bounce reserve, includes it below the 16 KiB-aligned
   `topOfKernelData`, and requires `0x11000` bytes of remaining bootstrap
   headroom. External-md therefore uses framebuffer `0x0885c000` and
   `topOfKernelData 0x088f4000`; a firmware-free startup self-check pins both.

3. **Exception returns word-aligned Thumb resume addresses.** Writing PC with S
   set copies SPSR into CPSR — restoring the interrupted mode *and* its T bit —
   and the next line then did `*next = res & ~3u`, unconditionally. Returning
   into Thumb code at an address 2 mod 4 rewound two bytes and re-executed the
   preceding halfword. It presented nowhere near its cause: a decrementer FIQ
   landed inside `_zfree`, the return rewound into the tail of the *locked*
   path, and the kernel unlocked a mutex at address 1 — a data abort at
   `_lck_mtx_unlock+0x8` with DFAR 0x1, deep inside IOKit. The tell was
   statistical rather than local: **761 of 761** exception returns into Thumb
   resumed at a 4-byte-aligned address rather than producing the mixed alignment
   expected from the observed return targets, and 372 of them (48.9%) needed a
   +2 correction. Two competing
   hypotheses — a failing exclusive monitor, an uninitialised IOKit structure —
   were instrumented and *refuted* rather than assumed: 70,008 exclusive stores
   executed in that boot, zero failed.

4. **Guest time ran 68× fast.** The timebase advanced once per retired
   instruction. On the hardware the CPU runs at 412 MHz and the timebase counts
   at 6 MHz — one tick per ~68 cycles — so the kernel could never finish
   servicing a decrementer deadline before the next was already in the past. It
   clamped the decrementer to its minimum and re-entered, forever. Measured
   before: **1,939,179 FIQ entries, 131,864,057 instructions inside FIQ, 65.9%
   of the run**. After converting instructions to ticks at the real cpu:tb ratio
   (carrying the remainder so it stays exact instead of drifting): **86**. Both
   rates are fields rather than constants, because a ratio that decides whether
   emulated hardware looks implausibly fast to the guest deserves to be a knob,
   not a buried assumption.

5. **The power-gate controller.** `AppleS5L8900XPowerController::start` writes
   the domains it wants gated and then spins until `STATE` agrees:
   `write(OFFCTRL, 0x12fc); do { s = read(STATE); } while ((s & 0x12fc) != 0x12fc);`
   Unmodelled, `STATE` read 0 forever — **3,887,707 reads** — and `start()`
   never returned, so the controller never registered and nothing downstream
   could power-gate anything. It is a real device model rather than a stub for a
   concrete reason: the guest never *writes* `STATE`, so read-back storage would
   return zero just as forever. Polarity came from the driver's own generic gate
   routine — power-up writes `ONCTRL` and waits for the bit to **clear**,
   power-down writes `OFFCTRL` and waits for it to **set** — not from a guess.
   With the model in place the whole page takes 7 reads in a 200M-instruction
   boot, and a wave of drivers came up behind it: I2S, SPI1, MBX, SDIO, the PMU,
   the camera sensor. Console output went 1,673 → 2,177 bytes.

Two smaller ones, just as instructive. `CLREX` was being silently swallowed by
the `PLD` decode — `0xF57FF01F` satisfies the PLD pattern, so with PLD tested
first `CLREX` became a hint that did nothing, which would let a `STREX` the
architecture requires to fail succeed instead: two threads holding one lock. And
in the long multiplies, bit 22 selects **signed**, and it was inverted — caught
only by a test that multiplies `0xFFFFFFFF` by itself both ways, because for
small operands the signed and unsigned answers agree.

### The walls cleared after `bsd_init`, in order

This chain is the most useful record in the repository, so it keeps going. Each
entry is a wall that stopped the boot dead, and how it was found.

1. **The interrupt storm.** The kernel raised a self-IPI on VIC software-interrupt
   line 4 and read `VICADDRESS` (0xF00) to find the source; our stub returned 0,
   which the PL192 driver decodes as spurious source 0, so the IPI was
   acknowledged but never cleared and `_fleh_irq` re-fired forever. Fixed by
   modelling `VICADDRESS` to return `source | 0x80000000`. Console output jumped
   2,177 → 8,191 bytes. (`8ebeb2a`)
2. **`AppleS5L8900XADMFMC::start` panic** ("ADM startup failed"). The NAND/DMA
   driver's `admStart` polls the ADM status register for a ready bit that never
   sets in emulation. Since we boot from a RAM disk we do not need NAND, and the
   driver's own `probe()` honours the boot-arg **`nand-enable-adm=0`** — with it,
   the driver never matches and never panics. That boot-arg is now part of the
   standard recipe.
3. **The MBX GPU driver.** `AppleMBX` matched `/arm-io/mbx` and wedged on an
   unmodelled 2D/3D block. Breaking that node's `compatible` string so nothing
   matches clears the graphics wedge and the boot goes idle instead; iPhone OS 3
   has a software-blit path, so the GPU is not required. (`559b633`)
4. **The historical IORTC wait.** In that run, `bsd_init` →
   `IOKitInitializeTime` waited 30 seconds for a service named `IORTC`; the
   PCF50635 PMU/RTC was not modelled and the service was not published. Patching
   that timeout to zero reached `IOFindBSDRoot`, and the kernel **mounted the RAM
   disk**: `BSD root: md0, major 2, minor 0`. (`9e29149`) Run16 now proves PMU
   start success and live PCF50635 traffic over the modelled i2c0 path, but it
   retained the zero-timeout patch. Direct `IORTC` publication therefore still
   needs a one-patch diagnostic option or clearly identified targeted build;
   `-K` disables the whole patch table and external-md rejects it.
5. **512 MiB is the current hard ceiling.** A historical run with `-R 768`
   panicked at ~34 M in early VM init with a null-zone dereference.
   `arm_vm_init` hardcodes `virtual_avail = 0xe0000000`, so at the documented
   virtual base the kernel's physical-linear window is exactly 512 MiB;
   advertising more makes `zone_virtual_addr` index a `pv_head_table` that is
   still zero during `zone_bootstrap`. Current source also rejects any RAM
   aperture that overlaps NOR at `0x28000000`, which is exactly 512 MiB above
   the `0x08000000` SDRAM base. Real S5L8900 devices shipped ≤256 MiB, so
   hardware never reached this oversized path.
   (`5625f5c`)
6. **ARMv6 `TTBCR.N` / `TTBR1` — the first genuinely systemic bug.**
   `arm_mmu_translate` walked TTBR0 unconditionally. ARMv6 splits translation
   between TTBR0 and TTBR1 at a boundary set by `TTBCR.N`, and this kernel runs
   with **N=2**: the only two `MCR p15,0,Rd,c2,c0,2` sites in the entire binary
   both write the literal 2, and `set_mmu_ttb` writes TTBR0 *alone*. So kernel
   text at 0xc0008000–0xc020d000 and the 0xffff0000 vector page live in TTBR1,
   while TTBR0 holds the current user pmap. Walking TTBR0 always was survivable
   only while both TTBRs happened to hold the same base — the first `pmap_switch`
   to a user pmap deleted kernel text and the vector page from the walk, and the
   CPU stormed on prefetch aborts at 0xffff000c forever. **That was the
   long-standing "unsymbolized kext spin".** With the split honoured the boot
   lets a much broader driver set run: timers, I²C, I²S, SPI, USB PHY, twelve
   DMA channels, uart0/1/3/4, the spi-baseband mux, `AppleMultitouchZ2SPI`,
   `AppleMobileFileIntegrity`, `ApplePCF50635PMU`. Tests cover the N=0 regression
   guard, the N=2 geometry, N=1/N=3 to prove the formulas scale, that
   a TTBR1 miss does not fall back to TTBR0, and the actual bug; they fail 17
   checks against the pre-fix walker. (`e97934d`, hardened in `aa4f0c5`)
7. **`DFSR.WnR` was never set — the second systemic bug.** Bit 11 says "the abort
   was caused by a write access", and XNU's `sleh_abort` derives `fault_type`
   from `tst r2,#0x800`. With the bit clear it always took the read path,
   rewrote the PTE with `AP=0b10` (privileged RW, user read-only), and returned
   `KERN_SUCCESS`. The faulting unprivileged store re-ran, hit the same
   permission fault, and the kernel repaired it the same wrong way — **~2.8
   million identical aborts** at `_copyout+0x40`, one every ~395 instructions,
   zero user-mode instructions in 1.1 billion. It hid for ~230 M instructions
   because privileged writes are *accidentally* satisfied by `AP=0b10`; only an
   unprivileged access can expose it, and the first one the kernel makes is the
   `copyout` of `"/sbin/launchd"`. Fixing it reached
   `Process 1 exec of /sbin/launchd`. (`85c4653`)
8. **The ARMv6 CPUID registers, and `EBADARCH`.** That exec then failed with
   errno 86. All 385 ARM Mach-Os in the rootfs are cputype 12 / cpusubtype 6, so
   the disk was never wrong — the kernel's idea of its own CPU was.
   `do_cpuid()` reads MIDR, sees architecture field 0xF (which the ARM ARM
   defines as "described by the CPUID scheme, not by this field"), and goes on
   to read `ID_ISAR1` to check for Jazelle. We modelled CP15 c0 only for CRm==0
   and returned zero, so the check failed, the arch field stayed 0xF,
   `cpu_init()` indexed past its 7-entry table and stored `CPU_SUBTYPE_ARM_ALL`,
   and `grade_binary`'s `__switch8` (count byte 5, covering host subtypes 5..9)
   missed and returned grade 0 — `EBADARCH` for every armv6 binary on the disk.
   Fixed by returning the ARM1176JZF-S feature identification block for CP15 c0
   CRm 1 and 2 (ARM DDI 0301H §3.2); **no kernel patch, the kernel's logic was
   right and we were the ones not answering.** `ID_DFR0` deliberately stays 0
   where the real part says 0x33, because we have no CP14 debug unit and
   `do_debugid()` would take a non-zero value as licence to publish a breakpoint
   count; a test pins the two together. (`30a95d3`)
9. **A hardware SHA-1 engine we do not model, silently fabricating digests.**
   With the exec path open, launchd's first text page failed its code-signature
   hash and the thread spun `cs_invalid_page` → `psignal` without retiring a
   single user instruction. **The bytes were never wrong.** Two independent
   private, untracked historical verifications exonerated the image first — a
   UDIF verifier that
   decompressed all 7 `blkx` tables and checked every per-`blkx` CRC32 (zero bad
   entries, and the reconstruction is byte-identical to `rootfs_apm.img`), and an
   HFSX reader that walked the catalog and reported code-directory page hashes
   for every signed Mach-O on the volume (155 files, 6,731 code pages, 27.6 MB,
   zero mismatches, launchd 46/46 and dyld 56/56). The real cause:
   `SHA1UpdateUsePhysicalAddress` branches to a hardware engine for buffers of
   exactly 4096 bytes whenever `_performSHA1WithinKernelOnly` is non-NULL — a
   function pointer installed by `IOCryptoAcceleratorFamily`, which matched in
   our boot. `cs_validate_page` hashes exactly 4096 bytes, so it took the
   hardware path every time, read six words out of an unmodelled register file at
   0x38000000, and `SHA1Final` emitted that. **The clinching evidence was
   timing**: `SHA1Transform` costs ~2,262 Thumb instructions per 64-byte block, so
   4 KB should cost ~145,000 instructions; the observed `SHA1Init` → verdict
   interval was 14,329, an order of magnitude too few. Software SHA-1 provably
   never ran. Un-matching the `sha1` nub keeps the hook NULL; `-S` restores it.
   (`f01a9a4`). Those verifier tools and outputs are not present in the public
   tree, so this evidence is recorded history rather than a reproducible current
   check.

### What M4 leaves behind, still unexplained

M4's criterion is met and then some, but two things are survivable rather than
understood, and that is the category that becomes a mystery three milestones
later.

- **The abort-site table saturates**, all data aborts with FSR 0x07 (page
  translation fault) on a marching sequence of kernel virtual addresses, in
  `IOBufferMemoryDescriptor::initWithPhysicalMask` and the kernel's own
  `_fleh_dataabt`. First at instruction ~116.6 M, DFAR 0xea110000. The kernel
  takes them and carries on; `_panic` is never reached. The table holds 48 entries
  and now **reports how many it dropped** rather than truncating silently — a
  silently truncated list reads as "these are all the abort sites", which is
  exactly the wrong thing to believe while diagnosing a wedge (`f01a9a4`).
- **22 distinct non-RAM physical pages** were touched in that historical run, up
  from 13, because far more drivers ran. The unmodelled ones then included the
  edge interrupt controller, GPIO, the clock/reset generator, i2c0/i2c1,
  spi0/spi1, the crypto
  block, and SDIO — where 10,003 of the 10,013 accesses are the CMD5 poll that
  correctly times out because no card is modelled. Every one is counted and
  attributed to a PC *and now to a kext*, which is the point. I2C0/I2C1 are no
  longer members of that unmodelled list: run16 exercised both MMIO controller
  paths and the PCF50635 slave at seven-bit address `0x73`; focused tests
  validate the IRQ21/IRQ22 behavior.
- **`AppleH1CLCD` was not observed starting in this run** — but NOT because the
  CLCD was unmodelled. That earlier claim was wrong on both halves:
  `core/src/soc/clcd.c` is a tested model, and the nub's registers were never
  read. The sampled profiler also recorded no PC in the kext, but its
  one-in-1,024 sampling interval cannot prove that the kext executed literally
  zero instructions. The display controller is
  `/device-tree/arm-io/clcd`, `compatible = "clcd,s5l8900x"`, physical
  0x38900000, interrupt 13.
- **`AppleMerlotLCD` needed a panel ID in this run.**
  `/device-tree/arm-io/spi0/lcd0` was
  `compatible = "lcd,merlot"` with `lcd-panel-id = 0x00000000`. Real iBoot reads
  the panel's ID over SPI. The current CLI can patch a non-zero value, seed CLCD
  window 0 and capture the active buffer. Run08 later proved exact instruction
  entries within this bundle's code range, but no CLCD MMIO or successful
  display-driver start. That remained the historical boundary through run09;
  run16 later proved both observed Merlot starts and H1 `start_hardware`
  returned success.
- **The CLCD seed needed real timing, not mislabeled window words.**
  Offsets `0x0d8..0x0ec` are per-window auxiliary configuration pairs; actual
  `VIDTCON0..3` lives at `0x20c..0x218`. The corrected N82 handoff seeds
  `VIDCON0 = 0x441` for the 54 MHz clock divided by five with scanout enabled,
  plus `VIDCON1 = 0x8` for inverted VCLK. The porch/sync values are fixed for
  N82, while `VIDTCON2` derives from the requested geometry; production 320x480
  yields `0x013f01df`. Initial `0x0d8`, `0x0e0`, and `0x0e8` window words are
  `0x1000`. Live scanout additionally requires start state, `CLCD_CTRL` global
  enable, and `VIDCON0` bit 0. These values make the seed internally coherent;
  they do not themselves prove driver start or SpringBoard.
- **The same audit recorded three fault-path gaps** (`e2d6c44`). Two have since
  been closed and regression-tested: instruction fetches enforce `XN`, and
  unaligned accesses that cross a page boundary translate both pages. The third
  remains: there is no external-abort source, so `DFSR.ExT`, status 8/c/e,
  `DFSR[10]` and `DFSR[12]` are not produced. The audit also fixed two real gaps:
  `CPSR.A` is now set on
  Prefetch Abort / Data Abort / IRQ / FIQ entry as the ARM ARM requires, and
  `CPS` is now correctly a no-op in User mode — honouring it was a privilege
  escalation that a kernel-only boot could never expose.

---

## 🔵 M5 — userspace → SpringBoard 🏆

**Criterion, in order, each independently observable:**

1. The kernel mounts a root filesystem and `launchd` executes its first
   instruction in user mode.
2. Daemons start, and the system log shows them doing it.
3. SpringBoard renders the home screen into the framebuffer.
4. A touch delivered from the host's screen moves something on the guest's.

**Last demonstrated boundary:** criteria 1 and 2 are met for the recorded CLI
path, and SpringBoard now executes real application code, but criterion 3 is not
met. The HFSX root filesystem mounted as `md0`, `launchd` executed user code,
and `mDNSResponder` ran as pid 14. Run15 then decoded the exact stock
SpringBoard request's `POSIX_SPAWN_SETEXEC` flag, followed image activation and
`_load_machfile`, and observed the shipped-kernel result epilogue return `r0=0`.
The revalidated replacement process retired 37,134,545 attributed user
instructions, reached stock SpringBoard's `LC_UNIXTHREAD`/exported `start` at
`0x34e8`, and later executed SpringBoard Objective-C methods. It never entered
exact-process `_exit1` and ended scheduled out in a validated `mach_msg` trap.
Run19 repeated run18's exact SETEXEC and display path, then exposed the model's
incorrect aggregate TV-out timing gate. Run20 validated the mixer+SDO correction
in exact commit `590d224`: 4 TV-out frames reached IRQ 30's shipped
filter/action, the gate woke, and PID 20 returned from the exact
`IOServiceClose` with `r0=0` at 1,915,263,517. UIKit returned from
`startWindowServer` at 1,919,831,289 with a 320x480 display, and SpringBoard
entered `applicationDidFinishLaunching:` at 1,923,358,329.

Run21 then replayed exact decoder-fix commit `debec04`, cleared that libm
`_fmod+0x1a8` stop, and exited 0 at its configured 2.5 B cap,
562,020,182 instructions beyond run20. SpringBoard again reached
`applicationDidFinishLaunching:`. `-[SBTetherController isTethered]` returned
false at 1,924,647,850, and SpringBoard continued through debugging/demo
preferences, lock-button, and platform-controller initialization before
entering `+[SBTelephonyManager sharedTelephonyManager]` at 1,965,837,070 and
`-init`. PID 20's last exact user instruction was 1,966,242,080, then its
thread switched out at 1,966,246,193 in a shared-cache `mach_msg` before
returning to the caller. Post-run resolution proves that
`_CTTelephonyCenterGetDefault` creates a CTServerConnection, whose successful
bootstrap lookup of literal `com.apple.commcenter` returns port name
**0x4f07**. Its initial generated handshake enters at **0x30a1177c** and calls
`mach_msg` at **0x30a117e0** with request ID **0x0054b557** and send/receive
sizes **0x834**/**0x30**, then blocks before **0x30a117e4**. This names the
service and wait boundary, but not a reply, deadlock, full queue, or baseband
cause.

That is still not criterion 3. `UIController` was unreached, live scanout
recorded zero mutations, and the active capture remained the seed-only 8x16
block with 0 changed pixels. A current checkpoint chain
restored at 2.2 B retired instructions,
crossed the former
`SMULBB` stop and wrote a 2.4 B checkpoint. The 2.4 B → 2.8 B interval wrote a
2.7 B checkpoint, observed one new `_execve` first at 2,605,595,575, and ended
with `systemShutdown false`. Restoring 2.7 B wrote a 2.85 B checkpoint and
reached the configured 2.9 B cap. None of those intervals reached `_panic`,
`Debugger`, or an emulator undefined-instruction stop. A diagnostic continuation
from 2.85 B then reached 2,944,340,624 instructions and stopped on `0xe6cf3073`,
ARMv6 `UXTB16 r3, r3`, in user mode. After the complete paired-extend family was
implemented and tested, replaying that same checkpoint cleared the instruction,
wrote a 2.97 B checkpoint and reached the configured 2.98 B cap with status
`OK`. The interval recorded two `_load_machfile` paths, 400 code-page
validations, 4,266 software-interrupt entries and 3,373 Unix syscalls. No log or
framebuffer capture from those restored 2.2-2.98 B intervals proves SpringBoard
started.

That diagnostic also crossed the free-page target without an immediate OOM:
the pool fell from 317 pages at 2.9 B to a low of 97 pages at instruction
2,934,505,472, recovered to 253 pages at the former opcode stop, and ended at
214 pages at 2.98 B, against a target of 250. The
roughly 445 MiB pinned RAM disk remains a severe device-memory constraint. The
completed audit's writable md bulk-copy design is now integrated as a guarded
cold-boot mode. A fresh 128 MiB real-firmware continuation reached `launchd`,
fsck, and the first raw `/dev/rmd0` read at 402,741,536 instructions with 21,187
free pages (82.76 MiB), 6,715 successful strategy reads, and no bridge failure.
That pre-raw-bridge run stopped intentionally at `_mdevrw`. Run03 crossed the
guard and reached its 420,000,000 cap, but fsck exited with signal 8. Run04
reproduced two exact causes by 405 M: the segment-5 offset-zero 32 KiB read
needed native write-side demand paging at user VA `0x01001000` (`FSR 0x807`),
and a 32 KiB read at `0x1bd30000` crossed media end `0x1bd33000` by 20 KiB.
Both recurred across fsck's `-p` and `-fy` passes.

Run05 cleared both request shapes in a fresh cold boot. It reached the
430,000,000-instruction cap with exit status 0 after `launchd`, `Running fsck
on the boot volume...`, and `/dev/md0 on / (hfs, local, noatime)`. The raw path
recorded two reads, two native redirects, two checked completions, zero guest
errors, and zero pending continuations. Of their 65,536 bytes, 45,056 came from
the media and 20,480 from the coherent guard. The aggregate external path completed
6,901 reads (28,295,168 bytes) and one 512-byte write with zero failures; 6,899
reads and that write used the strategy bridge. The work image remained
466,825,216 bytes, and the lowest free-page sample was 20,820 pages (81.33 MiB)
at instruction 425,852,928. `_execve` remained at 11 hits and `_load_machfile`
at 6. It has still not produced a SpringBoard framebuffer.

Run06 extended that same fresh-cold architecture to 1,000,000,000 instructions
with exit status 0 and empty stderr. The serial log retained the
launchd/fsck/root-mount sequence, added both `mDNSResponder[14]` Seatbelt lines,
and ended with `systemShutdown false`. It completed 10,004 external reads
(40,994,304 bytes), 27 writes (107,008 bytes), and zero failures; strategy
accounted for 10,002 reads and all 27 writes. The raw path remained two reads,
two native redirects, two checked completions, zero guest errors, and zero
pending continuations, with 45,056 media bytes plus 20,480 coherent-guard bytes.
The low-water sample was 17,221 pages (67.27 MiB) at instruction 980,615,168.
The work image remained 466,825,216 bytes and the source firmware hashes were
unchanged. `_execve` remained at 11 hits while `_load_machfile` advanced to 25.
No SpringBoard frame was captured.

Run07 extended the fresh 128 MiB external-md cold path to 2,000,000,000
instructions with exit status 0. Its 234,838-byte stdout retained launchd,
fsck, `/dev/md0` root mount, both `mDNSResponder[14]` Seatbelt lines, and
`systemShutdown false`; stderr was empty. The final PC was `0x3145ad4c` in USR
mode (`CPSR 0x20000010`), and 731,259,769 instructions (36.6%) retired in USR
mode. `_execve` reached 12, `_load_machfile` 32,
`_thread_bootstrap_return` 92,620, and `_unix_syscall` 58,166.

The external bridge completed 12,782 reads (52,372,992 bytes), 82 writes
(325,120 bytes), and zero failures; strategy handled 12,780 reads and all
writes. Raw I/O remained two reads, no writes, no guest errors, two native
redirects, two completions, and zero pending continuations. It read 45,056
media bytes plus 20,480 coherent-guard bytes and wrote neither region. The run
ended with 13,000 free pages (50.78 MiB); its low was 12,983 pages (50.71 MiB)
at instruction 1,836,056,576. The work image remained exactly 466,825,216 bytes
and the kernel, device-tree, and rootfs source hashes were unchanged.

Run07 deliberately had the framebuffer disabled. CLCD status, interrupt mask,
and scanning were all zero. It therefore provides no evidence that SpringBoard
started and no validation of the real display path.

Run08 was the first fresh corrected-handoff display diagnostic. It used
external-md, a 128 MiB guest, and `-F -H 0x38900000` for framebuffer seeding
plus exact CLCD-page tracing, then reached its 600,000,000-instruction cap with
`stopped ... OK`. The final PC was `0xc017056c` (`_SHA1Init+0xc4`) and stderr
was empty. The host wrapper accidentally left its exit-marker file empty, so no
OS process exit status was captured.

Exact instruction-entry coverage recorded 675 hits in
`AppleH1DisplayDrivers`, first at 126,211,220 and last at 201,032,245, plus 409
hits in `AppleMerlotLCD`, first at 209,372,737 and last at 211,410,011. The
CLCD MMIO page nevertheless recorded zero accesses. The final controller state
was still the host seed: IRQ status 1, mask 0, scanning 1,
`CLCD_CTRL = 0x41`, `VIDCON0 = 0x441`, `VIDCON1 = 0x8`, window 0 active,
running 1, and 386 frames.

The captured frame was nonblack only technically: 128 white pixels formed one
8x16 block at the top-left, every other pixel was black, and only 384 RGB bytes
were nonzero. The lifecycle ring retained 70 events with zero pathname-copy
failures, launchd/fsck/root mount, and service spawns through
`/usr/sbin/notifyd` at instruction 586,776,479. Exact SpringBoard path attempts
were zero. User mode retired 44,274,420 instructions (7.4%), and the free-page
low was 19,260 pages (75.23 MiB).

The external bridge completed 8,059 reads (33,034,752 bytes), 16 writes
(61,952 bytes), and zero failures. Both raw requests completed through two
redirects and two completions, with zero pending continuations or guest errors.
The source kernel, device-tree, and rootfs hashes remained unchanged.

This proves that the CPU reached PCs inside both bundle code ranges and that the
corrected seeded scanout survived. It does not prove instruction retirement, a
successful `AppleH1CLCD` start, guest CLCD programming, SpringBoard, or a useful
frame. The lack of MMIO is an important observation, not by itself an
identification of the exact blocker; the next evidence must come from a longer
run and lifecycle/display tracing.

Run09 supplied that longer fresh display-enabled run through a
2,000,000,000-instruction cap. The harness stopped `OK`, stderr was empty, and
the wrapper's OS process exit marker was unavailable. User mode retired
729,934,906 instructions (36.5%); free pages reached a low of 12,976
(50.69 MiB) at 1,829,371,904. The bridge completed 12,798 reads
(52,438,528 bytes), 82 writes (325,120 bytes), and zero failures, while source
firmware hashes remained unchanged.

The lifecycle ring retained 120 events and one exact stock SpringBoard
`posix_spawn` pathname attempt at 635,280,837. One unrelated later pathname
copy failed. That run predates the exact spawn-outcome probe and therefore does
not establish a child image or SpringBoard execution. `AppleH1DisplayDrivers`
rose to 687 entry
observations, first at 126,211,220 and last at 1,571,737,384, but the extension
was only six late two-instruction callbacks. `AppleMerlotLCD` remained frozen
at 409 observations, last at 211,410,011. SPI0 saw only 13 early platform
writes, and no CLCD MMIO was recorded. Seeded scanout advanced to 589 frames,
yet the PPM was byte-identical to run08: exactly 128 white pixels in one 8x16
top-left block on black.

Focused run11 used durable direct process redirection and reached a clean
700,000,000-instruction cap with empty stderr. It repeated the SpringBoard
request at 635,280,837 and recorded BTServer at 637,448,889. Storage completed
8,754 reads and 24 writes with zero failures; the final frame remained the same
seeded 8x16 block. A later `_exit1(proc=e0381ca8)` cannot be assigned to either
service from that older trace because their entry proc/PID identities were not
recorded.

Run11's raw old-wrapper return remained pending and it observed no associated
`_thread_resume`. Those are expected if the predicted SETEXEC flag is confirmed,
but run11 did not decode it; the absences are neither failure nor success proof.

Run15 populated the replacement-process probe. It read live flag `0x0040`,
exact-gated the shipped kernel's `_posix_spawn` result epilogue at `r0=0`,
validated `exec_activate_image` and `_load_machfile` while excluding vfork
phases, and committed a successfully stepped user instruction after
task/uthread/proc/PID revalidation. The exact process then retired 37,134,545
attributed user instructions and 882 traced traps without an exact-process
`_exit1`.

The first low-image PC, `0x34e8`, is the untouched stock SpringBoard Mach-O's
`LC_UNIXTHREAD`/exported `start`; later exact PCs resolve through Objective-C
metadata to real SpringBoard methods. This advances the frontier from “launchd
requested the stock path” to “stock SpringBoard application code executed.”
Criterion 3 still requires a recognizable framebuffer, and criterion 4 requires
host-to-guest touch.

### Run16 PMU/I2C and display-start diagnostic

Run16 used a fresh display-enabled external-md work image and stopped normally
at its 250,000,000-instruction cap with host exit status 0 and empty stderr. It
is an early-driver smoke run, not a SpringBoard run: no userspace instruction
retired by the cap.

Both S5L8900 I2C controllers are now modelled on their real MMIO windows and
VIC0 interrupt lines 21 and 22. The PCF50635 PMU/RTC is attached to i2c0 at
seven-bit address `0x73`; its state is included in snapshot v3 and covered by focused
transfer, NAK/W1C/IRQ, RTC, malformed-state, and restore tests. In run16, i2c0
recorded 57 START events. All 44 exact wait-condition hits reached the
post-wait checkpoint, the PMU start-failure checkpoint was never reached, and
the pre-I2C-parent and first-I2C checkpoints were reached. The combined live
trace and static control flow prove PMU start success and live PCF50635 bus
traffic. Because the run retained the existing zero-timeout IORTC patch, it does
not yet prove direct `IORTC` resource publication. That requires a one-patch
diagnostic option or clearly identified targeted build; `-K` disables the whole
patch table and external-md rejects it.

Run16 also supersedes the old “AppleMerlotLCD remained frozen at 409” boundary.
Both observed Merlot start calls returned success, H1 `start_hardware` returned
success, `AppleH1DisplayDrivers` accumulated 10,803 exact instruction-entry
observations, and `AppleMerlotLCD` accumulated 948. The guest performed 795
reads and 32 writes on the CLCD page and changed the controller and interrupt
mask. The display-adjacent `0x39100000`, `0x39200000`, and `0x39300000` pages
were still unmodelled fidelity risks at the run16 checkpoint, but their traffic
alone did not prove that they blocked rendering.

The final PPM still contained only the seed 8x16 white block. This is expected
at an early cap with no userspace, and it prevents a false claim that successful
observed return paths equal a SpringBoard frame. Run17 performed the planned
full-cap experiment; its later boundary is recorded below. The IORTC
publication experiment remains a separate, targeted unpatched run.

### Run17 CAWindowServer/IOMobileFramebuffer boundary

Run17 completed a fresh display-enabled external-md cold boot through the
2,000,000,000-instruction cap with status `OK`. Exact SETEXEC activation
succeeded, the replacement process did not enter exact-process `_exit1`, and
the CLCD was active at 320x480. The final PPM was byte-identical to run15 and
run16: only the seeded 8x16 white block was present, represented by 384 non-zero
bytes out of 460,800 RGB bytes.

The exact low-flow trace now localizes the reached userspace path. SpringBoard
called `UIApplicationMain` at `0x381e`;
`+[SpringBoard registerForSystemEvents]` returned to UIKit at `0x324a509c`;
and `+[SpringBoard rendersLocally]` returned `YES` at `0x324a5b88`.
`-[SpringBoard applicationDidFinishLaunching:]` at `0xa6f4` was not reached.
Run17 also reached `_IOMobileFramebufferGetDisplaySize+0x18` at `0x3110d024`
with LR `0x3123ef50`, in
`CA::WindowServer::IOMFBDisplay::update_framebuffer`. At the run17 checkpoint,
the observed boundary was therefore inside UIKit's local
CAWindowServer/IOMobileFramebuffer startup before the SpringBoard launch
callback. Run17 alone did not classify the difference from run15, which had
reached that callback, as extra latency or a model defect.

The latest exact-process Mach episode carried message ID 2816, the
`io_service_close` routine ID. The target switched out while the receive path
waited. H1 display-driver instruction entries occurred inside the episode
through instruction 1,873,360,702, before `_wait_queue_assert_wait` at
1,873,361,179. This does not assign the task-local port or connection to a
specific object, prove that the IOMobileFramebuffer finalizer was the caller,
or distinguish CLCD from TV-out. Static analysis made that finalizer a
candidate because it calls `IOServiceClose` at `0x3110dc1c`; run18 supplied
the later dynamic correlation.

Firmware-specific static mapping identified the exact path that run18 needed
to split. UIKit's `+[UIApplication _startWindowServerIfNecessary]` at
`0x324a5b70` obtains the local `CAWindowServer`, sets renderer flags, requests
the display array and first display bounds, then calls `_GSSetMainScreenInfo`.
QuartzCore's `-[CAWindowServer _detectDisplays]` at `0x3125408c` tries the
H1CLCD/TV-out display open functions. The H1CLCD path matches
`AppleH1CLCD`, obtains an IOKit service, and constructs the H1 display; its
IOMobileFramebuffer constructor opens the service, updates framebuffer
geometry, and requests layer zero's default surface. The adjacent
`0x39100000`-`0x39300000` pages map to H1 TV-out control/mixer/SDO blocks, so
their run16 traffic remains a fidelity concern but is not evidence of the
internal-LCD boundary.

Instrumentation for the next run was implemented at `9bab56c`. Exact
post-retirement checkpoints cover the
SETEXEC thread's UIKit/CAWindowServer/IOMobileFramebuffer call-and-return
ladder and retain
registers plus a bounded user-stack snapshot. A newest-retaining Mach ring
captures live request headers, selected in-kernel receive/wait milestones,
authoritative returns when observed, bounded receive-buffer bytes, and the most
recent UI checkpoint without treating a task-local name as a port identity.
Exact IOMobileFramebuffer finalizer/`IOServiceClose` checkpoints are included.
A separate newest-retaining H1/Merlot outside-to-inside edge ring preserves
late display-driver entries that the existing saturated first-N list can lose.
Run18 exercised that instrumentation and supersedes the candidate-only boundary
above.

### Run18 proved the optional TV-out swap/close blocker

Run18 reached its normal **2,500,000,000**-instruction cap with `OK` and empty
stderr. The exact-gated source kernel, device tree and rootfs retained their
documented SHA-256 identities; runtime kernel/device-tree edits were in guest
RAM and all filesystem writes stayed in the fresh work image.

The primary H1CLCD object completed IOMFB open, geometry update, layer-surface
lookup, constructor return and QuartzCore server construction. The second
display object reported 720x480 and entered TV-out-specific setters, identifying
it as optional `AppleH1TVOut`. Its shipped selector path leaves the generic
surface ID at zero, so the observed zero lookup is expected for this object and
does not indict the primary CLCD surface.

The second object's IOMFB finalizer called `IOServiceClose` at instruction
1,873,358,007. Its ID-2816 request entered `_wait_queue_assert_wait` at
1,873,361,179 and switched the exact SpringBoard thread out at 1,873,362,063
without a close return. Other userspace continued through the 2.5 B cap. Static
control flow proves that this close sleeps while TV-out swap work is active and
that only the TV-out IRQ 30 action clears the work and wakes the gate.

Run18 had no such source: `0x39100000`, `0x39200000`, and `0x39300000` were
unmapped despite 287, 150, and 275 accesses respectively, and VIC0 line 30
never became raw/pending. That is the root cause of the exact observed close
wait, not proof that no later boot blocker exists.

The initial post-run18 model provided storage for the three byte-lane-safe
4 KiB register banks, independent run/ready handshakes, W1C status/mask
behavior, and a 60 Hz VSYNC level on VIC0 IRQ 30. It incorrectly required every
bank's bit 0 for timing. It does not synthesize an IOSurface, framebuffer,
pixels, TV hotplug/signal, or IRQ 38. Run19, below, replaces the earlier
unit-only claim with a real-firmware verdict and narrows the required timing
predicate to mixer+SDO.

Post-run18 framebuffer work is similarly gated on revalidation. Boot_Video is
now reserved below `topOfKernelData` at
`0x0885c000..0x088f2000` in external-md mode, with TOKD aligned to
`0x088f4000` and `0x11000` bytes of mandatory bootstrap headroom. CLCD seed
validation now covers the page-rounded `stride * height` physical mapping used
by AppleH1CLCD and rejects 32-bit size/address overflow atomically. Run18 used
the old top-of-DRAM framebuffer and predates both hardening changes.

### Run19 validated layout and falsified the all-three-bank timing predicate

Run19 booted exact source commit
`afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9` to the normal
**2,500,000,000**-instruction cap with exit code 0, `OK`, empty stderr, and zero
external-md failures. Its original source hashes were reverified unchanged:

```text
kernel.macho    0d8cdb339d37cf37a1db2638fff79272ecd63a17764bf7666efa1618725df70c
devicetree.bin  4867c95fedf544bda2ecaa2626ae14c01a60d7771dc53ffe6fd3a6aac8b8ba57
rootfs.img      c3251e7f092c939d5818e92086cb47680981cfb03731de7b55d238c942eb5e82
```

The fresh work image was exactly 466,825,216 bytes. Boot arguments at
`0x087db000`, raw bounce `0x087dc000..0x0885c000`, framebuffer
`0x0885c000..0x088f2000`, and `topOfKernelData 0x088f4000` validated as
disjoint. CLCD window 0 used that framebuffer at 320x480, stride 1280, and
ended scanning/running with 662 frames.

The SpringBoard path did not advance beyond run18. Run19 called
`UIApplicationMain` at 1,849,444,535, returned `YES` from `rendersLocally` at
1,869,087,332, entered the optional IOMFB finalizer at 1,887,341,013, and
called `IOServiceClose` at 1,887,341,029. ID-2816 asserted its wait at
1,887,344,201 and switched the exact thread out at 1,887,345,137. From run18's
finalizer through its wait, every corresponding checkpoint is exactly
13,983,022 instructions later; this is timing drift, not new control flow.
Close return, `GSSetMainScreenInfo`, and
`applicationDidFinishLaunching:` remained unobserved.

All three TV-out pages reached the model, and modeled ready bit 1 removed
run18's shutdown-timeout warnings. The late first-word state was nevertheless
control/mixer/SDO `0/5/1`, SDO pending/mask `0/0`. Because the first model
required all three bit-0 states, it generated zero TV-out frames, zero raw IRQ
30, zero filter/action/completion-dispatch hits, and no gate wake or close
return.

Static driver disassembly establishes the surgical correction. Control `+0`
is conditional source-programming state; its independent stopped/ready
handshake is real and remains modeled. Mixer `+0` and SDO `+0` are the
persistent timing eligibility pair, and the IRQ filter does not read control
`+0`. The local gate is now green at 23/23 Release tests, SoC 5,504/0, and
snapshot 469/0. Exact correction commit `590d224` also passed hosted
[core run 30091220128](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220128)
and [unsigned iOS run 30091220122](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220122).
Run20 supplied that firmware rerun and completed the TV-out chain. The milestone
remains open because the visual and input criteria did not advance.

The final PPM was exactly the run18 seed:
SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`,
153,472 black pixels, 128 white pixels in the original 8x16 block, no other
color, zero changed pixels, and zero live-scanout writes. Correct layout and
662 CLCD frames do not prove SpringBoard rendered.

### Run20 cleared the TV-out wait but did not render SpringBoard

Run20 used exact source commit
`590d2248af4d7e5e92ec7bbd1be079c3bb415542`. The original firmware hashes
remained unchanged, external-md failures were 0, guest free memory bottomed at
51.76 MiB, and the retained run directory occupied 447.18 MiB on F:.

The corrected mixer+SDO predicate produced 4 TV-out frames. VIC0 IRQ 30 entered
the shipped filter/action, the close gate woke, and the exact PID 20
`IOServiceClose` user return carried `r0=0` at 1,915,263,517. UIKit's
`startWindowServer` returned at 1,919,831,289 with the display decoded as
320x480, stride 1280. SpringBoard then entered
`applicationDidFinishLaunching:` at 1,923,358,329.

The observable completion criterion still failed. `UIController` had zero hits,
the PPM remained the same seed-only 8x16 block, and there were **0 changed
pixels** or live RGB-visible scanout writes. A later lifecycle callback is not a
home screen.

Run20 exited 9 at 1,937,979,818 on `0xEE274B10`, resolved in PID 20 to libm
`_fmod+0x1a8`. The instruction is valid VFP11 `FMDHR d7, r4`, also written
`VMOV.32 d7[1], r4`, rather than NEON. The decoder correction passes the full
local Release suite 23/23 and targeted VFP/ARM/JIT binaries at 452/0, 810/0,
and 347/0 under focused strict builds. At the end of run20, the immediate gates
were:

1. pass hosted core/iOS workflows for the exact correction commit;
2. replay firmware through `0xEE274B10` and record the next fail-closed
   boundary;
3. reach `UIController` and capture recognizable changed pixels;
4. add the host touch path and demonstrate an interaction.

### Run21 cleared the VFP stop but did not render SpringBoard

Run21 used exact source commit
`debec04ff9b0faa469d5ad2ee7d75d1bf3b53b1a` and exited **0** at the configured
**2,500,000,000-instruction cap**. It crossed run20's failure coordinate by
**562,020,182 instructions**, firmware-validating the VFP11 high-word transfer
fix on the exact libm path.

SpringBoard again entered `applicationDidFinishLaunching:` at
1,923,358,329. `-[SBTetherController isTethered]` returned from `0x967ba` to
`0xa72c` at **1,924,647,850**, then took the false branch
`0xa730 -> 0xa74c`. SpringBoard continued through
`loadDebuggingAndDemoPrefs`, `_initLockButtonBearTrap`, and
`SBPlatformController`. It entered
`+[SBTelephonyManager sharedTelephonyManager]` at **1,965,837,070** and
`-init` at `0x28240`. PID 20's last exact-attributed user instruction was
**1,966,242,080**, and its thread switched out at **1,966,246,193** within a
shared-cache `mach_msg` before returning to `0xa77d`.

Post-run shared-cache resolution proves that
`_CTTelephonyCenterGetDefault` creates a CTServerConnection. The bootstrap
lookup for literal `com.apple.commcenter` succeeds and returns port name
**0x4f07**. The initial generated handshake enters at **0x30a1177c** and calls
`mach_msg` at **0x30a117e0** with request ID **0x0054b557**, send size
**0x834**, and receive size **0x30**; SpringBoard blocks before
**0x30a117e4**. The generated stub leaves `msgh_size` and `reserved` stale, so
the observed header size **6** is stock stack state, not emulator corruption.
The service identity is now exact, while reply, deadlock, queue-full status,
and baseband causality remain unresolved.

Criterion 3 remains open. `UIController` had **0 hits**, live scanout recorded
**0 mutations**, and the PPM remained the seed with SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`
and **0 changed pixels**. The source hashes remained unchanged, external-md
failures were **0**, guest free memory bottomed at **50.63 MiB**, and the
retained run directory occupies **447.27 MiB on F:**.

Exact-commit hosted
[core run 30095081111](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081111)
and
[unsigned iOS run 30095081184](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081184)
passed for `debec04`. The next test-only commit `0670ab8` passed
[core run 30096115501](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30096115501)
and
[unsigned iOS run 30096115527](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30096115527),
with VFP **469/0**. Latest hosted test-only `657e8d8` expands VFP helper coverage to
**488/0 locally** and passed hosted
[core run 30097023293](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023293)
and
[unsigned iOS run 30097023356](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023356).
Run21 is evidence for `debec04`, not those later test-only trees.

### Run22 proved the queue-full block but not its cause

Run22 used exact diagnostic commit
`40209b27cb10d01c552398ff918ee613c4908ed0` and exited **0** at the configured
**2,100,000,000-instruction cap**. The copied-in kernel request retained ID
`0x0054b557` and destination port object `0xc0d705a0`. Its mqueue
`0xc0d705b8` reported `msgcount=5`, `qlimit=5`, `seqno=0`, and
`fullwaiters=0` before the send.

The trace recorded the exact queue-full and pre-store `fullwaiters=1` PCs at
**1,966,245,373** and **1,966,245,387**, then blocked at
**1,966,245,550** and switched out at **1,966,246,193** without resuming
before the cap. The route recorder omitted `r8`, so those PCs are adjacent
candidates rather than a fail-closed same-kmsg route. Because Mach `msgcount`
includes reserved/in-flight slots, this also does not prove five linked
messages. The run22 decoder failed to discriminate the port's
receiver/destination/timestamp union before printing PID 1; that ownership
candidate is not accepted.

Run22 still recorded zero `UIController` hits, zero live-scanout mutations, and
zero changed pixels. Source hashes remained unchanged, external-md failures
were zero, guest-free memory bottomed at **50.63 MiB**, and the evidence
directory occupies **447.42 MiB on F:**. Exact source passed all eight jobs in
hosted
[core run 30106957804](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30106957804).

Exact pre-Run23 diagnostic commit
`5a40c5eec5bbf7c4b7d8909d0c1f364bc078338a` implements the required trace-only
evidence chain. Exact startup gates and adversarial self-checks validate active
receive-right ownership, circular queue topology and reserved-slot arithmetic,
same-kmsg route registers, sequence-bound per-thread waits, and nested-frame-
safe, same-retained-event AppleBaseband notification delivery with live
notifier/port identity. The final audit rejects cross-event aggregate joins,
repeated-send inheritance, stale dispatch sequences, and later candidate
overwrites of an already bound kmsg.
Strict compilation, the `bootkernel` target build, and a stock-7E18 zero-step
run pass locally. Hosted
[core run 30143448600](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143448600)
passes all eight jobs, and
[iOS run 30143455036](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143455036)
passes its package job for that exact commit. This is probe readiness only: the
exact committed cold replay remains required before any runtime gate below can
be checked.
Per-thread wait output proves an ordered last-observed block with no later
execution; until a final live wait-state reread exists, it does not exclude an
asynchronous wake that left the thread runnable but unscheduled at the cap.

### Run23 answered the measurement questions and moved the frontier

Run23 replayed exact commit `777afb4` — build inputs identical to hosted-green
`5a40c5e` — to a clean **2,100,000,000-instruction cap** in **1,434.86 s**,
with empty stderr, unchanged immutable hashes, zero external-md failures, and
a 50.63 MiB guest-free low-water mark.

Gates 1 through 4 above are now closed, and three of them closed with results
rather than confirmations:

1. **Owner.** `ip_receiver_name=0x1b03` validated first, then active space
   `c0acfe60`, task `c0ad7b10`, matching task-space backpointer, and proc
   `e0381d68` → **PID 1**, printed `AUTHORITATIVE`. **launchd** holds the
   receive right for the port SpringBoard sends to.
2. **Queue contents.** The bounded reciprocal walk closed consistent and
   untruncated on **five linked** kmsgs with **`reserved-or-in-flight=0`**.
   All five are the same CTServerConnection handshake — id `0x0054b557`,
   2,104 bytes, destination `c0d705a0` — with five distinct reply ports.
   SpringBoard's is the sixth against `qlimit=5`. Both route PCs are **BOUND**
   with `r4=c0d705b8`/`r8=c3d3c000`: queue-full slow branch `c00147ba`
   @1,966,245,373 and `fullwaiters=1` pre-store `c00147d6` @1,966,245,387.
3. **Service threads.** Six CommCenter threads retained. Three are in timed
   waits on the same semaphore `c0b239a0`; one, `e02f5888`, has been blocked in
   `_ipc_mqueue_receive` on port `c0dd99d8` since **932,507,189** with no
   resume observed. CommCenter never exited and took no signals.
4. **Baseband.** AppleBaseband located its reset function, created, committed
   and enabled an event source — and its **reset callback never fired**. Zero
   reads, changes, dispatches, handlers, sends, or routes. The delivered-
   notification hypothesis is retired.

The new frontier is narrower and better posed: **CommCenter is alive and
waiting, and has never taken the `com.apple.commcenter` receive right from
launchd.** The strongest available lead is that the single IOKit interest
CommCenter registered is on AppleBaseband, registered at 931,584,215 by the
same thread that blocked 923 K instructions later — but the blocked receive is
on a different port than the interest port, so this is correlation, not cause.

### Run29: the blocker is SRDY, and the guest said so

Run29 ran to **7e9** instructions (≈17 guest seconds, 4,679 s host, exit 0) —
the first replay ever to outlast the guest's own timeouts. It falsified the
expectation below: CommCenter does **not** give up. `_bootstrap_check_in` is
still never called, while `_ioctl` grew 15→177 and `_select` 1→10 evenly to
6.6e9. The bounded `SCPreferences` loop was an inner loop; the outer baseband
retry does not terminate.

Once the run was long enough the guest printed the cause itself:

```text
BasebandSPIIFXProtocolVersion1::handleSRDYTimeoutAction: Exit
AppleSerialMultiplexer: !! mux-ad(err)::bsdIoctl: Fatal error code=kASMFatalErrorSPI(11)
```

The Infineon baseband SPI driver times out waiting for **SRDY** (spi2
`function-srdy` GPIO `0x1804`), the multiplexer fails `ASMIOCNEWDLCI`, and
CommCenter loops. That completes the chain from unmodelled hardware to a black
screen. Making the failure faster or cleaner is **not** the fix — the ioctl
already fails and CommCenter retries regardless. The next step is to
disassemble the SRDY path before choosing between a minimal SRDY/GPIO input
model and a faithful permanent-failure state.

### The cap has always been shorter than the guest's own timeouts

Guest time advances from retired instructions at the real 412 MHz : 6 MHz
cpu:timebase ratio, so **one guest second costs about 412 million retired
instructions**. CommCenter's startup contains a bounded ten-attempt retry loop
(`SCPreferencesLock` / `SCNetworkSetCopyCurrent` / `SCPreferencesUnlock` /
`sleep(1)`) worth roughly **4.1e9 instructions** — nearly twice the largest cap
ever run here.

Every long run to date stopped part-way through a guest timeout rather than
observing one expire. That reframes the current frontier: run28 proved
CommCenter **never calls `_bootstrap_check_in`** (hits=0) and run26 proved its
last own-image instruction is a `_sleep` stub, so it is sleeping in a retry
loop rather than deadlocked. Whether it eventually gives up and proceeds has
never been observed. The launcher ceiling is raised to 24e9 to find out.

The current immediate gates are:

1. ~~resolve whether CommCenter's blocked receive port is a port set containing
   the AppleBaseband interest port;~~ **answered: it is not.** Run24 classified
   four CommCenter receives on non-interest mqueues and found **zero** port
   sets — every one an active `IOT_PORT`. CommCenter never established a
   receive that could deliver an AppleBaseband notification, so **that delivery
   route is closed**: building a GPIO-75 edge to deliver it would have nothing
   on the other end. This does not show the absent modem is irrelevant — the
   spi2 SRDY/MRDY handshake and any bounded timeout path remain open.
   Run24 also named the five queued clients as **PIDs 16, 18, 15, 12 and 13**,
   all blocked on the identical `0x0054b557` handshake: CommCenter has served
   nobody since boot, so this is systemic rather than a SpringBoard defect. The
   live question is now **why PID 24 never takes its own receive right**, which
   needs a user-code trace of CommCenter itself;
2. ~~resolve the shipped AppleBaseband reset event source to its exact trigger and
   decide from the binary — not by assumption — whether hardware with no modem
   present would ever fire it;~~ **resolved as far as static evidence allows.**
   It is an `IOInterruptEventSource` on **index 0** of AppleBaseband's provider,
   which `/device-tree/baseband` gives as interrupt **0x4b (75)** on the GPIO
   interrupt controller; the reset state is read by invoking the
   `function-reset_det` GPIO platform function (GPIO `0x1203`). The descriptor
   encoding puts `reset_det` in the same class as the modem-driven `srdy` line
   rather than the SoC-driven `bb_rst`/`mrdy` lines, which suggests hardware
   with no modem fitted would not fire interrupt 75 either — so the emulator's
   zero callbacks may be faithful, and the edge must **not** be fabricated. See
   `AGENT_HANDOFF.md` §13.0a;
3. determine where CommCenter's startup is actually gated relative to its
   `bootstrap_check_in` for `com.apple.commcenter`, and identify the senders
   behind the five queued reply ports;
4. ~~name or model the `0x3d200000` BasebandSPI register window, which is
   currently unmapped and answers the shipped driver with zeros;~~ **done.**
   All three device-tree-confirmed SPI windows are declared: spi0
   `0x3c300000`, spi1 `0x3ce00000`, and spi2 `0x3d200000`
   (`compatible "spi,s5l8900x,baseband"`). Exact disassembly of
   `BasebandSPI+0x1d42` shows the driver reads its four configuration
   registers back into a transfer descriptor without testing or polling them,
   so honest storage is faithful and nothing autonomous is fabricated. This is
   a named window, not a controller: no transfer, FIFO, DMA, chip-select, or
   interrupt behaviour is modelled, and it is **not** claimed to unblock the
   boot;
5. implement the minimum faithful graceful no-modem hardware behavior required
   for boot, only once one of the above proves which semantic is wrong;
6. reach `UIController`, capture recognizable changed pixels, then add the host
   touch path and demonstrate an interaction.

For chronology, this is the much earlier pre-VFP measurement from
`bootkernel`'s milestone probes:

```
_load_init_program        first @ 230,864,582
_execve                   first @ 230,895,729
_mac_vnode_check_exec     first @ 230,968,564
_grade_binary             hits 3
_load_machfile            first @ 231,011,045
_ubc_cs_blob_add          hits 2
_cs_validate_page         hits 15         first @ 232,201,298
cs_validate:hashing       hits 15
cs_validate:bad_hash      NEVER REACHED
cs_validate:no_hash_exit  NEVER REACHED
_cs_invalid_page          NEVER REACHED
_psignal                  NEVER REACHED
_fleh_swi                 hits 24         first @ 233,031,366
_mach_msg_overwrite_trap  hits 12         first @ 233,347,392
_unix_syscall             hits  5         first @ 234,013,919
_fleh_undef               hits  1         first @ 234,731,379
_panic                    NEVER REACHED

stopped after 234,731,493 instructions: UNDEFINED INSTRUCTION
  encoding at pc: 0xecb10a20 (ARM)
  lr 0xc006ae0d (_vfp_trap+0x38)
```

Fifteen pages validated cleanly, no page was invalidated, twenty-four SWIs were
taken, and twelve Mach traps and five BSD system calls were serviced. That
historical run then stopped on VFP.

XNU does not leave VFP enabled. `_init_vfp` grants CP10/CP11 full access once, and
from then on the gate is `FPEXC.EN` alone, cleared per thread — so **the first VFP
instruction a thread executes is supposed to take an Undefined exception**, which
the kernel handles by enabling VFP and re-running it. `d021205` made us vector
exactly those to the guest, using `_sleh_undef`'s own six encoding masks as the
discriminator so that a genuinely unimplemented encoding still names itself rather
than being swallowed by the guest's handler. That path now works and has been
crossed by the current run.

What halted that machine was the *next* instruction along it: `0xecb10a20`,
`VLDMIA r1!, {s0-s31}` — the load-multiple by which `_vfp_switch` restores a
thread's VFP register file. The interpreter correctly stopped instead of
guessing. `core/src/arm/vfp.c` now implements that family and its regression
tests; this trace remains evidence for why the implementation was needed, not a
current blocker report.

The next exact user-mode stop was `0xe1630381` at VA `0x33dba604`, decoded as
`SMULBB r3, r1, r3`. Implementing only that literal would have hidden adjacent
failures, so the interpreter now implements and tests the complete related
ARMv5TE set: `SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and `SMLAWy`, including
halfword selection, signed truncation, accumulator overflow/Q behavior, aliases,
conditions and invalid-register cases. Restoring the 2.2 B checkpoint with that
implementation cleared the instruction; chained restores then reached the 2.9 B
cap normally. The next fail-closed user-mode stop was `0xe6cf3073`, ARMv6
`UXTB16 r3, r3`, at instruction 2,944,340,624. The complete paired-extend
family now clears that stop, and the same snapshot reaches the 2.98 B cap.

**This is SpringBoard execution, and it is not completed M5.** Run15 crossed
the executable/application-code boundary, but a cap-limited process trace is
not a rendered, interactive home screen. The core has a CLCD model and panel
seed; the iOS app still runs only a synthetic guest, so although a finger on
its screen is now mapped and offered to the emulated touch controller, that
controller correctly refuses a report no driver could read — and there is no
audio path at all.

### The M5 work item list, as it now stands

- ~~**VFP reached-path completion.**~~ **DONE for the run20 stop.** The old
  `VLDMIA` restore wall remains cleared, and exact correction commit `debec04`
  handles the later `FMDHR` / `VMOV.32 d7[1], r4` form. Its local Release suite
  passed 23/23, targeted VFP/ARM/JIT binaries passed 452/0, 810/0, and 347/0,
  hosted core/iOS CI passed, and run21 replayed through the exact firmware site
  to a clean 2.5 B cap. This closes the named reached-path item, not every VFP
  encoding.
- ~~**ARM1176 idle handling.**~~ **DONE and covered by CPU/machine tests.** XNU's
  CP15 WFI advances timer/CLCD state to the next enabled VIC wake edge without
  counting made-up CPU instructions; a current real-guest run exercised it.
- ~~**ARMv5TE signed DSP multiplies.**~~ **DONE for the complete related family:**
  `SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and `SMLAWy`. The exact current-path
  stop `0xe1630381` was cleared. The separately forced dirty-volume `fsck_hfs`
  scenario still needs a new end-to-end run; implementation coverage is not a
  claim that the full check has completed.
- ~~**ARMv6 paired extend instructions.**~~ **DONE for the complete paired
  family.** `SXTB16`, `SXTAB16`, `UXTB16` and `UXTAB16` implement all four
  rotations and independent lane wrap, with alias, condition, flag-preservation,
  reserved-bit and invalid-register tests. The exact `0xe6cf3073` stop was
  replay-cleared to a clean 2.98 B cap. Adjacent `REV*` PC-operand gaps were
  hardened at the same time.
- **A shared, memory-bounded guest session.** Move real-boot orchestration out of
  the CLI into portable C used by both `bootkernel` and the app. The bounded
  primitives now exist: `vm_source` provides portable ranged reads and
  `bootkernel` validates the complete layout before direct-streaming the rootfs
  into final guest RAM. The app does not use them yet; it still needs a shared
  session, user-owned file selection and explicit errors.
- **Userspace attribution is partial.** The exact process trace classifies dyld,
  shared-cache, stack, and low-image regions. A read-only HFSX/Mach-O/Objective-C
  audit mapped run15's SpringBoard entry and retained low PCs, but the harness
  does not yet carry a general userspace image/symbol map. Run20 dynamically
  correlates the TV-out IRQ/filter/action to the waiting close, observes the
  exact PID 20 user return, and reaches `applicationDidFinishLaunching:`. It
  also resolves the next stop to libm `_fmod+0x1a8`; run21 clears that stop.
  It proves `isTethered` returned false, follows the next initialization
  methods, and enters `SBTelephonyManager -init`. PID 20 then last executes
  exact-attributed user code at 1,966,242,080 and switches out at 1,966,246,193
  in the initial CTServerConnection handshake before returning to the telephony
  caller. The bootstrap lookup for `com.apple.commcenter` succeeds with port
  `0x4f07`; request `0x0054b557` calls `mach_msg` at `0x30a117e0` and does not
  reach `0x30a117e4`. A matching reply, queue state, and baseband causality
  remain unknown. The next named low-image checkpoint, the SpringBoard
  `UIController` call, remains unreached.
- **The display path** — the CLCD model now separates `VIDTCON0..3` timing from
  the `0x0d8..0x0ec` window configuration, seeds an iBoot-compatible N82
  handoff, and gates frame publication and WFI edges on genuinely live scanout.
  The app's CoreGraphics bridge follows a validated active window only while
  those gates are live. Run15 proved the exact SpringBoard process executing
  application methods through 2 B. Run16 then proved both observed Merlot
  starts and H1 `start_hardware` returned success, with 795 CLCD reads and 32
  writes, but stopped before userspace. Run18 then completed the primary H1CLCD
  object and proved its exact SpringBoard-thread wait belongs to the optional
  TV-out swap close. Run19 exposed the over-strict timing predicate; run20
  validates exact correction commit `590d224` with 4 frames, IRQ 30
  filter/action, close wake/return, a 320x480 `startWindowServer` return, and
  entry to `applicationDidFinishLaunching:`. Run21 crosses the later VFP stop
  to a clean 2.5 B cap but still records zero `UIController` hits, zero
  live-scanout mutations, and the unchanged seed with 0 changed pixels. M5
  therefore remains open. The app still needs the shared real-guest session;
  Metal is optional and not implemented.
- **Multitouch**, mapped from the host touchscreen to the guest's controller.
  `AppleMultitouchZ2SPI` already starts and reports "using DMA for bootloading",
  which proves that the recorded boot reached that request. Device, DMA and
  input-report semantics remain unimplemented and unproven.
- ~~**Free space on the root volume.**~~ **DONE** — `bootkernel --grow <MB>`
  (default 32) grows the HFS+ volume in the loaded copy of the RAM disk;
  `firmware/rootfs.img` is untouched. TN1150 layout, four edits, validated
  before and after and refusing loudly on anything unexpected; see BOOTLOG
  "The volume had zero free blocks" for the detail and the numbers. Two things
  came out of it that are *not* done:
  - **The deliberately forced full `fsck_hfs -fy` check still needs separate
    revalidation.** The historical
    forced scan stopped on `SMULBB r6, r3, r5` at `fsck_hfs+0x12130`. The full
    related multiply family is implemented now, so that exact CPU gap is closed.
    Run04 entered both the `-p` and `-fy` paths, but each reproduced the raw
    demand-page/tail-read pair before fsck could establish a filesystem result.
    Run05 cleared that pair and the normal cold-boot fsck path progressed to
    `/dev/md0 on / (hfs, local, noatime)`; because it no longer needed the
    fallback, that run does not by itself exercise a deliberately forced full
    `-fy` scan.
  - **In the historical comparison it changed nothing by itself.** Out to 3 G
    instructions the console was
    identical with and without `--grow`. And `_execve` stuck at 11 was never
    the daemon counter it looked like: launchd spawns jobs with `posix_spawn`,
    which that probe does not see, and `mDNSResponder[14]` is running in both
    runs. Free space was not what was holding the LaunchDaemons — and neither
    was `execve`.
- **Recover pinned RAM without inventing an invalid physical map.** Direct
  streaming removed the second host-side rootfs copy, but the RAM disk is still
  static memory below `topOfKernelData`, so every mebibyte of volume is a
  mebibyte off the guest's free page pool: 58.93 MiB at the documented `-R 512`
  with `--grow 32` before later userspace consumption. A continuation beyond
  2.9 B fell to 97 pages (0.38 MiB), recovered to 253 pages at the former opcode
  stop and ended at 214 pages at 2.98 B against a 250-page target. Reclamation
  therefore appears active,
  but such headroom is still unsafe for an iOS host and the roughly 445 MiB
  pinned disk remains a major architecture frontier. The audit has ruled out a
  simple external PA aperture: `_bcopy_phys` converts both operands through one
  fixed DRAM direct-map delta. The portable bounded writable-block API, locked
  descriptor file adapter, privileged-only transactional SVC seam, and the
  writable range- and page-gated md-strategy bulk-copy bridge are now
  implemented and tested. An exact 7E18 manifest gates the complete decrypted
  kernel image, parsed ARMv6 Mach-O layout, fixed mapping, and all five expected
  patch sites before an atomic write. A bounded generic
  work-image provisioner now copies the immutable HFS source, validates reserved
  and allocation metadata, rewrites the unique stock fstab, grows/revalidates,
  flushes and publishes without replacement. `bootkernel --external-md` now
  exact-gates the supported kernel, device tree, and rootfs; publishes the md0
  media token outside fixed 128 MiB DRAM; and installs strategy plus raw-uio
  bridges after setup. A measured pre-raw-bridge cold run reached the first
  32 KiB `/dev/rmd0` read at 402,741,536 instructions after 6,715 strategy reads
  (27,479,552 bytes), zero writes, zero bridge failures, and with 82.76 MiB of
  guest pages still free. Run03 crossed that guard and reached 420 M, but fsck
  exited with signal 8. Run04 at 405 M reduced the failure to two repeatable
  contracts: native write-side demand paging at user VA `0x01001000`
  (`FSR 0x807`) and a 32 KiB read with 12 KiB inside the media plus 20 KiB in
  the adjacent allocation tail. The closest public XNU `_mdevrw` calls
  `uiomove64` once without a logical EOF check.

  The implemented replacement exact-patches `_mdevrw` to
  `svc #0xe3; svc #0xe4`, adds the explicit `ARM_SVC_REDIRECTED` control-flow
  result, and redirects faultable requests through exact Thumb `_uiomove64`
  at `0xc0128d14`. Four 128 KiB guest bounce slots, keyed by entry kernel SP,
  are reserved below `topOfKernelData`; the completion SVC validates and
  releases the pending slot. A zero-initialized, coherent 128 KiB in-memory
  tail preserves later reads without extending the immutable source or work
  image. Fresh run05 validated that design against the real firmware: two raw
  reads used two native redirects and two completions, with zero raw guest
  errors, zero pending continuations, and zero external failures. Run06 retained
  those results to a clean 1 B cap. Run07 then retained them to a clean 2 B cap,
  after fsck, the `/dev/md0` root mount, `mDNSResponder` Seatbelt setup, and
  `systemShutdown false`. Across that longest run the external path completed
  12,782 reads and 82 writes with zero failures, while the work image retained
  its exact 466,825,216-byte length and the firmware hashes remained unchanged.
  Display-enabled run09 independently completed 12,798 reads and 82 writes with
  zero failures through its 2 B cap; the firmware hashes again remained
  unchanged.
  Snapshot backing identity/overlay state follows, and global `_bcopy_phys`
  replacement remains forbidden. Historical
  older-source experiments reported 312 MiB and
  248 MiB pools with `-R 768 -Y`, but neither reached `_load_init_program` and
  current source correctly rejects both configurations because their RAM
  apertures overlap NOR at `0x28000000`. Making `-Y` useful now requires a
  deliberate physical-map design as well as resolving `gVirtBase` below the
  kernel's compiled-in `VM_MIN_KERNEL_ADDRESS`; the old 768 MiB commands are
  evidence, not valid current recipes.
- **NAND VFL/FTL**, if we ever mount a real NAND image rather than a RAM disk.
  This is the only route to a genuine `disk0`, and it is a large one. Both
  layers read undocumented Apple on-media formats: `AppleNANDFTL`'s FTL/VFL
  metadata, and the partition table that **`IOFlashPartitionScheme`** validates
  by magic and major version. (There is no `AppleAPM`/`AppleGPT`/`AppleFDisk`
  kext in this kernelcache — `IOFlashPartitionScheme` is what makes `disk0s1`
  and `disk0s2`, and it fails its probe outright unless its provider carries a
  `boot-from-nand` property.) Per the project rule we do not invent either
  format, so this stays parked rather than half-built.

### The two things that could have killed M5

Both were investigated in `docs/activation.md` and neither is currently judged
a blocker. Stock-signed `launchd` has passed fifteen page validations and reached
user mode without the enforcement-disable switches. The separate claim that
AMFI can be disabled through iBoot-style handoff properties and boot arguments is
confirmed by kernel disassembly but has **not** been exercised in a boot.
Activation remains a **plausibly manageable, unexercised risk**: an unactivated
device is expected to render SpringBoard's activation UI, while the exact 3.1.3
`lockdownd` data path remains to be verified against the binary. That could prove
a SpringBoard frame, but the home-screen criterion still needs an activation
route.

**Performance is a quantified desktop limitation and an unmeasured device
risk.** The interpreter ran a tight synthetic loop at roughly 14 million
instructions per second on the development host; real code with MMU translation
was slower still. No equivalent A9 result exists, so the desktop data establishes
that the current interpreter is far below the guest model's nominal rate, not
that SpringBoard will render or that the phone has a particular multiplier.

---

## ⚪ Parallel tracks

### Dynarec

An ARMv6→ARM64 JIT emitting into executable pages, with the interpreter as its
differential oracle. The backends have separate emitted and C semantics in
places. Current focused tests run short blocks through both engines and compare
their final architectural state and touched memory; a full per-instruction boot
differential harness remains planned.
The A9 host is chosen partly because it predates APRR (A11) and PAC/PPL (A12).
Whether the intended jailbroken
iPhone grants executable memory is still an on-device validation item, not a
portable-core assumption.

**Foundation present behind `-DIOS3VM_JIT=ON`, and off by default.** The
AArch64 emitter and ARM/Thumb translator have structural tests, and emitted
blocks run in the macOS arm64 CI jobs. A historical private, untracked
translation-eligibility sample improved substantially after Thumb support, but
that result is not reproducible from the public tree and is not boot coverage:

- There is no code cache, dispatcher, chaining or invalidation, so
  `s5l8900_run()` executes **zero** translated blocks.
- Unsupported forms are deliberately declined; the future dispatcher must
  synchronise state and run them through `arm_step()`. That boundary remains
  performance-critical correctness work.
- The iOS target excludes `core/src/jit/**`. Startup only reports
  `CS_DEBUGGED` and RWX mapping; it does not execute generated code. A future
  opt-in diagnostic checks that host precondition, not the emulator's translator
  or run loop, and no execution result from the target iPhone is recorded here.

The iPhone 6s Plus is the first optimization and validation target. The
translator stays in portable core code while executable-memory, cache-flush and
thread-policy details remain host adapters.

**Observable:** SpringBoard at interactive frame rates. Snapshot/restore
(`95eaf8b`) has already reduced one historical desktop replay from 140 seconds
for a cold 900 M-instruction run to 34 seconds when restoring at 200 M. That
reduces a known cold-replay cost; the current 2.2 B → 2.98 B checkpoint chain
also proves that restore works across the post-VFP userspace frontier, can
isolate a later opcode, and can replay through its fix. It does not
measure the phone's iteration loop or make the inactive JIT a boot prerequisite.

### Guest networking

Designed in [networking.md](networking.md). **The guest half of S0 is built and
met**; the host half is not started.

The selected route is **PPP over emulated uart4** — not UART3, as this section
said until run80; the device the guest's own `pppd` opens is uart4 at
`0x3cc10000`, and its devfs node is `/dev/tty.debug`. The stock guest `pppd`
turns the UART byte stream into `ppp0` with no `pppserial` plugin involved:
`"PPPSerial"` does not occur in the binary, the tty channel is `pppd`'s
default, and `PPPSerial.ppp` does not ship on this image anyway. A host-neutral
PPP/IP/NAT core is still to exit through ordinary socket adapters. It needs no
guest kext, emulator-specific tweak, `utun`, raw socket or phone-wide routing
change. The alternative paravirtual Ethernet kext and real Marvell Wi-Fi model
remain deferred routes, not the recommended implementation.

What S0 established, beyond the frame itself: the plist hijack survives into
the work image at the same 530 bytes, `launchd` starts the job, AMFI accepts
Apple-signed `pppd`, dyld loads it, the devfs node exists, `com.apple.nke.ppp`
answers, and the line discipline attaches. What it did **not** establish is
anything about a peer — there is none.

N1–N2 (NAT and PPP negotiation on the host side) can be built and tested with
small host fixtures against the 47 bytes run80 captured, which are a real
recorded Configure-Request rather than a synthetic one. N3 needs the shared
real-guest session and a reliable `launchd` job path before device claims are
meaningful. The CPU thread must never block on network I/O; queues and protocol
state stay portable while `kqueue`/BSD sockets are one host adapter.

**Observable:** the guest resolves a hostname and fetches a URL over plain HTTP.

**This route is explicitly a temporary workaround, and the owner chose it as
one.** A real iPhone 3G reached the internet over the Marvell 88W8686 on SDIO or
the Infineon baseband on SPI, and never over PPP on a serial line. PPP is being
built first because every component of it is honest — the UART is modelled
silicon, `pppd` is Apple's own shipped binary, and LCP/IPCP/HDLC-async are
public RFCs with test vectors — so it delivers real packets through the guest's
real network stack without inventing anything.

The real radio is deferred rather than abandoned. It is deferred because the
88W8686 executes its own firmware on its own processor, and a working 802.11
world would mean inventing what firmware build `9.108.5.p1-26524` does — which
TLVs it echoes, which events it sends unsolicited — none of it documented. That
is the one thing this project does not do. The SDIO host controller is already
driven by the guest's own `AppleS5L8900XSDIO` every boot, and the failure today
is a bounded, graceful CMD5 timeout, so the foothold exists whenever the
device-side work is taken on.

Consequence to state plainly in any future result: working networking over this
route means *the guest's TCP/IP stack works and reaches the internet*. It does
not mean emulated Wi-Fi works, and the interface will be `ppp0` rather than
`en0` or `pdp_ip0`. The standing entry is in
[QUALITY.md](QUALITY.md#fidelity).

### Guest audio — first-device priority

Audio is now a first-device track for the iPhone 6s Plus, but **no guest audio
device or host sink exists today**. Start by proving which I2S/controller and
codec driver path the 3.1.3 kernel expects; do not invent register behavior to
make sound appear. The eventual device model publishes PCM through a bounded
queue. A host adapter performs format conversion and playback outside the CPU
thread; underruns become counted silence and overruns are bounded and counted.
The guest-facing model and queue contract stay platform-neutral even though the
first playback adapter will use iOS audio APIs.

**Observable:** a deterministic guest tone reaches the speaker, survives pause
and route changes without deadlock, and reports bounded underflow/overflow
counters.

---

## What we learned

There is one lesson under nearly every bug in this project, and it is not
"ARMv6 is fiddly".

**Every bug found here was invisible until an unrelated fix unlocked the code
path that exposed it.** Not merely hard to find — *invisible*, because the code
containing it had never executed.

- Clearing the exclusive monitor on exception entry was wrong for a long time
  and could not matter, because no interrupt had ever fired. It became a real
  bug the instant the timer started working.
- The `MOVS pc,lr` alignment bug is dormant without interrupts, for the same
  reason. It needed the timer fix to become reachable — and then it did not
  announce itself either, presenting as a data abort on address 1 in a different
  subsystem, millions of instructions later.
- The timebase ratio was harmless while nothing armed a decrementer.
- The power controller could not be discovered until the drivers ran, and the
  drivers could not run until the kernel stopped panicking.
- Even the diagnostics had this shape. The FIQ logger sampled the first twelve
  interrupts, all healthy at 55,245-instruction spacing — and the storm began at
  instruction 66 million. The profiler silently dropped every function past its
  64-entry table, so it printed identical output at 200M and 400M instructions
  and looked exactly like coverage.

The practical consequence is that **progress here is not linear and cannot be
planned as though it were.** Every fix is also an excavation: it exposes code
that has never run on this emulator, and that code contains bugs which have
never had the opportunity to be wrong. The useful question is not "how many bugs
are left" but "what does the next unlocked path let us see".

What makes that survivable is the rule from M1: **trap what you do not
implement, never guess.** It is the difference between a bug and a mystery.

- An unimplemented instruction stops the machine *at* the instruction instead of
  computing something plausible and corrupting state that fails somewhere else.
  That is how `UMULL`, `LDREXD` and the Thumb `BLX` suffix were found — the
  emulator named them.
- Unmapped bus accesses are counted and attributed to a PC and a kernel symbol,
  so "which device does the kernel want next" is a report rather than a guess.
  That is how the real timer register map was recovered — not from a datasheet,
  but by logging what the kernel touched and correlating it against the kernel's
  own symbol table. `_s5l8900x_get_timebase` reading 0x080/0x084 told us more
  than any documentation could have.
- The one place the rule is deliberately relaxed — named MMIO stub windows — is
  bounded and argued, because for an MMIO read there *is* no neutral option:
  returning 0 is already a guess, and a demonstrably dangerous one. So a stub is
  honest storage (reads return what was written), and it is named and counted so
  the gap appears in the report instead of hiding among real faults. What a stub
  must never do is fabricate a value the guest is waiting for; when a driver
  needs a bit to change on its own, that is a device model, not a stub.

The corollary is uncomfortable and worth stating plainly: **a green test suite
proves nothing about the paths you have not reached.** Every suite was green
throughout every bug above. What found them was running real firmware and
insisting the emulator complain loudly the moment it was asked for something it
did not have.

### Three lessons the last session added

**Systemic beats device-specific, and the two look identical from the log.** Most
wall-clearing here has been device whack-a-mole: model one more peripheral,
un-match one more driver, patch one more wait. Each buys one symptom. Two fixes
were a different kind — `TTBCR.N`/`TTBR1` and `DFSR.WnR` were each a single
architectural gap in the CPU itself, and each unblocked a dozen symptoms at once
(the first unblocked a broad set of IOKit drivers observed in that run; the
second reached userspace).
Both *presented* exactly as another device problem: a spin in an unsymbolized
kext. So the question worth asking before modelling the next peripheral is
**"could one thing the CPU gets wrong explain all of these at once?"**

**A bug invisible for 200 M+ instructions is almost always reachable only from an
unprivileged or otherwise rare path.** `DFSR.WnR` hid for ~230 M instructions
because privileged writes are accidentally satisfied by the `AP=0b10` the kernel
repairs the PTE to; only `STRT`/`LDRT` or real user mode could expose it. `CPS`
being honoured in User mode is unreachable from a kernel-only boot by
construction. When something has been silently wrong for a very long time, do not
look for a rare *value* — look for a rare *mode*.

**"The profile blames one unsymbolized kext" is now a solved problem.** It cost
five separate diagnosis cycles (ADMFMC, MBX, IORTC, the TTBR abort storm, the
post-SDIO stall) before anyone fixed the tool instead of the symptom. The kext
symbolizer (`f105360`) maps `__PRELINK_TEXT` to bundle identifiers out of
`__PRELINK_INFO`, so an address now resolves to `<bundle-id>+0xNNNN` and the
report gained "time by prelinked kext" and "hottest individual PCs". Per-kext
*function* names are impossible, not merely unimplemented — the kernelcache
builder strips each kext's `LC_SYMTAB` — which is exactly why the hottest-PC list
exists. The whole procedure these tools add up to is written down in
[debugging.md](debugging.md), so it does not have to be rediscovered a sixth
time.

---

## Deliberately out of scope

- **Full telephony, baseband, SIM, and cellular service.** We emulate the
  application processor; a complete modem remains out of scope. M5 now includes
  only the minimum faithful **graceful no-modem** behavior required for the
  unmodified SpringBoard/CommCenter startup path to continue. Run22 proved the
  saturated Mach queue but not whether stubbed baseband hardware caused it.
  Run23 then showed that no AppleBaseband notification was ever delivered — the
  reset callback never fired — so the queue is not explained by baseband
  traffic. Whether the *absence* of that callback is what keeps CommCenter from
  checking in is the open question, and it is not yet a conclusion.
- **Wi-Fi through the real Marvell 88W8686** and GPU acceleration. "Route A"
  for networking — emulating the real NIC so Apple's driver binds unmodified —
  has documented SDIO/controller reconnaissance in `networking.md`, but the
  Marvell firmware protocol remains underspecified and the route is deliberately
  deferred. Guest audio is now in scope for the first device, but remains
  unimplemented.
- **App Store distribution.** Out of scope. The current ad-hoc signing,
  jailbreak-dependent executable-memory policy and user-supplied firmware plan
  are not an App Store distribution path.

---

## ⚪ P2 — Second machine profile: iPhone 5 (S5L8950X) / iOS 8.4.1

Requested as a second target. Recorded before any code exists so the scope is
honest: this is **a second emulator sharing a core, not a port**.

### What already transfers

- **IMG3 parsing (M3).** The A6 still uses IMG3; IMG4 arrives with the A7, so
  the existing container work largely applies.
- **The method and the tooling.** Fail-closed diagnostics, checkpoint and
  restore, `tools/dscmap.py`, `tools/hfsx_extract.py`, the Mach-O and HFS+
  readers, and the "un-match what is not modelled" discipline are all
  target-agnostic.
- **Core architecture.** Bus, MMU framework, snapshot format, portable C, CI.

### What is new, hardest first

1. **The GPU, and it may be decisive.** iPhone OS 3's SpringBoard renders in
   *software* into the CLCD framebuffer, which is the only reason the current
   approach reaches pixels at all. iOS 8's SpringBoard is fully GPU-composited:
   CoreAnimation to IOMobileFramebuffer to a PowerVR SGX543MP3, with no
   software fallback path. Either the SGX is emulated — undocumented and
   proprietary — or IOSurface/IOMobileFramebuffer is shimmed at a higher level.
   **Answer this before building anything else.** Every other item below is
   large but ordinary; this one is unbounded, and discovering it after an SoC
   model exists would waste that work.
2. **Thumb-2 — the real foundation, and bigger than it looks.** The decoder in
   `arm_interp.c` is Thumb-**1** only, because the ARM1176 has no Thumb-2:
   `0xE800..0xEFFF` is decoded as a Thumb-1 BLX suffix, not as a 32-bit
   instruction prefix. The iOS 8 kernel is compiled Thumb-2 throughout, so
   nothing of it decodes until 32-bit Thumb dispatch exists. Note that the
   MOVW/MOVT already implemented are the **ARM-state** encodings; the kernel
   needs the T3 forms, which are separate work.
3. **NEON — much later than first scoped.** A census of the real 12H321
   kernelcache (`tools/kcensus.py`, 1,407,613 instructions over 591 mnemonics)
   puts the entire `v*` family at roughly **0.37%**: `vst1` 1609, `vmov` 1468,
   `vld1` 1186, `vstr` 909. NEON is **not** a prerequisite for booting the
   kernel, which is what the first draft of this entry claimed. It stays
   essential for reaching SpringBoard, because the shared cache uses Advanced
   SIMD throughout in `memcpy`, string operations and CoreGraphics — but that is
   a userland problem, not a boot problem, and it sequences accordingly.

   The same census orders the rest of the work by evidence rather than by
   reading the ARM ARM front to back: after conditional forms (which are already
   covered), the genuinely missing set is dominated by `movw`/`movt` at 7.1%
   combined, then `addw` at 0.18%, then the small VFP/NEON core above.
4. **SMP.** The A6 is dual-core. Per-CPU state, exclusive monitors across
   cores, IPIs, per-core timers, memory ordering. Mitigation: XNU will come up
   single-core on a boot-arg, which defers all of it.
5. **ARMv7s integer.** VFPv4 with FMA and hardware integer divide. SDIV/UDIV
   and the ARM-state MOVW/MOVT are already implemented behind the profile
   gate; the rest is mechanical and testable with no hardware model.
6. **S5L8950X peripherals** from scratch, including the ANS NAND controller,
   which is nothing like the S5L8900's raw NAND.
7. **Activation.** Server-dependent on iOS 8 in a way iPhone OS 3 is not.

### Sequencing

De-risk the GPU question → ARMv7s + NEON in the interpreter (pure and
CI-testable) → S5L8950X SoC → boot. Not before M5 renders and takes a tap: a
finished first target is what makes the second tractable rather than miserable,
and every tool it produced is reused directly.
