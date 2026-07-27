# iOS3-VM continuation handoff

> **CURRENT STATE, 2026-07-26 — read §13.0j before anything else.**
> SpringBoard is **crash-looping**, not progressing. The guest's own
> `ReportCrash` wrote 35 byte-identical `EXC_BAD_ACCESS (SIGBUS)` reports:
> `KERN_PROTECTION_FAILURE at 0x00000048`, crashed thread 3, `pc 0x30e1ea50`
> in `_mbx2DDisable+0x20`, reached from `CA::RenderMBX2D::render` on the IOMFB
> vblank callback. `launchd` respawns SpringBoard roughly every 470 million
> instructions, forever. The root cause is a NULL `_mbx2DGlobalContext` that
> QuartzCore stores through after its own MBX2D initialisation has already
> failed. The fix is `CA_ENABLE_MBX2D=0` in SpringBoard's launchd environment,
> which selects QuartzCore's shipped CPU software compositor; **no GPU emulation
> is required.** That fix is verified at instruction level from disassembly and
> **has not been run. SpringBoard has still never rendered a frame.**
> §13.0j also lists six retractions that must not be re-derived.
>
> Everything below this line that frames the missing frame as a budget, latency,
> determinism, or checkpoint-fidelity question is superseded by §13.0j. The
> Run23-era text is retained as history.
>
> Last reconciled after publication and hosted CI: 2026-07-25, branch
> `codex/m5-hardening`. The diagnostic implementation and tracked Run23 launcher
> are exact commit
> `5a40c5eec5bbf7c4b7d8909d0c1f364bc078338a`.
>
> **Read this first:** the strongest completed firmware run is now **Run23**,
> exact source commit `777afb4c2350690ecd40cd9e69d12e3967a227cb` (build inputs
> byte-identical to hosted-green `5a40c5e`). SpringBoard has executed deep into
> `applicationDidFinishLaunching:`, but it has **not rendered a frame**:
> `UIController` hits, live-scanout mutations, and changed pixels are all still
> **zero**, and the PPM is byte-identical to the seed.
>
> Run23 answered Run22's three open measurement questions and retired one
> hypothesis:
>
> - both send-path route PCs are now **BOUND** to the exact mqueue `c0d705b8`
>   and kmsg `c3d3c000`;
> - the destination queue holds **five genuinely linked** messages with
>   **`reserved-or-in-flight=0`**, all carrying request ID `0x0054b557` and
>   2,104 bytes to `c0d705a0` with five distinct reply ports;
> - the receive right decodes **AUTHORITATIVELY** to **launchd, PID 1**
>   (`ip_receiver_name=0x1b03` validated before the union);
> - **AppleBaseband never delivered a notification.** It enabled its reset event
>   source and the callback never fired, so no baseband send or queue route
>   exists. The delivered-notification hypothesis is dead.
>
> Run23 did **not** prove why CommCenter has not checked in, that the missing
> reset callback is that reason, that any thread was still enqueued at the cap,
> a reply, a permanent deadlock, or anything about pixels.
>
> The diagnostic implementation itself is **committed, pushed, independently
> audited, and hosted-CI-green**.
> [Core run 30143448600](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30143448600)
> passed all eight jobs, and
> [iOS run 30143455036](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30143455036)
> passed its unsigned-package job for exact commit `5a40c5e`. Strict GCC,
> one-target build, `git diff --check`, exact zero-instruction firmware gates,
> and the tracked launcher's parser/fail-closed smoke also passed. Hosted CI
> contains no private firmware and establishes nothing about Run23.

This document is an operational and technical continuation guide for another AI
agent. It deliberately repeats critical facts from the README and project docs
so that a context-compacted agent cannot accidentally turn a diagnostic
candidate into a boot claim. The authoritative historical evidence remains in:

- [`README.md`](../README.md)
- [`QUALITY.md`](QUALITY.md)
- [`BOOTLOG.md`](BOOTLOG.md)
- [`ROADMAP.md`](ROADMAP.md)
- [`ARCHITECTURE.md`](ARCHITECTURE.md)
- [`BOOT_CHAIN.md`](BOOT_CHAIN.md)
- [`debugging.md`](debugging.md)
- [`dynarec.md`](dynarec.md)
- [`activation.md`](activation.md)
- [`networking.md`](networking.md)

## 0. What this emulator is and is not

