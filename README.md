<div align="center">

# S5LBox

### Project goal: boot **real iPhone OS 3** — Apple's actual kernel, `launchd`, and SpringBoard — inside an app on a modern iPhone. **No jailbreak required** — see *Requirements*.

*A from-scratch emulator of the 2007 iPhone's chip, written in portable C.*

[![core-tests](https://github.com/j0shua-SYSON/S5LBox/actions/workflows/core-tests.yml/badge.svg)](https://github.com/j0shua-SYSON/S5LBox/actions/workflows/core-tests.yml)
[![ios-build](https://github.com/j0shua-SYSON/S5LBox/actions/workflows/ios-build.yml/badge.svg)](https://github.com/j0shua-SYSON/S5LBox/actions/workflows/ios-build.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![platform](https://img.shields.io/badge/host-iOS%20·%20arm64%20·%20no%20jailbreak-black)
![guest](https://img.shields.io/badge/guest-iPhone%20OS%203.1.3%20·%20S5L8900-lightgrey)

</div>

---

S5LBox does not reimplement iPhone OS or fake its apps. It emulates the
**hardware** of the original iPhone — the Samsung **S5L8900** chip and its ARMv6
processor — in software, then runs Apple's own unmodified operating system on
top of that model. You supply the firmware; none is included here. To the
maintainers' knowledge no publicly documented open-source emulator has booted
iPhone OS 3.x to a home screen (the nearest prior art reaches 1.1 and 2.1.1 on
emulated iPod touch hardware) — positioning, not proof that no private
implementation exists.

## What this is, and what it is not

Read this before anything else on this page.

**What is real:** your own unmodified iPhone OS 3.1.3 (7E18) firmware executes.
Apple's real kernel (XNU 1357.5.30) boots, Apple's own drivers start, the real
root filesystem mounts, and the real background programs run: `launchd`,
`securityd`, `installd`, `mDNSResponder`, `fairplayd`, `itunesstored`,
`lockbot`, `CommCenter`, SpringBoard. The clearest evidence that this is
genuinely Apple's stack and not a reimplementation of it: when SpringBoard
crashed, the guest's **own** crash reporter wrote real crash reports into its
own filesystem, and that is how that blocker was diagnosed. The project
ships no Apple firmware and never modifies the files you supply.

**What it reaches:** as of 2026-07-30 a host-delivered gesture **slides the lock
screen open and reaches the home screen** — icons, the dock, and Apple's own
first-run tip — composited by Apple's software renderer onto the emulated panel.

Every frame on this page was drawn by the guest's own SpringBoard through
Apple's CPU compositor and scanned out through the emulated display
controller. Nothing in any of them is drawn by the host.

<div align="center">

<img src="docs/images/run85-lock-screen.png" width="240" alt="iPhone OS 3.1.3's lock screen: status bar, clock, Earth wallpaper and slide to unlock, composited by SpringBoard.">

*run85, instruction 3.5e9 — the lock screen. 273,206 of 460,800 framebuffer
bytes non-zero, 92,145 non-black pixels in 44,087 colours. Reached by giving
`/vram` room for a second surface; with one, the same boot drew 1,659.*

</div>

<div align="center">

<img src="docs/images/r194-home-screen-icons.png" width="240" alt="iPhone OS 3.1.3's home screen: Messages, Calendar, Photos, Camera, YouTube, Stocks, Maps, Weather, Voice Memos, Notes, Clock, Calculator, Settings, iTunes, App Store, and the dock.">

*r194 — the home screen, reached by a slide-to-unlock and then a tap on
*Dismiss*, both delivered by the emulator. Every icon composited by Apple's own
software renderer. Its control r195 — same build, same instruction count, no
tap — still has the dialog on screen.*

</div>

**Touch works, end to end.** The emulated Z2 digitizer has no flash, so Apple's
driver downloads its 54,156-byte firmware over the undocumented HBPP protocol on
every boot — and says so itself: *"downloaded 54156 bytes of firmware data
("0x0049.bin") in 106ms"*. A gesture the host then injects is read by
`AppleMultitouchZ2SPI`, normalised by MultitouchSupport, turned into an
`IOHIDEvent` by Apple's MultitouchHID plugin, and delivered to SpringBoard,
where it **drags the unlock knob and opens the phone**. The knob tracks the
finger: measured at centre 57.5, 80.5 and 156.5 as the contact advances.

**A tap works too, and it has a control.** r194 taps *Dismiss* on Apple's
first-run dialog and the dialog goes. r195 is the same build at the same
instruction count with no tap, and the dialog is still on screen. The two runs
are deterministic apart from that one contact, so the tap is what dismissed it.

**What is not real, and this is the honest remainder:**

- **Two fingers reach userspace; no two-finger gesture has moved anything yet.**
  A pinch delivered 26 frames carrying **two contacts each**, and Apple's
  `_mt_FillMTContactDirectFromBinary` — called once per contact — was entered
  exactly 78 times: 26 for the one-finger unlock plus 52 for the pinch. So
  simultaneous contacts do traverse the device, the driver and the normaliser.
  What has *not* been shown is an app responding to one, because a pinch on the
  home screen has nothing to zoom.
- **No sound has been produced.** Apple's audio drivers start
  (`AppleWM8991Audio::start`, both I²S controllers) and **zero** words have ever
  reached the transmit FIFO.
- **No packet has been carried.** The PPP link comes up — `IPCP Opened`,
  `10.0.2.15` — and every NAT counter is still zero.
- **Frame rate is unmeasured.** No cost-per-frame measurement exists for the
  frames that decide it; see *Speed*.

Two smaller things the picture shows honestly. The clock reads 4:00 on
31 December because the real-time clock answers with a placeholder nobody has
connected to anything. "Searching…" is the status bar correctly reporting no
baseband, which is one of five pieces of hardware deliberately hidden from the
guest. There is no graphics chip either, and while the audio hardware is
modelled, nothing has ever played through it — see the table below.

<div align="center">

<img src="docs/images/run59-first-frame.png" width="240" alt="The first frame this emulator ever drew: iPhone OS 3.1.3's activation screen, composited by SpringBoard.">

*run59, instruction 4.97e9 — the first frame. 14,264,987 changed scanout
bytes, 97,510 of 460,800 framebuffer bytes non-zero, against 384 in every
earlier run. Reached by giving `/vram` a real address; with `reg = {0,0}`
userspace received the framebuffer read-only and faulted on its first store.*

</div>

| | |
|---|---|
| **Real Apple software** | Your own unmodified 3.1.3 (7E18) firmware: the XNU 1357.5.30 kernel, Apple's own drivers, the real root filesystem, and the real background programs listed above. No Apple firmware is shipped, and the files you supply are never modified on disk. |
| **CPU** | ARM, Thumb and VFPv2 floating point — over the code the boot has actually reached, not the whole architecture. The ARMv6 rules for unaligned memory access are honoured, memory translation enforces no-execute pages, and the system-control coprocessor is modelled. Runs are bit-exact reproducible. |
| **Hardware modelled** | Serial ports, timers, both interrupt controllers, the GPIO controller, display controller, SPI, the multitouch controller, the power-management chip and its I2C bus, and the USB controller's configuration registers. |
| **Touch: works, one finger** | The Z2 is modelled on SPI and **bootloaded exactly as the real part is** — it has no flash, so its 54,156-byte firmware is downloaded on every boot over Apple's HBPP protocol, which had to be reverse-engineered before anything could work. Apple's driver confirms it in its own words: *"downloaded 54156 bytes of firmware data ("0x0049.bin") in 106ms"*. The part then leaves the bootloader, answers interrogation, and streams touch reports. A host gesture reaches SpringBoard and **completes a slide-to-unlock**. Measured through the whole stack: surface bounds `-75..4656` and `-75..7275` read out of the running guest and matching the model exactly, reports paced at 16.000 ms (62.5 Hz), and the knob tracking the contact linearly. Two simultaneous contacts also reach userspace — a pinch's 26 two-contact frames entered Apple's per-contact normaliser exactly 52 times — but no two-finger gesture has yet produced a visible response, and no tap has yet moved a control. |
| **Not modelled at all** | No cellular. No Wi-Fi. No Bluetooth. No camera. No accelerometer. No GPU. |
| **Audio: modelled, never heard** | The WM8991 codec, both I²S controllers and the PL080 DMA engine are modelled and unit-tested, and Apple's `AppleWM8991Audio` starts against them. That is the whole of it: **zero** words have ever reached the transmit FIFO and the DMA has never been enabled, because nothing asks a locked phone to play anything. There is also no host playback path, so even a guest that produced samples would not reach a speaker yet. |
| **Networking: a temporary substitution** | The guest's own stock `pppd` runs on an emulated second serial port and, as of run80, transmits a complete LCP Configure-Request — `7E FF 7D 23 C0 21 …`, 47 bytes, RFC 1662 framing, RFC 1661 options. **Nothing answers it.** There is no host-side PPP endpoint, no address is negotiated, and no packet has been carried; what is proven is one direction of one link layer. It is honest emulation — real modelled silicon, Apple's own binary, and a protocol fully specified in public RFCs — but **a real iPhone 3G never connected this way**; it used Wi-Fi over SDIO or the cellular baseband. The guest gets a `ppp0` interface rather than real wireless. This is a deliberate workaround, chosen because the Wi-Fi part runs undocumented firmware whose behaviour would have to be fabricated wholesale — and a fabricated device is one whose success proves nothing, because the emulator would only be agreeing with itself. Where this project cannot avoid choosing an unmeasured value, it chooses a **constant**, records it in this table as unmeasured, and leaves it replaceable by a real measurement; see *Invented register values* and *Speed* below. That is the line, and it is a narrower claim than the one this row used to make — it previously said inventing behaviour is not something this project does, which the two rows below it contradict. Modelling the real radio and controllers remains the goal. |
| **Hidden from the guest** | Five pieces of hardware a real iPhone has are deliberately declared absent, by editing the in-memory copy of the device tree — the hardware inventory the emulator hands the kernel at boot — so Apple's drivers for them never start: the PowerVR MBX graphics chip, the SHA-1 hashing accelerator, the cellular baseband, the serial link to that baseband, and the USB controller. The firmware on disk is never modified; only the loaded copy is edited. Each omission has a documented reason, but the net effect is that the guest is told it is running on a machine with less hardware than a real iPhone. |
| **Invented register values** | The USB controller's three configuration registers (`GHWCFG1`/`GHWCFG2`/`GHWCFG4`) hold a legal and sufficient configuration. They are **not** measured from real S5L8900 silicon. This is one of the two exceptions the networking row above draws its line around: three constants, named here so nobody has to discover them, and replaceable the day somebody reads the real part. |
| **Rendering** | QuartzCore, Apple's own graphics layer, is set to `CA_ENABLE_MBX2D=0` so it uses the CPU software renderer Apple built into it. The real device draws on the MBX graphics chip. This is a switch Apple's code reads and a renderer Apple shipped, but it is not the path real hardware takes. |
| **Speed** | Not cycle-accurate, and **slow enough that the UI is a slideshow**. Measured 2026-07-30 rather than estimated — the previous "roughly 200x slower" was wrong in both directions. Against the real part's 412 M instructions/second, measured on an idle box: a realistic synthetic loop (load/store, 4 KB pages, device tick) runs **20.49 M/s**, the same loop in **Thumb** runs **44.73 M/s**, an **iPhone 6s Plus runs 16.69 M/s** in the app, and a real firmware boot manages **3.82 M/s** — so real guest code is about **108x slower than the hardware**, not 200x. Real code costs **5-6x more than the closest synthetic loop**, and the obvious explanations are ruled out: the MMU is not it (a full 4 KB page walk costs +3.5 ns/insn because the TLB absorbs it) and neither is the Thumb decoder (Thumb is 1.45x *faster* than ARM on the identical loop). What is left is host branch prediction over a 123-case dispatch and the i-cache and TLB pressure of real code, which a five-instruction loop cannot reproduce — and which is what a translator exists to fix. The guest's clock uses an invented 412 MHz : 6 MHz instruction-to-tick ratio rather than real cycle timing — one chosen constant that the guest can actually observe, which is why it is named here rather than buried. |
| **Kernel patches** | The kernel is modified in memory as it loads: a real-time-clock timeout is forced to zero, the root-device lookup is redirected to the emulator's fake disk, and hooks are installed so the guest's disk access reaches the host. Applied only after checking a SHA-256 hash and a nine-segment layout check of the exact 7,942,144-byte kernel. |
| **Storage** | Not flash memory. The root filesystem is served from a file on the host into a fresh writable copy made for each run, and the guest's `/etc/fstab` is rewritten inside that copy to match. |
| **Boot chain** | No secure boot chain is executed. The kernel is loaded directly; the boot ROM, the low-level bootloader and iBoot are not run. Apple's firmware container format has been parsed and an extracted bootloader payload executed, but separately, never as a chain. |
| **Optional substitution** | Off by default: one of the guest's service-configuration files is rewritten in the work copy, without changing its size, to add `CA_ENABLE_MBX2D=0`. |
| **Rendering reached, use not** | run59 draws real frames — 14,264,987 changed scanout bytes, 97,510 of 460,800 framebuffer bytes non-zero, against 384 (the pre-guest seed) in every earlier run. What is on screen is the **activation** UI, because that run's guest is unactivated. |
| **Activation, and the home screen** | Provisioning `ActivationState = FactoryActivated` into the work image clears the lockout. That alone used to leave a boot spinner; with the digitizer bootload finished the guest reaches the lock screen and, given a gesture, the home screen. The historical dead ends behind that sentence — a SpringBoard crash loop, a read-only framebuffer, a stalled HBPP download, and a coordinate theory that turned out to be wrong — are in [`docs/BOOTLOG.md`](docs/BOOTLOG.md). |

> The evidence behind every claim above — what was measured, in which run, and
> what each result does *not* prove — is in
> [Quality and validation](docs/QUALITY.md). An AI agent continuing the project
> should begin with [the roadmap](docs/ROADMAP.md) for what is done and what is
> next, and [where the time goes](docs/hotpath.md) for the performance picture.
> Where a source comment cites a derivation by section number, that section is
> in [Derivations cited by the source](docs/derivations.md).

## Current status

Milestones 0 through 4 are done: the build and test pipeline runs in CI; the
emulated chip runs bare-metal code and prints over its serial port; Apple's
firmware containers parse and a real bootloader payload executes; and the real
XNU kernel boots, starts Apple's own drivers, and mounts the real 413 MiB root
filesystem. Here is that kernel introducing itself over the emulated serial
port, on hardware that exists only as C in this repository:

```
Seatbelt MACF policy initialized
BSD root: md0, major 2, minor 0
AppleS5L8900XIO::start: chip-revision: EVT0
AppleARMPL192VIC::start: _vicBaseAddress = 0xe3141000
AppleS5L8900XSerial: Identified Serial Port on ARM Device=uart0 at 0x3cc00000
AppleMultitouchZ2SPI: successfully started
IOSDIOController::enumerateSlot(): CMD5 failed ... (no card is modelled)
```

Those lines come from **Apple's own kernel extensions**, unmodified, after they
matched the emulated hardware — evidence the guest reached those drivers, not
that every device behind them is complete.

Milestone 5 — `launchd` starts SpringBoard, the home screen renders, and you tap
it — is **substantially reached: the home screen renders and a gesture drives
it.** Three blockers were cleared to get there, and the fourth turned out not to
exist.

The first blocker is fixed. SpringBoard used to die and be restarted roughly
every 470 million instructions — 30 times in a single run — because Apple's
rendering code had been told to use the MBX graphics chip this emulator
deliberately does not provide, and then stored through a null pointer. Setting
`CA_ENABLE_MBX2D=0` in SpringBoard's environment ended that loop: it is a switch
Apple's own code reads, selecting the CPU renderer Apple already ships, so it
needs no GPU emulation. Thirty restarts became one, and SpringBoard went on to
build its interface and make its window visible for the first time.

The second blocker is fixed too, and the guest now renders. SpringBoard used to
die writing the screen, with a memory-protection fault on the very first store.
The cause was four indirections away: `/device-tree/vram` ships with
`reg = {0,0}` and real iBoot fills it in. We load the kernel directly, never run
iBoot, and so never did. Without it Apple's IOSurface layer cannot publish its
video-memory region, the display driver falls back to describing the framebuffer
as output-only, and the kernel then maps output-only memory **read-only** into
the process that wanted to draw on it. Apple's code was correct throughout; we
had simply never told it where video memory lives.

Filling that one property in — the same in-memory device-tree patch this
emulator already applies for the DRAM bank and the panel ID — produced the frame
above: **14,264,987 changed scanout bytes**, where every previous run changed
zero. SpringBoard was still compositing when the run hit its cap.

That run's device is **unactivated**, so what SpringBoard draws is the
activation screen. Provisioning `ActivationState = FactoryActivated` into the
work image clears the lockout and the guest reaches the lock screen.

The third blocker was the touchscreen itself, and it was the hardest. A Z2
digitizer has no flash: the host must download its firmware over Apple's
undocumented HBPP protocol on **every** boot, and until that completes the part
never runs application firmware, so the driver correctly refuses to interrogate
it. Reverse-engineering that protocol took four separate fixes — a chip-select
edge that split a packet header in half, a receive FIFO left full by a
transmit-only DMA burst, an acknowledgement literal that differs between two
senders in the same driver, and one wrong inference that a call probe refuted.

The fourth blocker did not exist. With touch delivered end to end, a drag on the
unlock slider still appeared to do nothing — and five runs were spent hunting a
coordinate bug that was not there. Every screenshot had been captured about four
guest-seconds **after** the finger lifted, and iOS springs the knob back when a
slide falls short, so a working gesture and no gesture rendered the same frame.
Photographing the screen *during* the drag showed the knob tracking the finger
all along; the drag simply ended too early to cross the threshold. The lesson,
recorded in [`docs/multitouch.md`](docs/multitouch.md) §6.17.1, is that "no
difference from baseline" is a null result until the instrument is shown capable
of registering a change.

Milestones are tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md), the run-by-run
history including every dead end in [`docs/BOOTLOG.md`](docs/BOOTLOG.md), and
the diagnosis procedure in [`docs/debugging.md`](docs/debugging.md).

**Two separate things exist.** The command-line harness (`bootkernel`) runs
Apple's real software, on a desktop. The installable iPhone app runs only a
small synthetic test program exercising the CPU, serial port and screen bridge —
it cannot boot the OS yet, and has no audio or networking. A finger on its
screen is now mapped to panel pixels and handed to the emulated touch
controller, but that controller refuses reports while no driver has announced
itself, and the app's synthetic guest has none; the app says so on screen
rather than reporting a delivery that did not happen. Merging the two into a
shared guest session is the next app-side prerequisite.

## How it works

The emulator models the chip, not the operating system: an ARMv6 CPU
interpreter, memory translation, and device models for the serial port, timers,
interrupts, display controller and power management, with the guest's disk
served from a file on the host. Apple's kernel, `launchd` and SpringBoard run on
top, believing they are on a real 2009 iPhone. Everything the emulator claims
about the guest's hardware is handed over at boot as a device tree, which is why
hiding a device means editing that list rather than deleting code. All of it is
plain C11 with no third-party dependencies, so it tests quickly on an ordinary
desktop and drops into the iOS app unchanged; only the display and lifecycle
shell is Apple-specific. Detail in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

A translator that compiles guest ARM code into native arm64 exists and is tested,
but it has no code cache or dispatcher, the boot loop never calls it, and the iOS
app excludes it — the interpreter does all the work today, which is why emulation
runs roughly 100x slower than the real device on real firmware. An old phone is the intended host
precisely because the A9 chip predates Apple's later hardware defences against
generating code at runtime, but the app only *reports* whether it could create
writable-and-executable memory; it never jumps into generated code, because
guessing wrong could make it crash on every launch. Realtime speed is not
promised: [`docs/dynarec.md`](docs/dynarec.md) §10.3's unmeasured projection puts
a mature translator at roughly **0.15–0.45x** of the guest's nominal rate, with
no measurement on the phone yet, and §0 keeps the score.

## Build & run

Building the app needs no local Apple SDK or toolchain — CI does the
Apple-specific build. Booting Apple software needs firmware you supply yourself.

**Test the core locally** (any OS with a C compiler and CMake):
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The public suite runs without Apple firmware; extra symbol and driver checks
switch on only if you supply a kernelcache. Test counts move as coverage grows,
so trust the current output and CI logs, not a number copied into this file.
`Release` is the default on purpose: the interpreter is the hot loop here. Add
`-DS5LBOX_JIT=ON` in a separate build directory to compile the (inactive)
translator and run its tests.

**Boot the kernel** once you have supplied firmware. The recommended path leaves
your firmware untouched and needs a work path that does not already exist:
```sh
mkdir -p work
build/core/bootkernel firmware/kernel.macho \
    -d firmware/devicetree.bin \
    -c "debug=0x8 serial=1 nand-enable-adm=0" \
    --external-md firmware/rootfs.img work/rootfs-7e18-run01.img \
    -R 128 \
    -n 420000000
```

`--external-md` accepts only these exact inputs, checked by size and hash before
anything is opened:

| Accepted input | Bytes | SHA-256 |
|---|---:|---|
| `firmware/kernel.macho` | 7,942,144 | `0d8cdb339d37cf37a1db2638fff79272ecd63a17764bf7666efa1618725df70c` |
| `firmware/devicetree.bin` | 40,544 | `4867c95fedf544bda2ecaa2626ae14c01a60d7771dc53ffe6fd3a6aac8b8ba57` |
| `firmware/rootfs.img` | 433,274,880 | `c3251e7f092c939d5818e92086cb47680981cfb03731de7b55d238c942eb5e82` |

It then creates a writable work image — exactly 466,825,216 bytes (445.199 MiB)
at the default growth, up to a 512 MiB volume ceiling with larger `--grow`
values — and serves the guest's disk from it. Budget at least 500 MiB plus room
for logs; the parent directory must exist. This mode is cold-boot only and
rejects snapshots. Any guest storage error, undefined instruction, kernel panic
or halt exits nonzero; the work image is deliberately kept afterwards and the
next run refuses to reuse that path, so archive it or pick a new filename. A
cleanup warning means a second large temporary may also be left behind.

Your kernel, device tree and root filesystem stay byte-for-byte unchanged: the
five firmware patches touch only the loaded kernel copy, device-tree edits only
the in-memory copy, and filesystem changes only the work image — where the
guest's `/private/etc/fstab` is repointed at the emulator's fake disk and the
volume is **grown** by `--grow` MB, default 32. Both are required, for reasons and
numbers given in [`docs/BOOTLOG.md`](docs/BOOTLOG.md).

Other flags: `nand-enable-adm=0` stops a flash-controller driver panicking on
unmodelled hardware; three workarounds are automatic and printed in the run
header (the real-time-clock wait patch, and hiding the MBX graphics and SHA-1
accelerator nodes); `-g` and `-S` switch those two known-broken paths back on for
diagnostics; `--external-md` rejects `-K`, which would disable its kernel patch
set. The older mode that loads the root filesystem into guest RAM
(`-r firmware/rootfs.img -R 512`) remains for checkpoint replay and alone accepts
`--keep-fstab`, which reproduces the original `launchd` halt; its `-R 512` is a
hard limit imposed by the kernel and the modelled memory map, not a preference,
so historical 768 MiB experiments are not valid recipes.

### The tools

| | |
|---|---|
| `bootkernel` | boots the kernel and reports where it stopped: progress checkpoints, a sampled profile, every hardware page touched and what touched it, fault sites, and the guest's console output |
| `bootkernel --external-md <source> <new-work>` | the gated cold-boot path above: verify the firmware, create a writable work image, and serve the guest's disk from the host instead of pinning ~445 MiB inside guest RAM |
| `bootkernel -L` | print the map of drivers built into the kernel and exit without booting |
| `bootkernel --snapshot-at <insn> <file>` / `--restore <file>` | save and resume the running machine. Unreachable, missed, malformed or incompatible checkpoint requests fail loudly instead of silently producing nothing |
| `snapboot` | the snapshot acceptance harness — it also prints a report derived from the live machine, because comparing two snapshot files alone lets a field the format never stores cancel out on both sides |
| `machoinfo <kernel> -k` / `-r <addr>` | dump the driver map, or resolve one address to a kernel symbol |
| `img3dump`, `unlzss`, `runfw` | firmware container, decompression, and bare-payload tools |

[`docs/debugging.md`](docs/debugging.md) is the procedure these add up to.

**Get the app:** on a matching push or manual dispatch, the `ios-build` workflow
produces an ad-hoc (`ldid` fake-signed) `S5LBox.ipa` as a temporary GitHub
Actions artifact. CI builds, signs and packages it; it does not install or launch
it. Installing it is your affair. The emulator asks nothing of the host beyond
an ordinary app sandbox, so any method that gets a fake-signed `.ipa` onto
your device will do. No Apple Developer account is involved.

**Supply firmware:** put your **own** iPhone OS 3.1.3 files and the documented
decryption keys in the git-ignored `firmware/` directory — see
[`docs/BOOT_CHAIN.md`](docs/BOOT_CHAIN.md). Reaching the root mount also needs
the IPSW's root disk image decrypted with the published RootFS key. The app has
no firmware importer yet, so these instructions do not make the app boot the OS.
No Apple firmware is committed or bundled.

## Requirements

- **Host: a modern iPhone. A jailbreak is NOT required.** Corrected
  2026-07-29 against a measurement rather than an assumption: the app has been
  run on a **stock iPhone 17 (iPhone17,2) on iOS 26.1**, which reports
  `CS_DEBUGGED : no` in its own on-device self-test and boots the guest kernel
  anyway. This project was developed against a jailbroken iPhone 6s Plus (A9,
  iOS 15), and the README stated that as a requirement when it was only the
  first host it happened to run on.

  It asks nothing special of the host because it is a pure **interpreter**. No
  generated code is executed, so nothing has to be made writable-then-executable
  and no debug entitlement is needed. That changes if the code translator is
  ever finished: JIT execution needs both an executable mapping and a process
  the kernel will let run it, which is exactly what `CS_DEBUGGED : no` denies.
  The self-test already reports `JIT execute : not run at startup (capability
  preflight failed)` on that stock device, and the translator stays off unless
  an opt-in on-device check confirms generated code really can run.

  NOT established: which iOS versions or devices this works on in general. Two
  hosts are two data points, not a compatibility matrix.
- **Firmware:** your own iPhone OS 3.1.3 image and keys. **No Apple firmware
  image or decryption key is bundled.**

## Legal

S5LBox is an independently written emulator under the MIT license. It ships
**no Apple firmware images or decryption keys.** You supply firmware you are
entitled to use. "iPhone", "iOS", and "iPhone OS" are trademarks of Apple Inc.;
this project is not affiliated with or endorsed by Apple.

## Credits

Created by [**j0shua-SYSON**](https://github.com/j0shua-SYSON). Standing on the
shoulders of the reverse-engineering community whose public research made the
S5L8900 boot chain and iPhone OS 3 firmware keys knowable.

<div align="center">

**If a real 2009 iPhone booting inside a 2015 iPhone sounds worth watching —
give it a ⭐ and follow along, milestone by milestone.**

</div>
