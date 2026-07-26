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
path and host injection; (5) payload format, the only genuine unknown, reversed
from `_MT_ParsedMultitouchFrameRepCreate` (cache VA `0x33cf99c0`).

**`wake_line_enabled()` rejects any line ≥ 32×VIC_COUNT.** A wake-source entry
written as `{ "multitouch", 155, … }` returns false silently and can never wake
the CPU. The entry must be the cascade VIC line.

Two downstream kill-switches that will look like touch bugs and are not:
SpringBoard VA `0x00041268` drops every digitizer event when the byte at data VA
`0x001076b8` is non-zero, and run59 already logs 29 consecutive `IOSurface
warning: buffer allocation failed. 320 x 480`, which will bite the moment a tap
launches an app.

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