The full statement is
[README.md, "What this is, and what it is not"](../README.md#what-this-is-and-what-it-is-not).
The short form is repeated here because a fresh agent must not overstate this
project — to the user, in a commit message, or to itself.

- **Real.** The user's own unmodified iPhone OS 3.1.3 (7E18) firmware executes.
  Real XNU 1357.5.30 boots, Apple's own kexts match and start, the real root
  filesystem mounts, and real daemons run: `launchd`, `securityd`, `installd`,
  `mDNSResponder`, `fairplayd`, `itunesstored`, `lockbot`, `CommCenter`,
  SpringBoard. When SpringBoard crashed, the guest's **own** `ReportCrash` wrote
  real crash reports into its own filesystem, which is how the current blocker
  was diagnosed. CPU: ARM + Thumb + VFPv2 on the reached path, the ARMv6
  unaligned-access model with `SCTLR.U` and `SCTLR.A` both honoured, MMU with XN
  enforcement, CP15; runs are bit-exact reproducible. Modelled peripherals:
  UART, timers, VIC, CLCD, I2C/PMU, and the DWC2 USB configuration registers.
- **Not modelled at all.** No touch input. No audio. No networking. No cellular.
  No Wi-Fi. No Bluetooth. No camera. No accelerometer. No GPU.
- **Declared absent to the guest.** The loaded (in-memory) device tree
  un-matches five nodes that real hardware has: `arm-io/mbx` (the PowerVR MBX
  GPU), `arm-io/sha1`, `baseband`, `arm-io/spi2` (the baseband transport), and
  `arm-io/usb-otg`. The firmware on disk is never modified; only the loaded copy
  is edited. Each un-match has a documented reason, but the net effect is that
  the guest is told it is running on a machine with less hardware than a real
  iPhone.
- **Approximated or invented.** The DWC2 USB configuration register values
  (`GHWCFG1`/`GHWCFG2`/`GHWCFG4`) are a legal and sufficient configuration, not
  measured from real S5L8900 silicon. Rendering goes through QuartzCore's own
  CPU software compositor, selected with `CA_ENABLE_MBX2D=0`; the real device
  composites on the MBX GPU. That is a switch Apple's code reads and a renderer
  Apple shipped, but it is not the path real hardware takes. Timing is not
  cycle-accurate: an interpreter running roughly 200x slower than the real
  412 MHz part, with a synthetic 412 MHz : 6 MHz instruction-to-timebase ratio
  rather than real cycle timing.
- **Patched or substituted.** The kernel is patched in RAM at load — IORTC
  timeout forced to zero, `IOFindBSDRoot` redirected to `md0`, SVC hooks
  installed for the host storage bridge — exact-gated against a SHA-256 and a
  nine-segment check of the 7,942,144-byte kernel. Storage is not NAND: the root
  filesystem is served by a host-backed bridge (external-md) into a unique
  writable per-run work image, with `/etc/fstab` rewritten in that work image to
  match. No secure boot chain is executed; the kernel is loaded directly, and
  SecureROM, LLB and iBoot are not run (IMG3 parsing and an extracted LLB
  payload have been executed separately, but not as a chain). Optionally, and
  off by default, one LaunchDaemon plist in the work image is rewritten
  size-neutrally to add `CA_ENABLE_MBX2D=0`.
- **Unproven.** The single most visible property of an iPhone — that it displays
  a home screen you can touch — has never been demonstrated here. SpringBoard
  has not rendered a frame. Every run to date ends with an identical, unchanged
  framebuffer.

## 1. User objective and non-negotiable priorities

The user's objective is not merely to reach the old M5 label. The continuing
goal is to **finish the whole emulator**, with the immediate priority ordered as
follows:

1. Boot an original, user-supplied iPhone OS 3.1.3 (7E18) system reliably.
2. Reach a recognizable, guest-produced SpringBoard frame.
3. Make the guest usable as a phone-like environment: touch first, then sound
   and guest internet, with internet treated as a high-priority product feature.
4. Integrate the real guest into the installable iOS app.
5. Optimize initially for the jailbroken iPhone 6s Plus, the only physical test
   device, without coupling the emulator core to that phone or to native tweaks.
6. Preserve a portable core and host-adapter architecture so other platforms
   can be supported later.
7. Continue hunting rare, timing-sensitive, pointer-reuse, overflow, malformed
   state, and lifecycle edge cases. A silent diagnostic misattribution is
   especially dangerous because it can provoke a wrong hardware change and a
   later device crash.

The current bottleneck is the guest boot, not JIT, packaging, signing, or device
deployment. Do not divert the main effort into the iOS shell or JIT until the
next boot frontier is explained. The current interpreter is slow but has already
reached the relevant path. Correctness and evidence come before performance.

The project is an emulator engineering effort. Diagnostic disassembly and
inspection of the user's supplied firmware are used to reproduce hardware
semantics and understand the boot path. Keep this work scoped to the emulator
and the user's artifacts.

### Definition of the immediate M5 outcome

M5 is not complete when a SpringBoard symbol is entered. It requires, at
minimum:

- a fresh, repeatable cold boot from the exact supported immutable inputs;
- the real Apple kernel, root filesystem, `launchd`, and stock SpringBoard;
- a guest-driven recognizable SpringBoard or activation frame;
- evidence that the framebuffer changed through a live scanout surface rather
  than retaining the seed;
- no guest panic, debugger entry, unsupported-instruction stop, storage bridge
  failure, or host crash;
- enough guest memory headroom that the result is not a one-off near-OOM event;
- the first guest touch event accepted through an emulated guest input path.

Entering `applicationDidFinishLaunching:` or reaching `UIController` is useful
control-flow evidence. Neither alone is a render.

### Definition of the whole-project outcome

Do not stop permanently after M5. The user's full target also requires:

- moving the proven real-guest session from `bootkernel` into the app;
- robust iPhone 6s Plus execution without device-crashing launch behavior;
- touch through the guest's expected multitouch controller/report path;
- guest audio through an emulated controller/codec and bounded host PCM sink;
- guest networking, initially via the documented PPP-over-UART route and a
  portable host NAT/socket adapter;
- optional JIT acceleration only after a recoverable on-device execution probe,
  differential validation, dispatcher/cache/invalidation support, and safe
  fallback;
- portability: UIKit, CoreGraphics, iOS executable-memory policy, sockets, and
  audio APIs remain host adapters rather than contaminating the machine core.

## 2. Machine-resource policy: C: is critically low

Treat the user's low C: storage as a hard boundary, not a preference.

- Keep all mutable build trees, temporary files, caches, downloaded tools,
  copied binaries, logs, work images, and firmware-run artifacts below
  `F:\JOSHUA_1st_2021\projects\iOS3-VM`.
- Before any tool that may create temporary files, set `TEMP`, `TMP`, and
  `TMPDIR` to an existing F:-local directory such as `work\tmp` or the current
  run's `tmp` directory.
- Do not run package-manager installs globally. Do not change global PATH,
  global config, user profiles, or machine-wide state.
- If another dependency is genuinely required, install or unpack it inside this
  repository's ignored `work` tree on F:.
- Prefer tiny local checks: `git diff --check`, one-target compilation,
  syntax-only compilation, zero-instruction startup/self-checks, and focused
  tests.
- Run the broad platform, sanitizer, strict-warning, JIT, and iOS-build matrix
  in GitHub Actions through `gh`.
- A fresh external-md boot creates a 466,825,216-byte work image. Run22's
  retained evidence directory occupies 447.42 MiB. Budget at least roughly
  500 MiB plus logs and filesystem headroom for every fresh run.
- External-md is create-only and refuses an existing work image. Do not bypass
  this guard.
- Do not delete historical `work` directories or rootfs images merely to regain
  space. They are user evidence. Inspect exact targets and ask before material
  deletion.

PowerShell setup for a small local command:

```powershell
$repo = 'F:\JOSHUA_1st_2021\projects\iOS3-VM'
$env:TEMP = Join-Path $repo 'work\tmp'
$env:TMP = $env:TEMP
$env:TMPDIR = $env:TEMP
Set-Location -LiteralPath $repo
```

The prepared Run23 launcher sets those variables to its own F:-local
`work\run23-commcenter-baseband\tmp` directory.

## 3. Repository and Git state

### Remotes, branch, and committed baselines

- Repository:
  `F:\JOSHUA_1st_2021\projects\iOS3-VM`
- Public remote:
  `https://github.com/j0shua-SYSON/iOS3-VM.git`
- Active branch:
  `codex/m5-hardening`
- Tracking branch:
  `origin/codex/m5-hardening`
- Current boot-diagnostic implementation:
  `5a40c5eec5bbf7c4b7d8909d0c1f364bc078338a`
- Diagnostic subject:
  `Harden CommCenter boot diagnostics`
- Strongest completed private-firmware baseline:
  `40209b27cb10d01c552398ff918ee613c4908ed0`
- Firmware-baseline subject:
  `Trace the CommCenter boot frontier`
- At handoff time, local and remote branch tips match.
- `main` is far behind this continuation branch. Do not switch to or merge into
  `main` casually.

The exact Run22 baseline passed all eight jobs in hosted core run
`30106957804`. That firmware result belongs to `40209b2`. The current
diagnostic implementation has its own exact hosted results:

- [core run 30143448600](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30143448600):
  success, eight of eight jobs;
- [iOS run 30143455036](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30143455036):
  success, one unsigned-package job.

The documentation publication that contains this final status is a docs-only
descendant of `5a40c5e`; its `CMakeLists.txt`, `core/`, and `tools/` build inputs
are unchanged from that validated implementation.

### Dirty-tree ownership boundary

At handoff, `git status --short` reports:

```text
 M .github/workflows/ios-build.yml
 M app/iOS3VM.entitlements
 M docs/activation.md
 M docs/networking.md
?? .codex/
```

Treat the following existing dirty paths as **user-owned and protected** unless
the user explicitly reassigns them:

```text
.github/workflows/ios-build.yml
app/iOS3VM.entitlements
docs/activation.md
docs/networking.md
.codex/
```

Do not edit, discard, stage, commit, or push those paths as part of the current
boot work. Their diffs overlap app-signing and documentation work that is
outside the diagnostic ownership boundary.

The published boot-effort paths in `5a40c5e` are:

```text
README.md
docs/BOOTLOG.md
docs/QUALITY.md
docs/ROADMAP.md
tools/bootkernel.c
tools/run23-cold-replay.ps1
docs/AGENT_HANDOFF.md
```

Re-run `git status`, `git diff --name-only`, and path-specific diffs immediately
before staging. Stage explicit paths, never `git add -A` or `git add .`.

The user initially asked to remove a Claude contributor and then explicitly
reversed that request. **Do not change the Claude contributor/credit or rewrite
history to remove it.**

### Commit and evidence discipline

- A hosted workflow result belongs only to its exact commit.
- A private-firmware run belongs only to its exact source commit and copied
  executable hash.
- A later test-only commit does not inherit a prior firmware run.
- Build the long-run binary from the committed source that will own the result.
- Copy that binary into a run-specific directory and record byte length and
  SHA-256 before launch.
- Do not edit source while claiming a running binary represents the new tree.
- Push documentation updates with code regularly, but only after the evidence
  exists.

## 4. Firmware integrity and what “unmodified” means here

The canonical inputs are user-supplied and git-ignored. They must not be
committed, distributed, or edited in place.

| Input | Exact bytes | SHA-256 |
|---|---:|---|
| `firmware/kernel.macho` | 7,942,144 | `0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C` |
| `firmware/devicetree.bin` | 40,544 | `4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57` |
| `firmware/rootfs.img` | 433,274,880 | `C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82` |

The original files have remained byte-for-byte unchanged across the documented
runs. The emulator does make compatibility changes, but their ownership is
important:

- Five firmware-specific kernel patches are exact-gated and applied to the
  **loaded kernel copy in guest memory**, never to `kernel.macho` on disk.
- iBoot-style properties are edited only in the **in-memory device-tree copy**.
- In external-md mode, the source rootfs is hashed and copied into a unique,
  unpublished temporary/work image. Fstab retargeting and HFS growth occur only
  in that separate work image.
- Legacy `-r` mode applies filesystem transformations only to the loaded RAM
  copy.
- Guest writes through external-md go only to the unique work image.

Therefore the honest wording is:

> The canonical firmware inputs are original and unmodified on disk. The
> emulator uses exact-gated, in-memory boot compatibility patches and a separate
> writable rootfs work image.

Do not simplify this to “the guest is totally unpatched.” It is equally wrong to
say that the original firmware files were modified.

External-md accepts only the exact 7E18 identities above. The default 32 MiB
growth produces an exact 466,825,216-byte work image. It is cold-boot-only and
incompatible with snapshots.

No Apple firmware or decryption keys belong in source control, CI artifacts,
docs, chat summaries, or device bundles.

## 5. Architecture: what exists and what does not

### Boot route

The active route is:

```text
bootkernel host harness
  -> exact-gated loaded XNU kernel
  -> synthesized iBoot-style boot_args/device-tree handoff
  -> real XNU and prelinked Apple drivers
  -> external-md host-backed root filesystem
  -> launchd
  -> stock SpringBoard
```

The current path does **not** execute SecureROM or iBoot. M3 can parse/decrypt
IMG3 and execute an LLB payload, but the long M5 boot enters XNU directly using
the handoff synthesized by `bootkernel`.

### Portable core

`core/` is plain C11 and holds:

- ARM1176/ARMv6 interpreter and VFP;
- MMU, CP15, exceptions, exclusive monitor, and memory access;
- S5L8900 machine and device models;
- VIC, timers, UART, power, I2C/PCF50635, CLCD, NOR, storage primitives;
- IMG3, AES, LZSS, Mach-O, kernel-symbol, and device-tree parsing;
- snapshots;
- optional AArch64 JIT foundation.

The guiding rule is: unsupported architectural behavior traps instead of
guessing. MMIO placeholders are bounded, named, and counted. A register that
must change autonomously requires a real device-state model, not a magic stub
value.

### Host tools and policy

`tools/` owns host-only policy and diagnostics:

- `bootkernel.c`: real-kernel boot harness, exact diagnostics, reports;
- `snapboot.c`: snapshot acceptance/replay harness;
- `machoinfo.c`: kernel and prelinked-kext mapping/symbol resolution;
- `ios3_kernel_patch.c`: exact 7E18 kernel-patch manifest;
- `guest_patch.c`: bounded atomic guest patch transaction;
- `rootfs_work.c`: bounded create-only work-image provisioning;
- `file_block.c`: host file-backed block adapter;
- `runfw.c`, `img3dump.c`, `unlzss.c`: firmware tools.

The current post-Run22 work is concentrated in `tools/bootkernel.c`. It is
diagnostic-only by intent: it observes exact stock paths but does not change
baseband lines, queues, Mach messages, or guest outcomes.

### External-md memory layout

For the exact 128 MiB display-enabled external-md path:

```text
DRAM                 0x08000000..0x10000000
boot_args            0x087db000
raw bounce slots     0x087dc000..0x0885c000
Boot_Video           0x0885c000..0x088f2000
topOfKernelData PA   0x088f4000
```

The framebuffer is 320x480x4, stride 1280. The placement and
`topOfKernelData` relationship are load-bearing. Old layouts accidentally put
framebuffer pages into the free pool or misaligned XNU's L1 setup.

The general modeled SDRAM aperture is `[0x08000000, 0x28000000)` and NOR begins
at `0x28000000`. Historical 768 MiB commands overlap NOR and are invalid current
recipes. In legacy direct-RAM mode, 512 MiB is also constrained by the kernel's
compiled virtual layout. Do not revive old 768 MiB commands as a memory fix.

### External-md bridge

The root filesystem is no longer pinned into guest DRAM in the current cold
path. Exact kernel patches redirect the memory-disk strategy and native
faultable `uiomove64` work through privileged SVC seams and bounded host block
operations. Four 128 KiB bounce slots sit below `topOfKernelData`. A coherent
in-memory tail handles the stock `_mdevrw` behavior beyond logical media end.

The bridge must report:

- zero strategy/raw guest errors;
- zero host read/write/flush failures;
- zero pending continuations;
- expected work-image length;
- unchanged canonical source hashes.

A clean instruction-cap exit is not enough if any bridge invariant fails.

### iOS app boundary

The installable app does **not** boot the real OS. It currently runs a small
synthetic ARM guest to exercise the CPU, UART, and CoreGraphics framebuffer
bridge. It has:

- no shared real-guest session;
- no guest touch;
- no guest audio;
- no guest networking;
- no active JIT boot dispatch.

`app/Sources/VMGuest.c` is the portable seam. UIKit/CoreGraphics/QuartzCore
belong in the iOS shell. The next app prerequisite, after a stable CLI boot, is
to extract the proven session ownership/lifecycle from `bootkernel` into a
shared portable guest-session API.

## 6. Milestone and boot-history summary

### M0 through M4

- **M0, pipeline:** portable core builds and tests across hosted platforms; the
  unsigned/fake-signed iOS app is built on hosted macOS. CI does not install or
  launch the app.
- **M1, CPU:** the ARMv6 interpreter and VFP execute the reached real-firmware
  path. This is reached-path coverage, not complete ARM architecture coverage.
- **M2, SoC:** the S5L8900 machine boots a bare payload and models enough bus,
  interrupt, timer, power, UART, I2C/PMU, CLCD, NOR, and storage behavior for
  XNU. NAND VFL/FTL remains unimplemented.
- **M3, firmware:** IMG3 parsing/decryption, LZSS, Mach-O, device tree, and LLB
  execution exist.
- **M4, XNU:** the real XNU kernel logs, starts many original Apple drivers,
  mounts the real root filesystem as md0, and runs `launchd`.

### The external-md sequence

- Initial exact external-md work removed the roughly 445 MiB guest-RAM rootfs
  pin while preserving a create-only writable work image.
- Run03 crossed the first raw bridge guard but fsck failed.
- Run04 isolated faultable native write-side paging and a read spanning media
  plus allocation tail.
- Run05 implemented the native `uiomove64` redirect/completion path and cleared
  fsck/root-mount I/O.
- Run06 sustained the path to 1 billion instructions.
- Run07 sustained it to 2 billion with `launchd`, `/dev/md0`,
  `mDNSResponder[14]`, and zero bridge failures.

### Display and SpringBoard sequence

- **Run08:** display bundles executed, but no CLCD MMIO or useful frame.
- **Run09:** reached a SpringBoard launch request, not proof of SpringBoard
  execution.
- **Run11:** exposed the `POSIX_SPAWN_SETEXEC` shape.
- **Run15:** proved exact stock SpringBoard executable entry and later real
  SpringBoard Objective-C method execution. No UI readiness or rendering.
- **Run16:** PMU/I2C and display-driver startup progressed; CLCD became active,
  but the run ended before userspace and retained the seed-only frame.
- **Run17:** combined process and display paths and reached the local window
  server/IOMobileFramebuffer boundary.
- **Run18:** identified the exact SpringBoard-thread wait as an optional TV-out
  close/swap path.
- **Run19:** proved the model's all-three-bank TV-out timing predicate was wrong.
  The shipped path persistently used mixer+SDO while the control/coefficient
  bank could remain zero.
- **Run20:** exact commit `590d224` corrected TV-out timing. The guest generated
  four TV-out frames, delivered IRQ 30 through the shipped filter/action, woke
  the close gate, returned `IOServiceClose` to PID 20 with `r0=0`, returned
  UIKit `startWindowServer` with a decoded 320x480 display, and entered
  SpringBoard `applicationDidFinishLaunching:`. It then stopped on a valid VFP11
  `FMDHR`/`VMOV.32 d7[1], r4` misclassified as NEON. The framebuffer still had
  zero changed pixels.
- **Run21:** exact commit `debec04` fixed that VFP11 word transfer and reached a
  clean 2.5-billion-instruction cap, 562,020,182 instructions beyond Run20's
  stop. SpringBoard continued through tether/debug/demo/platform-controller
  initialization and entered telephony singleton initialization. Its initial
  CTServerConnection handshake looked up `com.apple.commcenter`, received port
  name `0x4f07`, and blocked in `mach_msg`. This still did not prove queue
  saturation or baseband cause.
- **Run22:** exact commit `40209b2` instrumented the queue boundary and is the
  current completed baseline.

## 7. Exact honest Run22 status

Run22 was a fresh 128 MiB display-enabled cold run of exact source:

```text
40209b27cb10d01c552398ff918ee613c4908ed0
```

Its copied `bootkernel.exe` was:

```text
bytes    548,933
SHA-256 D3B9C9BF543409C9EDC95C1A1233B000D0B6C42AF5C132B69B47C19207196614
```

It exited 0 at the configured 2,100,000,000-instruction cap, with empty stderr.
The canonical kernel, device tree, and rootfs hashes were unchanged.
External-md reported zero failures. Guest free memory bottomed at 50.63 MiB.
The retained evidence directory occupies 447.42 MiB on F:.

### SpringBoard and CommCenter coordinates

- SpringBoard entered `applicationDidFinishLaunching:` at 1,923,358,329.
- It previously returned `isTethered` false at 1,924,647,850.
- It entered `+[SBTelephonyManager sharedTelephonyManager]` at 1,965,837,070.
- The initial CoreTelephony generated request ID was `0x0054b557`.
- The copied-in kernel message retained that exact ID.
- Destination port object: `0xc0d705a0`.
- Embedded mqueue: `0xc0d705b8`.
- At `_ipc_mqueue_send` entry, instruction 1,966,245,348:
  `msgcount=5`, `qlimit=5`, `seqno=0`, `fullwaiters=0`.
- Adjacent queue-full route PC candidate: 1,966,245,373.
- Adjacent pre-store `fullwaiters=1` PC candidate: 1,966,245,387.
- `_thread_block_reason`: 1,966,245,550.
- SpringBoard thread switch-out: 1,966,246,193.
- The sender did not resume before the cap.

### What Run22 proves

- The exact initial CoreTelephony request reached the copied-in kernel message.
- Its destination mqueue was saturated at entry:
  `msgcount == qlimit == 5`.
- The same SpringBoard sender later blocked and switched out.
- It did not resume in the observed interval.

### What Run22 does not prove

- The old route recorder did not retain decisive `r8`, so the queue-full and
  `fullwaiters` PC hits cannot be fail-closed-bound to the same kmsg.
- `msgcount` includes reserved/in-flight slots. It does not prove five linked
  queue nodes.
- The old owner decoder did not first validate `ip_receiver_name`, so its
  printed PID-1 owner is not authoritative.
- Repeated Mach ID 1000 activity from a CommCenter worker is only a candidate
  periodic `clock_get_time` path. The old counter did not require SEND and did
  not resolve the destination.
- No dequeue, receiver wake, reply, permanent deadlock, or reason for saturation
  is proved.
- The fatal baseband UART/SRDY/reset messages are temporally interesting, not
  causal proof.
- A clean cap is not boot completion.

## 7a. Exact honest Run23 status

Run23 is a fresh 128 MiB display-enabled cold run of exact source
`777afb4c2350690ecd40cd9e69d12e3967a227cb`, launched through the tracked
`tools/run23-cold-replay.ps1` (SHA-256
`FF29B15C09AAEF8EAAC461C5503CC04C4586EA0D52AB08C9B3CA25B174D6596D`). Its copied
binary was:

```text
bytes    625,423
SHA-256  978987BE339A7C11A3A3CBB87CBE28DB450518AD3AEB8C7CE5E5A6558ACAD67E
```

It exited **0** at the 2,100,000,000-instruction cap in **1,434.86 seconds**
(`04:29:18.4242467Z` → `04:53:13.2799675Z`), stderr empty, launcher postflight
passed. Canonical kernel/device-tree/rootfs hashes re-verified unchanged; the
work image was exactly 466,825,216 bytes. External-md: 12,195 reads
(49,968,640 bytes), 178 writes (725,504 bytes), **0 failures**; raw path 2
reads, 2 native redirects, 2 completions, **0 pending**. Guest free memory
bottomed at **12,961 pages (50.63 MiB)** at 1,957,101,568. The retained
directory occupies **447.43 MiB on F:**. No `_panic`, `_Debugger`, or
undefined-instruction stop.

### What Run23 proves

- The initial CoreTelephony request reached copied-in kmsg `c3d3c000`
  (header `c3d3c2c4`, bits/size `80001211/2104`, destination `c0d705a0`, reply
  `c39e7000`, id `0x0054b557`); destination and id both matched.
- The containing port `c0d705a0` was an active port object of the expected type
  (`io_bits=80000000`).
- **Owner is authoritative:** `ip_receiver_name=0x1b03` validated first, space
  `c0acfe60` active, task `c0ad7b10`, task-space backpointer matching, proc
  `e0381d68`, signed **PID 1 (launchd)**.
- **Queue walk closed:** head `c21e3000`, `linked=5`, closed, consistent,
  untruncated, no read fault, **`reserved-or-in-flight=0`**. Entries
  `c21e3000`, `c31d7000`, `c3f50000`, `c3e52000`, `c448c000`, each size 2,104,
  destination `c0d705a0`, id `0x0054b557`, reply ports `c2bf6ea0`, `c34d2630`,
  `c2d33d80`, `c31c32d0`, `c31c3cf0`.
- **Both routes BOUND** with `r4=c0d705b8`, `r8=c3d3c000`: queue-full slow
  branch `c00147ba` @1,966,245,373; `fullwaiters=1` pre-store `c00147d6`
  @1,966,245,387. One candidate, one bound, zero rejected on each.
- Block at 1,966,245,550; SpringBoard switch-out at 1,966,246,193; episode
  `OPEN/UNRESOLVED`; receive buffer never read.
- CommCenter identity: armed @517,086,676, task `c2ca2760`, proc `e037f890`,
  **PID 24**; never invalidated; 0 signals; 0 `_exit1`; 10,975,004 user and
  58,986,380 kernel instructions before the send entry, 0 after.
  Owner correlation: **MISMATCH**.
- Six retained CommCenter threads. `e038dbb8`, `e0376000`, `e02e4774` are in
  timed waits on semaphore `c0b239a0` (waitq `c0b239a8`, continuation
  `c0026fc5`, timer active, distinct nonzero deadlines). `e02f5888` blocked in
  `_ipc_mqueue_receive` on mqueue `c0dd99f0` / port `c0dd99d8` (task-local name
  `0x10004001`) at 932,507,189. `e0379bb8` and `e035d000` are historical
  resolved waits.
- **AppleBaseband:** object `c0c3a700`; reset-function setup 1/1 (`c0b6b020`);
  event source result/committed/enable-entry 1/1/1/1 (`c0b6c340`); **reset
  callback hits 0**; reset reads 0; changes 0; dispatch attempts 0; handlers 0;
  send headers 0; routes 0. Frontier line: `event-source enable call was
  entered, but no reset callback was observed since trace start`.
- **The one accepted IOKit interest is CommCenter's**, on AppleBaseband:
  `interest[0] service/port/mqueue=c0c3a700/c3c59ab0/c3c59ac8`, thread
  `e02f5888`, registrations/success 1/1 @931,584,215, baseline receiver
  `0x3503`/`c0acf7e8`/`c2ca2760`/`e037f890`/PID 24. Receive entries on that
  port: **0**. 14 wrappers seen, 13 service-rejected.
- Non-RAM page `0x3d200000` is touched only by
  `com.apple.driver.BasebandSPI` (4 reads, 11 writes) and is **not** a declared
  stub, so reads return zero. Write burst @933,033,890–933,033,922, read-back
  of offsets `0x000/0x004/0x008/0x034` @1,757,842,145–1,757,842,149, final
  writes @1,760,475,736/740, then no further access.

### What Run23 does not prove

- Why CommCenter has not called `bootstrap_check_in` for
  `com.apple.commcenter`, or that it never will.
- That the missing AppleBaseband reset callback is the reason. The blocked
  receive is on port `c0dd99d8`, **not** the interest port `c3c59ab0`, and no
  port-set relationship between them was established.
- That real no-modem hardware would fire that reset callback at all. That must
  come from the shipped binary, not from assumption.
- That the `0x3d200000` window blocks anything. The driver does not poll it.
- Which processes sent the other five queued messages.
- That any thread was still enqueued at the final cap. Every wait is reported as
  "last observed unresolved block; no resume observed"; no final live
  wait-state reread exists.
- A dequeue, reply, permanent deadlock, or any pixel.

### Run23 diagnostic-integrity caveats

The per-thread observer reported **16** exact-hook attribution omissions
(first @551,530,083 pc/thread `c0024dc2`/`e02f6998`; last @1,388,875,916
pc/thread `c0024e14`/`e02f7220`) and **50** unreadable classifications with
**0** readable contradictions. Two threads carry `schedule-uncertain=yes`.
None of these overlaps the decisive `_ipc_mqueue_send` episode, whose route
bindings, queue walk, and owner decode are each independently marked bound,
closed, or authoritative.

### Run23 rendering result

SpringBoard is **not rendered**. TV-out produced 4 frames with one IRQ-30
filter entry/acceptance, one action entry, one completion dispatch, and 8 IOMFB
completions; `startWindowServer` returned at 1,919,831,289;
`applicationDidFinishLaunching:` entered at 1,923,358,329; CLCD ended
scanning/running `1/1` with 604 frames on a 320x480 stride-1280 window at
`0x0885c000`. And yet:

```text
UIController hits             0
live-scanout mutations        0
changed pixels                0
PPM SHA-256
CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB
```

### Rendering result (Run22)

SpringBoard is **not rendered**.

```text
UIController hits             0
live-scanout mutations        0
changed pixels                0
PPM SHA-256
CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB
```

That PPM is the seed-only frame: 153,472 black pixels and 128 white seed pixels,
with no other colors.

## 8. Committed diagnostic hardening at `5a40c5e`

The post-Run22 diff in `tools/bootkernel.c` is very large: it adds exactly
6,668 lines and removes 161 relative to `40209b2`. Most of this is
bounded diagnostic state, exact firmware gates, reporting, and adversarial
startup self-checks. Review the actual diff; do not assume size implies a
hardware behavior change.

The intended invariant is:

> Observe exact stock control flow and guest objects without changing guest
> behavior. If identity, pointer lifetime, linkage, code shape, counter
> integrity, or lifecycle order is uncertain, report the observation as
> incomplete/rejected/unproven rather than promoting it.

### 8.1 Fail-closed CommCenter queue/owner probe

The hardened probe now:

- captures the exact initial user-header signature without pretending the stock
  generated stub's stale `msgh_size` is corruption;
- captures and validates the copied-in kernel kmsg;
- requires the kmsg destination to match the containing port;
- requires the kmsg ID to match `0x0054b557`;
- validates the mqueue-to-port containing-object relationship;
- validates pointer alignment, kernel address range, and guest readability;
- validates active IOKit port type rather than testing only a generic active bit;
- requires an authoritative nonzero `ip_receiver_name` before interpreting the
  receiver/destination/timestamp union;
- validates receiver ipc space, active state, task, task-to-space backpointer,
  proc, and signed PID;
- walks the queue's reciprocal next/previous links into a bounded node array;
- tracks unreadable head/node, non-closure, inconsistent linkage, truncation,
  and read-fault evidence separately;
- computes `reserved-or-in-flight = msgcount - linked_count` only if:
  - msgcount is valid,
  - a queue snapshot was attempted,
  - the head was readable,
  - the walk closed,
  - links were consistent,
  - it was not truncated,
  - there was no queue read fault, and
  - linked count does not exceed msgcount;
- otherwise prints `reservation-count=UNPROVEN`.

Route binding is now register-specific:

- `_ipc_mqueue_post` routes require the exact mqueue/kmsg in `r5`/`r6`.
- `_ipc_mqueue_send` routes require the exact mqueue/kmsg in `r4`/`r8`.
- Route PC candidates are counted separately from bound hits.
- Wrong-register or wrong-object candidates are retained as
  `REJECTED-CANDIDATE`, not silently promoted.
- Counter overflow poisons hit totals instead of wrapping.

The relevant route classes are:

- post receiver woken;
- post no eligible receiver;
- queue-full slow path;
- mark fullwaiters;
- immediate slot reserve.

Startup self-checks include correct bindings, cross-mappings, missing valid bits,
wrong IDs, non-authoritative owner unions, wrong task-space backpointers,
inactive spaces, port-set object types, queue cycles/link mismatches, truncation,
read failures, and invalid reservation arithmetic.

### 8.2 Fail-closed per-thread CommCenter wait observer

The wait observer is keyed by the exact pathname-qualified CommCenter SETEXEC
attempt identity and then by kernel thread. PID is discovered, never hardcoded.

It tracks independent monotonic sequences for:

- user trap episode;
- semaphore-core episode;
- block episode.

It tags:

- SWI entry;
- exact semaphore trap handler;
- semaphore core;
- committed semaphore fields;
- queue assertion;
- `_thread_block_reason` entry;
- committed block fields;
- switch-out;
- switch-in/raw resume;
- block return;
- continuation;
- user return or later SWI.

The key anti-staleness behavior is:

- a new SWI resolves the previous user-trap episode before filtering the new
  trap;
- starting a block clears the prior wait snapshot;
- a snapshot becomes usable only at the committed-fields PC;
- any later switch-in, raw instruction on the switched-out thread, block return,
  continuation, user return, or later SWI resolves the old block;
- unreadable/ambiguous scheduler transitions poison terminal classification;
- exact sparse hooks force a fresh full identity rewalk so cached unreadability
  cannot silently omit decisive evidence;
- if an exact hook cannot be attributed, omission counters and first/last
  PC/thread coordinates are reported and relevant state is poisoned;
- sequence exhaustion is permanent fail-closed state rather than wraparound.

Exact 7E18 code/data gates cover:

- Mach trap table slots 31 and 36 through 39;
- `_semaphore_wait_trap`;
- `_semaphore_wait_signal_trap`;
- `_semaphore_timedwait_trap`;
- `_semaphore_timedwait_signal_trap`;
- semaphore core/field/queue-assert instruction shapes;
- thread wait fields;
- semaphore count at `+0x24`;
- semaphore active state at `+0x28`;
- timer-active at thread `+0x154`;
- expected continuation literals.

An exact last-observed unresolved semaphore block requires, among other checks:

- a live non-terminal CommCenter process identity;
- an open current block;
- committed snapshot and switch-out tagged to the same block sequence;
- strict timestamp ordering;
- no same-block switch-in, return, continuation, raw resume, or uncertainty;
- semaphore core, committed fields, queue assertion, and block tagged to the
  same semaphore sequence;
- exact semaphore count/readability and `active == 1`;
- wait queue equal to semaphore wait-queue address;
- exact event/type;
- consistent wait/signal arguments;
- a recognized continuation.

This proves an ordered block and switch-out with no later execution observed.
It does **not** prove that an off-CPU thread is still enqueued at the final
instruction cap: another thread could wake or terminate it and leave it runnable
but unscheduled. The current report does not perform a final live reread of every
thread's wait queue/state/result. Keep user-facing wording at “last observed
block; no resume observed” until that final validation exists.

Direct Mach semaphore classification additionally requires one open exact trap
from `-36` through `-39`, with handler/core/fields tagged to that same trap.
Timed waits require both a timer-active field and a nonzero deadline. Signal
variants require a signal semaphore; non-signal variants reject one.

Mach ID 1000 is now counted only when a readable outgoing Mach header has SEND
set. It remains a `send-candidate`, not an asserted `clock_get_time` call until
destination/service correlation proves it.

Focused local validation of this wait hardening completed successfully:

- strict GCC validation;
- `git diff --check`;
- one-target rebuild;
- startup self-checks;
- exact firmware code/data gate execution with exit 0;
- adversarial readable pointer-reuse poisoning;
- adversarial exact-hook omission poisoning.

This focused validation is not a long firmware result. The same committed code
also passed the exact eight-job hosted core run `30143448600`.

### 8.3 Read-only AppleBaseband-to-CommCenter causal observer

This observer is implemented and its independent final logic audit is complete.
Its opening source comment is the contract:

> It never synthesizes an edge, alters a queue, or answers a modem request. It
> retains exact-path instruction-entry observations from the stock 7E18
> prelinked image.

The observer exact-gates code and data for:

- AppleBaseband object vtable slots;
- reset-state read function;
- reset callback and high/low/change/suppress/dispatch path;
- event-source factory/action pair and enable call;
- `registerInterest` wrappers and call/result;
- `messageClients`;
- per-client delivery;
- notification handler and Mach send/result;
- notifier activation/free and port teardown;
- `_ipc_mqueue_receive`;
- IPC route PCs and their decisive registers.

It is designed to correlate this full chain:

```text
baseband reset-state setup
  -> event-source creation and enable
  -> reset callback
  -> readable reset state / actual change
  -> high or low messageClients dispatch
  -> a live successful registerInterest subscription
  -> frame-linked notification handler
  -> readable notification Mach header
  -> destination matches the current live registered interest port
  -> Mach-send result
  -> same-frame/same-kmsg bound IPC route
  -> no waiter / queue full / exact selected receiver
  -> exact CommCenter receiver identity or receive entry
```

Every promoted link must belong to the **same retained event**. In particular,
route binding requires a handler frame linked to a notification event, that
event's `linked_dispatch`, an exact still-retained dispatch sequence with the
same service/message/thread, and `messageClients` entry evidence on that
dispatch. A linked handler from one reset event must never be combined with a
matching header or route from a different unlinked event. An unlinked
notification may remain raw diagnostic evidence but cannot increment causal
delivery/frontier conclusions.

This invariant exists because the final integration review caught three real
diagnostic overclaims before Run23:

- the initial route predicate did not require `linked_dispatch`, allowing a
  handler from one event and an unlinked routed send from another to satisfy
  aggregate frontier counters;
- a repeated SEND hook in one handler could inherit the first send's bound
  route state;
- a later failed/mismatched route candidate captured directly into the retained
  object and could overwrite a previously bound kmsg while its bound bit stayed
  set.

The final implementation revalidates the exact dispatch at route and report
time, rejects/poisons repeated sends, captures route candidates into a local
object, copies only successful bindings, and adversarially self-checks unlinked
cross-event mixing, stale/ring-overwritten dispatches, repeats, and
bound-then-bad candidates.

The observer tracks:

- whether subscription history is complete from cold start or unknowable after
  restore;
- unique interests, current port/mqueue identity, and baseline receiver;
- active and retired notifiers;
- pointer reuse and live identity changes;
- pending wrapper frames by thread and stack;
- nested/reentrant message and handler frames;
- bounded newest-retaining dispatch and notification rings;
- overflow, overwrite, mismatch, teardown, stale, and uncertainty state;
- readable/unreadable notification headers;
- header size versus send argument;
- send success/failure;
- exact route candidates versus frame/kmsg-bound routes;
- selected receiver identity;
- exact CommCenter receives on successful registration ports.

The long-lived AppleBaseband owner itself is currently accepted by pointer plus
exact vtable. Unlike interest/notifier/port objects, it has no independent
generation or teardown token. A destroy/recreate cycle at the same address and
vtable is therefore a residual cross-lifetime risk; if runtime evidence suggests
such a lifecycle, treat setup/callback aggregation as unproved.

The report's `frontier:` line intentionally stops at the first unproved link.
For example, it distinguishes:

- reset setup never observed;
- setup returned null;
- event source not enabled;
- callback never fired;
- state read failed;
- state did not change;
- low transition suppressed;
- `messageClients` attempted but entry missed;
- dispatch entered but no frame-linked notification handler;
- notification handler entered but header unreadable;
- header destination not matched to a current successful registration;
- every retained send failed;
- send observed but no exact frame-and-kmsg-bound route;
- no eligible receiver;
- receiver woke but was not proven CommCenter;
- exact CommCenter thread woke.

Strict GCC, the one-target build, `git diff --check`, and exact
zero-instruction firmware-gate validation passed for this observer. Root's
integrated validation also passed the queue/owner, per-thread wait, and
AppleBaseband gates and the `-n 0` smoke. The zero-instruction report shows the
exact 7E18 AppleBaseband code/data gate as validated and all counters at zero,
as expected. That establishes startup gating, not causal correctness under live
firmware or Run23 behavior. Exact commit `5a40c5e` also passed hosted core run
`30143448600` and iOS-package run `30143455036`.

If this implementation changes:

1. Preserve the invariants accepted by the completed independent audit; if the
   implementation changes again, repeat that audit.
2. Re-inspect every ring overwrite, sequence wrap, stale object, nested-frame,
   teardown, and restored-baseline path.
3. Ensure a route cannot bind only by matching a numeric port after pointer
   reuse.
4. Ensure a successful registration is proven by the call/result and still live
   at send/receive time.
5. Ensure an unreadable current identity can only weaken a conclusion.
6. Confirm all per-instruction work is bounded and the common path remains
   cheap enough for a 2.1-billion-instruction run.

## 9. Capstone, Mach-O tooling, and `macholib`

### Capstone

Capstone is useful and has materially accelerated exact ARM/Thumb disassembly:

- distinguishing VFP11 from a misleading modern NEON classification;
- resolving TV-out instruction shapes and register ownership;
- validating exact function windows and route PCs;
- reconstructing AppleBaseband reset/notification paths.

The repository-local Python package is below:

```text
work\tools\capstone-python
```

`work\tmp\capstone` exists only as an empty scratch directory; do not mistake it
for the installed package. Keep any Capstone use local to F:. It is a
disassembler, not a replacement for runtime evidence. A statically plausible
branch still needs exact register, object, identity, and lifecycle correlation.

### Matching XNU source

Public XNU source and Capstone are complementary, not alternatives. Source is
useful for naming structures, union discriminators, lifecycle contracts,
continuations, and IOKit notification semantics. Capstone is useful for proving
the exact instructions, registers, offsets, branches, and stripped prelinked
kext shapes in the actual 7E18 image. The AppleBaseband audit cross-checked
notification-object lifecycle and callback-argument semantics against Apple's
closely related public
[XNU 1456 `IOUserClient.cpp`](https://github.com/apple-oss-distributions/xnu/blob/xnu-1456.1.26/iokit/Kernel/IOUserClient.cpp).

Never promote a source-level inference until the exact guest binary and runtime
path support it. A nearby XNU release can clarify a contract while still
differing in layout or code generation. Conversely, binary disassembly alone
can show a load/store without explaining the higher-level object lifetime.

### `macholib`

`macholib` is not needed for the current blocker. It is a Python Mach-O metadata
parser, not an ARM/Thumb disassembler and not a runtime object/lifecycle tracer.
This repository already has:

- `core/src/firmware/macho.c`;
- `core/include/macho.h`;
- `tools/machoinfo.c`;
- `core/src/firmware/ksyms.c`;
- exact prelinked-kext mapping.

Use the native parser and `machoinfo` for load commands, segments, symbols, and
kext ranges; use Capstone for instruction semantics. Install `macholib` only if
a specific unsupported Mach-O metadata task emerges. If that happens, install
it into an F:-local environment, never globally or on C:.

The `md` in files such as `md_raw_bridge.h` means the XNU memory-disk device
(`md0`/`rmd0`), not Markdown.

## 10. Validation sequence before Run23

Do these in order. Do not launch a long firmware run from a dirty,
not-yet-reviewed source binary.

### 10.1 Reconcile concurrent work

```powershell
git status --short --branch
git diff --name-only
git diff --stat
git diff --check
```

Review the full `tools/bootkernel.c` diff. Confirm the observer is read-only and
that no GPIO, UART, SPI, queue, Mach send, or hardware-return behavior changed.
The AppleBaseband audit is complete. If any source changes follow it, re-run the
focused audit and all validation gates after the final edit.

### 10.2 Small local build and startup gates

Use the existing F:-local build tree and one target:

```powershell
$repo = 'F:\JOSHUA_1st_2021\projects\iOS3-VM'
$env:TEMP = Join-Path $repo 'work\tmp'
$env:TMP = $env:TEMP
$env:TMPDIR = $env:TEMP
Set-Location -LiteralPath $repo

gcc -std=gnu11 -Icore/include -Itools -Wall -Wextra -Werror `
  -fsyntax-only tools/bootkernel.c

cmake --build build --target bootkernel --parallel 1

& 'build\core\bootkernel.exe' `
  'firmware\kernel.macho' `
  -d 'firmware\devicetree.bin' `
  -R 128 `
  -n 0 2>&1 |
  Tee-Object -FilePath 'work\tmp\integrated-strict-n0.log'
$LASTEXITCODE

git diff --check
```

The older focused build tree is also usable when it is known current:

```powershell
$env:TEMP = "$PWD\work\tmp"
$env:TMP = $env:TEMP
$env:TMPDIR = $env:TEMP
cmake --build work\build-strict-vfp --target bootkernel --parallel 1
```

If the existing generator/build cache is stale, configure a new F:-local tree,
not a default temp tree:

```powershell
cmake -S . -B work\build-run23 -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_FLAGS="-Wall -Wextra -Werror"
cmake --build work\build-run23 --target bootkernel --parallel 1
```

Run a zero-instruction startup/self-check with the exact kernel and device tree.
Use the current `bootkernel --help` for exact option syntax if the CLI changed.
The expected result is:

- process exit 0;
- all startup self-checks pass;
- ownership wait and AppleBaseband exact code/data gates print `VALIDATED`;
- zero guest instructions retire.

Keep the log under `work\tmp`. This output validates startup and exact firmware
gates only; it implies no real boot behavior or runtime causal result.

The tracked Run23 launcher's PowerShell parser and failure contract were also
checked without launching firmware. An isolated F:-local invocation supplied a
synthetic all-zero source commit; it failed before binary/firmware preflight,
returned wrapper exit **99**, and created the standard exit, error, end-time,
and launcher-log evidence files. This proves only the guarded failure path.

Do not run the full suite locally unless a focused failure requires it. Hosted CI
is the broad source of truth on this low-resource machine.

### 10.3 Documentation truth pass

Before commit:

```powershell
rg -n "rendered|boots fully|boot complete|deadlock|five linked|PID 1|baseband cause|clock_get_time" `
  README.md docs\QUALITY.md docs\BOOTLOG.md docs\ROADMAP.md docs\AGENT_HANDOFF.md
```

Every positive claim must have an exact evidence owner. In particular, the
working diagnostics must not be written as Run23 results.

### 10.4 Commit only owned paths

Example path-scoped staging:

```powershell
git add -- `
  tools/bootkernel.c `
  tools/run23-cold-replay.ps1 `
  README.md `
  docs/QUALITY.md `
  docs/BOOTLOG.md `
  docs/ROADMAP.md `
  docs/AGENT_HANDOFF.md
git diff --cached --name-only
git diff --cached --check
git diff --cached --stat
```

Ensure none of these protected paths appears:

```text
.github/workflows/ios-build.yml
app/iOS3VM.entitlements
docs/activation.md
docs/networking.md
.codex/
```

Commit with a message describing diagnostics, not a boot fix. Then push:

```powershell
git commit -m "Harden CommCenter and baseband boot diagnostics"
git push origin codex/m5-hardening
```

### 10.5 Hosted workflows

The publication gate is complete for exact diagnostic commit
`5a40c5eec5bbf7c4b7d8909d0c1f364bc078338a`:

- [core run 30143448600](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30143448600)
  completed successfully with all eight Linux/macOS/Windows, JIT,
  ASan+UBSan, and warnings-as-errors jobs green;
- [iOS run 30143455036](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30143455036)
  completed successfully with its unsigned arm64 package job green.

For a future source change, follow the same process below.

Because `tools/**` changed, pushing should trigger `core-tests.yml`. Confirm the
run rather than assuming it appeared:

```powershell
$head = git rev-parse HEAD
gh run list --branch codex/m5-hardening --limit 20
```

If no matching core run exists, dispatch it:

```powershell
gh workflow run core-tests.yml --ref codex/m5-hardening
```

The core workflow should produce eight jobs:

- Linux build/test;
- macOS build/test;
- Windows build/test;
- Ubuntu JIT;
- macOS 14 JIT;
- macOS 15 JIT;
- ASan+UBSan;
- warnings-as-errors.

The current `ios-build.yml` is user-dirty locally and must not be swept into the
boot commit. If the committed remote workflow is suitable, explicitly dispatch
the branch build after the source commit:

```powershell
gh workflow run ios-build.yml --ref codex/m5-hardening
```

Watch exact run IDs:

```powershell
gh run watch <run-id> --exit-status
gh run view <run-id> --json `
  databaseId,headSha,status,conclusion,event,workflowName,jobs,url
```

Record exact SHA, run IDs, job count, and conclusions. A green iOS build proves
compile/package only. It does not prove installation, launch, entitlements, JIT,
or guest boot.

If CI fails, fix and repeat on a new commit. Do not launch Run23 from a commit
whose public gates are red unless the failure is clearly external and recorded.

## 11. Prepared exact Run23 procedure

The run directory is:

```text
work\run23-commcenter-baseband
```

The tracked fail-closed launcher is:

```text
tools\run23-cold-replay.ps1
```

**Run23 has completed.** The directory now holds its retained evidence:
`manifest.txt`, `run23.stdout.log` (1,241,357 bytes, SHA-256
`C33EEEACD5B0A8BDC5B718DF323F0A0F098C3AB393DEF1CCD579A9980A3A9DE7`),
`run23.stderr.log` (0 bytes), `run23.exit.txt` (`0`), the launcher log, start
and end timestamps, `bin\bootkernel.exe`, `firmware\screen.ppm`, and the
466,825,216-byte work image
`rootfs-7e18-run23-777afb4c2350-978987be339a.img`. Total 447.43 MiB on F:.
**Do not delete it; it is user evidence.**

The launcher refuses every pre-existing output, so a *new* long run needs a new
run directory (for example `work\run24-<topic>`) passed through
`-RunDirectory`. Do not weaken the freshness guard to reuse this one.

The procedure below is retained because it is the exact recipe a Run24 should
follow. At the time it was written:

- `bin\` existed but did not contain the final Run23 binary;
- `firmware\` existed for the run-local `screen.ppm`;
- `tmp\` existed;
- an older ignored workspace-local launcher also exists at
  `work\run23-commcenter-baseband\launch-run23.ps1`;
- the tracked launcher is the source of truth for a fresh clone.

The launcher:

- is tracked without firmware, output, device credentials, or other secrets;
- requires a 40-hex source commit;
- requires the copied binary's 64-hex SHA-256 and exact positive byte length;
- checks current repository HEAD equals the supplied source commit;
- refuses staged, unstaged, or untracked drift beneath `core/` or `tools/`, or
  in the root CMake build file, from that HEAD. This conservatively covers
  `bootkernel`, `emucore`, and all linked host-helper build inputs while allowing
  the user's protected app/docs/workflow changes to remain untouched;
- verifies the copied binary;
- verifies exact kernel, device-tree, and immutable-rootfs lengths/hashes;
- keeps mutable paths under the F:-local Run23 directory;
- refuses every pre-existing output;
- creates a uniquely named 466,825,216-byte work image;
- launches hidden and waits;
- records its own hash, the exact argument vector, start/end, exit status,
  binary/source hashes, stdout/stderr hashes, work-image length/hash, and screen
  length/hash;
- re-hashes canonical inputs after the run;
- writes exit 99 for guarded structural, preflight, launch, and postflight
  failures whenever the run directory can be established.

The configured guest command is equivalent to:

```text
bootkernel.exe kernel.macho
  -p 0x08000000
  -V 0xc0000000
  -d devicetree.bin
  -c "debug=0x8 serial=1 nand-enable-adm=0"
  --external-md rootfs.img <unique-run23-work.img>
  --grow 32
  --fstab "/dev/md0 / hfs rw,update 0 1"
  -R 128
  -F
  -H 0x3d200000
  -W 1900000000:2100000000
  -Z 100000000
  -n 2100000000
```

### 11.1 Build from the exact committed tree

After commit and hosted validation, ensure the boot-source diff is clean:

```powershell
git diff --exit-code HEAD -- CMakeLists.txt core tools
git status --porcelain=v1 --untracked-files=all -- CMakeLists.txt core tools
$commit = (git rev-parse HEAD).Trim()
```

The final handoff publication is docs-only; its `CMakeLists.txt`, `core/`, and
`tools/` inputs are identical to exact hosted-green parent `5a40c5e`. Use the
current clean HEAD as the private-run provenance commit and let the launcher
record it together with the rebuilt binary hash.

Rebuild the committed source, then copy the exact binary into the empty Run23
`bin` directory:

```powershell
cmake --build work\build-strict-vfp --target bootkernel --parallel 1
$built = Resolve-Path 'work\build-strict-vfp\core\bootkernel.exe'
$runBinary = 'work\run23-commcenter-baseband\bin\bootkernel.exe'
Copy-Item -LiteralPath $built -Destination $runBinary
$binarySha = (Get-FileHash -Algorithm SHA256 -LiteralPath $runBinary).Hash
$binaryBytes = (Get-Item -LiteralPath $runBinary).Length
```

Do not overwrite a previous copied Run23 binary or output. If any output exists,
stop and inspect; use a new run directory/name rather than weakening freshness.

### 11.2 Storage preflight

Check F: free space and exact Run23 directory contents. Ensure enough headroom
for at least the work image, logs, PPM, and temporary provisioning overhead. A
cleanup warning can mean a second large temporary remains.

Do not move the run to C:.

### 11.3 Launch

For an autonomous agent, start the long-running wrapper in a hidden background
PowerShell process so progress can be checked and reported at intervals of no
more than 60 seconds:

```powershell
$repo = (Resolve-Path '.').Path
$run = Join-Path $repo 'work\run23-commcenter-baseband'
$launcher = (Resolve-Path 'tools\run23-cold-replay.ps1').Path
$pidPath = Join-Path $run 'run23.wrapper.pid.txt'
if (Test-Path -LiteralPath $pidPath) {
  throw "Freshness failure: $pidPath already exists"
}
$launcherArgs = @(
  '-NoProfile',
  '-ExecutionPolicy', 'Bypass',
  '-File', $launcher,
  '-RunDirectory', $run,
  '-SourceCommit', $commit,
  '-BootkernelSha256', $binarySha,
  '-BootkernelBytes', [string]$binaryBytes
)
$wrapper = Start-Process -FilePath 'powershell.exe' `
  -ArgumentList $launcherArgs `
  -WorkingDirectory $repo `
  -RedirectStandardOutput (Join-Path $run 'run23.wrapper.stdout.log') `
  -RedirectStandardError (Join-Path $run 'run23.wrapper.stderr.log') `
  -WindowStyle Hidden `
  -PassThru
$wrapper.Id | Set-Content -LiteralPath $pidPath -Encoding ASCII
```

Run22 took about 945 seconds on this host. Keep the user updated during a long
run. Do not kill it merely because no new console line appears during a quiet
interval; the launcher and `-Z 100000000` heartbeat provide progress evidence.
Poll the saved PID, launcher log, stdout byte count/tail, and eventual exit file
through short tool calls. Do not put a single shell/tool call to sleep for the
whole run.

For a human who explicitly wants a blocking foreground shell, the equivalent
form is:

```powershell
& 'tools\run23-cold-replay.ps1' `
  -RunDirectory 'work\run23-commcenter-baseband' `
  -SourceCommit $commit `
  -BootkernelSha256 $binarySha `
  -BootkernelBytes $binaryBytes
$LASTEXITCODE
```

## 12. Run23 analysis checklist

Do not derive a verdict from one grep line. Review the manifest, exit marker,
stderr, complete terminal diagnostic sections, framebuffer, and post-run hashes.

### 12.1 Run integrity

Require:

- launcher did not fail closed;
- exit status interpretation is explicit;
- `stopped after 2100000000 instructions: OK` only if the cap was reached;
- stderr is empty or every byte is explained;
- copied binary hash is unchanged;
- kernel, device tree, and source rootfs hashes are unchanged;
- work image is exactly 466,825,216 bytes;
- external-md strategy/raw failures are zero;
- no pending raw continuation;
- no panic/debugger/fatal hook;
- no unsupported instruction or nonzero CPU stop;
- guest free-memory low-water remains safe and is recorded;
- run directory size is recorded on F:.

### 12.2 Exact SpringBoard identity and lifecycle

Confirm:

- the pathname-qualified SETEXEC attempt armed;
- task/proc/PID/thread identity remained valid;
- successful SETEXEC and stock SpringBoard entry still reproduce;
- `applicationDidFinishLaunching:` reproduces;
- telephony singleton and initial request reproduce;
- no identity invalidation or terminal process event occurred;
- any switch/resume/read failure is interpreted fail-closed.

Instruction coordinates can drift with added diagnostics or scheduling. Path and
identity correlation matter more than exact equality to Run22.

### 12.3 CommCenter queue

Answer these independently:

1. Did request ID `0x0054b557` reach a complete copied-in kmsg?
2. Did destination and containing port/mqueue match?
3. Was the port an active port object of the expected type?
4. Was `ip_receiver_name` authoritative?
5. Was the receiver chain complete and internally consistent?
6. Was the queue head readable?
7. Did the bounded linked-list walk close?
8. Were reciprocal links consistent?
9. Was it untruncated and fault-free?
10. How many messages were actually linked?
11. Was a reservation/in-flight count exact or `UNPROVEN`?
12. Which route PC candidates occurred?
13. Which routes were **BOUND** to the exact mqueue/kmsg using decisive
    registers?
14. Was there a queue-full route, immediate reserve, receiver-woken route, or
    no-eligible-receiver route?
15. Did any service thread dequeue or receive?
16. Did the SpringBoard sender resume or return?

Never convert `msgcount=5` to “five queued messages” without the exact queue
walk.

### 12.4 Per-thread CommCenter waits

For each retained thread:

- identify the trap type;
- distinguish outgoing Mach send candidate, Mach receive, direct semaphore,
  timed semaphore, signal variant, BSD semaphore, and other wait;
- require one ordered last-observed unresolved block rather than a historical
  resolved wait;
- inspect sequence numbers and timestamp order;
- inspect snapshot completeness and read failures;
- inspect semaphore count and active state;
- inspect wait queue/event/type and continuation;
- inspect switch-out and all possible resume/return/continuation evidence;
- inspect schedule uncertainty;
- inspect exact-hook attribution omissions;
- inspect sequence exhaustion or table overflow.

If any exact-hook omission or uncertainty overlaps the decisive episode, the
unresolved-block classification must remain unproved. Even a complete episode
means “no resume observed,” not “still enqueued at stop,” until a final live
wait-state reread is implemented.

Repeated Mach ID 1000 SEND headers remain candidates until their destination and
service are resolved.

### 12.5 AppleBaseband causal chain

Read the report in causal order:

1. Was reset platform-function setup observed?
2. Did it return a nonzero function?
3. Was an event source created, committed, and enabled?
4. Did the reset callback execute?
5. Was reset state read successfully?
6. Did a state change occur, and was it high or low?
7. Was a low transition intentionally suppressed?
8. Was `messageClients` attempted and entered?
9. Was subscription state known at the exact dispatch?
10. Was a frame-linked notification handler invoked?
11. Was its Mach header readable and size-consistent?
12. Did the destination match a still-live, successfully registered interest?
13. What was the Mach send result?
14. Did an exact frame-and-kmsg-bound queue route occur?
15. Was the route reserve, queue full, no waiter, or receiver wake?
16. If a receiver woke, was its exact identity CommCenter?
17. Did exact CommCenter enter receive on that live interest port?
18. Were there notifier/port teardowns, pointer reuse, identity changes,
    overflows, frame mismatches, or restored-baseline uncertainty?

Only a continuous, exact chain can support a baseband-causality claim.
Temporal proximity between an AppleBaseband log and the saturated service queue
is not enough.

### 12.6 Display and render

Record:

- TV-out frame/IRQ/filter/action/close-return counts;
- `startWindowServer` and display geometry;
- `applicationDidFinishLaunching:` hits;
- `UIController` hits;
- active CLCD window, base, stride, dimensions, and scanning/running state;
- live scanout overlapping/RGB-visible mutations;
- PPM byte length and SHA-256;
- changed-pixel count relative to the known seed;
- visual inspection of any changed frame.

Known seed:

```text
460,815-byte PPM
SHA-256 CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB
```

A changed hash alone is not necessarily SpringBoard; it could be the kernel
graphics console or corruption. Correlate guest writes, live scanout, process
progress, pixel distribution, and the actual image.

## 13. Decision tree after Run23

**Run23 selected Case D, with a Case B shape underneath it.** No reset
notification was ever delivered, so Case A and Case C do not apply. Use the
Case D and Case B guidance below, and read §13.0 first.

### 13.0 The Run23 verdict and the next three read-only questions

Run23's queue walk gives Case D exactly what it asks for: linked IDs
(five × `0x0054b557`), destination (`c0d705a0`), reservation count (zero), and
service receive/dequeue activity (none). The earliest complete unresolved
message producer is whichever sender owns kmsg `c21e3000` (reply `c2bf6ea0`);
that sender was not identified.

The blocker is now stated cleanly: **the `com.apple.commcenter` receive right
is held by launchd, CommCenter is alive and waiting, and CommCenter has never
taken the port.** Answer these three read-only questions before changing any
hardware behavior:

1. **Is CommCenter's blocked receive port a port set containing the
   AppleBaseband interest port?** Thread `e02f5888` blocked on port
   `c0dd99d8` (mqueue `c0dd99f0`, task-local name `0x10004001`) at
   932,507,189, having registered interest on port `c3c59ab0` (mqueue
   `c3c59ac8`) at 931,584,215. If `c0dd99d8` is a port set with `c3c59ab0` as a
   member, the missing reset notification directly explains the stall. If it is
   not, the baseband lead is dead and the gate is elsewhere. This is decidable
   from guest memory plus the exact `ipc_pset`/`ipc_mqueue` layout; the
   ownership probe already validates `mqueue == port + 0x18`.
2. **What triggers the shipped AppleBaseband reset event source, and would
   no-modem hardware fire it?** The observer proves setup (`c0b6b020`), event
   source (`c0b6c340`), and an enable call, and zero callbacks. Disassemble the
   enable path with Capstone to identify whether it is an interrupt event
   source on a GPIO line, a timer, or a command-gated source, then decide from
   the binary whether a device with no modem attached would ever produce that
   edge. If real hardware would not, the emulator is faithful here and
   CommCenter must have a timeout path that is itself blocked — look there
   instead. If real hardware *would* (for example a power-on reset-detect
   assertion), that is the hardware gap to implement.
3. **Where is CommCenter's `bootstrap_check_in` relative to its blocking
   point?** If check-in precedes any baseband work in the shipped binary, then
   neither of the above is the cause and the real gate is earlier in
   CommCenter's startup.

### 13.0j SPRINGBOARD IS CRASH-LOOPING. THE BLOCKER IS MBX2D. READ THIS FIRST.

SpringBoard is not slow, not deadlocked, and not short of instructions. It is
being killed and respawned by `launchd`, roughly every **470 million
instructions**, and has been for every display-enabled run that got this far.
One run recorded **30 exec attempts and 29 deaths across 13.7e9 instructions**.

**The guest diagnosed itself and nobody had read the output.** The guest's own
`ReportCrash` wrote **35** crash reports into the rootfs work images; they are
extracted to `work\analysis\crashes\` (32 from run40, 3 from run46). All 35 are
byte-identical in the crashed thread:

```text
Process:         SpringBoard [20]
Parent Process:  launchd [1]
OS Version:      iPhone OS 3.1.3 (7E18)

Exception Type:  EXC_BAD_ACCESS (SIGBUS)
Exception Codes: KERN_PROTECTION_FAILURE at 0x00000048
Crashed Thread:  3

  pc: 0x30e1ea50   lr: 0x3123d928   cpsr: 0x60000010   sp: 0x007b75d4
```

Thread 3, symbolicated innermost first:

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

The faulting instruction, in ARM mode:

```text
30e1ea3c  ldr    r3, [r3]          ; r3 = *(0x381200d8) = NULL (MBX2D global ctx)
30e1ea50  strbeq r0, [r3, #0x48]   ; store to 0x00000048   <== FAULT
```

A NULL dereference into `__PAGEZERO` is `KERN_PROTECTION_FAILURE` under Darwin,
which is delivered as **SIGBUS**, not SIGSEGV. No CPU bug is involved: a
separate audit found the interpreter's ARMv6 unaligned-access model **correct**
(SCTLR.U and SCTLR.A both honoured), and no alignment fault appears anywhere in
the 2,052 recorded faults.

**The causal chain, fully verified:**

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

Thread 0 meanwhile blocks in `semaphore_signal_trap` under
`CA::Render::Context::did_commit` <- `+[CATransaction flush]`. That is exactly
why `SpringBoard:UIApplicationMain-return` is `hits=0` in every run, and why
`IOSurface:create-entry` is `hits=0`: the process dies in the render thread
before anything downstream of the window server can run.

#### The fix: one environment variable, verified statically, NOT YET RUN

QuartzCore ships a complete CPU software compositor.
`CA::WindowServer::MBXServer::render_update` (`0x3124207c`) is a three-way
fallback:

```text
gles_context()  ->  mbx2d_context()  ->  CA::WindowServer::Server::render_update
```

`mbx2d_context()` (`0x31241a8c`) reads exactly one byte — `enable_mbx2d` at
`0x38190db1` — and returns NULL early at `0x31241aa8` when it is zero. The
fallback reaches `Server::sw_renderer()` ->
`CARenderOGLNew(_kCARenderSoftwareCallbacks)` -> `CA::OGL::SWContext`, a genuine
CPU rasteriser. `enable_mbx2d` is set from `getenv("CA_ENABLE_MBX2D")` or,
failing that, `getenv("LK_ENABLE_MBX2D")`, defaulting to **ENABLED**.

So setting **`CA_ENABLE_MBX2D=0`** in SpringBoard's launchd environment selects
software rendering and never reaches the NULL store. **No GPU emulation is
required.** A candidate `com.apple.SpringBoard.plist` carrying that variable is
staged under `work\analysis\envvar\`.

This is verified at instruction level from disassembly. **It has not been run.**
Nothing here says SpringBoard has rendered; it has not.

**Keep `dt_unmatch("arm-io/mbx")`.** Only its explanation was wrong (below).
Un-matching the node is what makes the software path reachable at all; the
defect is that userspace then stores through a context it never checked.

#### Retractions — established, do not re-derive

1. **"SpringBoard is progressing normally and merely runs out of instruction
   budget."** FALSE. It is crash-looping. Every run since run35 ends with the
   identical framebuffer hash
   `CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`
   because nothing was ever going to draw.
2. **"Checkpoint restore loses fidelity; cold and restored runs disagree by
   4.6e9 instructions."** FALSE. Restore is **bit-exact**. Heartbeat PC streams
   are byte-identical for run35 vs run36d across **27/27** samples, and likewise
   for run37/38, run38/40/41/43, and capA/capB. The apparent disagreement was an
   artifact of the `SPRINGBOARD POST-SETEXEC TRACE` block, which reports only
   the **most recent** SpringBoard generation: a longer run simply prints a
   later generation of the same crash loop.
3. **"The instruction cap perturbs guest execution."** FALSE — same artifact,
   same evidence as (2).
4. **"`-[UIWindow makeKeyAndVisible]` is never called, so no window is made
   visible."** MISLEADING. That selector has exactly **one** call site in the
   whole 1.19 MB SpringBoard binary, inside `-[SBSyncController
   _delayedBeginReset]`, a restore path never taken at boot. SpringBoard 3.1.3
   uses `-[UIWindow orderFront:]` and `-[UIWindow makeKey:]`. A zero there is
   the **expected** reading on healthy hardware.
5. **"Un-matching only `/arm-io/usb-otg/usb-device` is insufficient."**
   Understated — it was a complete **no-op**. Driver output census: run46
   (child un-matched) had **24** `AppleSynopsysOTGDevice` lines; run37 (parent
   un-matched) had **0**. IOKit matching keys off the parent node.
6. **Any claim resting on run39 or run42 "with USB un-matched" is void.** Both
   restored from a snapshot taken with the driver already matched in guest RAM,
   and a device-tree patch cannot affect a restored run.

#### Two factual corrections to the MBX story

- **The MBX register block is at physical `0x3B000000`, not `0x03000000`.** The
  device-tree `reg` value is `{0x03000000, 0x01000000}` and `/arm-io` `ranges`
  adds `0x38000000`.
- **The "busy-polls a reset bit" description of the MBX hang is wrong.** That
  poll (`AppleMBXController`, `0xC07799E0`) is gated on `fVariant == 2`
  (s5l8720x) and cannot execute on s5l8900x; the controller's `fRegs` is NULL
  because the node has only one `reg` pair. The actual wedge is
  **`AppleMBXDevice` at `0xC077E8D8`, spinning on physical `0x3B00012C` bit 6,
  with no timeout and no exit.**

#### Where this leaves the next agent

The next experiment is to place `CA_ENABLE_MBX2D=0` into SpringBoard's launchd
environment and run it. Until that run exists, the only supported claims are the
ones above: the crash loop is measured, the cause is disassembled, and the fix
is statically verified and unexecuted. A `hits=0` on any post-window-server
checkpoint carries **no information** while the process is dying upstream of it,
and neither does a repeated framebuffer hash.

### 13.0d GPIO pin encoding, and why touch and baseband need the same two blocks

Touch and the baseband transport converge on the same unmodelled hardware, so
work on either pays for both.

`/device-tree/arm-io/spi1/multi-touch` is `compatible "multi-touch,n82"` with
`interrupts {0x9b, 0x00}` and `interrupt-parent 0x00b05320` — that is **GPIO
interrupt 155 on the GPIO interrupt controller**, with `function-reset` GPIO
`0x0606` and `function-power_ldo` GPIO `0x0701`, hanging off **spi1**
(`0x3ce00000`). The baseband path needs the same two blocks: spi2
(`0x3d200000`) plus GPIO `reset_det 0x1203`, `srdy 0x1804`, `mrdy 0x1702`.

So the two remaining goal items — a rendered SpringBoard and a guest touch —
both reduce to **a real S5L8900 SPI controller model and a real GPIO interrupt
controller model**. Neither exists; all five windows are declared stubs with no
transfer, interrupt, or autonomous behaviour.

**The GPIO pin encoding is now decoded from the driver itself.**
`AppleS5L8900X`'s pin accessor at `0xc05a4494` reads:

```text
lsr r1, r5, #8        ; group = pin >> 8
lsl r1, r1, #5        ; group * 32
add r1, r1, #4        ; + 4
ldr pc, [r3, #0x384]  ; <accessor>(group * 32 + 4)
and r1, r5, #0xff     ; bit = pin & 0xff
lsr r0, r0, r1
and r0, r0, #1        ; return (state >> bit) & 1
```

so a platform-function pin id splits as **group = pin >> 8, bit = pin & 0xff**,
and a pin's level lives at **`<base> + group * 32 + 4`**. `#gpio-ports` is 25
and `#interrupt-groups` is 7, consistent with groups 0..24. Concretely:

```text
lcd0 reset        0x0001 -> group  0 bit 1   +0x004
lcd0 ctrl_enable  0x0304 -> group  3 bit 4   +0x064
mt-reset          0x0606 -> group  6 bit 6   +0x0c4
mt-power_ldo      0x0701 -> group  7 bit 1   +0x0e4
bb_rst            0x0700 -> group  7 bit 0   +0x0e4
mrdy              0x1702 -> group 23 bit 2   +0x2e4
reset_det         0x1203 -> group 18 bit 3   +0x244
srdy              0x1804 -> group 24 bit 4   +0x304
```

The two thin accessors are also decoded: object `+0x68` is the **gpio** base
(`0x3e400000`, first touched at `0xc05a44d0`) and object `+0x6c` is the
**gpioic** base (`0x39a00000`, first touched at `0xc05a44e8`).

**A suspected power/gpioic overlap was investigated and does NOT exist.**
The device tree gives `/arm-io/gpio` reg `{0x06400000,0x1000, 0x01a00000,0x1000}`
and `/arm-io/power` reg `{0x01a00000,0x1000}` — both naming the same physical
page `0x39a00000`, which the guest maps twice at different VAs
(`gpioicBaseAddress 0xe3949000`, `_pcBaseAddress 0xe394a000`). Our model splits
that page as power `0x00..0x7f` and gpioic `0x80..0xfff`, so if the pin-level
register lived at `gpioic + group*32 + 4`, groups 0..3 would land inside
power.c's claim — and lcd0's `reset`, `mpl_rx_enable`, `power_enable`,
`pixel_clock_enable` and `control_enable` are all group 0 or 3 pins.

It does not. The four accessors sit consecutively at `c05add68` (`+0x68`
read), `c05add6c` (`+0x68` write), `c05add70` (`+0x6c` read) and `c05add74`
(`+0x6c` write). Scanning the kernelcache for the implied vtable bases shows
`c05ad9e4` referenced as a literal at seven constructor sites and `c05ad9ec` at
none, and `c05ad9e4` is preceded by three zero words — the vtable start
pattern. So the vptr is **`0xc05ad9e4`**, and:

```text
slot 0x384 = c05ad9e4 + 0x384 = c05add68 = the +0x68 accessor = GPIO base
```

**Pin level is therefore at `0x3e400000 + group*32 + 4`**, in the gpio block,
never in the gpioic/power page. The existing power/gpioic split is unaffected
and must not be "fixed" on the strength of the earlier suspicion.

That also fixes the register map a GPIO model has to provide: pin state for
groups 0..24 at gpio `+0x004..+0x304`, `fsel-offset 0x320` from the device
tree, and the separate gpioic page carrying the 7 interrupt groups.

### 13.0e External-md snapshots: why they are now mandatory, and the design

Iteration cost is the binding constraint on reaching a rendered SpringBoard.
§13.0c shows the frontier sits past 5e9 instructions, and at the measured
~1.3-1.7M instructions/second that is **60-90 minutes per attempt**. Whack-a-mole
past this point without checkpointing spends the entire budget on re-executing
the same first five billion instructions.

`--external-md` currently refuses `--snapshot-at` and `--restore` outright
(`"--external-md is cold-boot only; snapshots and restore are unsupported"`).
The reason is real but narrow: the core snapshot serialises `s5l8900_t` only —
GEOM, CPU, MACH, RAM, NOR, STUB — and the md bridges live in `tools/`, so none
of their host-side state is captured, and the work image is a host file that
keeps moving after the checkpoint.

**The host-side state that actually has to survive is small.** From
`md_raw_bridge_t`: `config` is re-derivable from setup, `scratch`, `iov_plan`
and `data_spans` are per-call transients, and `pending[4]` must simply be
empty. What genuinely persists is:

1. `guard_tail[131072]` — the 128 KiB coherent allocation-tail overlay;
2. the stats counters, for reporting continuity only;
3. the contents of the work image at the checkpoint instant.

Everything else that matters is already inside the core snapshot: the
exact-gated kernel patches live in the loaded kernel copy in guest RAM, and the
four 128 KiB bounce slots are guest DRAM below `topOfKernelData`.

**Design — two sidecars beside the snapshot, owned by `bootkernel`, leaving
`core/` and the snapshot format untouched:**

```text
<snap>            existing core snapshot, unchanged format
<snap>.mdimage    byte copy of the work image at the checkpoint
<snap>.mdstate    magic/version, media size, image length,
                  guard_tail[131072], strategy + raw stats
```

Save, inside `save_due_snapshots()` after `snapshot_save()` succeeds:
fail closed if any `pending[i].active`; flush the block adapter; refuse either
sidecar if it already exists; copy the image; write the state.

Restore, at the provisioning site (`rootfs_work_create()` around
`tools/bootkernel.c:21004`): when `--restore` is given, do **not** provision
from the immutable source. Copy `<snap>.mdimage` into the new work path
create-only, verify its length against `.mdstate`, open the adapter, install
the bridges as usual, then apply `guard_tail` and the stats after
`md_raw_bridge_init()`.

Freshness discipline is preserved throughout: the restore still writes a new,
uniquely named work image and still refuses to overwrite anything.

Cost: 466 MiB per checkpoint, which is nothing against 1.5 TB free, and a
few seconds to copy. Benefit: a checkpoint at ~4.5e9 turns each subsequent
experiment from 60-90 minutes into seconds plus the delta.

**Do this before the next round of frontier whack-a-mole.** It is roughly 400
lines in `tools/bootkernel.c` plus option plumbing and a focused test, and it
touches the storage bridge — the one subsystem that has never failed a run — so
it deserves its own validation pass rather than being bolted onto a diagnostic
change.

### 13.0g THE COMMCENTER BLOCKER IS GONE — but §13.0j is the current state

> The "CURRENT STATE" claim this section used to carry has moved to **§13.0j**.
> What remains true below: the telephony blocker is resolved, the three CPU gaps
> are closed, and checkpointing works. What is superseded: every sentence that
> reads the boot's position as forward progress toward a frame.

The multi-run CommCenter/telephony blocker described in §13.0b/§13.0f is
**resolved**. Do not spend further effort on it.

**The fix** (`f7f0f04`): the in-memory device tree un-matches `/baseband` and
`/arm-io/spi2` by default, exactly as it already did for the MBX GPU and the
SHA-1 engine. Run29 had shown that a *declared but unanswering* baseband makes
BasebandSPI time out on SRDY, the multiplexer fail `ASMIOCNEWDLCI` with
`kASMFatalErrorSPI(11)`, and CommCenter retry forever. A device that never
responds is not a device that is absent, and the stock stack treats them very
differently. `-B` restores the old behaviour for anyone modelling the real
transport; at that point delete the un-match rather than keep it.

**Run30 proved it worked:** `_bootstrap_check_in` hits=1 with
`r1 = 0x00085ee4` (`"com.apple.commcenter"`), return `r0 = 0` (KERN_SUCCESS),
SUCCESS arm taken, FAILURE arm never. SpringBoard's telephony singleton now
enters *and returns*.

**Three CPU gaps have followed, each fixed as a whole class:**

| Run | Stop | Reached | Fix |
|---|---|---:|---|
| 30 | `VCVTR` refused | 2,061,479,415 | `c5be9ee` conversions honour RMode |
| 31 | `VCVT.F32.S32` refused | 2,061,479,416 | `cddd53c` implement FPSCR.RMode |
| 32 | `SADD8` undefined | 2,191,848,855 | `f898753` parallel add/sub family |

Run31 gaining exactly **one instruction** is the lesson worth keeping: UIKit
sets a directed rounding mode and runs whole sequences under it, so clearing
encodings one per half-hour replay never terminates. Fix the class.

**Where the boot now is (updated after run35):** `SpringBoard:UIController-call`
**has been reached**, hits=1 at instruction 3,478,858,148, and the telephony
singleton does not merely return — `CTCenterGetDefault` call *and* return are
both recorded. Runs 30-33 saw 0 hits for a simple reason that was not obvious at
the time: `UIController` lies past 2.5e9 and every earlier cap stopped short of
it. Run33 reaching its cap "cleanly, with no CPU stop" was therefore not
evidence of a block; it was evidence of running out of budget. Read a clean
cap-stop as *no information* about blockers.

The remaining distance was ordinary reached-path coverage, as expected above —
but the current frontier is not a CPU gap at all. In run35's final window 99.6%
of samples are userspace and ~40% sit in one 22-instruction loop resolving to
`Security.framework` `_mulg_common`, a 16-bit-limb giant-integer multiply
(§13.0h). The framebuffer is still the seed and `UIController` is the newest
checkpoint reached, so **no render claim is available.**

**Method that is working, and should continue:**

1. Run to a stop; the harness names the encoding and reason fail-closed.
2. Decode it, and implement the **entire related family**, never the single
   encoding, with tests covering lane/mode/edge behaviour and PC refusal.
3. Re-run. Expect a new stop; that is progress, not regression.

**Checkpointing is implemented (§13.0e) and now works end to end.** Each replay
to the frontier costs 25-30 minutes. `-SnapshotAt <n>` writes the checkpoint;
`-RestoreFrom <path>` starts from one, hashing all three sidecars into the
manifest so a restored run records the state it inherited. Both flags key on the
machine's own retired-instruction counter, which is part of the snapshot, so
`-InstructionCap` stays **absolute** across a restore: restoring 2.4e9 with a
12e9 cap runs 9.6e9 further, not 12e9 further. Taking a checkpoint *during* a
restored run is refused, not silently ignored; that combination is unimplemented.

### 13.0h THE FRONTIER IS NOW RSA-CLASS ARITHMETIC IN Security.framework

> **Superseded as a frontier claim by §13.0j.** The `_mulg_common` resolution
> below is an accurate account of where userspace time was going, and its
> "bounded arithmetic, not a spin-wait" conclusion still stands. It is **not**
> the reason nothing renders: SpringBoard is crash-looping on a NULL MBX2D
> context. Do not plan an instruction budget around this section.

After `UIController`, the guest spends essentially all of its time in a
22-instruction loop at `0x3145ad4c..0x3145ada4`. Every PC above `0x30000000`
lands in one 96 MB dyld shared cache spanning 273 libraries, which is why the
logs could only ever call this "userspace". `tools/dscmap.py` resolves it:

```text
image:  /System/Library/Frameworks/Security.framework/Security  (+0x2bd4c)
symbol: _mulg_common at 3145ac70  (+0xdc)
```

The disassembly is schoolbook multiplication on **16-bit limbs** — `mul`, mask
against a literal-pool `0xffff`, carry-propagate, `strh`, loop bounded by a limb
count reloaded from `[sp,#0x14]`. `r1` advances two bytes per iteration and `lr`
is used as scratch, not as a return address. **This is bounded arithmetic, not a
spin-wait**, and it should not be diagnosed as a hang. `0x33aae484`, the address
the episode tracker reports alongside it, is `svc #0x80` in libSystem — an
ordinary syscall, so the process is alive and calling throughout.

Neighbouring symbols place the work in the certificate/key family:
`_SecRSAPrivateKeyRawSign`, `_SecCertificateIsSignedBy`,
`_SecPolicyCreateiPhoneApplicationSigning`, `_SecGenerateSelfSignedCertificate`.
The kernel side shows `_prngInitialize`, `_SHA1Init`, `_prngOutput`.

**This work demonstrably terminates and recurs.** Run36's restore banner reports
the 2.4e9 checkpoint was taken at `pc 0x3145ad98` — inside the same loop, and
*before* `UIApplicationMain` at 3.268e9. So that earlier block of giant-integer
work finished and the boot moved on. Treat the cost as recurring, not as a
single terminal computation.

**What is still open:** which higher-level operation drives it, and how much
total work remains. "RSA-class arithmetic in Security.framework" is the entire
claim. A bounded inner loop does not bound the outer computation, so do not
assert the boot will finish in any particular budget without a run that shows
it. If a very large cap still ends inside `_mulg_common` with no new SpringBoard
checkpoint beyond `UIController`, the next hypothesis to test — untested, and
stated here only so it is not re-derived — is whether the guest's randomness
source is degenerate enough to make a probabilistic prime search never succeed.

### 13.0i NO SURFACE IS EVER CREATED — SUPERSEDED BY §13.0j, WHICH SAYS WHY

> **Superseded.** The central observation here is correct — no surface is ever
> created — but its framing is not. SpringBoard is not alive and failing to ask
> for one; it is dying in the CoreAnimation render thread on a NULL MBX2D
> context and being respawned by `launchd` (§13.0j). Read §13.0j first. The
> address-arithmetic warning and the `0x0000a7c4` discriminator below remain
> valid; the determinism bullet is retracted in place.

`UIController` has now been reached in runs 35, 36d and 38, and the framebuffer
is **byte-identical every time** (`CBAD1C11…`, 384 of 460,800 bytes non-zero).
Run36d ran **607 million instructions past it** and changed nothing. The
standing hypothesis — "SpringBoard just needs more budget after
`UIController`" — is not supported and should not be assumed again.

The display side is not the problem. CLCD is programmed and live: `scanning=1`,
1026 frames, window0 320x480 at `0x0885c000`, descriptor refreshes 98-102. The
guest never writes pixels.

Run36d's own checkpoints say why:

```text
IOSurface:create-entry      hits=0
H1:createSurface-null-test  hits=0
H1:createSurface-nonnull    hits=0
H1:surface-field24-store    hits=0
```

**No surface is ever created.** iPhone OS 3's UIKit does not draw into the
scanout buffer; it renders into a CoreSurface/IOSurface that the window server
composites. With no surface allocated there is nothing to render into, and no
instruction budget produces a pixel. The question is not "how much further" but
"why is `IOSurface::create` never called" — **and §13.0j answers it**:
SpringBoard is killed by SIGBUS in the IOMFB vblank callback before anything
downstream of the window server runs, then respawned, indefinitely.

Instrumentation stops exactly where it is needed, and the address arithmetic
here is easy to get wrong: SpringBoard's `__TEXT` is `vm 0x1000, file 0`, so
**VA = file offset + 0x1000**. Disassembling the raw file at offset `0xa7ba`
reads VA `0xb7ba` and produces plausible but entirely unrelated code. (An
earlier revision of this section did exactly that and described the wrong
instructions.)

At the correct VA, `SpringBoard:UIController-call` at `0x0000a7ba` **is** the
`blx` to `objc_msgSend`:

```text
0000a7b4  ldr  r0, [r0]        ; receiver, from a classref literal at 0xaa14
0000a7b6  ldr  r1, [sp, #0x2c] ; selector
0000a7ba  blx  #0xb1bc8        ; objc_msgSend   <- the checkpoint
0000a7c4  str  r0, [r4]        ; stores the returned object
```

So `hits=1` proves the send was **entered**, not that it returned. The
discriminator is one checkpoint at **`0x0000a7c4`**: if it fires, `UIController`
was constructed and the failure is downstream of it; if it never fires,
SpringBoard is stuck inside that message and the hunt is for whatever it waits
on — the same shape as the telephony blocker.

`tools/objcsel.py` resolves a PC-relative literal load to its selector name for
this stripped binary (literal -> selref -> `__objc_methname`), which is how the
sends around this site can be named rather than guessed at.

Two measurements that bound what is worth trying:

- **RETRACTED — "runs are not deterministic".** They are, and restore is
  bit-exact. The heartbeat PC streams for run35 and run36d are byte-identical
  across **27/27** samples, as are run37/38, run38/40/41/43, and capA/capB. The
  apparent 4.6e9 divergence — run36d reporting `UIApplicationMain` at 7.90e9
  where run35 reported it at 3.27e9 — was an artifact of the
  `SPRINGBOARD POST-SETEXEC TRACE` block, which reports only the **most recent**
  SpringBoard generation; a longer run prints a later generation of the same
  crash loop (§13.0j). The instruction cap does not perturb guest execution
  either, on the same evidence. The operating rule survives its retracted
  justification: do not attribute an instruction-count difference between two
  runs to a code change without repeating it. The earlier retracted claim that
  un-matching USB made the boot 4.3x slower had this same cause.
- **Stripping diagnostics buys ~10%, not a multiple.** Measured over 500e6
  instructions: minimal 1,408k inst/s, full 1,278k inst/s. A "dash with
  diagnostics off" mode is not worth building. Parallel runs (the machine has 8
  cores and had been running one) and the dynarec are the only real
  accelerators.

### 13.0f RUN29: THE BLOCKER IS SRDY, AND THE LONGER-RUN HYPOTHESIS WAS WRONG

Run29 (exact commit `cf2f7d1`, **7e9** cap ≈ 17 guest seconds, 4,679 s host,
exit 0, hashes unchanged, external-md 0 failures) is the first replay ever to
outlast the guest's own timeouts. Two results, and the first is a correction.

**§13.0c's prediction is falsified.** Waiting longer did *not* let CommCenter
give up and proceed. `_bootstrap_check_in` is still `hits=0`, while `_ioctl`
grew 15 → **177** and `_select` 1 → **10**, spread evenly to 6.6e9, and
CommCenter retired **3,235,016 user instructions after** SpringBoard's send.
The bounded ten-attempt `SCPreferences` loop was real but *inner*; the outer
retry against the baseband mux does not terminate. §13.0c's arithmetic about
guest time remains correct and useful — its conclusion about this frontier does
not.

**The guest named the blocker itself**, in console output no 2.1e9-capped run
could produce:

```text
BasebandSPIIFXProtocolVersion1::handleSRDYTimeoutAction: Exit
AppleSerialMultiplexer: !! mux-ad(err)::bsdIoctl: Fatal error code=kASMFatalErrorSPI(11)
```

The Infineon baseband SPI protocol driver times out waiting for **SRDY**
(`/arm-io/spi2 function-srdy` GPIO `0x1804` = group 24, bit 4 by §13.0d's
encoding); the multiplexer then fails `ASMIOCNEWDLCI` with
`kASMFatalErrorSPI(11)` — matching CommCenter's own
`ioctl(ASMIOCNEWDLCI) failed` string — and CommCenter retries forever.

Full chain, now confirmed rather than inferred:

```text
no SRDY / no SPI transfer model
  -> SRDY timeout -> kASMFatalErrorSPI(11) -> ASMIOCNEWDLCI fails
  -> CommCenter retries forever, never calls bootstrap_check_in
  -> launchd keeps the port -> queue full at qlimit=5
  -> SpringBoard blocks -> UIController never runs -> no pixels
```

**Do not "fix" this by making the timeout fire faster or cleaner.** The mux
already fails the ioctl and CommCenter retries anyway — 177 times over 5.7e9
instructions. A prompt clean failure is demonstrably insufficient.

**The next step is read-only, and must precede any device work.** The entry
points are already located, so resume exactly here rather than re-deriving them:

```text
BasebandSPI+0x71a8  c05f81a8  literal pool holding "handleSRDYTimeoutAction"
BasebandSPI+0x7170  c05f8170  handleSRDYTimeoutAction itself (Thumb)
                                vtable slot 0x50 -> name/getter
                                IOLog c0174730 "Enter"
                                real handler via literal c05f34d1
                                IOLog "Exit"
BasebandSPI+0x24d0  c05f34d0  THE SRDY TIMEOUT DECISION (Thumb)
                                branches on the state field at [this+0x68]:
                                  (state - 1) <= 1  -> c05f3578
                                  else              -> formats and logs, and
                                                       calls c05f33c0
BasebandSPI+0x24bc  c05f33c0  called on the non-trivial arm; not yet read
AppleSerialMultiplexer+0x5040 c060d040  the kASMFatalErrorSPI reference
```

Read `c05f34d0` through both arms and `c05f33c0`, and establish what `[this+0x68]`
is set from, to answer (a) exactly what SRDY is sampled from — the GPIO input,
an SPI status bit, or an interrupt — and (b) whether any path exists in which
the driver concludes "no modem" durably instead of rearming. Only then decide
between:

- a minimal SRDY/GPIO input model that lets the handshake complete and the mux
  come up, with the modem reporting no service; or
- a faithful permanent-failure state, if and only if the binary actually has
  one that CommCenter honours.

Both are real device-model work on the two blocks §13.0d identifies (SPI
controller, GPIO interrupt controller) — the same pair multitouch needs, so
neither is wasted.

Use the new external-md checkpointing (§13.0e, implemented) to iterate: take a
checkpoint around 1e9, before the retry storm, and restore instead of
re-running five billion instructions per attempt.

### 13.0c THE INSTRUCTION CAP HAS ALWAYS BEEN SHORTER THAN THE GUEST'S TIMEOUTS

Read this before planning any further long run. It is the most consequential
thing found so far and it is arithmetic, not a defect.

Guest time advances from retired instructions at the real cpu:timebase ratio —
a 412 MHz CPU model against a 6 MHz timebase, roughly **68.7 instructions per
tick**. Therefore:

```text
one guest second  ~= 412,000,000 retired instructions
sleep(1)          ~= one fifth of the entire historical 2.1e9 cap
```

CommCenter's startup contains a **bounded ten-attempt retry loop** —
`SCPreferencesLock`, `SCNetworkSetCopyCurrent`, `SCPreferencesUnlock`,
`sleep(1)`, `cmp r4,#0xa`, give up — which is about **4.1 billion instructions
of guest patience on its own**. Run21 reached 2.5e9; runs 22, 23, 24, 26 all
stopped at 2.1e9.

**No run in this project has ever watched one of the guest's own timeouts
expire.** Every long run stopped part-way through one. "CommCenter never checks
in" is, so far, indistinguishable from "we stopped the machine after about five
guest seconds".

The launcher's `-InstructionCap` ceiling was raised from 4e9 to 24e9 for this
reason. Budget wall clock accordingly: at the observed ~1.3-1.5M instructions
per second, 7e9 is roughly 75-90 minutes.

Corollary worth internalising: **any future "X never happens" conclusion about
stock software must be checked against the guest-time cost of X's own timeout
before it is believed.** Several past frontier claims deserve re-examination in
that light.

### 13.0b Run24 closed the notification route and named the blocked clients

**Scope this claim precisely.** What Run24 kills is the *IOKit-notification
delivery route* — the idea that CommCenter is parked waiting for
AppleBaseband's reset `messageClients` to arrive on a port. It does **not**
establish that the absent modem is irrelevant. CommCenter could still be
waiting on the modem through a different mechanism entirely: the spi2
SRDY/MRDY handshake, a bounded timeout loop, or a state machine that never
advances. Those remain open.

**The notification route is dead.** Run24 (exact commit `8a08e44`, exit 0 at
the 2.1 B cap in 1,625.1 s, all integrity invariants held) classified four
CommCenter receives on non-interest mqueues and found **zero port sets**:
every one resolved as an active `IOT_PORT` under the `+0x18` hypothesis, never
the `+0x1c` pset hypothesis, so no membership walk was ever entered. The
newest is `mqueue=c2966918` → object `c2966900`, `io-bits=80000000`, the same
object run23's per-thread dump carried for threads `e0379bb8`/`e035d000`.

CommCenter never established a receive that could deliver an AppleBaseband
notification, so delivering that notification could not have unblocked it.
Do not build GPIO interrupt generation or a `reset_det` edge *in order to
deliver that notification* — there is nothing on the other end to receive it.
§13.0a remains accurate as topology, and if a later trace shows CommCenter
waiting on the modem through the spi2 handshake instead, the GPIO and SPI work
becomes relevant again for that different reason.

**The queue is five different daemons.** Each linked kmsg's reply port
resolved AUTHORITATIVE:

```text
[0] c21e3000 reply c2bf6ea0 -> pid 16   [1] c31d7000 reply c34d2630 -> pid 18
[2] c3f50000 reply c2d33d80 -> pid 15   [3] c3e52000 reply c31c32d0 -> pid 12
[4] c448c000 reply c31c3cf0 -> pid 13
```

Five daemons plus SpringBoard (PID 20) are blocked on the identical
`0x0054b557` CTServerConnection handshake. **CommCenter has served no client
since boot.** The problem is therefore not in SpringBoard, not in
CoreTelephony's client path, and not in the queue: it is that PID 24 never
takes its own receive right.

**The next question is CommCenter's own execution.** The harness already
retains a full post-SETEXEC user trace for the SpringBoard generation (region
attribution, low-image flow, Mach episodes, exact call/return checkpoints).
The equivalent does not exist for PID 24 — the CommCenter watch tracks
identity, scheduling and waits, but not where its user code goes. Extending
that attribution to the CommCenter generation is the direct route to the
answer: find its last retired user instruction, resolve it in the shared cache
and its own image, and identify what it called before parking on semaphore
`c0b239a0`.

### 13.0a The exact baseband topology, resolved read-only after Run23

Question 2 above is now partly answered, and the hardware map is exact. All of
this comes from the shipped 7E18 device tree and Capstone disassembly of the
prelinked image; none of it is inferred from behaviour.

**The reset event source is an interrupt event source.** `AppleBaseband`'s
setup path at `0xc0558c98` calls
`IOInterruptEventSource::interruptEventSource(OSObject *owner, Action, IOService *provider, int index)`
(`__ZN22IOInterruptEventSource20interruptEventSourceEP8OSObjectPFvS1_PS_iEP9IOServicei`,
`0xc0189e94`) with `r0` = the AppleBaseband object, `r2` = its provider, and
`r3 = 0` — **interrupt index 0**. It then adds it to the workloop returned by
vtable slot `0x1dc` and calls
`IOInterruptEventSource::enable()` (`0xc0189d58`) through event-source vtable
slot `0x68`. The retained object's vtable is `__ZTV22IOInterruptEventSource`.

So the callback at `0xc0558358` can only run when that hardware interrupt is
delivered. Its body is short and worth knowing exactly: it calls reset-state
read through AppleBaseband vtable slot `0x35c` into a two-word stack buffer,
returns immediately if the read fails, compares the 64-bit result against the
remembered state at object `+0x70`, returns if unchanged, and otherwise calls
`IOService::messageClients` through vtable slot `0x238` with `0xe3ff8000` for a
non-zero (high) state or `0xe3ff8001` for zero (low) — the low path first
checking a suppression byte at object `+0x69`.

**Which interrupt.** `/device-tree/baseband` carries:

```text
name                'baseband'          compatible 'baseband,n82'
interrupts          {0x0000004b, 0x00000005}
interrupt-parent    {0x00b05320}
function-reset_det  {0x00b05320, 'GPIO', 0x00001203, 0x00000100}
function-bb_rst     {0x00b05320, 'GPIO', 0x00000700, 0x00000101}
function-bb_on      {0x00b14140, 'GPIO', 0x00000003, 0x00070001}
function-radio_on   {0x00b05320, 'GPIO', 0x00001507, 0x00010101}
```

`0x00b05320` is the `AAPL,phandle` of `/device-tree/arm-io/gpio`
(`compatible 'gpio,s5l8900x'`, `device_type 'interrupt-controller'`,
`reg {0x06400000,0x1000, 0x01a00000,0x1000}` → PA `0x3e400000` and
`0x39a00000`, `#interrupt-cells 2`, `#interrupt-groups 7`,
`fsel-offset 0x320`).

Therefore **index 0 is GPIO interrupt `0x4b` (75) on the GPIO interrupt
controller**, and the reset-detect signal itself is GPIO `0x1203`. The
emulator declares `gpio` and `gpioic` as storage-only stub windows with no
interrupt generation, so GPIO interrupt 75 can never be delivered, which is
exactly why the callback has zero hits.

**The transport.** `/device-tree/arm-io/spi2` is the baseband SPI:
`compatible 'spi,s5l8900x,baseband'`, `reg {0x05200000,0x1000}` → PA
`0x3d200000`, `interrupts {0x07, 0x02}`, with `function-srdy` GPIO `0x1804`,
`function-mrdy` GPIO `0x1702`, `function-mosi` GPIO `0x1806`, `function-sclk`
GPIO `0x1805`, `function-fail_gpio` GPIO `0x0c03`, and DMA channel descriptors
pointing at `0x3d200010`/`0x3d200020`. Its siblings are `/arm-io/spi0`
(`0x04300000` → `0x3c300000`, interrupt 9) and `/arm-io/spi1`
(`0x04e00000` → `0x3ce00000`, interrupt 10).

**What was changed as a result.** All three SPI windows are now declared named
stubs. Exact disassembly of `BasebandSPI+0x1d42` shows the driver reading
offsets `0x00/0x04/0x08/0x34` into a heap transfer descriptor without testing
or polling them, and `BasebandSPI+0x1eca` shows the configuration burst that
wrote them, so honest storage is the faithful answer and nothing autonomous is
fabricated. This is a window, not a controller, and it is **not** claimed to
unblock the boot.

**How the reset state is actually read.** Vtable slot `0x35c` resolves to
`AppleBaseband+0x11bc` (`0xc05581bc`), and it is short enough to state exactly.
It loads the reset platform-function object from AppleBaseband `+0x6c` — the
one the observer reports as `value=c0b6b020` — returns a failure literal if it
is null, zeroes a one-word stack slot, calls the object's vtable slot `0x50`
with that slot as the out parameter, returns on a non-zero result, and
otherwise widens the returned word to the 64-bit `{value, 0}` the callback
compares against `+0x70`. So "read reset state" means "invoke the
`function-reset_det` GPIO platform function", nothing more.

**The platform-function descriptors, and what their last word appears to mean.**
Across the shipped tree the fourth word is consistent with a direction/operation
code, and the pattern is worth writing down because it bears directly on
whether an edge is faithful:

```text
baseband function-reset_det  {gpio, 'GPIO', 0x1203, 0x00000100}
spi2     function-srdy       {gpio, 'GPIO', 0x1804, 0x00000100}
baseband function-bb_rst     {gpio, 'GPIO', 0x0700, 0x00000101}
spi2     function-mrdy       {gpio, 'GPIO', 0x1702, 0x00000101}
baseband function-radio_on   {gpio, 'GPIO', 0x1507, 0x00010101}
spi2     function-fail_gpio  {gpio, 'GPIO', 0x0c03, 0x00000102}
spi2     function-mosi       {gpio, 'GPIO', 0x1806, 0x00000002}
```

`0x100` lands on exactly the two signals the application processor must
*sense* — `reset_det` and `srdy`, the modem's ready line — while `0x101` lands
on the two it must *drive*, `bb_rst` and `mrdy`. Treat that as a strong reading
of the encoding, not a decoded specification: it has not been confirmed against
the GPIO platform-function implementation in `AppleS5L8900X`.

**Why this argues against fabricating the edge.** If `reset_det` is an input
sensing a line the modem drives, then hardware with no modem fitted would not
produce a `reset_det` transition either, and GPIO interrupt 75 would not fire
on a real device in the same condition. The emulator's zero callbacks would
then be *faithful*, and the missing notification would not be what gates
CommCenter — because a real iPhone whose modem is dead still reaches
SpringBoard. Under that reading the blocker is elsewhere in CommCenter's
startup, and asserting interrupt 75 would be inventing hardware behaviour to
paper over a different bug. Do **not** assert it on the current evidence.

Note also that `AppleBaseband: Could not find mux function` is **stock
behaviour, not an emulator gap**: `/device-tree/baseband` genuinely has no
`function-mux` property, and that line appears in the earliest recorded
framebuffer consoles too.

**Therefore the deciding evidence is §13.0 question 1**, and it needs runtime
state this run did not capture: whether CommCenter's blocked receive port
`c0dd99d8` (mqueue `c0dd99f0`, task-local name `0x10004001`) is an
`ipc_pset` whose member set contains the AppleBaseband interest port
`c3c59ab0`. If it is not a port set containing that port, the baseband lead is
dead and the next frontier must be found by following the earliest of the five
queued senders instead. The ownership probe already validates the
`mqueue == port + 0x18` relationship and the active port-type check that
distinguishes `IOT_PORT` from a port set, so extending it to walk port-set
membership is a small, read-only addition — and it is cheap enough to answer
with a short bounded run rather than a full 24-minute replay.

Do not, on the current evidence, force a queue dequeue, retarget ownership away
from launchd, synthesize a baseband reset edge, inject a CommCenter reply, or
patch SpringBoard/CommCenter.

### Case A: AppleBaseband reset notification is exactly causal

If a complete reset-edge to registered-port to bound-queue route is proved and
it explains the saturation:

1. Determine whether the emulator is generating a reset edge that real
   no-modem/failed-modem hardware would not generate repeatedly.
2. Inspect stock driver logic and any reliable hardware documentation for
   RESET_DET, SRDY, UART/SPI mux, and event-source polarity/debounce.
3. Implement the smallest faithful **stable no-modem state**:
   - do not fabricate a working modem;
   - do not inject a successful telephony handshake;
   - do not patch SpringBoard or CommCenter;
   - do not drop arbitrary Mach messages;
   - do not wake a thread without the hardware transition that justifies it.
4. Add firmware-neutral unit tests for edge polarity, debounce, stable-state
   behavior, interrupt delivery, and reset sequencing.
5. Add exact device-model diagnostics and repeat the full validation/CI/run
   process.

The goal is for stock software to observe “modem absent/unavailable” gracefully,
not “modem fully functional.”

### Case B: Reset notification reaches its port but no receiver is eligible

Investigate CommCenter's receive lifecycle:

- Did registration happen before or after the notification?
- Is the notifier/port still live?
- Which exact thread owns the receive right?
- Was its last observed unresolved episode a semaphore/timed wait rather than
  Mach receive, and could it have been asynchronously woken while unscheduled?
- Did it ever enter `_ipc_mqueue_receive` on that mqueue?
- Did a schedule/identity omission hide a resume?
- Did queue saturation consist largely of reserved slots?

Fix the actual missing hardware or scheduler/CPU semantic that prevents the
service from reaching receive. Do not redirect ownership to PID 1 or force a
dequeue.

### Case C: AppleBaseband sends fail

Resolve the exact Mach result and route. A send failure may mean a dead/stale
port, size mismatch, invalid right, or lifecycle error. Do not interpret it as
queue fullness without a bound full route.

### Case D: AppleBaseband is not causal

Use the hardened queue walk to identify linked IDs, destinations, reservation
count, and service receive/dequeue activity. Follow the earliest complete
unresolved message producer. The next frontier may be a different IOKit service,
a timer/clock worker, or a generic Mach scheduling issue.

### Case E: The sender resumes but SpringBoard stops later

Treat that as a new reached path:

- identify the exact last SpringBoard user instruction;
- inspect exception/trap/switch lifecycle;
- resolve low-image/shared-cache address to stock code;
- look for unsupported CPU behavior before modeling another device;
- add a small regression for any CPU/MMU/VFP issue;
- instrument the next exact boundary;
- repeat through render.

Expect whack-a-mole: each fix unlocks previously unexecuted code. A newly exposed
bug does not invalidate prior progress.

## 14. Graceful no-modem and “building the baseband”

Do not attempt a full cellular baseband as the current M5 solution. A complete
GSM/UMTS modem, SIM stack, cellular protocol stack, and network registration
would be a separate major project.

For M5, “baseband support” means only the minimum hardware-faithful behavior
needed for unmodified iPhone OS to continue startup when no usable modem exists:

- stable/reset GPIO semantics;
- correct interrupt/edge behavior;
- correct UART/SPI mux availability or failure;
- bounded timeouts;
- no fabricated service success;
- stock CommCenter/SpringBoard receive their natural unavailable/no-service
  outcome.

Only implement whichever part Run23 proves is wrong.

Full telephony, SIM, cellular service, and real carrier data are explicitly out
of scope for the boot criterion. They can be revisited after the emulator is
usable.

## 15. iPhone 6s Plus and cross-platform constraints

The iPhone 6s Plus is the first performance and runtime target:

- Apple A9, arm64;
- iOS 15;
- jailbroken;
- 2 GiB host RAM;
- older than APRR-era JIT hardening.

The user explicitly authorized SSH access to the physical device. Credential
material was supplied in conversation, but **do not copy the password, device
address, or other secret into this repository, this handoff, shell history,
logs, process arguments, CI, or commits**. Retrieve it from the active secure
context or ask the user again when required. Prefer interactive authentication
or an ephemeral environment/credential mechanism. Never echo a password.

Device work must remain recoverable:

- do not make app launch depend unconditionally on JIT;
- do not execute unsigned generated code at startup until an opt-in diagnostic
  proves policy and cache maintenance on the device;
- preserve interpreter fallback;
- capture app crash logs and memory warnings;
- use bounded allocations suitable for a 2 GiB phone;
- keep guest CPU and host I/O off mutually blocking paths;
- do not depend on a jailbreak tweak for guest hardware semantics.

The A9 is an optimization target, not an architectural dependency. Keep:

- machine, CPU, devices, queues, protocols, and guest-session ownership in
  portable C;
- executable-memory policy in a small host adapter;
- UIKit/touch/audio/socket APIs in host adapters;
- deterministic core tests runnable on desktop/CI.

## 16. Later feature tracks

These tracks matter to the user's final objective but remain subordinate to the
current boot.

### Touch

`AppleMultitouchZ2SPI` starts in the guest, which identifies the expected
driver path. It does not prove a working device. Required work:

- disassemble/trace bootloader and report protocol;
- model SPI and any DMA/interrupt behavior faithfully;
- map normalized host coordinates to 320x480 guest coordinates;
- encode guest-compatible contacts, phases, pressure/major/minor if required;
- queue input without blocking the CPU thread;
- test bounds, multi-contact transitions, cancellation, rotation policy, and
  malformed reports;
- prove a guest tap through stock SpringBoard, not a host-side UI overlay.

### Audio

No guest audio device or host sink exists. Required work:

- identify the exact 3.1.3 I2S/controller and codec driver path;
- derive register, DMA, clock, format, and interrupt behavior;
- publish PCM through a bounded portable queue;
- perform host format conversion/playback outside the CPU thread;
- count underruns as silence and bound/count overruns;
- survive route changes, interruption, pause/resume, and app background policy;
- prove a deterministic guest tone reaches the speaker.

### Networking

Internet is a high-priority product feature. The documented recommended first
route is PPP over emulated UART3:

```text
stock guest pppd/pppserial
  <-> emulated S5L8900 UART byte stream
  <-> portable PPP/IP/NAT core
  <-> ordinary host socket adapter
```

Reasons:

- no guest kernel extension;
- no emulator-specific native tweak;
- no `utun` or raw-socket requirement;
- no phone-wide routing change;
- portable protocol state;
- host-neutral sockets boundary.

The CPU thread must never block on network I/O. Use bounded queues and explicit
backpressure/drop counters. First proof: guest `ppp0`, DNS resolution, and a
plain HTTP fetch. TLS support in modern 2026 servers is a separate compatibility
problem from basic IP connectivity.

Do not begin with full Marvell 88W8686 Wi-Fi emulation or “be the baseband.”

### JIT

The AArch64 emitter and ARM/Thumb translator exist behind
`-DIOS3VM_JIT=ON`. Hosted Apple Silicon executes emitted test blocks. The
machine run loop does not use the translator; there is no production code cache,
dispatcher, chaining, or invalidation, and the iOS app excludes the JIT sources.

Do not describe the current emulator as JIT-accelerated. Do not enable boot
dispatch until:

- the cache/dispatcher/invalidation lifecycle exists;
- unsupported forms fall back with exact state synchronization;
- interpreter/JIT differential tests cover reached paths;
- exception, MMU, self-modifying code, WFI, and interrupt boundaries are safe;
- an opt-in on-device probe proves executable-memory and cache-flush policy;
- launch remains recoverable if JIT is unavailable.

## 17. Documentation and evidence-maintenance rules

The user asked for regular README/docs updates and GitHub pushes. Keep them
truthful and commit-bound.

### Where each update belongs

- `README.md`: concise current headline, capability table, M5 line, latest run,
  strongest evidence, explicit “not rendered” boundary.
- `docs/QUALITY.md`: exact commit/run ledger, local/hosted/firmware validation,
  what each check establishes and does not establish.
- `docs/BOOTLOG.md`: chronological firmware-run narrative and exact coordinates.
- `docs/ROADMAP.md`: milestone status, current frontier, next work item.
- `docs/ARCHITECTURE.md`: only when a lasting ownership, interface, or
  host/core contract changes.
- `docs/debugging.md`: reusable diagnostic procedure, not every transient
  number.
- `docs/networking.md` and `docs/activation.md`: currently protected dirty
  paths; do not edit until ownership is resolved.
- `docs/AGENT_HANDOFF.md`: update at major handoff boundaries, not after every
  minor probe.

### Required language discipline

Use:

- “entered” for an instruction-entry probe;
- “returned with `r0=...`” only at an exact return;
- “candidate” when decisive register/object binding is absent;
- “bound” only when same-object/kmsg criteria pass;
- “saturated at entry” for `msgcount == qlimit`;
- “linked messages” only after a complete consistent queue walk;
- “did not resume before cap” rather than “permanent deadlock”;
- “source hashes unchanged” rather than “nothing was patched”;
- “rendered” only after guest-driven live pixels are captured and inspected.

Every report must state what it does **not** prove.

### Common overclaims to reject

- Exit 0 at an instruction cap means the configured cap was reached, not that
  the OS booted fully.
- A method-entry PC does not prove retirement or return.
- `applicationDidFinishLaunching:` does not prove UI readiness.
- `startWindowServer` returning does not prove pixels.
- CLCD `running=1` does not prove guest writes.
- A changed PPM is not necessarily SpringBoard.
- A seed-only PPM is not a black SpringBoard screen.
- A green public suite cannot execute private firmware.
- A green iOS build does not prove device launch.
- `msgcount=5` does not prove five linked messages.
- An address in a receiver union is not an owner until its discriminator and
  object graph pass.
- A nearby baseband log does not prove queue causality.
- A repeated ID 1000 does not prove `clock_get_time` without destination and
  SEND correlation.
- A static disassembly path does not prove it ran.
- Capstone's mnemonic does not replace architecture-version checks.
- A per-process trace block that reports one generation does not describe the
  whole run. If the process respawns, a longer run prints a **later**
  generation, and the difference between the two is not divergence.
- A checkpoint-restored run that reports different coordinates from its cold
  parent is not evidence of lost restore fidelity until the heartbeat PC stream
  itself is compared.
- A `hits=0` checkpoint proves nothing while the process is dying upstream of
  it; neither does a framebuffer hash that repeats across runs.
- A `hits=0` on a selector is not evidence the work did not happen until that
  selector's call sites are counted in the binary. `-[UIWindow
  makeKeyAndVisible]` has exactly one, on a restore path never taken at boot.
- Un-matching a child device-tree node is not un-matching the device; IOKit
  matching keys off the parent.
- A device-tree patch cannot affect a restored run: the snapshot already holds
  the matched driver in guest RAM.
- JIT tests do not mean boot uses JIT.
- Running on a jailbroken iPhone does not justify embedding a native tweak
  dependency in the core.

## 18. Useful commands

### Repository state

```powershell
git status --short --branch
git log -1 --format='%H%n%s%n%ci'
git diff --stat
git diff --check
git diff -- tools/bootkernel.c
git diff -- README.md docs/QUALITY.md docs/BOOTLOG.md docs/ROADMAP.md
```

### Search diagnostic implementation

```powershell
rg -n "springboard_commcenter|commcenter_wait|applebaseband" tools\bootkernel.c
rg -n "reservation-count|REJECTED-CANDIDATE|BOUND|frontier:" tools\bootkernel.c
rg -n "Run22|SpringBoard is|not rendered" README.md docs
```

### Build/test

```powershell
cmake --build work\build-strict-vfp --target bootkernel --parallel 1
ctest --test-dir work\build-strict-vfp -C Release --output-on-failure
```

Run only focused/local gates unless necessary; use GitHub Actions for the full
suite.

### Symbol and kext resolution

```powershell
work\build-strict-vfp\core\machoinfo.exe firmware\kernel.macho -k
work\build-strict-vfp\core\machoinfo.exe firmware\kernel.macho -r 0xc00146f4
```

Use the actual built path if it differs.

### CI

```powershell
gh run list --branch codex/m5-hardening --limit 20
gh workflow run core-tests.yml --ref codex/m5-hardening
gh workflow run ios-build.yml --ref codex/m5-hardening
gh run watch <run-id> --exit-status
gh run view <run-id> --log-failed
```

### Run23 first-pass log search

```powershell
$run = 'work\run23-commcenter-baseband'
Get-Content "$run\manifest.txt"
Get-Content "$run\run23.exit.txt"
Get-Item "$run\run23.stderr.log"
rg -n "stopped after|CPU stopped|panic|Debugger|external-md|COMMCENTER|APPLEBASEBAND|UIController|scanout|framebuffer" `
  "$run\run23.stdout.log" "$run\run23.stderr.log"
```

The grep is an index. Read complete surrounding report sections before drawing
conclusions.

## 19. File map for the next agent

```text
CMakeLists.txt
  top-level C11 build and IOS3VM_JIT option

core/CMakeLists.txt
  portable targets, 23 public test entries, host tools

core/include/
  public CPU, SoC, firmware, storage, bridge, snapshot, and JIT APIs

core/src/arm/
  ARM interpreter and VFP

core/src/mmu.c
  guest translation and access semantics

core/src/soc/
  S5L8900 machine and devices: VIC, timer, UART, power, I2C/PMU, CLCD, storage

core/src/firmware/
  IMG3/AES/LZSS/Mach-O/kallsyms/device-tree handling

core/src/jit/
  optional inactive AArch64 JIT foundation

core/tests/
  firmware-free public tests, including external-md CLI preflight

tools/bootkernel.c
  current boot harness and all Run23 diagnostic work

tools/ios3_kernel_patch.c
  exact loaded-kernel patch manifest

tools/rootfs_work.c
  immutable-source to create-only writable work-image provisioning

tools/file_block.c
  host file-backed block adapter

core/include/md_raw_bridge.h
core/src/md_raw_bridge.c
  portable bridge for XNU raw memory-disk I/O ("md" means memory disk)

app/Sources/VMGuest.c
  portable app/core seam; currently synthetic guest

app/Sources/VMEngine.m
app/Sources/VMFramebufferView.m
app/Sources/EmulatorViewController.m
  current iOS host shell

.github/workflows/core-tests.yml
  8-job public core/JIT/strict/sanitizer matrix

.github/workflows/ios-build.yml
  hosted unsigned/fake-signed IPA build; currently user-dirty locally

work/run22-commcenter-queue/
  retained exact Run22 binary, manifest, logs, rootfs work image, screen

tools/run23-cold-replay.ps1
  tracked fail-closed exact Run23 launcher; contains no firmware or secrets

work/run23-commcenter-baseband/launch-run23.ps1
  older ignored workspace-local launcher; not available in a fresh clone

work/tools/capstone-python/
  ignored F:-local Capstone Python package; reinstall locally in a fresh clone
```

## 20. Likely failure modes and edge cases

When reviewing or extending the current observer, explicitly consider:

- pointer reuse at the same guest address after port/notifier teardown;
- inactive IOKit objects that retain plausible fields;
- port-set versus port object type;
- receiver union interpreted without a discriminator;
- stale task/proc/PID after SETEXEC, vfork, or exit;
- cached unreadable identity suppressing a sparse exact hook;
- trap state from an old episode combined with a new block;
- a historical resolved wait printed as terminal;
- switch-out recorded without a readable switch-in;
- raw resumed execution missed by scheduler-only tracking;
- nested/reentrant `messageClients` or notification handlers;
- a linked handler from one reset event combined with an unlinked send/route
  from another event through aggregate counters;
- repeated SEND hooks in one handler inheriting prior header/route evidence;
- a failed later route candidate overwriting the retained successfully bound
  kmsg;
- bounded ring overwrite losing the decisive older half of a chain;
- sequence counter wrap;
- hit counter wrap;
- queue list cycles, malformed reciprocal links, truncation, or read faults;
- `linked_count > msgcount`;
- in-flight reservation counted as linked;
- kmsg numeric reuse after free;
- route PC hit with wrong decisive registers;
- send result associated with the wrong nested handler frame;
- registration result associated with the wrong stack/thread;
- notifier removed between destination match and delivery;
- AppleBaseband owner destruction/recreation at the same pointer and vtable,
  which currently lacks an independent owner-generation token;
- restored snapshot with incomplete subscription history;
- exact code shape from the wrong firmware revision;
- ARM/Thumb low-bit normalization mistakes;
- instruction-entry observations reported as completed stores/calls;
- diagnostics adding enough hot-loop overhead or host memory to perturb the
  run;
- a blocked off-CPU thread being asynchronously awakened or terminated but
  remaining unscheduled, leaving no resume instruction for the observer;
- a diagnostic virtual-memory reread traversing guest page tables whose corrupt
  descriptors point at side-effectful MMIO rather than RAM;
- formatted-output width/sign errors at billion-scale counters;
- Windows/LLP64 integer-width assumptions;
- guest pointer addition overflow before bounds checks;
- host path freshness bypass;
- duplicate external-md work image consuming another 445 MiB;
- a build or tool silently using C: temp.

Fail closed on all of them.

## 21. Stop/continue criteria for the next agent

### Continue automatically when

- a small local check fails with an actionable source error;
- hosted CI exposes a code/test portability problem;
- Run23 reaches a new exact guest frontier;
- a hardware semantic can be derived from exact guest code and runtime evidence;
- a bounded, portable fix can be tested without changing unrelated user files.

### Pause and report when

- the requested action would delete material run evidence;
- F: lacks safe headroom for another work image;
- protected dirty files must be changed to proceed;
- a firmware identity differs from the exact supported hashes;
- the only proposed solution is to patch stock SpringBoard/CommCenter or fake a
  successful modem;
- device work would require storing credentials or enabling an unrecoverable
  startup JIT path;
- evidence supports multiple materially different hardware behaviors and the
  choice cannot be resolved by further read-only analysis.

### Never declare completion merely because

- the token/context budget is low;
- a long run reached its cap;
- CI is green;
- the app packaged;
- SpringBoard executed code;
- one framebuffer byte changed.

The project is complete only when the user's actual outcome is complete and
repeatably evidenced.

## 22. Immediate continuation checklist

Publication gates already completed:

- diagnostic implementation and tracked launcher committed/pushed as
  `5a40c5eec5bbf7c4b7d8909d0c1f364bc078338a`;
- final adversarial logic and handoff audits found no remaining blocker/high
  issue;
- strict GCC, `bootkernel` rebuild, `git diff --check`, exact 7E18 `-n 0`, and
  launcher parser/fail-closed checks passed with F:-local mutable state;
- hosted core run `30143448600` passed eight of eight jobs;
- hosted iOS run `30143455036` passed its package job;
- **Run23 launched and completed** from exact commit `777afb4` with the Release
  build tree, exiting 0 at the 2.1 B cap in 1,434.86 s. Its results and
  non-results are in §7a.

Build-tree note for the next long run: use the F:-local **Release** tree at
`build\` (`cmake --build build --target bootkernel --parallel 1`). Run22's
548,933-byte and Run23's 625,423-byte binaries both came from it.
`work\build-strict-vfp` is configured `CMAKE_BUILD_TYPE=Debug` and produces a
~1.06 MiB unoptimised binary; it is fine for strict-warning checks but would
make a 2.1 B-instruction replay several times slower.

The next agent should:

1. Re-read `README.md`, `QUALITY.md`, `BOOTLOG.md`, `ROADMAP.md`, and this
   handoff, especially §7a and §13.0.
2. Verify the branch, that the source-build paths are clean, and that only the
   protected dirty paths remain.
3. Preserve the completed AppleBaseband/queue/wait invariants; repeat
   adversarial review if any implementation changes.
4. Answer §13.0's three read-only questions from the exact 7E18 binaries and
   retained Run23 evidence, using Capstone and `machoinfo`. Prefer read-only
   analysis and zero-instruction probes to another 24-minute replay.
5. Only then implement the smallest faithful hardware correction that analysis
   proves, with firmware-neutral unit tests.
6. Rebuild only `bootkernel` in the F:-local Release tree, re-run the strict
   gates and the exact 7E18 `-n 0` self-check, commit, push, and confirm hosted
   CI for the exact commit.
7. Launch the next long replay into a **new** run directory (`work\run24-...`)
   through the tracked launcher's `-RunDirectory`, in the documented hidden
   background process, polling at intervals no longer than 60 seconds. Never
   reuse or overwrite Run23's evidence.
8. Analyze integrity, exact identity, queue, waits, baseband chain, memory, and
   pixels independently.
9. Update README/QUALITY/BOOTLOG/ROADMAP and this handoff with the exact result
   and explicit non-results.
10. Push the evidence update.
11. Repeat until guest-driven SpringBoard pixels appear, then add guest touch.
12. Continue to app integration, audio, networking, device hardening, optional
    JIT, and portability rather than stopping at a screenshot.

The central lesson from every prior breakthrough is still the right operating
model: each fix unlocks code that has never run here before, and that newly
reachable code will expose another hidden bug. Make every stop exact, make every
claim narrower than the evidence, and keep moving the real untouched input
forward one proved boundary at a time.

## 23. State after 2026-07-26 — rendering reached; read this before §22

§22 is retained for history and is superseded from item 11 onward: "guest-driven
SpringBoard pixels" happened in run59. This section carries what a fresh agent
needs that exists nowhere else, because several findings below were produced by
analysis rather than by a commit and would otherwise be lost.

### 23.1 The guest renders. What did it.

`/device-tree/vram` ships with `reg = {0,0}`; iBoot fills it and we never ran
iBoot. Without it `IOSurfaceRoot` cannot publish its `PurpleGfxMem` region
(`0xc0529248`, skipped at `0xc052942c`), so `AppleH1CLCD` takes its fallback at
`0xc07060ac` and builds the surface with `withPhysicalAddress(..., kIODirectionOut)`
instead of `withSubRange(..., kIODirectionOutIn)`. `IOSurfaceClient` slot 21 then
does `cmp r0,#2 / moveq r2,#0x1000` at `0xc0526a38`, mapping any output-only
descriptor `kIOMapReadOnly`. Userspace received the framebuffer read-only and
SpringBoard's compositor faulted on its first store. One `dt_set_reg` call fixed
it (`691b727`). Screenshot: `docs/images/run59-first-frame.png`.

**Generalisable:** three top-level nodes ship `{0,0}` and iBoot fills all three.
We patch `/memory` and now `/vram`. **`/pram` is still zero** and nobody has
checked what reads it.

### 23.2 The console log is lossy. Distrust "message X never appeared".

`core/src/soc/uart.c:47` is `if (u->tx_len < UART_TX_BUFFER - 1)` — a
**first-8191-bytes cap, not a ring**. run59 wrote roughly 16,575 bytes to UTXH,
so **about half of every boot's console output is silently discarded, and the
discarded half is the tail.** Any negative claim about late-boot messages is
unsound until this is fixed, including several made on 2026-07-26.

Fixing it in `uart.c` means either a ring (which changes `tx_len` semantics and
therefore `SNAPSHOT_VERSION`, since `snap_uart` validates
`tx_len < UART_TX_BUFFER`) or a harness-side tee in `tools/bootkernel.c` that
streams UTXH to a file. The tee costs no snapshot bump and is preferred.

Also free and not currently used: `mt-strings=1` in `-c` turns on the entire
multitouch log stream (`mtlog` at `0xc0439bb0` needs `PE_parse_boot_argn` plus
`PE_i_can_has_debugger()`, and `debug=0x8` already satisfies the second).
`mt-bytes=1` additionally hex-dumps every SPI packet.

### 23.3 Activation: settled, and `docs/activation.md` is wrong

`docs/activation.md` is a protected file and could not be corrected in place, so
the corrections live here. All of the following is byte-verified against
`/usr/libexec/lockdownd` (VA = file offset + 0x1000, ARM not Thumb).

**SpringBoard does not gate the home screen on `ActivationState`.** It reads
`BrickState`. lockdownd derives `BrickState` *from* `ActivationState` at boot.
Two hops the document never modelled:

```
ActivationState --(lockdownd determine_activation_state 0xd340)--> BrickState
BrickState      --(SB _setupActivationState, key _kLockdownBrickStateKey)--> brickedDevice
brickedDevice   --(SBAwayView updateInterface)--> lockout screen
```

**The value the era's tools used does not work here.** Seeding
`ActivationState = Activated` is overwritten to `Unactivated` every boot
(`0xd4f8` CFEqual against `FactoryActivated` fails → `0xd50c` forces
`Unactivated` → tail `0xdba8`/`0xdd00` rewrites). Seeding **`FactoryActivated`**
survives (`0xd508` → `bne 0xda2c`; tail `0xdbb8` not taken → write skipped).

`ActivationState` is normally **non-persistent** (attribute 2, set at `0xdd14`),
which is why the runtime-created `data_ark.plist` has no such key at all.

**No Apple signature is needed to read state.** `verify_activation_record`
(`0xe218`) is called only from `dealwith_activation` and `handle_activate` — the
apply paths — never on boot-default read.

The file to provision, at `/private/var/root/Library/Lockdown/data_ark.plist`:

```xml
<key>-ActivationState</key>  <string>FactoryActivated</string>
<key>-BrickState</key>       <false/>
```

The leading `-` is the global-domain composed form (key builder `0x7b08`). The
second key is not redundant: it makes the fix hold even if `is_phone` is false,
since `determine_activation_state` then skips brick management entirely.

Note the pristine rootfs has `/private/var/root/Library` (CNID 4541) but **no
`Lockdown` subdirectory**, so provisioning must create a directory as well as a
file.

### 23.4 Touch: fully specified, ~750-900 lines, bounded

`AppleMultitouchZ2SPI` already starts. It is asleep in an **unbounded
`IOCommandGate::commandSleep()` with no deadline** waiting for an SPI completion
interrupt our stub cannot raise — failure shape number five. Arithmetic proof:
`spi1 r=0 w=19`; one stalled transfer costs `11 + N` writes (3 power-on + 6 setup
+ arm + 8 FIFO + go), so `N=8` is exactly the 8-deep TX FIFO filled by the
16-byte `isInHBPP` probe, and `r=0` proves the ISR never ran.

> **Corrected 2026-07-26 after implementation.** An earlier revision of this
> section gave the sleep as `0xc05a5f0c` with its loop at `0xc05a5f18`. **That is
> `AppleS5L8900XI2CController`, not SPI** — the metaclass constructor at
> `0xc05a620c` names it. The behaviour described was right; the address was the
> wrong driver, and it would have sent a reader to the I2C bus. The SPI
> controller's no-deadline `commandSleep` is **`0xc05a6d80`**, its loop is
> `beq 0xc05a6d70` at `0xc05a6d8c`, and its done flag is `this+0xE0`. Three
> further corrections in the same revision: the success `printf` is **conditional**,
> reachable only through `beq 0xc0442714` at `0xc04426d0` (the `isInHBPP()==false`
> edge), and its literal is `"%s: Could not detect HBPP. Returning false from
> finishStarting().\n"` — **grep for `HBPP`, not the sentence**; `isInHBPP` is
> reached by **virtual dispatch through slot `0x4d0`**, not a direct `bl`, though
> `provider->v[0x368](tx,16,rx,16,0)` at `0xc04410a4` is confirmed exactly; and
> there is **no `chip-select` property anywhere in the device tree** — the select
> is `reg[0]` of `/arm-io/spi1/multi-touch`, which the driver masks with
> `and r3,r3,#3`. `internal-cs` appears zero times, hence `_spiInternalCS = 0`.

Touch is on **`/arm-io/spi1`** (`multi-touch,n82`, chip-select 0, PA
`0x3CE00000`, VIC 10). **`/arm-io/spi2` has zero children** and is the baseband
transport — un-matching it is irrelevant to touch.

**The guest has already armed the interrupt**: `GPIOIC INTEN group 4 =
0x08000000` (bit 27 = line 155), measured in run59. "GPIO 155" is an
interrupt-controller line index, not a pin; 155 = group 4, bit 27, stride 32.
Reset is group 6 bit 6, power_ldo group 7 bit 1.

**You never implement the HBPP firmware download.** Apple built the escape
hatch, but it is direction-sensitive and getting it backwards uninstalls the
driver:

| probe # | required answer |
|---|---|
| **1st exchange**, from `finishStarting` `0xc0442670` | **in HBPP** — accepted BE16 set `{0x1AA1,0x18E1,0x1F01,0x4879,0x4969,0x4BC1,0x4AD1}` |
| **every later exchange**, from `attemptToBootloadDevice` `0xc04414c4` | **not in HBPP** — zeros |
| then `getReportInfo(0xD3)` | must succeed → `isBootloaded()` true |

> **Corrected again, 2026-07-26.** A previous revision said the device should
> discriminate on `resetDevice(TRUE)` having occurred between probe 1 and the
> rest. **It cannot: `resetDevice` is not on this path at all.**
> `attemptToBootloadDevice` calls `isInHBPP` as its *first act* with nothing
> before it — no reset, no GPIO write, no delay, no power call (`0xc04414d0`-
> `0xc04414dc`) — and the retry loop re-enters with nothing in between
> (`c043aa0c`/`c043aa10`), so attempts 1, 2 and 3 are **byte-identical on the
> wire**. A whole-range scan for `ldr pc,[r3,#0x44c]` across
> `0xc0437000..0xc0446a00` finds one call site, `0xc0438160`, in a third vtable
> reached from none of these paths. `setDownloadMode` is also a no-op here
> (`function-enable_download` is absent from the node, so it returns
> `kIOReturnUnsupported`). **The model must use a probe counter — accept the
> first exchange, decline every later one.** There is no other signal.
>
> Two consequences. Answering "in HBPP" runs `strb r3,[r4,#0x1bc]` at
> `0xc04426fc`, and `deviceReadResultData` (`0xc0441324`) then **rejects `0xEB`
> unless `0x1bc != 0`** — so the frame opcode is downstream of the HBPP answer.
> And `isBootloaded` (`0xc043ac58`) is `getReportInfo(0xD3, &out4, retries=4,
> useCache=0)` with `rsbs r0,r0,#1 / movlo r0,#0`: **true iff the call returns
> 0, inspecting no payload field**, and `useCache=0` forces a real transaction.
> Its wire format is `tx[0]=0xE3, tx[1]=id, tx[14..15]=LE16(0xE3+id)`, a 16/16
> transfer, `IODelay(25)`, then an identical 16-byte transfer carrying the
> answer, which must have `rx[0]==0xE3` and `LE16(rx[14..15]) ==
> sum16(rx[0..13])`.
>
> Also: the failure literal has a **double space** after the colon. Grep `HBPP`.
> And slot `0x458` holds `0xc044061c`, a bare `bx lr` leaving `r0 = this`, which
> makes the following `IOSleep(r0)` nonsensical; the vtable base is anchored
> twice independently so it is probably not a mis-index, but it is unexplained
> and nothing should be built on it.

Answering "not in HBPP" at probe 1 makes `finishStarting` return false and the
driver detaches. Cost of the skip is three cosmetic `Bootload attempt N of 3
failed` lines, and it drops the 54,156-byte firmware **and the entire
`AppleARMPL080DMAC` model** out of scope. Normal operation is PIO, two
transactions per frame.

**Do not raise the SPI interrupt with an empty RX FIFO.** The ISR at
`0xc05a6688` begins `if (((status>>8)&0xF) == 0) return 0;` — it bails without
waking anything, reproducing the current hang exactly.

**And do not raise it without checking the enable bit either.** `0x100` of the
`0x180` written to SETUP is the interrupt enable, and gating the line on it is
**mandatory**, not optional. The driver's order is prefill → arm → enable, so a
line that ignores the enable fires on the **first prefill store** and runs the
filter against transfer counts it has not finished building. Pinned twice: the
filter clears exactly `0x100` on completion (`bic r3,r3,#0x100` at `0xc05a6808`),
and `finishTransfer` writes the bare base word back. An earlier revision of this
section described the line without that gate; implementing it as written would
have been a live bug, and the model's mutation test №5 exists for it.

Two more details the first revision omitted. The STATUS field positions are
**version-dependent** — v1 uses `(s>>6)&0x1F` and `(s>>11)&0x1F` with depth 16,
base `0x4000`, mask `0x0040000F` and a register at `0x4c` — which is harmless
here only because the device tree gives all three controllers `spi-version {0}`.
And completion is **by count exhaustion with no hardware done bit**
(`0xc05a67d4`-`0xc05a67ec`); the `0x34` word count is latched and decremented for
visibility but does not gate the shifter, since both the driver's own DMA path
(`0xc05a6b10`) and BasebandSPI store zero there on controllers they then use.

Build order, each step independently observable: (0) fix the UART capture;
(1) SPI controller + **null slave returning 0x00**, whose success criterion is
the `printf` at `0xc0442734` — **grep for `HBPP`**, and expect two lines in
order, `"...: Could not detect HBPP. Response: 0x.."` from inside `isInHBPP`
followed by the `finishStarting()` line. **Done: `core/src/soc/spi.c`, 230 lines,
25/25 ctest, five mutation checks.** (2) GPIO interrupt controller, 7 groups ×
{INTLEVEL 0x80, INTSTAT 0xA0 W1C, INTEN 0xC0, INTTYPE 0xE0}, cascading to VIC
lines `{0,1,2,3,31,32,33}` — group 4 → VIC 2; (3) Z2 slave bring-up; (4) frame
path and host injection; (5) payload format, the only genuine unknown.
**Steps 4 and 5 are done — see §23.4b and §23.4c, which correct eight more
things in this section and record the format that was actually reversed. The
frame parser is `_mt_HandleMultitouchFrame` at cache VA `0x33cfb3ec`, NOT
`_MT_ParsedMultitouchFrameRepCreate` at `0x33cf99c0`, which is a constructor.**

**`wake_line_enabled()` rejects any line ≥ 32×VIC_COUNT.** A wake-source entry
written as `{ "multitouch", 155, … }` returns false silently and can never wake
the CPU. The entry must be the cascade VIC line.

Two downstream kill-switches that will look like touch bugs and are not:
SpringBoard VA `0x00041268` drops every digitizer event when the byte at data VA
`0x001076b8` is non-zero, and run59 already logs 29 consecutive `IOSurface
warning: buffer allocation failed. 320 x 480`, which will bite the moment a tap
launches an app.

#### 23.4a Corrected, twice more, by run65 and run67. Read this before §23.4

§23.4 is right about *what* the HBPP answer must be at each site and wrong about
every mechanism it suggests for deciding it. So was the first revision of this
subsection. Four designs have now been tried; two of them shipped and were
caught by a boot. What follows is what the firmware actually does.

**FIRST: HOW TO SEE ANY OF THIS.** Three things that were got wrong repeatedly:

- **LAUNCH FROM POWERSHELL, NOT FROM GIT BASH.** MSYS path conversion rewrites
  any argument that looks like a Unix path, and the one that matters is
  `--fstab "/dev/md0 / hfs rw,update 0 1"`, which arrives as
  `C:/PortableGit/dev/md0 / hfs rw,update 0 1`. The guest's fstab then names a
  Windows path, `fsck_hfs` fails with `CAN'T CHECK FILE SYSTEM`, launchd halts,
  and the console ends at `pmu go stdby` — about 1.1 billion instructions of
  boot that all look fine and prove nothing. Run69 lost a whole 27-minute run
  to exactly this. The recipe in §11 is PowerShell for a reason. If you must
  use `Start-Process -ArgumentList`, note that it joins the array with spaces
  and does NOT quote: `-c` and `--fstab` need their own embedded `"` or the
  boot dies on `unknown option: serial=1`.
- **`spy_install()` memsets the whole of `G`.** Any option parsed straight into
  `G` is silently discarded, and `--print-config` CANNOT see it, because
  `--print-config` exits before `spy_install` runs. `-H` and `--call-probe`
  already parse into locals and copy afterwards; `--touch` did not, and the
  result was a 27-minute run that accepted two taps, injected neither, and
  printed no touch section at all. Every such option now prints an "armed on"
  line in the run header for exactly this reason: a header that does not carry
  it is a run that did nothing.

- **`debug=0x8` does NOT turn on the multitouch log.** `PE_i_can_has_debugger()`
  reads `/chosen/debug-enabled`, so the boot needs
  **`-D chosen:debug-enabled=1`**. With it, `mt-strings=1` produces the whole
  `mtlog:` stream and the bring-up becomes readable instead of inferred.
- **The bootload timer does not fire until instruction 1,112,618,577.** Every
  cap used before run65 — 400e6, 700e6 — stopped short of the entire bootload
  path, so no observation about it made before then means anything. Use a cap of
  at least **1.5e9**.

**The anchor for all slot arithmetic is vtable base `0xc0449f40`**:
`base + 0x4d0 = 0xc044a410`, which holds `0xc0441008` = `isInHBPP`. A base of
`0xc0449f3c` is off by one slot and makes `0x4d0` resolve to `getCMDStatus`.

**1. `resetDevice` DOES run, at every probe site, and it clocks a decoy.** The
first revision of this subsection claimed resetDevice was unreachable on this
path. Wrong — run65's mtlog shows it at both sites, and each site is:

```
mtlog: AppleMultitouchSPI::setPowerEnabled[false]
mtlog: Asserting reset line
mtlog: disabling power
mtlog: AppleMultitouchSPI::setPowerEnabled[true]
mtlog: enabling power
mtlog: ensuring S_CLK is high
mtlog: initiating dummy transfer        <- 16 bytes, reset still ASSERTED
mtlog: Deasserting reset line
mtlog: checking if in HBPP              <- 16 bytes, reset RELEASED
```

The dummy transfer at `0xc0440d7c` builds **the same sixteen bytes as the
probe** — `mov r3,#0x1a / strb / sub r3,r3,#0x79 / strb` then `0x18,0xE1`
seven times — sends them through the same `v[0x368]`, and discards the answer.
So each site makes **two byte-identical exchanges** and only the second one's
answer is read. Any scheme that counts probes spends itself on the decoy.

**2. The reset line is ACTIVE LOW, and its level is the only discriminator.**
Measured to the instruction from the GPIO trace with `-H 0x3e400000`:

| instruction | fsel write | meaning |
|---|---|---|
| 220,635,069 | `0x0006060e` | assert (group 6, bit 6, level 0) |
| ~309,530,000 | — | the dummy transfer runs |
| 309,541,162 | `0x0006060f` | release |
| 316,898,121 | — | `isInHBPP` entered, `lr = 0xc04426cc` |
| ~316,910,000 | — | the probe runs |
| 316,965,809 | `0x0006060e` | assert again |

The dummy is entirely inside the asserted window and the probe entirely outside
it. A part held in its reset pin drives nothing, so modelling that one physical
fact separates the decoy from the probe with no counting at all. Note also that
the guest's **first** store to this pin writes level 0 — no change from the
pin block's all-zero power-on state, so **no watch callback fires**; a model
must start out believing the part is held down.

**3. A reset must NOT re-arm the claim.** `098ce49` made a reset restore the
in-HBPP state, on the reasoning that a part with no firmware resets into its
bootloader. That reasoning is correct about silicon and fatal here: the guest
resets before *every* site, so it makes the bootload site identical to the
first, and run65 watched the driver answer affirmatively and begin
`MTSPIBootloader_Z2::bootloadDevice()` / "sending preconstructed firmware
bytes". A reset restores the part's **state**; it does not un-ask a question the
host has already had answered. The working model is one monotonic bit spent by
the first probe answered **out of reset**.

**4. Declining at `attemptToBootloadDevice` DOES produce the three
`Bootload attempt` lines.** The first revision of this subsection said the
opposite, from a mis-derived call graph. The literal
`"not in HBPP, so skipping bootload"` (`0xc04486f0`) is referenced from exactly
one place, `0xc04415e4`, which is `attemptToBootloadDevice`'s own literal pool:
the FALSE path logs it and returns 0, and the retry loop at `0xc043a980`
(`subs r2,r0,#0 / beq 0xc043a9e8`) counts that as one failed attempt and prints
`Bootload attempt %d of %d failed`. Three of those, then `isBootloaded()`.

**4a. "Device has firmware?!" DOES NOT PRINT on the cycle that succeeds.**
Expecting it as the success marker will read a working boot as a failure. After
the three attempts the routine calls `isBootloaded()` and branches
`bne 0xc043aa80` (`0xc043aaac`), which lands PAST the log at `0xc043aa64`. The
string only prints on a LATER bootload cycle, when the cached byte at
`this+0x69` is already 1 and `0xc043a950` branches to it. run68 confirms the
shape with `--call-probe-kernel` on all three targets: `0xc043ac58`
(`isBootloaded`) captured 1, `0xc043aa80` (the true branch) captured 1,
`0xc043aa64` (the log) captured 0, and `0xc043aa48`
("No firmware running, and couldn't load any") captured **0**. The correct
success criterion is the ABSENCE of `0xc043aa48`, plus three
`Bootload attempt N of 3 failed` lines.

**5. "Device has no firmware - will attempt to bootload" is a cache read, not a
failed report exchange.** The string at `0xc04472b4` is referenced from
`0xc043aac8`, inside the timer-fired routine, and the top of that routine tests
the cached byte at `this+0x69` (`ldrb r6,[r4,#0x69]` at `0xc043a948`) which is
only written after the three attempts (`strb r0,[r4,#0x69]` at `0xc043aaa8`).
So seeing that line does not mean `getReportInfo(0xD3)` failed — on the first
pass it has not been asked yet.

**6. THE SINK IS NOT BLOCKED BY DMA.** §23.4 says skipping the download "drops
the entire `AppleARMPL080DMAC` model out of scope", which is true, but the
implication that implementing it would *require* that model is not.
`MTSPIBootloader_Z2` pushes through the ordinary SPI entry `v[0x368]` at
`0xc0444ff8`, `0xc0445224` and `0xc04454d0`, and the controller only arms DMA
when `this+0xf4` is non-zero — `ldr r5,[r4,#0xf4] / cmp r5,#0 /
beq 0xc05a6cb4` at `0xc05a6c24`, the branch that skips `orr r2,r2,#0x40`. Our
16-byte transfers run in PIO today, which proves that field is zero, and it is
not per-transfer. So a firmware sink would receive the 54,156 bytes through the
same slave callback everything else uses. What blocks it is that the
bootloader's own multi-stage protocol is unread, its failure mode is a hang
inside `commandSleep` rather than an error, and the feedback loop is an
18-minute boot. It is a fidelity upgrade, not a prerequisite.

Confirmed unchanged and now derived rather than assumed: the probe pattern
(`1A A1` then `18 E1` seven times, loop `0xc0441030`-`0xc0441048`); that **both**
`BE16(rx[0..1])` and `BE16(rx[2..3])` must pass (`0xc04410fc`, `0xc044110c`)
while `rx[4..15]` is only printed; and the accepted set, whose literal pool
gives `0xc04406b4 = 0x18E1`, `0xc04406b8 = 0x1AA1`, `0xc04406bc = 0x4879` with
`+0x620 -> 0x1F01`, `+0x2CC0 -> 0x4BC1`, `+0xF0 -> 0x4969` and
`-0xF0 -> 0x4AD1`. The compared value is `uxth`-truncated. The failure `printf`
has a **double space** after the colon, so grep for `HBPP`.

Also confirmed: `/arm-io/spi1`'s `function-spi_cs0` (GPIO `0x1800`, group 24
bit 0) is really driven, `0x0018000e` at 316,902,467 and `0x0018000f` at
316,918,158, bracketing the probe transfer — so a slave that wants packet
framing from the chip select can have it.

One thing flagged and unexplained: slot `0x458` holds `0xc044061c`, a bare
`bx lr` that leaves `r0 = this`, which makes the following `IOSleep(r0)`
nonsensical. The base is anchored twice, so it is probably not an indexing
error. Do not reason from that line. **RESOLVED — and it was simply misread.
See §23.4b item 1.**

#### 23.4b Corrected an eighth time, by implementing steps 4 and 5

Eight more things §23.4 and §23.4a get wrong or leave out, every one of them
found by writing the code rather than by reading the section again. The pattern
has now held for every revision: the parts that were *measured* survive and the
parts that were *inferred from a call graph* do not.

**1. Slot `0x458` is not a bare `bx lr`. It is a constant accessor returning
15, and `IOSleep(15)` is exactly what it is for.** `0xc0449f40 + 0x458 =
0xc044a398`, which holds `0xc044061c`, and `0xc044061c` disassembles as
`mov r0, #0xf` / `bx lr` — two instructions, not one. The neighbouring
`0xc0440614` is `mov r0, #0xc8` / `bx lr`, the 200 in the guest's own
`"Delaying poweron by 200 ms"`. §23.4a's advice to "not reason from that line"
should be reversed: it is one of the most ordinary lines in the driver.

**2. The kext DOES parse one byte of the frame payload.** §23.4 says it parses
none. `0xc0441400`'s `v[0x414]` lands at `0xc04389fc`, which is
`ldrb r3,[r1] / cmp r3,#0x50` and, on a match, calls `v[0x418]` = `0xc043d1ac`,
a separate decoder for an 8-byte status record. Only a payload whose FIRST BYTE
IS NOT `0x50` tail-calls `v[0x3c4]` = `0xc043c31c`, which fans it out to up to
32 subscribed clients through `0xc043d684`:
`IODataQueue::enqueue(payload, (len + 3) & ~3)`. The rounding is confirmed
(`add r2,r2,#3 / bic r2,r2,#3`), so what userspace measures is up to three bytes
longer than what the device sent.

**3. There are TWO `readOneFrameOfData` implementations and they use different
transfer lengths.** `0xc043bea0` — in a different vtable, at `0xc044961c` —
calls the data read with **`L + 1`** (`add r1,r1,#1` at `0xc043bf98`).
`0xc0442f70` — AppleMultitouchZ2SPI's own, at vtable slot `0x398` =
`0xc044a2d8` — calls it with **`L + 5`** (`add r1,r1,#5` at `0xc04430c4`).
§23.4's `L+5` is right for the class that runs. A reader who lands on the other
function first will measure `L+1` and conclude the section is wrong; it is not,
but nothing in it says there are two.

**4. The data read's TRANSMIT checksum is not at `[14..15]`.** It is at
`tx[len-2..len-1]`, the end of the TRANSFER (`0xc0441254` and `0xc044126c` store
it relative to the length argument), while still being `sum16` of only the first
FOURTEEN bytes. For the 16-byte length read the two coincide; for the `L+5` data
read they do not, and a device that validated the host's checksum at `[14..15]`
would reject every data read.

**5. The receive side's length field is at a DIFFERENT offset in the two
reads,** which looks like a transcription slip in §23.4 and is not.
`0xc04425b0` reads the length answer as `rx[1] | rx[2] << 8`; `0xc0441370` reads
the data answer's as `rx[2] | rx[3] << 8`. Both are LE16 and they really are one
byte apart. The data read's `rx[4]` then carries whatever makes the first five
bytes sum to zero mod 256 — that is the ONLY header check the driver makes
(`0xc0441330`) — and `L` of 0 or 2 is tested BEFORE the payload checksum, so an
empty frame never needs one.

**6. The frame opcode and the toggle are `this+0x7e5` and `this+0x7e6`, and the
toggle really does alternate.** `0xc0440560` writes `0xEB` when `this+0x1bc` is
non-zero and `0xEA` when it is not — so §23.4a's "the frame opcode is downstream
of the HBPP answer" is confirmed at the instruction — and initialises
`this+0x7e6` and `this+0x7e7` to 1. `0xc0443100` flips `this+0x7e6` between 1
and 2 after each SUCCESSFUL data read. Nothing in any receive path is validated
against it, so a device may ignore it; but §23.4's "toggle(1/2)" is now measured
rather than assumed.

**7. `Z2F13` is the wrong personality, and
`_MT_ParsedMultitouchFrameRepCreate` is not the frame parser.** Both are
step-5 pointers in §23.4 and both send a reader to the wrong place.

  - The n82 kext is **`AppleMultitouchSPIZ2F52`**, personality
    `"AppleMultitouchSPI - Z2,N82"`, `IONameMatch multi-touch,n82`,
    `mt-merge-personality Z2F52,1` — read out of the prelinked Info.plists in
    `firmware/kernel.macho` at file offset `0x763a3d`. `multi-touch,k48` and
    `Z2F13` appear **zero** times in this kernelcache; Z2F13 is iPad 1. The
    `Z2F52,1` dictionary in `iPhone.mtprops` holds exactly three keys —
    `Constructed Firmware` (54,156 bytes), `Constructed Firmware Version`
    (`"0x0049.bin"`) and `PreconstructedBootloadPacketType` (`"Z2"`). **No
    geometry anywhere in any shipped plist**, which is why the device has to
    invent it.
  - `_MT_ParsedMultitouchFrameRepCreate` (`0x33cf99c0`) is a two-`malloc`
    constructor called once from `_MTDeviceCreate+0x98`. It allocates `0x3c8`
    bytes, then a second buffer sized by the `"Max Packet Size"` property which
    it stores at `rep+0x14` as the IMAGE buffer, then `memset`s the first. The
    real dispatcher is **`_mt_HandleMultitouchFrame` at `0x33cfb3ec`**.

**8. `function-power_ldo`'s polarity cannot be read off the device tree.** The
node gives `function-power_ldo {phandle, 'GPIO', 0x0701, 0x00000101}` beside
`function-reset {..., 0x0606, 0x00010001}`, and the obvious reading of that
fourth word is contradicted immediately: `/baseband`'s `function-bb_rst` is
`{..., 0x0700, 0x00000101}` — the same word for a reset line rather than a
supply. The reset line's polarity was settled by MEASURING the guest's fsel
stores; nothing equivalent has been measured for the power line, so
`core/src/soc/mtz2.c` tracks its level and publishes it and gates nothing on
it. Gating on a guessed polarity would refuse every injection for a whole boot
with the device looking healthy.

#### 23.4c The frame payload, and how it was pinned

The kext imposes only item 2 above. Everything else is userspace's, and it was
read by two agents given the same question and no access to each other's
answer — one reading `MultitouchSupport.framework` inside the shared cache at
`work/analysis/dsc_armv6` (load address `0x33cf6000`, **`__TEXT` file offset =
VA − 0x30000000**; the per-image `__TEXT fileoff` is 0 and using it decodes a
different dylib entirely), the other reading `MultitouchHID.plugin` at
`work/analysis/touch2/MultitouchHID.bin` (**VA == file offset**). Both binaries
are **ARM, not Thumb**.

They agree on all of this:

- **The format is self-describing.** `_mt_HandleMultitouchFrame` switches on
  `payload[0]`: `0xC5/0xC6` image, **`0xCC/0xCE` contacts with a fixed 32-byte
  stride**, `0x43/0x44` V2 binary, `0x73/0x74` V3 binary, `0x50` and `0x02`
  silently ignored, anything else logged and dropped. **The device chooses.**
- For `0xCC`: header is a CONSTANT ten bytes — `[0]` type, `[1]` frame counter,
  `[2]` button state, `[3]` contact count — and the contact stride is 32
  (`lsl r2,r3,#5` at `0x33cfbbcc`).
- Per record: `[0]` path id, `[1]` stage, `[2]` finger id, `[3]` hand id
  (SIGNED), `[4..7]` X as a 32-bit value the parser shifts right by 8,
  `[8..11]` Y the same, `[12..19]` the two velocities (shifted right by 9),
  `[20..21]` Z total (÷256), `[28..29]` ellipse major and `[30..31]` minor
  (÷100 → mm).
- **Coordinates are signed 16-bit HUNDREDTHS OF A MILLIMETRE** — the same unit
  report `0xD9`'s surface size is already published in. Normalisation is
  `(v − min) / (max − min)` over the surface bounds.
- Path stages 0..7 are `NotTracking, StartInRange, HoverInRange, MakeTouch,
  Touching, BreakTouch, LingerInRange, OutOfRange`. There is no
  began/moved/ended triple; it is a hover-capable lifecycle.
- Path identifiers must be **1..11** (`sub r3,r1,#1 / cmp r3,#0xa` at
  `0x33cfd5f4`); outside that they silently alias to slot 0, and
  MultitouchHID's own `mthm_ExpandAndFilterPackedContacts` `memcpy`s into
  `gMTHMPathStates + id*0x5c` with no bounds check at all.
- **No magic number and no checksum anywhere in userspace.** The rejections are:
  an unknown `payload[0]`; a length at or below 9; a length below
  header + count × stride.

Two things only one reader established, and they are marked as such in the
code:

- **The `Endianness` selector.** `_mt_SwapInt32DeviceToHost` (`0x33cfb8e0`) is
  `cmp r0,#1 / beq done / rev`, with `r0` the cached `Endianness` property. So
  **1 means NO SWAP and every other value byte-swaps** — zero is *not* "native".
  The model's `endianness = 1` was a guess when it was written and is now the
  right value for a stated reason.
- **The Y axis is flipped between the two coordinate systems.** MultitouchHID
  computes a pixel row as `py = H * (1 − norm.y)`, i.e. device Y increases
  UPWARD (the trackpad convention this stack came from) while a panel row
  increases downward. `to_surface()` in mtz2.c applies that flip and
  `test_mutations_are_caught` pins it, because a mirrored digitizer looks like
  "the tap went somewhere else" rather than like a bug.

And one number worth knowing before the geometry is ever revisited: when
`"Sensor Surface Width"`/`"Sensor Surface Height"` are ABSENT the framework
defaults to **5000 × 7500** (`0x1388`/`0x1d4c` at `0x33cf7db8`/`0x33cf7dd8`),
i.e. 50.00 mm × 75.00 mm, and MultitouchHID's hardcoded fallback rect is the
same 50 × 75. This project reports 4800 × 7200 instead, which buys an exact
15 units per pixel in both axes; Apple's own number would be 15.625. The choice
stands, but it is now a choice made against a known alternative rather than in
the dark.

#### 23.4d The GPIO interrupt controller's pending latch is EDGE-triggered

This is the ninth correction, and unlike the other eight it is a correction to
*this project's own model* rather than to a reading of Apple's. It is also the
one that decided whether touch works at all, so it is worth the space.

`core/src/soc/gpioic.c` shipped with a LEVEL-sensitive pending latch: the
guest's write-one-to-clear cleared the bit and then immediately re-asserted it
from whatever the board was still driving. The file argued the case in three
points, and the third — "for a device that does deassert it is
indistinguishable from an edge latch, so nothing that behaves correctly can
tell the difference" — is simply false. run71 measured how false:

```
HOT PAGE 0x39a00000, offset 0x0b0 (INTSTAT group 4):
    reads 1,193,122   writes 1,193,123   lastval 0x08000000
@1799986776 R 0x39a000b0 val 0x08000000  pc 0xc05a44dc lr 0xc05a4310
@1799986797 W 0x39a000b0 val 0x08000000  pc 0xc05a44e8 lr 0xc05a4358
@1799987195 R 0x39a000b0 val 0x08000000  pc 0xc05a44dc      <- 419 later
@1799987216 W 0x39a000b0 val 0x08000000  pc 0xc05a44e8
```

One touch report was queued at instruction 1,300,000,000 and the attention line
came up. The GPIO interrupt controller's filter read the pending word, wrote it
back to acknowledge, and the re-latch undid the acknowledge inside the same
store — so it re-entered every ~419 instructions and did so **1,193,122 times**,
for the whole remaining half-billion instructions of the run.
`IOWorkLoop::signalWorkAvailable` ran every time; the work loop's thread never
got scheduled; `AppleMultitouchZ2SPI`'s handler never ran; the frame read never
happened. The device's own counters said so from the other side:
`queued 1, length-reads 0, data-reads 0, read 0` with `in-reset 0` and
`hbpp-answered 1`. A LIVELOCK, not a lost interrupt, and it looks exactly like
"touch is broken" from every direction except this register.

The driver settles it independently: it writes this group's `INTEN` **twice in
the whole boot** (offset 0x0d0, writes 2), so it does not mask the line while
servicing it. A controller whose pending bit survived its own acknowledge would
be unusable by this driver, which means the real part's does not.

So the write-one-to-clear CLEARS, and only a RISING edge on the incoming line
sets a pending bit again. Deasserting still does not clear the latch, so a
pulse shorter than the guest's polling interval is still delivered — the one
property the level design was actually right to want. Four mutations pin it.

**A third downstream kill switch, alongside the two §23.4 already lists.**
MultitouchHID drops every frame while its `UILocked` flag is set, and that flag
is **initialised to 1**. Neither of the two probes §23.4 names as the step-5
milestone can fire while it is.

### 23.5 The HFS+ provisioner blocks two features, not four

`tools/rootfs_work.c` does size-neutral in-place rewrites only and has no
catalog B-tree code. It genuinely blocks **activation** (§23.3, which needs a new
directory *and* a new file) and the **jailbreak payload** (§23.6, 555 files).

It does **not** block touch (§23.4) — every multitouch file ships and
`mtmergeprops` already runs.

And it may not block **PPP** either. The rootfs ships **no ppp launchd job** and
`/private/etc/ppp` is empty, but several shipped launchd plists point at
binaries that **do not exist on this image**, so rewriting one in place is
size-neutral with zero collateral damage — exactly the mechanism already used
for `/etc/fstab` and the SpringBoard `CA_ENABLE_MBX2D` plist. Candidates, best
first:

| plist | size | format | target | why inert |
|---|---|---|---|---|
| `com.apple.chud.pilotfish.plist` | **530 B** | XML | `/Developer/usr/libexec/pilotfish` | absent — **best, largest budget** |
| `com.apple.chud.chum.plist` | 515 B | XML | `/Developer/usr/libexec/chum` | `/Developer` absent |
| `com.apple.graphicsservices.sample.plist` | 447 B | XML | `/usr/local/bin/sampled` | `/usr/local` absent |
| `com.apple.tcpdump.server.plist` | 433 B | XML | `/usr/libexec/tcpdumpserver` | absent, and not `RunAtLoad` |

Budget is computed, not guessed: a fully-argumented job with `RunAtLoad` is
**549 bytes**; dropping `KeepAlive` gives **522**, which fits pilotfish's 530
with 8 bytes spare. A minimal job is 383 and fits all four. So the precise
requirement is **one file rewritten in place at exactly 530 bytes**, roughly 40
lines in `rootfs_work.c` — a new constant and a length gate, exactly as the
SpringBoard plist rewrite already does.

XML beats binary here because the `<array>` can be rewritten freely and
whitespace-padded back to the identical byte count. `com.apple.wifiFirmwareLoader.plist`
is **binary** and only 170 B, whose single 31-byte argv string leaves room for
`/usr/sbin/pppd` and nothing else — not enough for a device or options, so it
only works paired with an `/etc/ppp/options` file we also cannot create.

**Verified `pppd` facts that make the standalone invocation viable.** It is Apple
pppd 2.4.2, armv6, at `/usr/sbin/pppd` (284,608 bytes). A missing
`/etc/ppp/options` is **non-fatal** — it warns and runs on built-in defaults plus
argv. It daemonises by default, so `nodetach` is required under launchd. It uses
modem control by default and will watch for carrier, so an emulated UART with no
DCD needs `local` and probably `nocrtscts`. Peer authentication is not required
at first bring-up (2.4.2 only sets `auth_required` when a default route already
exists), but pass `noauth` for determinism — no secrets ship. So:

```
/usr/sbin/pppd <dev> <speed> local nocrtscts noauth nodetach
```

The Apple `PPPController`/`serviceid` path exists but is optional; classic
standalone invocation works.

**What the host-side peer must tolerate:** pppd sends CCP (0x80FD) by default
with bsdcomp and deflate enabled, so the peer must Protocol-Reject it. IPV6CP,
Apple ACSP and ECP protents are also compiled in — Protocol-Reject any unknown
NCP rather than assuming the set. VJ compression is on by default (16 slots);
`novj` is optional. First LCP Config-Req carries MRU 1500, ASYNCMAP 0x00000000,
a random magic number, PCOMP and ACCOMP, with no auth option and echo off.

**Observability is the real constraint.** The image has **no shell at all** — no
`sh`, no `bash`, no coreutils, no `getty`, and `/bin` contains exactly
`launchctl`. Also absent: `ifconfig`, `ping`, `netstat`, `route`, `curl`,
`tcpdump`, `nc`, `telnet`. Present and usable: `scutil`, `scselect`,
`ipconfig`, `configd`, `bootpd`, plus MobileSafari. Any milestone phrased as
"ping succeeds" has to be rephrased against what actually ships.

Two saving graces. `/etc/resolv.conf` is a 20-byte indirection to
`/var/run/resolv.conf`, which `usepeerdns` writes — **DNS needs no provisioning
at all**. And we are the peer, so the first observable is on the *host* side and
needs no PPP implementation: **`7E FF 7D 23 C0 21` in the UART TX capture**, an
HDLC-framed LCP Configure-Request, byte-checkable against RFC 1662. That one
hex dump proves the DMA skip, driver bring-up, the devfs node name, the plist
hijack, launchd starting the job, AMFI accepting Apple-signed `pppd`, dyld
loading its frameworks, the tty opening, and the line discipline attaching —
nine unknowns at once.

### 23.5.1 DMA is optional. The answer is a boot argument, not a DMA model.

The open question was whether `AppleS5L8900XSerial` insists on PL080 DMA, since
every boot log brackets it with `AppleARMPL080DMAC::_initDMAChannel` and the
PL080 is unmodelled. **Verified by disassembly: it does programmed I/O, and DMA
setup is gated three ways at `0xc065e410`**, all three skips landing on the same
non-error continuation at `0xc065e6f4` — build the interrupt event source, call
the superclass, no log, no failure:

1. no `dma-types` property on the nub → skip
2. `dma-disable` property present → skip
3. boot argument **`<node>_dma_enable=0`** → skip

The base class agrees: `AppleOnboardSerial`'s vtable supplies a **default DMA
capability of zero** (`movs r0,#0 / bx lr` at `0xc046f154`), queried once at
start and cached; all three consumers treat it as a guard. The PIO receive loop
is real and reads `URXH` in a counted loop at `0xc047212a`.

All four UARTs *do* carry `dma-types`, so the driver will attempt DMA unless
told otherwise — which makes **`uart4_dma_enable=0` in the `-c` string the
entire fix, zero code.** That mechanism is already proven here: `nand-enable-adm=0`
works the same way.

**Use uart4, not uart3** — this contradicts `docs/networking.md` §6, on two
grounds that section did not have. uart3 is the only UART **without**
`no-flow-control`, so the driver enables hardware flow control and reads UMSTAT
for CTS, whereas uart1/uart4 short-circuit `getFlowStatus` to asserted without
touching the register (`0xc065e0bc`). And uart3's child is `bluetooth`, with
`BTServer` (1.1 MB) and its launchd plist both shipping — a live contender for
the port. Nothing owns uart4's `debug` child, and `/etc/ttys`'s getty lines are
inert because getty does not ship.

| node | phys | VIC | child |
|---|---|---|---|
| uart0 | 0x3CC00000 | 24 | `iap` — taken, `boot-console` |
| uart1 | 0x3CC04000 | 25 | `umts` |
| uart3 | 0x3CC0C000 | 27 | `bluetooth` — contended |
| **uart4** | **0x3CC10000** | **28** | `debug` — free |

Register semantics read out of Apple's binary rather than guessed: **UFSTAT
(+0x18)** bits[3:0] RX count, bit8 RX full, bits[7:4] TX count, bit9 TX full,
**FIFO depth 16**; **+0x10 is not a read-only status register** — the interrupt
filter at `0xc065eed8` reads it, masks, and **writes the result back, so it is
write-1-to-clear**.

**Threading caveat that must not be got wrong.** Existing wake sources answer
"how many ticks until my next edge", which a host-delivered byte cannot. The
honest shape is `S5L_WAKE_NEVER` when the RX FIFO is empty and an immediate edge
when it is not — safe because the timer always bounds the sleep. And the
host→guest handoff must happen **on the CPU thread between run slices**, never
from a socket callback: `core/` has no threading vocabulary and a data race
there would be the worst bug this project could acquire.

**Named risk.** `setBaud` (`0xc065ea4c`) divides by a 64-bit `nclk` rate read
from the platform at `0xc065e44c`. `pppd` calls `cfsetspeed`, exercising that
path for the first time — today's boots only *identify* the ports. If our
unmodelled clock tree returns 0 there, expect a divide-by-zero rather than a
graceful message.

Requirements discovered from the acquired payload, each of which is a silent
breakage if missed: **setuid/setgid bits must survive** (`MobileCydia` 6755,
`bin/su` 4555, `var/local` 2775 gid=50); **89 symlinks** including `/etc`,
`/var`, `/tmp` into `private/`; and the volume ships `freeBlocks = 0`, so
`grow_volume` must run first.

### 23.6 The payload, already acquired and verified

In `work/payload/` (gitignored, never commit it). Every Mach-O in all sets is
`cputype 12` subtype 0 or 6 — **zero armv7**, censused from headers by two
independent code paths.

Use `gala-Cydia.tar`'s skeleton (555 files, seeded dpkg database, apt keys) but
substitute `cydia_1.0.3044-66` from `debs-3.1.3/`: the tarball's Cydia is 1.1.8
from 2012 and links the iOS 5 SDK, while 1.0.3044-66 links exactly the 3.1 SDK
that 7E18 provides. Both Cydia binaries are setuid root with
`LC_CODE_SIGNATURE`, so this depends on the code-signing disable in §23.7.

### 23.7 Code signing: armed, result inconclusive

`docs/activation.md` §A.4 confirmed the switch from disassembly:
`/chosen/debug-enabled = 1` plus boot-args `cs_enforcement_disable=1
amfi_get_out_of_my_way=1 amfi_allow_any_signature=1`. run60 applied it — both
confirmed in the run header — and booted clean to 800e6.

**The result is inconclusive and must not be read as failure.**
`_cs_validate_page` still fires 1,705 times, but that symbol is the kernel's
page-hashing primitive, not AMFI's policy gate; and the counts are not
comparable to run59's 5,690 because run59 ran 5e9 instructions to run60's 800e6.
Deciding it needs either an unsigned binary to exec (blocked on §23.5) or a
`--call-probe-kernel` on AMFI's `vnode_check_signature` return.

### 23.9 Audio: the nub is published, so the first milestone is days not weeks

Today's failure is one line — `AppleWM8991Audio: I2C register read failed (0):
device error` — and everything above it already works: both I²S controllers
start, `mediaserverd` is spawned, `IOAudio2Family` is loaded at 0.0% residency.
The codec is a Wolfson **WM8991** on **i2c0** (`0x3C600000`), slave address
**0x1B**: write an index byte, read 2 bytes MSB-first, and **register 0 must
return `0x8990`** (literal at `c068b124`); then R1 bit 5 must be writable and
read back. Nothing else is validated. `core/src/soc/pcf50635.c` (172 lines) is
the working precedent for an I²C slave, and `S5L_I2C_SLAVES = 4` has room with
no header change.

**run62 settled the question that branched the plan.** `--call-probe-kernel` on
`AppleS5L8900XI2SController::start`'s tail: `0xc05a3f40` (publishChildren) and
`0xc05a3f44` (return true) each **captured 2**, `0xc05a3f4c` (return false)
captured **0**. Both controllers publish, at instructions 235,126,205 and
236,694,444. So the `AppleARMIISDevice` nub named `audio0` **exists today**, the
two `IODMAEventSource`s are created successfully, and the three NULL-timeout
`waitForService` calls ahead of it already resolve.

Consequence: **the PL080 can be deferred.** Steps 1-2 — the codec slave plus the
I²S window — are 2-3 days and produce a fully attached codec with a live
`IOAudio2Family`. Samples need the PL080 later, and there is **no boot-argument
escape** for it: all 40 `PE_parse_boot_argn` sites were enumerated and the
`<node>_dma_enable` pattern exists only in `AppleS5L8900XSerial`.

The I²S window itself is nearly free. The driver funnels access through two
accessors and **`readRegister` (`c05a3c84`) has no caller anywhere in the
kernelcache** — it writes seven offsets (`0x00, 0x04, 0x08, 0x30, 0x34, 0x3C,
0x40`) and never reads. The FIFOs at `+0x10`/`+0x38` are touched only by the
PL080 as physical addresses in an LLI. That independently explains the zero
MMIO traffic the census shows on `0x3CA00000`/`0x3CD00000`, and makes the window
80-120 lines rather than 250-350.

> **Do not land the codec alone.** Today's failure is loud, correct and
> harmless, and the boot continues past it. There are **five unbounded waits**
> on the audio path — three NULL-timeout `waitForService` in series plus two
> uninterruptible `IOLockSleep`s in the transfer path — so a codec that answers
> `0x8990` with no I²S window behind it converts a good failure into a **silent,
> uninterruptible hang**. That is a regression, not progress. Steps 1 and 2 are
> one unit.

Two things to keep honest about scope. **Nothing makes a sound today** and
nothing would even with a perfect stack: the activation screen is silent and
iPhone OS 3 has no boot chime, so audio output is downstream of touch exactly as
Wi-Fi is. And **you cannot hear it live** — at 200-294× slower than the 412 MHz
part, the smallest UI sound costs about 6 seconds of wall clock and one guest
second costs about 200. The right design is to clock the model off the guest
timebase, capture PCM to a WAV, and let the host play it back afterwards.

One method note worth carrying. Before run62, the evidence for the nub was that
the device tree declares 14 DMA channels while only 10 initialise, a shortfall
of exactly 4 matching i2s0's 2 plus i2s1's 2. That fit was suggestive and
**wrong** — other partitions exist, the analysis labelled it cannot-determine
rather than concluding from the coincidence, and a four-minute read-only run
settled it properly. The tidy arithmetic would have sent the plan down the
expensive branch.

### 23.8 The trap that has now cost three retractions

A per-process trace block that reports one generation does not describe the
whole run, and **host-side counters are not machine state**: `core/include/snapshot.h`
says a restored process starts them fresh, so a zero in a restored run means
"not seen since the restore point", never "never happened". On 2026-07-26 that
misreading produced a committed claim that `AppleH1CLCD::createSurface` never
ran, when it runs and succeeds at instruction ~238,400,000.

**Compare the heartbeat pc stream before reading divergence into differing
counters.** Restore is bit-exact — 20/20 heartbeats identical between cold run52
and restored run56 across 3.0e9-4.9e9, agreeing 862 million instructions past
the restore point, across three different binaries.

### 23.10 Networking step S0: uart4, the plist hijack, and what a run must carry

The first half of `docs/networking.md`'s Route D is implemented. This section
records what was built, the three things it corrected in §23.5/§23.5.1, and the
exact command that reproduces it — because two of the corrections would
otherwise send the next reader down a path that does not exist.

**This is a temporary workaround and the code says so in three places** (the
`--ppp` help text, `rootfs_work.h`'s option comment, and `PPP_PLIST_STOCK`'s
provenance block). PPP over an emulated UART is what is cheap today because
both halves already ship. Real drivers and controllers replace it.

#### What was built

- **`uart4` is a decoded device window**, 0x3cc10000, transmit-only, a second
  `s5l_uart_t` next to `uart0`. `SNAPSHOT_VERSION` 9 -> 10.
- **`--ppp`** rewrites `com.apple.chud.pilotfish.plist` in place, at exactly
  its own 530 bytes, into a `RunAtLoad` job running
  `/usr/sbin/pppd /dev/uart.debug local nocrtscts nodetach` with
  `StandardErrorPath = /dev/console`, and appends `uart4_dma_enable=0` to the
  boot arguments.
- **`uart4-ppp.bin`**, a per-run binary tee, plus an automatic scan for
  `7E FF 7D 23 C0 21` reported in a `=== UART4 / PPP ===` section on every
  armed run.

#### Three corrections to §23.5 and §23.5.1

**1. The `setBaud` "named risk" is not a boot-killer, and the reasoning behind
it was inverted.** §23.5.1 warned that `pppd`'s `cfsetspeed` would exercise
`setBaud` (`0xc065ea4c`) "for the first time" and that a zero `nclk` would give
"a divide-by-zero rather than a graceful message". Both halves are wrong.

- `setBaud` is **vtable slot +0x374** and has **zero direct `bl` call sites**.
  `AppleOnboardSerial` reaches it from `start()` through `programHardware`,
  after storing a default of **19200 8N1** at `0xc047244a` — the only baud
  constant in either kext, and there is no device-tree baud property anywhere.
  So it runs **unconditionally at start, before any tty is opened**, and
  `pppd`'s speed argument adds nothing that was not already going to happen.
- The divide is `__udivdi3(nclk << 17, baud)` then `__udivsi3(q, 2*framebits)`,
  using helpers **compiled into the kext**, and the kext's own `__aeabi_idiv0`
  at `0xc065f55c` is a bare **`bx lr`**. A zero divisor returns garbage and
  **does not trap**. The consequence is a nonsense `UBRDIV`, not a panic — and
  this VM derives no baud rate from `UBRDIV`, so it is not even observable
  here.

  `nclk` itself is **not** a device-tree property and **not** an MMIO read by
  this driver: `0xc065e44c` loads the C string `"nclk"` at `0xc065fb40` and
  makes a virtual call on the provider nub (slot **+0x358**), which forwards up
  the provider chain (parent slot **+0x354**) into AppleARMPlatform's clock
  layer. The string `nclk` occurs **zero times** in `devicetree.bin`. Nobody
  has walked that chain to a register; do not claim one.

**2. `AppleS5L8900XSerial` is ARM, not Thumb.** `tools/kdisasm.py` defaults to
Thumb, so every address in §23.5.1 disassembles as garbage without `--arm`.
`AppleOnboardSerial` *is* Thumb-1. Exact ranges from `__PRELINK_INFO`:
`AppleOnboardSerial` `0xc046e000..0xc0479000`, `AppleS5L8900XSerial`
`0xc065d000..0xc0662000`.

**3. The window was undeclared, not merely unmodelled.** run59's census
recorded `0x3cc10000 r=8 w=15` falling through to the **unmapped** path, so
every `UTRSTAT` read answered 0 — "transmitter busy". A driver that waits for
room before storing would have waited forever. Decoding the window is what
makes a transmit path terminate at all; it is not a tidiness change.

Confirmed unchanged from §23.5.1, now read out of the tree rather than
inferred: uart4 carries `dma-types {3}`, a zero-length `no-flow-control`,
`interrupts {0x1c}`, `reg {0x04c10000, 0x1000}`, and — unlike uart0 — **no
`boot-console`**, so nothing contends for it. Its `debug` child has exactly two
properties, `name` and `AAPL,phandle`, and nothing else.

#### The plist, byte-exactly

Stock `com.apple.chud.pilotfish.plist` lives at **offset 0xf5f000** in
`firmware/rootfs.img`, is **530 bytes**, and the full pattern occurs **exactly
once** in the whole 413 MiB image (verified by a full scan, not assumed).
SHA-256 of those 530 bytes:
`882ebd0b14088120b03750090ef9b6885a7b3bfbbe286df9ac23ecb431f55312`.

Its DOCTYPE says **"Apple Inc."** where the SpringBoard plist's says **"Apple"**
— two different Apple toolchains, and normalising either breaks the match.

The budget arithmetic in §23.5 is confirmed and its candidate ranking stands:
a fully-argumented job is **515 bytes**, so pilotfish's 530 is the only one of
the four inert candidates that fits. `chud.chum` at 515 would fit with **zero**
bytes of slack, which is not a margin.

#### What a run must carry, and one trap that costs the whole run

**`nand-enable-adm=0` is mandatory.** Without it the boot panics at
**instruction 218,615,894** in `AppleS5L8900XADMFMC::start` —
`"ADM startup failed"`, called from `0xc04d679c`. This is not new and is not
related to PPP: **run72 died there too**, and so does a clean build of
`55ebb98` with no PPP code in it at all. It is reproducible to the exact
instruction across three different binaries. run71 has it in `-c` and never
reaches the ADM path.

Also note `-F` invalidates **`firmware/screen.ppm` relative to the process
working directory**, so a run launched with its own working directory needs a
`firmware/` subdirectory containing that file or it exits 1 before booting.

Reproduction, from PowerShell (**not** Git Bash — §23.4a explains why
`--fstab` dies there); written as one line per argument so it can be pasted
without continuation characters:

    $r = "F:\JOSHUA_1st_2021\projects\iOS3-VM"
    $d = "$r\work\run74-ppp"
    New-Item -ItemType Directory -Force "$d\firmware" | Out-Null
    Copy-Item "$r\firmware\screen.ppm" "$d\firmware\screen.ppm" -Force
    Push-Location $d
    & "$r\work\build-ppp\core\bootkernel.exe" "$r\firmware\kernel.macho" -d "$r\firmware\devicetree.bin" -F --usb-otg --ca-software-render --ppp -R 128 --grow 32 --external-md "$r\firmware\rootfs.img" "$d\rootfs-run74.img" --fstab "/dev/md0 / hfs rw,update 0 1" -c "debug=0x8 serial=1 nand-enable-adm=0" -n 850000000 1>"$d\run74.stdout.log" 2>"$d\run74.stderr.log"
    Pop-Location

**A cap of 850e6 is enough and 1.2e9 is waste.** `pppd` is spawned at
557,124,470 and, as run74 measured, is dead by 739,184,188. Everything S0 can
observe has happened by then; 850e6 is about twelve minutes of wall clock
against roughly eighteen for 1.2e9. Only raise it once `pppd` stops exiting.

#### Measured, run73 (700e6 cap)

The pipeline is proven as far as the exec. In order:

| what | evidence |
|---|---|
| plist rewritten in the work image | `ppp : com.apple.chud.pilotfish.plist @ image+0x00f5f000` |
| boot argument appended | `cmdline "debug=0x8 serial=1 nand-enable-adm=0 uart4_dma_enable=0 rd=md0"` |
| window decoded, driver bound | `AppleS5L8900XSerial: Identified Serial Port on ARM Device=uart4 at 0x3cc10000(0xea9d6000)` |
| **launchd spawned our job** | `syscall 244 posix_spawn ... path "/usr/sbin/pppd"` **at instruction 557,124,470** |
| bytes on uart4 | **none by 700e6** |

That `posix_spawn` is the load-bearing observation: it proves the hijack
survived into the work image, that launchd parsed the rewritten plist, and that
`RunAtLoad` fired. run73 capped only 143e6 instructions later, which is not
enough for dyld to map a 284,608-byte binary and its frameworks, open the tty
and transmit.

#### Measured, run74 (1.2e9 cap) — THE BLOCKER, AND WHAT IT IS NOT

**`pppd` runs, and then it calls `exit(1)`.** From the process-lifecycle
section:

```text
#65  @557124470  syscall 244 posix_spawn   path "/usr/sbin/pppd"
                 task/task-proc/pid c2e151d8/e03832cc/19
#84  @739184188  syscall 1   exit          args a0=00000001
                 task/task-proc/pid c2e151d8/e03832cc/19
#85  @739184282  _exit1 proc=e03832cc rv/status=00000100
```

So pid 19 lived for **182,059,718 instructions** — it was not a failed exec,
dyld mapped it and it ran — and then exited **1**. Zero bytes ever reached
uart4, across the whole 1.2e9.

**The exit code is itself evidence, and it rules out four things.** pppd 2.4.2
has distinct exit codes, and ONE is `EXIT_FATAL_ERROR`. It is therefore:

| not | which would mean |
|---|---|
| `EXIT_OPTION_ERROR` = 2 | the command line failed to parse |
| `EXIT_NOT_ROOT` = 3 | the job ran unprivileged |
| `EXIT_NO_KERNEL_SUPPORT` = 4 | `ppp_available()` said no — the line discipline is missing |
| `EXIT_OPEN_FAILED` = 7 | `/dev/uart.debug` could not be opened |

**So the devfs node exists under the predicted name, the argv parsed, the job
ran as root, and `com.apple.nke.ppp` answered.** Four of the nine unknowns S0
was supposed to settle are settled, by an exit code. What remains is a
`fatal()` call, and the two candidates the strings support are
`"Couldn't set tty to PPP discipline: %m"` (`TIOCSETD` with `PPPDISC`) and
`"Baud rate for %s is 0; need explicit baud rate"` — both confirmed present in
the image, once each.

**Why that could not be narrowed further in run74, and the fix.** `fatal()`
writes to stderr, and launchd gives a job with no `StandardErrorPath`
`/dev/null`. The message was destroyed. The job now carries
`StandardErrorPath = /dev/console`, so it lands in the same console capture
every other guest message does; the key is confirmed present in the image
rather than assumed supported. Two arguments were spent to afford the 61 bytes
— see `PPP_PLIST_JOB`'s comment for the trade, which is stated there rather
than made quietly, and note that dropping the explicit speed makes the
*second* candidate above self-diagnosing rather than hiding it.

#### Reading the verdict without fooling yourself

`0x7E` is a byte, not a proof. The section reports three distinct outcomes and
they are not interchangeable:

- **`NOTHING was written to uart4`** — the guest never transmitted. This says
  nothing about *which* step failed.
- **`NOT the milestone`** — something opened the port and wrote to it. Compare
  the dump against RFC 1662 before concluding anything about `pppd`.
- **`*** MILESTONE ***`** — the exact six bytes, with the stream offset **and**
  the instruction they landed at. Both are printed because "it appeared" and
  "it appeared at a plausible point in the boot" are different claims.

The scan uses a six-byte sliding window rather than a match counter, because
`0x7E` starts every frame and a counter would miss `7E 7E FF 7D 23 C0 21` —
an idle flag followed by a frame, which is exactly what a real line looks like.

#### Measured, run75 (850e6 cap) — the arguments are not the cause, and
#### `StandardErrorPath` did not deliver the message

Same configuration, one change: `StandardErrorPath = /dev/console` added,
`115200` and `noauth` removed to pay for it. Two results, and the second is
more useful than the one that was being chased.

**1. The failure is invariant to those arguments.** Side by side:

| | spawn | exit(1) | `pppd` lifetime |
|---|---|---|---|
| run74 (`115200`, `noauth`) | 557,124,470 | 739,184,188 | 182,059,718 |
| run75 (neither) | 557,135,323 | 739,143,287 | 182,007,964 |

**51,754 instructions of difference out of 182 million — 0.028%.** Removing an
argument shortens option parsing by about that much and changes nothing else.
So `pppd` is failing at the *same place* with or without an explicit baud rate,
which retires the `"Baud rate for %s is 0"` hypothesis: that fatal would fire
only in the second row and would have moved the exit. Do not spend the 25
bytes putting the speed back on the strength of that theory — it has been
tested.

**2. `pppd` printed nothing, anywhere.** The console is byte-identical to
run74's across all 5,371 bytes both runs produced. So either launchd on 3.1.3
does not honour `StandardErrorPath`, or `pppd` never gets far enough to have a
usable stderr, or its `fatal()` on this build logs only through syslog. This
was worth trying — it is one plist key and the alternative was guessing — but
it did not work, and the honest reading is that **`pppd`'s message is not
reachable this way** rather than that `pppd` had nothing to say.

#### The next step, concretely: probe the exit site

The lifecycle record carries a lead that costs one flag to follow. At the exit
syscall, register **r3 = `0x00039c30`**:

```text
#84  @739143287  syscall 1 exit   args a0=00000001 a1=0000000c a2=2ffffeec a3=00039c30
                 user pc 33ad7138 (ARM, spsr 00000010)
```

`a0 = 1` is the status. `user pc 33ad7138` is inside the dyld shared cache —
libSystem's `exit` thunk, i.e. every caller looks the same there and it is
useless as an identifier. But `0x00039c30` is in the **pppd image's own**
address range (the binary is 284,608 = `0x457C0` bytes and loads low), so it is
a `pppd` text address and almost certainly the neighbourhood of the `fatal()`
or `die()` that called `exit`.

So: **`--call-probe 0x00039c30`**, which captures `pc/lr/sp`, `r0`-`r3` and two
stack words at a user-mode PC. `lr` from that capture names `pppd`'s own caller
and turns "a fatal() somewhere" into an address to disassemble. Pair it with
`--call-probe` on a second candidate if the first is a thunk. A cap of 850e6 is
enough, so this is about twelve minutes per iteration.

The static side is cheap too and has not been done: `pppd` is at
`/usr/sbin/pppd` in the image and can be extracted and disassembled directly
(`tools/kdisasm.py --arm`), so `0x39c30` can be resolved to a function and its
string references read before any run is spent. Both `"Couldn't set tty to PPP
discipline: %m"` and `"Baud rate for %s is 0; need explicit baud rate"` are
present in the image exactly once, so their addresses are findable and can be
compared against whatever `0x39c30` turns out to be near.

What is already settled and should not be re-litigated: the plist hijack, the
boot argument, the devfs node name, launchd starting the job, AMFI accepting
the binary, dyld loading it, and `com.apple.nke.ppp` answering — the first six
of the nine unknowns S0 was posed to settle.

#### 23.10a `pppd` disassembled: the blocker is `tty_establish_ppp`, and
#### `0x39c30` is not a string

The static work proposed above was done, and it answers the question without a
run. `/usr/sbin/pppd` extracted from `work/run75-ppp-log/rootfs-run75.img` with
`tools/hfsx_extract.py` is 284,608 bytes, `MH_MAGIC` `cputype 12 cpusubtype 6`
(armv6), **not stripped** — 576 defined symbols, and **zero** carry
`N_ARM_THUMB_DEF`, so the whole `__text` decodes as ARM. That symbol table is
what makes everything below cheap; disassemble with it, not without it.

**`0x00039c30` is `fd_ppp`, not a message.** `__cstring` ends at `0x00035d9e`;
`0x39c30` is in `__DATA,__data`, its file image is `ffffffff` (`= -1`), and the
whole binary references it in exactly two places:

- `_die+0x10` (`0x00013a1c`) — `if (fd_ppp >= 0) the_channel->disestablish_ppp(devfd)`
- `_main+0xc6c` (`0x00015684`) — `ldr sl, [pc, #0x45c]`, held across the store
  at `_main+0xca8`: `fd_ppp = the_channel->establish_ppp(devfd)`

That is `fd_ppp`'s definition, use and initialiser exactly. The lead was worth
following and it did not lead where §23.10's last block guessed.

**The exit is `die(EXIT_FATAL_ERROR)`.** `_fatal` (`0x00023ad0`) ends in
`mov r0,#1; bl _die`, and `_die` (`0x00013a0c`) ends in `mov r0,r5; bl _exit`.
Every `exit(1)` in the image is either that or `_load_kext+0x6c`, and
`load_kext` `fork()`s first — **pid 19 never forked** (the run75 lifecycle ring
retained 92 of 92 events, so it is complete), so `load_kext` never ran. Two
things follow immediately: `ppp_available()`'s
`socket(PF_PPP /*34*/, SOCK_RAW, PPPPROTO_CTL)` at `_ppp_available+0x24`
**succeeded** — it only forks when that fails — so `com.apple.nke.ppp` really
does answer; and no connector/initializer/welcomer script ran either.

**`status = 1` has exactly one reachable writer.** Scanning every reference to
`_status` (`0x00040418`, reached through the non-lazy pointer at `0x000373c8`)
finds 36 stores. Only two store `1`:

- `_connect_tty+0x9c` (`0x000207fc`), guarded by `using_pty || record_file` —
  unreachable with this argv, which names a real device
- `_main+0xcb0` (`0x000156c8`), immediately after
  `fd_ppp = the_channel->establish_ppp(devfd); if (fd_ppp < 0)`

`the_channel` is `tty_channel` at `0x0003ae2c` — `+0xc = _connect_tty`,
`+0x14 = _tty_establish_ppp`, `+0x24 = _cleanup_tty`, the standard `struct
channel` layout. **So `_tty_establish_ppp` (`0x0001c528`) returned negative.**

#### A per-process execution trace, from page faults, at zero run cost

`=== DISTINCT ABORT SITES ===` is a symbolisable trace of userspace, and this
had not been used. Each record carries `L1=`, the L1 descriptor for the faulting
VA — a *per-address-space* value, which is what separates one process's
low-address faults from another's. `pppd`'s is `0x0b139001` in both run74 and
run75 (identified by the `IFETCH FAR 0x00002334` right after the spawn:
`0x2334` is `start`, pppd's entry point). Filtering on it and symbolising
against the extracted binary gives 98 records for run75. The tail:

```text
IFETCH @731392297  0x000040b4  _auth_check_options
IFETCH @731778474  0x0001ce7c  _sys_init
IFETCH @735408996  0x000294ac  _acsp_init_plugins
IFETCH @735607256  0x00020760  _connect_tty
IFETCH @735794554  0x0001bf80  _set_up_tty
IFETCH @738641962  0x0001a6ec  _sys_cleanup      <- first call inside die()
       @739143287  exit(1)
```

run74 is the same sequence, offset by ~37k instructions. Read it carefully: a
missing entry proves nothing (`_tty_establish_ppp` at `0x1c528` shares page
`0x1c000` with `_sys_init`, already resident since 731.8M), but a *present*
entry is proof the function was entered. So `sys_init` ran to completion — none
of its six `fatal()`s fired, including `"Couldn't open PF_PPP: %m"` and
`"SCDynamicStoreCreate failed: %s"` — and `connect_tty` reached `set_up_tty`.

**Reuse this.** `L1=` plus an unstripped guest binary turns the abort table into
a function-entry trace for any process, retroactively, on logs already on disk.

#### Both of `set_up_tty`'s `fatal()`s are excluded

`_set_up_tty` (`0x0001bf80`) has two, and one non-fatal early return that has
been mistaken for one:

| site | call | condition |
|---|---|---|
| `0x0001bfd0` | `error("tcgetattr: %m")` | **returns**, does not die |
| `0x0001c12c` | `fatal("Baud rate for %s is 0…")` | `inspeed == 0 && cfgetospeed() == 0` |
| `0x0001c164` | `fatal("tcsetattr: %m")` | `tcsetattr(fd, TCSAFLUSH, &tios) < 0` |

The baud fatal is excluded by construction, not by the 0.028% argument above:
run74's argv carries `115200`, so `inspeed != 0`, so the branch at `0x0001c0ec`
takes `cfsetospeed`/`cfsetispeed` and the fatal is *unreachable in that run* —
and run74 still exits 1, at the same place, with the same `r3`. run75 takes the
same path: between `_set_up_tty` and `exit` the two runs differ by **3,905
instructions out of 3.35 million (0.12%)**, which is not enough to contain a
skipped `tcsetattr` and everything after it.

And `tcsetattr` ran. The uart4 register census is the witness:

| run | cap | `0x3cc10000` | reached |
|---|---|---|---|
| run73 | 700e6 | `r=8 w=15` | died at the cap **before** `connect_tty` (735.6M) |
| run74 | 850e6 | `r=41 w=100` | `set_up_tty` |
| run75 | 850e6 | `r=41 w=100` | `set_up_tty` |

`r=8 w=15` is the "identified, never opened" baseline — uart1 reads exactly
`r=8 w=15` and uart3 `r=10 w=17` in every run. The +33/+85 in run74/run75 is the
`open()` and the termios programming, and it is **byte-identical** between a run
that asked for 115200 and one that asked for whatever the port reported. The
port was opened and programmed; `set_up_tty` returned.

#### The message, and the honest limit of what the binary can say

`_tty_establish_ppp` returns `-1` from five places. All five call `error()`, not
`fatal()` — which is why exit code 1 arrives with no `fatal()` anywhere, and why
chasing `fatal()` call sites was the wrong search. In execution order:

| # | string VA | message | guard |
|---|---|---|---|
| 1 | `0x00032a34` | `Couldn't set tty to PPP discipline: %m` | `ioctl(tty_fd, TIOCSETD 0x8004741b, &disc)`, `disc = PPPDISC = 5` (14 if `sync_serial`), failing with `errno != EIO` |
| 2 | `0x0003296c` | `Couldn't get link number: %m` | `ioctl(tty_fd, PPPIOCGCHAN 0x40047437, &chindex)` |
| 3 | `0x0003299c` | `Couldn't reopen PF_PPP: %m` | `socket(PF_PPP,SOCK_RAW,PPPPROTO_CTL)` + `connect()` in the helper at `0x0001c2b0` |
| 4 | `0x000329b8` | `Couldn't attach to the ppp link %d: %m` | `ioctl(sock, PPPIOCATTCHAN 0x80047438, &chindex)` |
| 5 | `0x00032a10` | `Couldn't attach to PPP unit %d: %m` | `ioctl(fd_ppp, PPPIOCCONNECT 0x8004743a, &ifunit)` |

Each string occurs exactly once in the image. **#3 is excluded** — the identical
`socket(PF_PPP,…)` already succeeded twice (in `ppp_available`, proved by the
absent fork, and in `sys_init`, proved by `sys_init` returning). **#1 is the
first and the one every later step presupposes**, and is the single most likely
answer. But say plainly what the binary cannot: it cannot distinguish #1 from
#2/#4/#5, because all four are `error()` calls on already-resident pages and
leave no trace the run logs captured.

#### What must hold, named exactly — and it is not a device model

Nothing here is a missing register, a device-tree property or a file. The four
operations that must succeed on the fd for `/dev/uart.debug` are:

```text
TIOCSETD        0x8004741b   _IOW('t',27,int)   value 5 (PPPDISC)
PPPIOCGCHAN     0x40047437   _IOR('t',55,int)
PPPIOCATTCHAN   0x80047438   _IOW('t',56,int)
PPPIOCCONNECT   0x8004743a   _IOW('t',58,int)
```

All four are served by `com.apple.nke.ppp`, and that kext is present and armed
in this exact kernelcache: `pppserial_ioctl: … TIOCSETD` / `… PPPIOCGCHAN = %d`
are at `0x005583fc`+ in `firmware/kernel.macho`, and the **only** reference to
`&linesw[PPPDISC]` (`0xc0221b4c`) in the entire 7.9 MB image is the literal at
`0xc05925c0`, loaded by an eight-word `ldm/stm` structure copy at
`0xc0592554`-`0xc0592568` — i.e. `linesw[PPPDISC] = pppdisc`, by direct
assignment rather than `ldisc_register` (which has zero call sites anywhere).
`nlinesw` is 8, so `PPPDISC = 5` also passes `ttioctl`'s range check.

**This is a small change or none at all, and that is the finding.** There is no
new emulated device between here and the LCP frame. The blocker is one of four
ioctls inside a kext that is loaded and whose sibling entry point (the PF_PPP
socket) already works — so the fix is either a one-line device-tree/`ioctl`
detail or nothing, and the remaining cost is *identification*, not construction.
Do not size S0's remainder as a driver.

#### Why run75's console was empty, and the one-key fix

`StandardErrorPath` was aimed at a file descriptor `pppd` never writes to.
`error()`/`fatal()` funnel into the formatter at `0x000230d0`, which calls the
emitter at `0x0002245c`, and that function does exactly two things: `syslog()`
unconditionally, then — if `*log_to_fd >= 0` — a timestamp and the message with
`write(*log_to_fd, …)`. `_log_to_fd` is at `0x00039c70` and its file image is
**`00000001`, i.e. stdout**, and `nodetach` means nothing lowers it. fd 2 is
never touched.

So the next run's plist needs **`StandardOutPath`**, not `StandardErrorPath` —
one key, same 530-byte budget, and it puts the exact string from the table above
on the console and collapses the four-way ambiguity to one. That is the whole
measurement: *which of those four `error()` strings `pppd` prints*. Nothing else
needs to change, and no probe or new flag is required.

#### Step S1, when someone takes it

S0 is transmit-only *by design*, and the header comment in `soc.h` says what
changes when that stops being true. The receive direction needs, in this order:
a bounded host-to-guest queue; `UFSTAT`'s receive count and `UTRSTAT` bit 0
becoming real; `UTRSTAT` growing a genuine write-one-to-clear latch; VIC line
**28** becoming a wired `S5L8900_IRQ_UART4` and a wake-source entry; and the
host-to-guest handoff happening **on the CPU thread between run slices** —
§23.5.1 is right that a socket-callback race there would be the worst bug this
project could acquire. `core/tests/test_uart4.c`'s
`test_uart4_raises_no_interrupt_line` is the test that must be rewritten rather
than deleted at that point.

Only after that does the host-side LCP/IPCP peer (RFC 1661/1332/1662) become
worth writing, and `pppd` will Protocol-Reject-test us first: it sends CCP
(0x80FD) by default with bsdcomp and deflate, and IPV6CP, Apple ACSP and ECP
protents are all compiled in.
