# Quality and validation

This file records what was actually checked for the post-run18 TV-out,
framebuffer-planning, CLCD-bounds, and boot-diagnostic change at commit
`afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9`, plus run19's real-firmware
verdict, the surgical timing-predicate correction at exact commit
`590d2248af4d7e5e92ec7bbd1be079c3bb415542`, run20's firmware verdict, and the
VFP11 decoder correction and run21 firmware replay at exact commit
`debec04ff9b0faa469d5ad2ee7d75d1bf3b53b1a`, plus the run22 CommCenter queue
trace at exact commit `40209b27cb10d01c552398ff918ee613c4908ed0`. It separates
committed/hosted, real-firmware, visual, and local-only evidence so that neither
a green unit test nor a later lifecycle callback is presented as a rendered
SpringBoard. Later test-only commits are recorded separately and do not inherit
run21's firmware evidence.

## 2026-08-11 VFP short-vector and physical regression evidence

Exact source commit `7f37acc426cc3aa563afede163e87e927344e630`
contains the VFPv2 short-vector implementation from `64bd021` plus only the
hosted-workflow oracle-count correction. This result supersedes the historical
statement below that short vectors were unimplemented; it does not rewrite or
inherit the older run evidence.

- The default local build passed **61/61** CTest tests and the JIT/static build
  passed **66/66**. `test_vfp` passed **607/0** in both builds. The directed
  cases include the exact Voice Memos instruction `0xee22aa00` with captured
  `FPSCR=0x03030000`, bank wrapping, stride two, scalar broadcast, overlap,
  cumulative exceptions, and fail-atomic invalid shapes.
- Exact-head hosted runs were green: [core run
  31460531585](https://github.com/j0shua-SYSON/S5LBox/actions/runs/31460531585)
  and [unsigned iOS run
  31460547163](https://github.com/j0shua-SYSON/S5LBox/actions/runs/31460547163).
  The installed IPA was 1,292,866 bytes with SHA-256
  `CE5A6ED4D4CCA7EFA0C5D54860F6E7A127D5F34E1023ACE87E08D20925A1B7EB`.
- On a physical iPhone8,2 running iOS 15.8.5, a saved legacy guest reached
  SpringBoard under the current app-wide MBX launch setting. Because that
  machine predates `.graphics-v1`, its image-time renderer cannot be proved;
  this is not a controlled CPU-renderer or MBX result. Voice Memos entered its
  list view twice and remained
  `running`; the displayed retired counter advanced from **4,250.1 M** to
  **4,826.1 M** after the first activation. Safari's far-right Pages view and
  Spotlight with its keyboard also opened and remained running. Spotlight's
  counter advanced from **7,579.2 M** to **7,740.5 M** during the final hold.
  A final console scan contained no terminal-CPU, undefined-instruction,
  machine-stopped, or `0x04b78d94` marker.
- A separate machine was then created from scratch with **MBX on** and the
  QuartzCore software renderer **off**. Its first open wrote the exact ownership
  record `mbx 1` / `ca-software-render 0` before preparing its 466,825,216-byte
  work image. That machine cold-booted to the guest lock screen, unlocked into
  SpringBoard, and launched Voice Memos. The black framebuffer observed after
  the long unattended boot was the guest's normal display sleep, not a stopped
  boot: one guest Power pulse immediately exposed the live lock screen.
- Leaving that controlled MBX machine wrote a 121,007,450-byte `state.snap`, a
  131,248-byte block-device sidecar, and its one-shot restore marker. Reopening
  it reported `restored at 9659.6 M insn`, returned to the visible Voice Memos
  screen, remained `running`, and had advanced another **93.6 M** instructions
  when observed. This validates one physical MBX cold boot and one MBX
  checkpoint/restore cycle. It does **not** establish repeated restore safety,
  long-session stability, or readiness to make MBX the default.
- This is evidence for those three previously fatal interaction paths, not for
  general stability. Voice Memos' list view fell to roughly **1.3--1.6 M
  insn/s**, and the sampler reported a single-region loop at
  `0xc0062300--0xc006233f`; the guest continued retiring instructions, but that
  low-throughput behavior remains unresolved. Two separate cold starts also
  remained black beyond 4.5 billion instructions; one was observed sleeping
  in the modeled WFI path at roughly 1.8 M insn/s. This run therefore does not
  validate cold boot, MBX stability, internet, sound, or sustained everyday
  use.

## Current verdict

- The post-run19 TV-out correction built successfully in Release mode, and all
  **23/23** then-registered CTest tests passed on that tree.
- The corrected affected binaries reported **5,504 passed, 0 failed** for
  `test_soc` and **469 passed, 0 failed** for `test_snapshot`.
- The pre-run19 `afa650e` targeted CTest gate for `s5l8900_machine`, `snapshot`,
  and `external_md_cli_preflight` had also passed **3/3**.
- `external_md_cli_preflight`, strict compiler checks, diagnostic analyzer
  checks, and zero-step tool smokes passed as recorded below.
- Hosted GitHub Actions passed for the pre-run19 exact commit: the
  [core run](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30088519878)
  was green across Linux, macOS, Windows, warnings-as-errors, ASan+UBSan, and
  JIT jobs; the
  [unsigned iOS run](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30088519892)
  was also green.
- Hosted GitHub Actions also passed for exact correction commit `590d224`: the
  [core run](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220128)
  and
  [unsigned iOS run](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220122)
  were both green.
- Run19 exposed the over-strict all-three-bank timing predicate. Run20 then
  validated the `590d224` mixer+SDO correction in real firmware: 4 TV-out
  frames, IRQ 30 filter/action, swap-gate wake, and exact `IOServiceClose` user
  return `r0=0` at 1,915,263,517.
- Run20 advanced UIKit's `startWindowServer` return to 1,919,831,289 with a
  decoded 320x480 display and entered SpringBoard
  `applicationDidFinishLaunching:` at 1,923,358,329.
- Run20 exited **9** at 1,937,979,818 on `0xEE274B10` in PID 20's libm
  `_fmod+0x1a8`. The encoding is valid VFP11 `FMDHR` /
  `VMOV.32 d7[1], r4`, not NEON.
- The VFP11 decoder correction at exact commit `debec04` passes the local
  Release suite **23/23** and focused strict builds. Targeted `test_vfp`,
  `test_arm`, and `test_jit` binaries pass **452/0**, **810/0**, and **347/0**
  respectively. Exact-commit hosted
  [core run 30095081111](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081111)
  and
  [unsigned iOS run 30095081184](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081184)
  are green.
- Run21 firmware-validated that exact correction: it cleared `_fmod+0x1a8` and
  exited **0** at the configured **2,500,000,000-instruction cap**,
  **562,020,182 instructions beyond** run20's stop.
- SpringBoard again entered `applicationDidFinishLaunching:` at
  1,923,358,329. `-[SBTetherController isTethered]` returned false at
  1,924,647,850, and SpringBoard continued through debugging/demo preferences,
  lock-button, and platform-controller setup. It entered
  `+[SBTelephonyManager sharedTelephonyManager]` at 1,965,837,070 and `-init`;
  PID 20's last exact user instruction was 1,966,242,080, then its thread
  switched out at 1,966,246,193 in a shared-cache `mach_msg` before returning
  to the caller. Post-run resolution proves that
  `_CTTelephonyCenterGetDefault` creates a CTServerConnection, successfully
  looks up literal `com.apple.commcenter`, receives port name **0x4f07**, and
  blocks in its initial generated handshake before the `mach_msg` return.
  No reply, deadlock, queue-full state, or baseband cause is inferred from
  run21.
- Run22 replayed exact diagnostic commit `40209b2` to a clean
  **2,100,000,000-instruction cap**. The copied-in kernel request
  `0x0054b557` reached destination port object `0xc0d705a0`, whose mqueue
  reported **`msgcount=5`, `qlimit=5`**. The trace then recorded the exact
  queue-full and `fullwaiters` PCs before the SpringBoard sender blocked and
  did not resume before the cap. The old recorder omitted `r8`, so those PCs
  are adjacent candidates rather than a fail-closed same-kmsg route.
- Run22 does not prove that five messages were linked because `msgcount`
  includes reserved/in-flight slots. Its printed PID-1 receiver was later
  rejected as authoritative because the probe did not first discriminate the
  active-receiver/in-transit/timestamp union using `ip_receiver_name`.
- Exact run22 source passed all eight hosted jobs in
  [core run 30106957804](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30106957804).
- Run23 replayed exact commit `777afb4` — build inputs identical to hosted-green
  `5a40c5e` — to a clean **2,100,000,000-instruction cap** in 1,434.86 s. It
  **bound** both send-path route PCs to the exact mqueue/kmsg pair, **walked**
  the queue to a closed consistent ring of **five linked** kmsgs with
  **zero reserved/in-flight slots** (all id `0x0054b557`, all 2,104 bytes, five
  distinct reply ports), and decoded the destination's receive-right owner
  **authoritatively** as **launchd, PID 1**.
- Run23 retires the delivered-baseband hypothesis: AppleBaseband created,
  committed, and enabled its reset event source, but its **reset callback never
  fired**, so there was no notification, no Mach send, and no queue route. The
  one IOKit interest CommCenter registered is on AppleBaseband, and the thread
  that registered it blocked in a Mach receive 923 K instructions later and was
  never observed running again — a correlation, not a proof.
- Run23 also names an unmapped peripheral: `0x3d200000`, first touched by
  `com.apple.driver.BasebandSPI+0x1eca`, is not a declared stub, so the shipped
  baseband transport reads zeros back from it.
- **SpringBoard was not rendered.** Run22 and Run23 both recorded zero
  `UIController` hits, zero live-scanout mutations, and zero changed pixels;
  the final PPM was the unchanged seed in both.
- Test-only commit `0670ab8` passed hosted core/iOS runs 30096115501 and
  30096115527 with VFP **469/0**. Latest hosted test-only commit `657e8d8` expands
  helper-sequence coverage to VFP **488/0 locally** and passed hosted core/iOS
  runs 30097023293 and 30097023356.
- Run34 **failed closed at exit 4** without writing a checkpoint: the sidecar
  opened the work image a second time with `fopen` while the `file_block`
  adapter held the only handle Windows grants. Recorded as a pass of the
  fail-closed rule, not as a run result — it refused to write a snapshot it
  could not stand behind. Fixed in `a6febff` by copying through the adapter's
  own `vm_block.read_at`, the same path the guest's I/O takes.
- Run35 (cap 5e9, checkpoint at 2.4e9) exited **0** with **no CPU stop** and
  reached **`SpringBoard:UIController-call`, hits=1 at 3,478,858,148** — the
  first time in the project. Telephony **completed**: `CTCenterGetDefault` call
  at 3,335,082,498 and return at 3,335,312,957. Checkpoint sidecars were written
  and verified (87,457,413 / 466,825,216 / 131,248 bytes).
- Runs 30–33 reporting `UIController` at 0 hits was **not** evidence of a
  blocker. `UIController` lies past their 2.5e9 cap. A clean cap-stop means the
  budget ran out and carries no information about blockers; earlier wording that
  read it as a stall was wrong.
- **SpringBoard still has not rendered.** Run35's final PPM is the unchanged
  seed: `CBAD1C11…`, 384 of 460,800 RGB bytes non-zero, 128 non-black pixels in
  the top-left corner, confirmed **by eye** via `tools/ppm2png.py` and not by
  hash alone. Live CLCD scanout recorded 0 changed bytes. CLCD itself is
  correct and live around it — `scanning=1`, `frames=1026`, window0 320×480 at
  `0x0885c000`, descriptor refreshes 71 → 102 — so the display path is ready and
  the guest has not drawn into it.
- The post-`UIController` frontier is **RSA-class arithmetic, not a wait**:
  99.6% of the final window is userspace and ~40% is one 22-instruction loop
  resolving via `tools/dscmap.py` to `Security.framework` `_mulg_common`,
  schoolbook multiplication on 16-bit limbs bounded by a limb count. The
  co-reported `0x33aae484` is `svc #0x80` in libSystem, an ordinary syscall.
  Run36's restore banner shows the 2.4e9 checkpoint was taken inside the same
  loop and *before* `UIApplicationMain` at 3.268e9, so that block of work
  finished and the boot moved on: the cost recurs, it does not hang. Which
  operation drives it, and the total remaining, are **open**.
- **SpringBoard is in a `launchd` crash/respawn loop, and every bullet above
  that treats the missing frame as a progress, budget, or latency question is
  superseded.** The guest's own `ReportCrash` wrote **35** SpringBoard crash
  reports into the rootfs work images (extracted to `work\analysis\crashes\`:
  32 from run40, 3 from run46). All 35 are byte-identical in the crashed thread:
  `EXC_BAD_ACCESS (SIGBUS)`, `KERN_PROTECTION_FAILURE at 0x00000048`, crashed
  thread 3, `pc 0x30e1ea50`, `lr 0x3123d928`, `cpsr 0x60000010`,
  `sp 0x007b75d4`. The process dies roughly every **470 million instructions**;
  one run recorded **30 exec attempts and 29 deaths across 13.7e9
  instructions**.
- **The SIGBUS is identified, and it is not a CPU defect.** Thread 3 is the
  CoreAnimation render thread on the IOMFB vblank callback —
  `_mbx2DDisable+0x20` <- `CA::RenderMBX2D::render` <- `render_display` <-
  `CA::WindowServer::MBXServer::render_update` <- `Server::render_for_time` <-
  `IOMFBServer::link_callback`. The faulting instruction is
  `30e1ea50 strbeq r0, [r3, #0x48]` after `30e1ea3c ldr r3, [r3]` loads
  `_mbx2DGlobalContext` (`0x381200d8`) as NULL, because `_mbx2DCtxInitialize`
  failed (`IOIteratorNext` at `0x30e1fd60` returned `MACH_PORT_NULL` from a
  successful-but-empty `IOServiceGetMatchingServices("AppleMBXDevice")`) while
  `MBXServer::MBXServer` left `enable_mbx2d` set to 1. A NULL dereference into
  `__PAGEZERO` yields `KERN_PROTECTION_FAILURE` and therefore SIGBUS, not
  SIGSEGV. A separate audit found the interpreter's ARMv6 unaligned-access model
  **correct** (SCTLR.U and SCTLR.A both honoured), with no alignment fault among
  the 2,052 recorded faults.
- **The fix is verified statically and has NOT been run.**
  `MBXServer::render_update` (`0x3124207c`) is a three-way fallback whose third
  arm is `CA::WindowServer::Server::render_update` ->
  `CARenderOGLNew(_kCARenderSoftwareCallbacks)` -> `CA::OGL::SWContext`, a real
  CPU rasteriser; `mbx2d_context()` (`0x31241a8c`) returns NULL early at
  `0x31241aa8` when the byte `enable_mbx2d` at `0x38190db1` is zero, and that
  byte comes from `getenv("CA_ENABLE_MBX2D")` or `getenv("LK_ENABLE_MBX2D")`.
  Setting `CA_ENABLE_MBX2D=0` in SpringBoard's launchd environment therefore
  selects software rendering with **no GPU emulation**. This is disassembly, not
  a run result. **SpringBoard has still never rendered a frame.**
- **Retracted:** "SpringBoard is progressing normally and merely runs out of
  instruction budget." It is crash-looping; the identical framebuffer hash
  `CBAD1C11…` in every run since run35 is the signature of that.
- **Retracted:** "Checkpoint restore loses fidelity — cold and restored runs
  disagree by 4.6e9 instructions," and "the instruction cap perturbs guest
  execution." Restore is **bit-exact**: heartbeat PC streams are byte-identical
  for run35 vs run36d across **27/27** samples, and likewise for run37/38,
  run38/40/41/43, and capA/capB. The apparent disagreement was an artifact of
  the `SPRINGBOARD POST-SETEXEC TRACE` block reporting only the most recent
  SpringBoard generation.
- **Retracted:** "`-[UIWindow makeKeyAndVisible]` is never called." That
  selector has exactly one call site in the whole 1.19 MB SpringBoard binary,
  inside `-[SBSyncController _delayedBeginReset]`, a restore path never taken at
  boot; 3.1.3 uses `-[UIWindow orderFront:]` and `-[UIWindow makeKey:]`. Zero
  hits there is the **expected** reading on healthy hardware.
- **Retracted:** "un-matching only `/arm-io/usb-otg/usb-device` is
  insufficient." It was a complete no-op — run46 (child un-matched) printed
  **24** `AppleSynopsysOTGDevice` lines against run37's (parent un-matched)
  **0**; matching keys off the parent node. Consequently any claim resting on
  run39 or run42 "with USB un-matched" is **void**: both restored from a
  snapshot taken with the driver already matched in guest RAM, and a device-tree
  patch cannot affect a restored run.
- **Corrected:** the MBX register block is at physical **`0x3B000000`**, not
  `0x03000000` (device-tree `reg` `{0x03000000, 0x01000000}` plus `/arm-io`
  `ranges` `0x38000000`); and the "busy-polls a reset bit" account of the MBX
  hang is wrong — that poll (`AppleMBXController`, `0xC07799E0`) is gated on
  `fVariant == 2` (s5l8720x) and cannot run on s5l8900x, where `fRegs` is NULL.
  The real wedge is `AppleMBXDevice` at `0xC077E8D8` spinning on physical
  `0x3B00012C` bit 6 with no timeout and no exit. The `dt_unmatch("arm-io/mbx")`
  call itself stays; only its explanation was wrong.
- `2b08c4d` implements **LDRD/STRD** and gates CP15 c1 on CRm: `test_arm`
  **939 passed, 0 failed** (was 878).
- `a09478e` models the **DWC2 configuration registers** the OTG driver reads, in
  new `core/src/soc/usbotg.c` (`GHWCFG1=0`, `GHWCFG2=0x228de550`, `GHWCFG4=0`
  read-only; `PCGCCTL` read/write). It removes the deterministic panic at
  instruction **8,728,148,009**, and `test_soc` reports **5,621 passed, 0
  failed** (was 5,591). Snapshot format **4 → 5**, which invalidates existing
  checkpoints. This path is **not** reached by a default boot: bootkernel still
  un-matches the USB complex unless `-u` is passed, so the fix is covered by
  tests rather than by a firmware run.

## Fidelity

A standing divergence ledger: what executes as genuine Apple software, and every
place where the modelled machine differs from an S5L8900 iPhone. Unlike the run
entries below, these facts are not owned by a single commit or run and do not
expire when the next run lands. The prose version is
[README.md, "What this is, and what it is not"](../README.md#what-this-is-and-what-it-is-not).

- **Genuine.** The user's own unmodified iPhone OS 3.1.3 (7E18) firmware
  executes: XNU 1357.5.30 boots, Apple's own kexts match and start, the real
  root filesystem mounts, and `launchd`, `securityd`, `installd`,
  `mDNSResponder`, `fairplayd`, `itunesstored`, `lockbot`, `CommCenter` and
  SpringBoard run. The evidence that this is Apple's stack rather than a
  reimplementation of it: the guest's **own** `ReportCrash` wrote the 35
  SpringBoard crash reports that diagnosed the current blocker into its own
  filesystem. The project ships no Apple firmware and never modifies the
  canonical inputs on disk.
- **Networking transport is a deliberate, temporary substitution.** The chosen
  route is PPP over a second emulated S5L8900 UART, using the guest's own stock
  `/usr/sbin/pppd`. **Partially built as of 2026-07-27:** `uart4` is modelled
  and decoded, and the guest is given a `pppd` launchd job by a size-neutral
  in-place rewrite of an inert LaunchDaemon plist — one that points at a
  binary this image does not contain, so nothing is displaced. launchd spawns
  the job (measured, instruction 557,124,470), `pppd` runs, and then exits;
  see `docs/derivations.md` §23.10 and the run entries in
  `docs/BOOTLOG.md`. **No host-side PPP peer exists**, so even a `pppd` that
  transmits will get no answer: LCP will retransmit and time out. Every part
  of it is honest emulation — the UART is real
  modelled silicon, `pppd` is Apple's own binary, and LCP/IPCP/HDLC-async are
  specified in RFC 1661/1662/1332 with public test vectors, so no device
  behaviour is invented and packets genuinely traverse the guest's own network
  stack. But **a real iPhone 3G never reached the internet this way.** It used
  the Marvell 88W8686 over SDIO, or the Infineon baseband over SPI. So the
  interface the guest ends up with is `ppp0`, not `en0` or `pdp_ip0`, and its
  topology is point-to-point with no ARP, no DHCP and no broadcast.

  This is recorded as a **workaround, not an endpoint**. The reason it was chosen
  over the real radio is that the 88W8686 runs its own firmware on its own
  processor: reaching a working 802.11 world would require inventing the
  behaviour of firmware build `9.108.5.p1-26524`, which is documented nowhere,
  and inventing it is exactly what this project refuses to do. Modelling the
  real SDIO controller and the real baseband remains the intended destination;
  see `docs/ROADMAP.md`. Anyone reading a future run that reports working
  networking should understand it as "the guest's TCP/IP stack works and can
  reach the internet", never as "the emulated Wi-Fi hardware works".
- **CPU.** ARM, Thumb and VFPv2 on the reached path; the ARMv6 unaligned-access
  model with `SCTLR.U` and `SCTLR.A` both honoured; MMU with XN enforcement;
  CP15. Runs are bit-exact reproducible — heartbeat PC streams are byte-identical
  across **27/27** samples for cold versus restored runs.
- **Modelled peripherals.** Two UARTs — `uart0`, the kprintf console, and
  `uart4`, the PPP line — timers, VIC, the CLCD display controller, I2C/PMU,
  and the DWC2 USB configuration registers.
- **`uart4` is transmit-only, and that is a statement rather than an
  unfinished job.** It has no receive source, so its receive FIFO reports
  empty — which is the truthful answer for a machine with no peer attached,
  and the alternative would be answering a guest read with a byte no host ever
  sent. It follows that it can never raise its interrupt line (device-tree
  line 28), and it is deliberately absent from the wake-source table for that
  reason. The day a receive path lands, `UFSTAT`'s receive count, `UTRSTAT`
  bit 0 and a real write-one-to-clear latch all become load-bearing; the
  header comment in `core/include/soc.h` says so, and
  `core/tests/test_uart4.c`'s `test_uart4_raises_no_interrupt_line` is the
  test that must then be rewritten rather than deleted.
- **Not modelled at all.** No touch input. No audio. **No networking beyond a
  transmit-only wire** — there is no host-side PPP peer, so the guest can send
  LCP Configure-Requests and nothing answers them. No cellular. No Wi-Fi. No
  Bluetooth. No camera. No accelerometer. No GPU.
- **Declared absent to the guest.** The loaded (in-memory) device tree
  un-matches five nodes that real hardware has: `arm-io/mbx` (the PowerVR MBX
  GPU), `arm-io/sha1`, `baseband`, `arm-io/spi2` (the baseband transport), and
  `arm-io/usb-otg`. The firmware on disk is never modified; only the loaded copy
  is edited. Each un-match has a documented reason recorded above, but the net
  effect is that the guest is told it is running on a machine with less hardware
  than a real iPhone.
- **Invented register values.** The DWC2 USB configuration registers
  (`GHWCFG1=0`, `GHWCFG2=0x228de550`, `GHWCFG4=0`) are a legal and sufficient
  configuration. They are **not** measured from real S5L8900 silicon.
- **Rendering diverges from hardware.** QuartzCore is configured with
  `CA_ENABLE_MBX2D=0` so it uses its own CPU software compositor
  (`CARenderOGLNew(_kCARenderSoftwareCallbacks)` → `CA::OGL::SWContext`). The
  real device composites on the MBX GPU. This is a switch Apple's code reads and
  a renderer Apple shipped, but it is not the path real hardware takes.
- **Timing is not cycle-accurate.** An interpreter running roughly **200x
  slower** than the real 412 MHz part, with a synthetic 412 MHz : 6 MHz
  instruction-to-timebase ratio rather than real cycle timing.
- **Kernel patched in RAM.** At load the IORTC timeout is forced to zero,
  `IOFindBSDRoot` is redirected to `md0`, and SVC hooks are installed for the
  host storage bridge. Exact-gated against a SHA-256 and a nine-segment check of
  the 7,942,144-byte kernel; `firmware/kernel.macho` itself is never written.
- **Storage is not NAND.** The root filesystem is served by a host-backed bridge
  (external-md) into a unique writable per-run work image, and `/etc/fstab` is
  rewritten in that work image to match.
- **No secure boot chain is executed.** The kernel is loaded directly;
  SecureROM, LLB and iBoot are not run. IMG3 parsing and an extracted LLB
  payload have been executed separately, but not as a chain.
- **Optional plist substitution.** Off by default: one LaunchDaemon plist in the
  work image is rewritten size-neutrally to add `CA_ENABLE_MBX2D=0`.
- **Unproven.** The single most visible property of an iPhone — that it displays
  a home screen you can touch — has never been demonstrated here. SpringBoard
  has not rendered a frame. Every run to date ends with an identical, unchanged
  framebuffer.

## Evidence ledger

| Check | Result | What it establishes | What it does not establish |
|---|---|---|---|
| Full CMake Release build | Passed locally on the post-run19 correction | All configured core tests and host tools compiled and linked together | Cross-platform or iPhone compilation |
| Full Release CTest suite | **23/23 passed** on the post-fix tree | The public firmware-free suite, including storage, firmware parsers, CPU, SoC, snapshot, framebuffer bridge, and CLI preflight, is green with the corrected predicate | Private-firmware or device behavior |
| Final focused unit binaries | `test_soc`: **5,504/0**; `test_snapshot`: **469/0** | The real `0/5/1` timing state, independent control transitions, IRQ/WFI, MMIO, CLCD, and snapshot invariants pass locally | Real Apple driver behavior after the correction |
| Baseline targeted CTest | **3/3 passed** at `afa650e`: `s5l8900_machine`, `snapshot`, `external_md_cli_preflight` | The linked baseline targets and startup preflight passed before run19 | The post-run19 correction by itself |
| Baseline focused unit binaries | `test_soc`: **5,498/0**; `test_snapshot`: **468/0** at `afa650e`, with the complete Release build green and **23/23** CTest on that tree | The pre-run19 hardening tree was green in the same focused binaries later reported as 5,504/0 and 469/0 | The post-run19 correction, which owns the later counts |
| `external_md_cli_preflight` | Passed | Startup self-checks pin framebuffer PA `0x0885c000` and `topOfKernelData` `0x088f4000`; incompatible external-md/tree/RAM/root/snapshot combinations fail closed before firmware is opened | A rootfs copy, guest boot, or long storage run |
| Strict GCC pass | Passed with `-std=c11 -Wall -Wextra -Werror` on the changed core and focused test sources | The affected portable-C paths are warning-free under the local GCC frontend | Clang, MSVC, Xcode, sanitizers, or runtime behavior |
| Targeted diagnostic warnings | Relevant `-Wformat=2` and `-Wconversion` checks passed | New diagnostic formatting and selected conversion-sensitive paths were checked more strictly than the default build | A whole-repository conversion-clean guarantee |
| GCC static analyzer | Passed for the changed `bootkernel` diagnostic paths | No analyzer finding remained in the new TV-out/framebuffer diagnostic control flow | Proof that the analyzer models every guest/host interaction |
| Zero-step tool smokes | `bootkernel` and `snapboot` passed | Startup invariants, option/report plumbing, and the new non-invasive state diagnostics execute without retiring guest instructions | Any emulated-time progress or firmware stability |
| [Hosted PMU core-tests run 30073161392](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30073161392) | Passed for exact commit `3963d22` | The S5L I2C/PCF50635 PMU model that run16-pmu-smoke exercised is green in the hosted core matrix | The later `0bc18ea` and `9bab56c` diagnostics, which runs 17 and 18 exercised and this earlier run does not cover |
| [Hosted PMU ios-build run 30073161386](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30073161386) | Passed for the same exact commit | The same revision packaged in the unsigned iOS workflow | Private firmware, a SpringBoard boot, or device execution — hosted CI holds no firmware |
| [Hosted core run 30088519878](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30088519878) | Passed for exact commit `afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9` | Linux, macOS, Windows, warnings-as-errors, ASan+UBSan, and JIT jobs were green in GitHub Actions | Private-firmware execution or an on-device boot |
| [Hosted iOS run 30088519892](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30088519892) | Passed for the same exact commit | The unsigned iOS workflow built successfully on the hosted runner | Installation, signing, JIT entitlement activation, or iPhone runtime stability |
| [Hosted correction core run 30091220128](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220128) | Passed for exact commit `590d2248af4d7e5e92ec7bbd1be079c3bb415542` | The corrected core passed the hosted platform, strict-warning, sanitizer, and JIT matrix | Private-firmware execution or an on-device boot |
| [Hosted correction iOS run 30091220122](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220122) | Passed for the same exact commit | The corrected revision built successfully in the unsigned iOS workflow | Installation, signing, JIT entitlement activation, or iPhone runtime stability |
| [Hosted VFP core run 30095081111](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081111) | Passed for exact commit `debec04ff9b0faa469d5ad2ee7d75d1bf3b53b1a` | The VFP correction passed the hosted platform, strict-warning, sanitizer, and JIT matrix | Private-firmware execution, pixels, or an on-device boot |
| [Hosted VFP iOS run 30095081184](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081184) | Passed for the same exact commit | The decoder-fix revision built successfully in the unsigned iOS workflow | Installation, signing, JIT activation, or iPhone runtime stability |
| [Hosted exact-path test core run 30096115501](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30096115501) | Passed for test-only commit `0670ab8cbf6b9febbfe059b17ffdeb755ee0133a`; VFP **469/0** | The exact libm sequence regression passed in the hosted core matrix | A second firmware run or evidence belonging to `debec04` |
| [Hosted exact-path test iOS run 30096115527](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30096115527) | Passed for the same test-only commit | The expanded-test revision still built in the unsigned iOS workflow | Device execution or private-firmware behavior |
| [Latest hosted helper core run 30097023293](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023293) | Passed for test-only commit `657e8d8f2f42d09c573a4012a618e0f896307bdf`; VFP **488/0 locally** | Additional post-`_fmod` helper sequences are covered and the hosted core matrix is green | Firmware execution at that test-only commit or rendering |
| [Current helper iOS run 30097023356](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023356) | Passed for the same test-only commit | The current expanded-test revision builds in the unsigned iOS workflow | Device execution or private-firmware behavior |
| [Run22 diagnostic core run 30106957804](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30106957804) | **8/8 jobs passed** for exact commit `40209b27cb10d01c552398ff918ee613c4908ed0` | Warnings-as-errors, ASan+UBSan, Windows/Linux/macOS builds and tests, and Ubuntu/macOS JIT jobs are green for the run22 source | Private-firmware execution, SpringBoard rendering, or baseband behavior |
| Run19 integrity/layout | Exit 0, `OK`, stderr 0 B; exact source hashes unchanged; work image 466,825,216 B | The `afa650e` firmware run was clean and persistent filesystem writes were confined to its work image; framebuffer/TOKD planning reached real firmware | A rendered frame or correct TV-out timing |
| Run19 TV-out chain | Pages mapped; final control/mixer/SDO `0/5/1`; frames/IRQ/filter/action/close-return all zero | The all-three timing condition was not the shipped active state and suppressed completion | That mixer+SDO alone will clear the wait until rerun |
| Run19 primary CLCD/frame | Correct 320x480, stride-1280 window; 662 CLCD frames; final PPM identical to seed | Corrected layout and controller handoff operated for the full run | Any SpringBoard pixel or live-scanout write |
| Run20 integrity/resources | Exit **9** at 1,937,979,818; original source hashes unchanged; external-md failures **0**; guest-free low **51.76 MiB**; run directory **447.18 MiB on F:** | The retained `590d224` run stayed inside the expected storage/resource envelope until its explicit CPU stop | A clean cap, boot success, or decoder-fix validation |
| Run20 TV-out chain | **4 frames**; IRQ 30 filter/action hit; gate woke; exact PID 20 close user return `r0=0` at 1,915,263,517 | The mixer+SDO correction fixes the observed run19 TV-out completion wait in real firmware | Framebuffer mutation or a rendered home screen |
| Run20 SpringBoard/display | `startWindowServer` returned at 1,919,831,289; display decoded 320x480; `applicationDidFinishLaunching:` entered at 1,923,358,329; `UIController` unreached; **0 changed pixels** | Control flow advanced past the old close wait and into the application delegate | SpringBoard rendering, UI readiness, touch, or on-device runtime |
| Run20 VFP stop | Exit 9 on `0xEE274B10` in PID 20 libm `_fmod+0x1a8` | The committed decoder's NEON classification is wrong; the word is VFP11 `FMDHR` / `VMOV.32 d7[1], r4` | That the local decoder correction works in firmware |
| `debec04` VFP local gate | Release **23/23**; `test_vfp` **452/0**; `test_arm` **810/0**; `test_jit` **347/0**; focused strict builds passed | All 16 D registers, both halves and directions, decoder boundaries, disabled-VFP behavior, conditions, JIT refusal, system-register privilege, and guest-Undefined disposition are locally covered | Firmware behavior outside the reached path |
| Run21 integrity/resources | Exit **0** at 2,500,000,000; source hashes unchanged; external-md failures **0**; guest-free low **50.63 MiB**; run directory **447.27 MiB on F:** | The `debec04` cold run reached its cap within the expected source/storage boundary | Boot completion, rendering, or on-device runtime |
| Run21 VFP replay | Crossed the run20 stop by **562,020,182 instructions** | The exact `_fmod+0x1a8` VFP correction is firmware-validated | General VFP completeness |
| Run21 SpringBoard/display | `applicationDidFinishLaunching:` at 1,923,358,329; `isTethered` false at 1,924,647,850; telephony singleton entry 1,965,837,070; successful `com.apple.commcenter` lookup returned port `0x4f07`; initial request `0x0054b557` entered `mach_msg` at `0x30a117e0` with send/receive sizes `0x834`/`0x30` and did not reach `0x30a117e4`; `UIController` **0**; live scanout **0**; PPM seed with **0 changed pixels** | SpringBoard progressed beyond the former CPU stop into the initial CTServerConnection handshake; header size 6 is stale state left by the stock generated stub, not emulator corruption | A service reply, deadlock, queue-full condition, baseband causality, UI readiness, or rendered SpringBoard |
| Run22 integrity/resources | Exit **0** at 2,100,000,000; stderr empty; source hashes unchanged; external-md failures **0**; guest-free low **50.63 MiB**; evidence directory **447.42 MiB on F:** | The exact `40209b2` diagnostic run reached its cap within the immutable-input and storage boundary | Boot completion, rendering, or device runtime |
| Run22 CommCenter send route | Copied-in kernel ID `0x0054b557`; destination port `0xc0d705a0`; mqueue `0xc0d705b8`; `msgcount=qlimit=5`; adjacent full-queue PC candidate at 1,966,245,373; adjacent `fullwaiters` pre-store candidate at 1,966,245,387; block at 1,966,245,550; switch-out at 1,966,246,193 | The initial SpringBoard sender reached a saturated Mach queue and blocked without resuming before the cap | Fail-closed binding of those route PCs to the same kmsg, five linked kmsgs, the active receiver owner, why the queue is saturated, a permanent deadlock, or baseband causality |
| Run22 SpringBoard/display | `UIController` **0**; live scanout mutations **0**; PPM unchanged seed SHA `CBAD1C...AF2AB`; **0 changed pixels** | The saturated queue remains before the current rendering gate | UI readiness, a rendered home screen, or touch |
| Run23 integrity/resources | Exit **0** at 2,100,000,000 in **1,434.86 s**; stderr empty; launcher postflight passed; source hashes unchanged; work image 466,825,216 B; external-md failures **0**; guest-free low **50.63 MiB**; directory **447.43 MiB on F:** | The exact `777afb4` cold replay stayed inside the immutable-input, storage, and memory envelope | Boot completion, rendering, or device runtime |
| Run23 queue topology | Bounded reciprocal walk: head `c21e3000`, **linked=5**, closed, consistent, untruncated, **reserved-or-in-flight=0**; all five kmsgs carry id `0x0054b557`, size 2104, destination `c0d705a0`, and five distinct reply ports | Five genuinely linked identical CTServerConnection handshakes occupy a `qlimit=5` queue; SpringBoard's kmsg `c3d3c000` is the sixth | Which processes sent the other five, why they were never dequeued, or that the state is permanent |
| Run23 route binding | **BOUND** queue-full slow branch `c00147ba` @1,966,245,373 and **BOUND** `fullwaiters=1` pre-store `c00147d6` @1,966,245,387, each with `r4=c0d705b8`, `r8=c3d3c000`; 1 candidate, 1 bound, 0 rejected | Run22's adjacent PC candidates are now fail-closed bound to the exact mqueue and kmsg | A permanent deadlock, or that no other sender could later drain the queue |
| Run23 receive-right owner | `ip_receiver_name=0x1b03` validated **first**; space `c0acfe60` active, task `c0ad7b10`, task-space backpointer matches, proc `e0381d68`, **PID 1**; decode **AUTHORITATIVE** | launchd holds the receive right for the port SpringBoard sends to; run22's rejected PID-1 candidate is confirmed through the discriminator | That CommCenter failed, since launchd legitimately pre-creates and holds service ports |
| Run23 CommCenter identity | SETEXEC attempt armed @517,086,676; task `c2ca2760`, proc `e037f890`, **PID 24**; never invalidated, **0** signals, **0** `_exit1`; 10,975,004 user + 58,986,380 kernel instructions before the send entry and **0** after; owner correlation **MISMATCH** | CommCenter exists, ran, and never exited, but does not hold the destination receive right | Where its startup is gated, or that it will never check in |
| Run23 per-thread waits | 6 retained threads; `e038dbb8`/`e0376000`/`e02e4774` in timed waits on the same semaphore `c0b239a0` (continuation `c0026fc5`, waitq `c0b239a8`); `e02f5888` blocked in `_ipc_mqueue_receive` on `c0dd99f0` @932,507,189 with no resume observed | Every retained CommCenter thread's last observed episode is an unresolved wait | That any thread was still enqueued at the cap; no final live wait-state reread exists |
| Run23 AppleBaseband chain | Gate `VALIDATED`; object `c0c3a700`; reset-function setup 1/1 (`c0b6b020`); event source created/committed/enabled 1/1/1/1 (`c0b6c340`); **reset callback hits 0**; dispatches 0; handlers 0; sends 0; routes 0 | `frontier: event-source enable call was entered, but no reset callback was observed since trace start` — no baseband notification was delivered, so a delivered notification does not explain the saturation | That the *absence* of the callback is harmless, or that it is the reason CommCenter has not checked in |
| Run23 baseband/CommCenter correlation | 14 `registerInterest` wrappers, 13 service-rejects, **1 accepted**: CommCenter (`c2ca2760`/`e037f890`/PID 24) on AppleBaseband, port `c3c59ab0`, thread `e02f5888` @931,584,215; receive entries on that port **0** | The one interest CommCenter registered is on AppleBaseband, and the registering thread blocked 923 K instructions later and never resumed | A causal link: the blocked receive is on port `c0dd99d8`, not the interest port, and no port-set relationship was established |
| Run23 BasebandSPI window | Non-RAM page `0x3d200000`, first pc `com.apple.driver.BasebandSPI+0x1eca`, 4 reads / 11 writes; not among the five declared stubs, so reads return zero; write burst @933,033,890–933,033,922, read-back @1,757,842,145–1,757,842,149, final writes @1,760,475,736/740 | An identified baseband-transport register window is unmapped and answers the shipped driver with zeros | That this window blocks the boot — the driver does not poll it and stops touching it entirely |
| Run23 SpringBoard/display | `applicationDidFinishLaunching:` @1,923,358,329; `startWindowServer` return @1,919,831,289; 4 TV-out frames; CLCD scanning/running `1/1`, 604 frames, 320x480 stride 1280; `UIController` **0**; live scanout **0**; PPM seed SHA `CBAD1C...AF2AB`, **0 changed pixels** | The corrected display chain reproduces exactly, and the frontier is still the telephony send | SpringBoard rendering, UI readiness, or touch |
| Run23 diagnostic integrity | 16 exact-hook attribution omissions (first @551,530,083, last @1,388,875,916); 50 unreadable classifications; **0** readable contradictions; none overlapping the decisive send episode | The observers reported their own gaps instead of silently completing a chain | That every unobserved transition in the run is accounted for |
| [Hosted SPI-window core run 30145593885](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30145593885) | **8/8 jobs passed** for exact commit `3faa04a3b9e57e1fe4404f644354c09ae03c2c53` | Windows/Linux/macOS builds and tests, three JIT jobs, ASan+UBSan, and warnings-as-errors are green with the three SPI windows declared | Private-firmware execution, rendering, or that spi2 affects the boot |
| [Hosted SPI-window iOS run 30145593887](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30145593887) | Passed for the same exact commit | The unsigned arm64 package job still builds | Installation, launch, entitlements, JIT, or guest boot |
| Run30 CommCenter check-in | `_bootstrap_check_in` **hits=1** @777,240,124 with `r1=0x00085ee4` (`"com.apple.commcenter"`); return **`r0=0`** (KERN_SUCCESS); **SUCCESS arm taken**, FAILURE arm never | Un-matching the baseband nubs lets CommCenter claim its port and stand up its MIG server; SpringBoard's telephony singleton now enters *and returns* — the blocker that held every run since run21 is gone | Any rendering; `UIController` still 0 hits and the PPM still the seed |
| Post-blocker CPU coverage | Run30 stopped on `VCVTR` `0xeefd7a67` @2,061,479,415; run31 on `VCVT.F32.S32` `0xeef84ae7` @2,061,479,416 (**+1**); run32 on `SADD8` `0xe611ef9e` @**2,191,848,855** (**+130,369,439**) | Each stop is fail-closed and self-naming; implementing `FPSCR.RMode` as a class rather than per-encoding bought 130 M instructions in one step | That the remaining distance is only CPU coverage — the next stop could still be architectural |
| `FPSCR.RMode` implementation | `vfp_execute()` adopts the mode on the host FPU per instruction and restores it; float→int rounds explicitly in software; short vectors and trap enables still refuse. `test_vfp` **488 → 513/0** | Directed rounding is implemented rather than refused, covering conversions *and* arithmetic, with tests proving arithmetic actually rounds (1.0f + 2⁻²⁵ under RZ/RM/RP) | Short vectors or trapped FP exceptions, which remain unimplemented |
| ARMv6 parallel add/subtract | All six classes × six lane operations with GE flags, lane-independent wide intermediates, and PC refusal. `test_arm` **810 → 839/0** | The family that `SADD8` belongs to is implemented whole, so an adjacent encoding cannot hide the next stop | Other unimplemented media encodings (PKH, SEL, USAT, SMLAD…) |
| Run29 integrity | Exit **0** at **7,000,000,000** in **4,679 s** at exact commit `cf2f7d1`; stderr empty; postflight passed; hashes unchanged; external-md **0 failures**, 0 pending | The first replay to outlast the guest's own timeouts stayed inside every immutable-input and storage invariant | Any rendering: `UIController` 0 hits, live scanout 0, PPM the unchanged seed |
| Run29 falsifies the longer-run hypothesis | `_bootstrap_check_in` **still hits=0**; `_ioctl` 15→**177**, `_select` 1→**10**, spread evenly to 6.6e9; CommCenter retired **3,235,016 user instructions after** SpringBoard's send | Waiting longer does **not** let CommCenter give up; the bounded `SCPreferences` loop was an inner loop, and the outer baseband-mux retry does not terminate | That no timeout path exists at all — only that this one does not fire |
| Run29 guest-named blocker | Console, only reachable beyond 2.1e9: `BasebandSPIIFXProtocolVersion1::handleSRDYTimeoutAction: Exit` and `AppleSerialMultiplexer: !! mux-ad(err)::bsdIoctl: Fatal error code=kASMFatalErrorSPI(11)` | The Infineon baseband SPI driver times out on **SRDY** (GPIO `0x1804`, group 24 bit 4); the mux fails `ASMIOCNEWDLCI`; CommCenter retries forever. The chain from missing hardware to missing pixels is now confirmed by the guest's own diagnostics | What a faithful fix is — a clean prompt failure is demonstrably insufficient, since the ioctl already fails and CommCenter retries anyway |
| Guest-time arithmetic | Timebase 6 MHz against a 412 MHz CPU model ⇒ ~68.7 instructions per tick ⇒ **one guest second ≈ 412M retired instructions**; CommCenter's ten-attempt `sleep(1)` loop ≈ **4.1e9 instructions** | Every cap used so far (2.1e9, and run21's 2.5e9) is shorter than a single one of the guest's own retry budgets, so **no run has ever observed a guest timeout expire** | That a longer run will change the outcome |
| Run28 CommCenter checkpoints | `_bootstrap_check_in` **hits=0 NEVER CALLED**; check-in function entry **hits=0**; `_IOServiceOpen` 4, `_IOConnectCallScalarMethod` 3, `_ioctl` 15 (`fd=6`, `req=0xc004799a`), `_select` 1 | CommCenter does not fail check-in — it never reaches it; it is doing IOKit and `/dev/mux.spi-baseband` DLCI work instead | Why it never reaches it, or that the ioctl/select work is what blocks |
| Run26 CommCenter tail | Last own-image PC `0x0009bf08` = stub[508] **`_sleep`** @1,966,338,697, still executing after SpringBoard blocked at 1,966,246,193 | CommCenter is in a sleeping retry loop, not deadlocked | Which loop, or what it is waiting for |
| CommCenter static decode | Exactly **one** `_bootstrap_check_in` call site (`0x0001a9be`), in a function that creates `CPCreateMIGServerSource` + the server thread only on success; the name argument is the literal `"com.apple.commcenter"` at `0x00085ee4`; the function has exactly one caller (`0x0000cb08`) gated on `bl 0xcc50` | CommCenter's MIG server exists only if that single call succeeds, which explains run24's queue from the other side | That the gate at `0xcc50` is what fails |
| Userspace symbol tooling | `tools/hfsx_extract.py` extracted the stock CommCenter (724,208 B, UUID `b4b87526…`) from a retained work image; `tools/machosyms.py` resolved stub[494] `_rename`, stub[508] `_sleep`, stub[407] `_bootstrap_check_in` via the indirect symbol table | Guest PCs below `0x10000000` are now resolvable to imported functions and local symbols, closing a long-standing gap | Any runtime conclusion by itself; a stub hit is a call, not a completed call |
| Run24 integrity | Exit **0** at 2,100,000,000 in **1,625.1 s** at exact commit `8a08e44`; stderr empty; postflight passed; hashes unchanged; work image 466,825,216 B; external-md failures **0**; guest-free low **50.63 MiB** | The two new read-only probes cost ~13% throughput and changed nothing else | Any new boot progress or rendering |
| Run24 port-set classification | 4 CommCenter receives on non-interest mqueues classified; **sets-walked=0**; newest is `mqueue=c2966918` → object `c2966900`, `io-bits=80000000`, **IOT_PORT** | CommCenter is **not** receiving on a port set, so no AppleBaseband IOKit notification could have reached it — that delivery route is closed | Why CommCenter has not checked in; that the absent modem is irrelevant (the spi2 SRDY/MRDY handshake and timeout paths remain open); or that no set existed at some other moment |
| Run24 queued-sender identity | Five linked kmsgs resolve **AUTHORITATIVE** to **PIDs 16, 18, 15, 12, 13** through reply ports `c2bf6ea0`/`c34d2630`/`c2d33d80`/`c31c32d0`/`c31c3cf0` | Five distinct daemons plus SpringBoard are blocked on the identical `0x0054b557` handshake: CommCenter has served nobody since boot, so this is systemic, not SpringBoard-specific | Which client enqueued first, or why the service is silent |
| Post-Run23 SPI window declaration | `test_soc` **5,584/0**; full local Release suite **23/23**; strict `-Wall -Wextra -Werror` syntax checks on `machine.c`, `test_soc.c`, and `bootkernel.c`; exact 7E18 `-n 0` with every gate `VALIDATED` and exit 0; firmware hashes unchanged | The three device-tree-confirmed SPI windows — spi0 `0x3c300000`, spi1 `0x3ce00000`, spi2 `0x3d200000` — are declared named storage, so BasebandSPI's read-back of the configuration it wrote returns that configuration instead of an unmapped zero. A regression replays run23's exact write burst and four-offset read-back | That spi2 blocks the boot, that any SPI transfer, FIFO, DMA, chip-select, or interrupt behaviour is modelled, or any new firmware-run result |
| Diff hygiene | `git diff --check` passed after the documentation update | No whitespace-error patch was introduced | Markdown rendering on every client |

Run19 is the real-firmware verdict on `afa650e`: routing/layout passed, but the
aggregate timing predicate failed. The 23/23 result and focused 5,504/0 plus
469/0 totals validate the surgical correction off-firmware; the `590d224`
hosted runs validate its public build/test/package surface; run20 validates its
TV-out behavior in private firmware. None of those establishes rendering or
on-device behavior. Exact commit `debec04` owns the VFP hosted results and
run21 firmware validation. The later `0670ab8` and `657e8d8` test-only commits
must not inherit run21's firmware evidence.

## Run19 real-firmware evidence

The exact-gated source hashes were reverified after the run:

```text
kernel.macho    0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C
devicetree.bin  4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57
rootfs.img      C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82
```

The run used a fresh, exact 466,825,216-byte work image. Boot arguments at
`0x087db000`, raw bounce `0x087dc000..0x0885c000`, framebuffer
`0x0885c000..0x088f2000`, and physical
`topOfKernelData 0x088f4000` were accepted. The bridge recorded zero failures;
its native raw path completed two redirects and two completions with zero
pending.

Run19 repeated run18's boundary. `UIApplicationMain` ran at 1,849,444,535,
`rendersLocally` returned `YES` at 1,869,087,332, the optional finalizer/close
ran at 1,887,341,013/1,887,341,029, and ID-2816 asserted its wait at
1,887,344,201 before the exact thread switched out at 1,887,345,137. From
run18's finalizer through its wait, every matching checkpoint moved by exactly
13,983,022 instructions. No close return or later SpringBoard checkpoint was
gained.

The exact late TV-out state was control/mixer/SDO `0/5/1`, SDO pending/mask
`0/0`, zero model frames, zero raw IRQ 30, zero filter/action hits, and zero
close-return hits. Control ready reads returned `0x2` and removed run18's
shutdown-timeout warnings, so that independent handshake remains valid. Static
driver disassembly shows control `+0` is conditional source state; mixer+SDO
are the persistent timing eligibility pair.

CLCD window 0 was 320x480 at `0x0885c000`, stride 1280, and counted 662 frames.
The 460,815-byte PPM SHA-256 was
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`,
identical to run18's seed: 153,472 black pixels, 128 white seed pixels, no
other colors, zero changed pixels, and zero live-scanout writes.

## Run20 real-firmware evidence

Run20 used exact source commit
`590d2248af4d7e5e92ec7bbd1be079c3bb415542`. Unlike run19, it did not reach the
configured 2.5 B cap: it exited 9 at 1,937,979,818 instructions on an explicit
undefined-instruction stop. Before that stop, the original source inputs
reverified unchanged:

```text
kernel.macho    0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C
devicetree.bin  4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57
rootfs.img      C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82
```

The external-md bridge reported zero failures. The 466,825,216-byte work image
remained inside the retained run directory `work/run20-tvout-timing`, measuring
447.18 MiB on F:, and the
guest-free low-water sample was 13,250 pages, 51.76 MiB, at instruction
1,937,571,840.

The corrected completion path and downstream checkpoints were:

```text
TV-out IRQ 30 filter entry                         1,894,168,651
TV-out IRQ action entry                            1,894,171,336
IOMFB close sleep-gate return                      1,894,175,066
IOMFB close epilogue                               1,915,251,328
IOServiceClose PID 20 user return, r0=0            1,915,263,517
UIKit startWindowServer return                     1,919,831,289
SpringBoard applicationDidFinishLaunching: entry  1,923,358,329
VFP decoder stop                                   1,937,979,818
```

The TV-out model counted 4 frames, and the shipped filter/action cleared the
work and woke the gate. The primary display decoded as 320x480 with stride
1280. These are runtime confirmations of the `590d224` correction, not visual
evidence.

The `UIController` call had zero hits. The 460,815-byte PPM retained SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`,
the run18/run19 seed: 153,472 black pixels, 128 white seed pixels, no other
colors, and **0 changed pixels**. The live observer likewise recorded zero
overlapping or RGB-visible scanout writes. Therefore SpringBoard is **not
rendered**, even though its application delegate was entered.

The terminal word was `0xEE274B10` at userspace PC `0x33acca88`, resolved under
PID 20 to libm `_fmod+0x1a8`. VFP11 names it `FMDHR d7, r4`; modern tools may
print `VMOV.32 d7[1], r4`. It is a VFPv2 32-bit transfer into the high word of
`d7`, not an Advanced SIMD/NEON lane operation.

## Run21 real-firmware evidence

Run21 used exact source commit
`debec04ff9b0faa469d5ad2ee7d75d1bf3b53b1a`. It exited **0** at the configured
**2,500,000,000-instruction cap**, rather than stopping at run20's
1,937,979,818 boundary. The replay therefore advanced
**562,020,182 instructions beyond** the former stop and firmware-validates the
VFP11 high-word transfer correction in libm `_fmod+0x1a8`.

The immutable source inputs reverified unchanged:

```text
kernel.macho    0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C
devicetree.bin  4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57
rootfs.img      C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82
```

The external-md bridge reported **0 failures**. Guest free memory reached a
low of **50.63 MiB**, and the retained run directory occupies **447.27 MiB on
F:**.

SpringBoard entered `applicationDidFinishLaunching:` at the same exact
1,923,358,329 checkpoint. `-[SBTetherController isTethered]` returned from
`0x967ba` to `0xa72c` at **1,924,647,850**, then took the false branch
`0xa730 -> 0xa74c`. SpringBoard continued through
`loadDebuggingAndDemoPrefs`, `_initLockButtonBearTrap`, and
`SBPlatformController`, disproving `isTethered` as the later frontier.

It then entered `+[SBTelephonyManager sharedTelephonyManager]` at
**1,965,837,070** and `-init` at `0x28240`. PID 20's last exact-attributed user
instruction was **1,966,242,080**. Its thread switched out at
**1,966,246,193** inside a shared-cache `mach_msg`, before returning to
`0xa77d`, while other guest work continued to the cap.

Post-run shared-cache resolution and message-episode decoding remove the
service ambiguity. `_CTTelephonyCenterGetDefault` creates a
CTServerConnection. The bootstrap lookup for the literal
`com.apple.commcenter` succeeds and returns port name **0x4f07**. The initial
generated handshake enters at **0x30a1177c** and calls `mach_msg` at
**0x30a117e0** with request ID **0x0054b557**, send size **0x834**, and receive
size **0x30**. SpringBoard blocks before **0x30a117e4**. The generated stub does
not initialize `msgh_size` or `reserved` before the call, so the observed
header size **6** is stale stock stack state rather than emulator corruption.

These facts prove the service identity and initial handshake boundary. They do
not prove a reply, deadlock, queue-full condition, or causal link to the absent
baseband. The next diagnostic gate is to trace the `ipc_mqueue_send` receiver
and queue state, then correlate CommCenter PID 24's wait state with the observed
baseband/SPI behavior. Since this synchronous stock path gates UI startup, the
minimum graceful no-modem behavior needed to continue boot is part of M5; full
telephony, SIM, and cellular operation remains outside that boot gate.

The presentation result did not improve. `UIController` had **0 hits**, the
live-scanout observer recorded **0 mutations**, and the PPM remained the exact
seed with SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`
and **0 changed pixels**. SpringBoard is therefore **not rendered**.

Run21 belongs only to `debec04`. Exact-commit hosted
[core run 30095081111](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081111)
and
[unsigned iOS run 30095081184](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081184)
also passed. Test-only commit `0670ab8` later passed
[core run 30096115501](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30096115501)
and
[unsigned iOS run 30096115527](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30096115527),
with VFP **469/0**. Latest hosted test-only commit `657e8d8` expands the local VFP
result to **488/0** and passed hosted
[core run 30097023293](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023293)
and
[unsigned iOS run 30097023356](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023356).

## Run22 real-firmware evidence

Run22 used exact source commit
`40209b27cb10d01c552398ff918ee613c4908ed0` and an exact copied binary with
SHA-256
`D3B9C9BF543409C9EDC95C1A1233B000D0B6C42AF5C132B69B47C19207196614`.
It cold-booted the display-enabled 128 MiB configuration, exited **0** at the
configured **2,100,000,000-instruction cap**, and produced empty stderr.

The exact initial CoreTelephony request was copied into kernel memory with ID
`0x0054b557` and destination port object `0xc0d705a0`. At
**1,966,245,348**, `_ipc_mqueue_send` entered with embedded mqueue
`0xc0d705b8`, `msgcount=5`, `qlimit=5`, `seqno=0`, and `fullwaiters=0`. It
then recorded:

- the queue-full slow branch at **1,966,245,373**;
- the pre-store `fullwaiters=1` site at **1,966,245,387**;
- `_thread_block_reason` at **1,966,245,550**; and
- the SpringBoard thread switch-out at **1,966,246,193**.

The sender did not resume before the cap. This is direct evidence of queue
saturation at entry and a later block, but the route recorder retained only
`r0-r4`; exact send-path binding requires `r4=mqueue` and `r8=kmsg`. The two
route-PC observations are therefore adjacent candidates, not authoritative
proof that this same kmsg took the named path. `msgcount` also includes slots
reserved by in-flight senders, so it is not evidence that five kmsgs are
linked. The committed run22 owner decoder read the port's `+0x3c` union without
first validating `ip_receiver_name`; its printed PID-1 receiver is not accepted
as authoritative. The next probe must require an active nonzero receiver name,
validate the copied-in kernel header, bind exact route registers, walk
reciprocal queue links, and distinguish linked entries from reserved slots.

CommCenter PID 24 retained its exact process identity, but run22's last
scheduled worker repeatedly issued Mach ID `1000` at roughly
412-million-instruction intervals before a semaphore wait. This is consistent
with a periodic `clock_get_time` worker, but the destination service was not
resolved and the old counter did not require an outbound send. It is therefore
a candidate, not evidence that the service/bootstrap receive thread caused the
stall. Per-thread trap, continuation, waitq, semaphore, and schedule state must
be retained before any service-thread or baseband claim.

The presentation result remained negative: zero `UIController` hits, zero
live-scanout mutations, and zero changed pixels. The PPM remained the seed,
SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`.
The original inputs reverified unchanged:

```text
kernel.macho    0d8cdb339d37cf37a1db2638fff79272ecd63a17764bf7666efa1618725df70c
devicetree.bin  4867c95fedf544bda2ecaa2626ae14c01a60d7771dc53ffe6fd3a6aac8b8ba57
rootfs.img      c3251e7f092c939d5818e92086cb47680981cfb03731de7b55d238c942eb5e82
```

The external-md bridge reported **0 failures**, guest-free memory bottomed at
**50.63 MiB**, and the retained evidence directory occupies **447.42 MiB on
F:**. Hosted
[core run 30106957804](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30106957804)
passed all eight jobs for the exact commit: warnings-as-errors, ASan+UBSan,
Windows/Linux/macOS builds and tests, and Ubuntu/macOS JIT execution. Hosted CI
does not contain the private firmware and does not establish this boot result.

## Pre-Run23 diagnostic validation

Exact diagnostic commit
`5a40c5eec5bbf7c4b7d8909d0c1f364bc078338a` adds diagnostic evidence only; it
does not change emulated baseband/GPIO/SPI behavior and has not yet supplied a
long-run firmware verdict. Its startup self-checks adversarially reject the
failure modes exposed by the run22 audit:

- ownership requires a complete copied-in kmsg, coherent
  `mqueue == port + 0x18`, active `IOT_PORT`, authoritative receiver-name
  union state, readable live space/task/proc/PID chain, and exact destination
  and request ID;
- exact reserved-slot arithmetic requires a readable, closed, unique circular
  queue with reciprocal links, complete same-port nodes, valid
  `msgcount <= qlimit`, and no truncation or read fault;
- route hits require the same kmsg in `r5/r6` on post paths or `r4/r8` on send
  paths; rejected candidates and saturated counters remain separate;
- each CommCenter wait is bound to one thread, trap, semaphore attempt, block,
  committed snapshot, and verified switch-out. Later execution, switch-in,
  return, continuation, identity uncertainty/reuse, or sequence exhaustion
  invalidates an unresolved-block classification. The report proves “no resume
  observed,” not that an off-CPU thread remained enqueued at the cap; an
  asynchronous wake followed by no scheduling is still possible;
- AppleBaseband reset dispatch, IOKit interest registration, notifier
  lifetime, kernel-message contents, route kmsg, live port ownership, and
  exact CommCenter receiving thread must form one nested-frame-safe,
  same-retained-event causal chain. The final adversarial pass rejects a linked
  handler from one event combined with an unlinked send/route from another,
  repeated-send inheritance, stale/ring-overwritten dispatch sequences, and a
  failed later route candidate overwriting a prior bound kmsg. Restored history,
  teardown, pointer reuse, overflow, and mismatched frames are uncertainty, not
  evidence.

Root validation used F:-local temp storage and passed a strict
`-Wall -Wextra -Werror` syntax check, the `bootkernel` target build,
`git diff --check`, and a zero-instruction execution against the exact 7E18
kernel and device tree. The ownership, wait, and AppleBaseband discovery gates
all printed `VALIDATED`; the harness printed
`stopped after 0 instructions: OK`. These results establish that the
diagnostics and their exact byte/data gates accept the supported firmware.
They do **not** establish any runtime event, CommCenter reply, baseband
causality, SpringBoard progress, pixel mutation, or on-device behavior.

The tracked `tools/run23-cold-replay.ps1` launcher also passes PowerShell parser
validation and an isolated F:-local fail-closed smoke. A synthetic all-zero
source commit was rejected before firmware or the copied binary was opened; the
wrapper exited **99** and created its standard exit, error, end-time, and
launcher-log evidence files. This tests the failure contract only. Run23 remains
unlaunched.

Hosted validation belongs to that same exact commit:

- [core run 30143448600](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143448600)
  passed all eight jobs: Windows/Linux/macOS builds and tests, three JIT jobs,
  ASan+UBSan, and warnings-as-errors;
- [iOS run 30143455036](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143455036)
  passed its unsigned arm64 package job.

These hosted jobs contain no private firmware. They validate the public
build/test/package surface, not Run23, SpringBoard rendering, or device launch.

## Local edge coverage of the surgical correction

### TV-out MMIO and interrupt behavior

- All three independent 4 KiB banks are present at `0x39100000`,
  `0x39200000`, and `0x39300000`, with exact window names and sizes.
- Reset exposes hardware-derived ready bit 1 while stopped, ignores guest
  attempts to store that bit, and preserves unrelated control bits such as the
  mixer's bit 2.
- Byte, halfword, and word accesses use little-endian lanes. An unaligned
  halfword crossing two backing words round-trips, while an access crossing a
  4 KiB bank boundary is rejected and counted as unmapped.
- The real run19 state control/mixer/SDO `0/5/1` is timing-active. Control may
  independently transition between run and ready without resetting phase;
  stopping mixer or SDO resets phase.
- SDO pending bit 0 latches, its mask gates the level without destroying
  pending state, and a byte write-one-to-clear acknowledgement deasserts it.
  Timing-gate transitions do not silently consume a latched completion, and
  restart does not manufacture an immediate stale VBlank.
- Large tick intervals preserve the number of elapsed boundaries and residual
  phase. VIC0 line 30 reaches the CPU; mixer acknowledgement does not fabricate
  IRQ 38.
- WFI advances to the next deliverable TV-out boundary without adding retired
  instructions, and existing earliest-edge behavior remains covered against
  timer and CLCD activity.

### Snapshot format and malformed state

- Snapshot format v4 serializes all three TV-out register banks, frame period,
  residual phase, and frame counter; the live `0/5/1` state with nonzero phase
  must round-trip.
- Invalid `frame_accum >= frame_ticks`, residual phase while mixer+SDO timing
  is stopped, a stored hardware-owned ready bit, unsupported SDO status bits,
  and nonzero nonasserting mixer status are rejected. Control-ready with live
  mixer+SDO phase is valid.
- Every malformed save case starts with non-null output sentinels and proves
  that failure returns no allocation and a zero length.
- A legitimate pending VSYNC may remain latched while stopped until explicit
  W1C and is accepted. Version mismatch, malformed structure, checksum, and
  transactional-load behavior remain covered by the broader snapshot suite.

### Framebuffer planning and CLCD bounds

- The external-md startup invariant requires the 320x480x4 framebuffer range
  `0x0885c000..0x088f2000`, advances physical `topOfKernelData` to the next
  16 KiB boundary at `0x088f4000`, and preserves at least `0x11000` bytes of
  bootstrap headroom.
- CLCD seeding validates `stride * height` in 64-bit arithmetic, its 4 KiB
  round-up, the driver's 32-bit allocation-size limit, and the complete
  physical end address.
- Tests cover multiplication overflow, page-rounding overflow, padded final
  stride, a physical span crossing 4 GiB, and the valid boundary case ending
  exactly at 4 GiB. Rejection is atomic: no controller field changes.

### Diagnostic integrity

- Firmware-free startup self-checks exercise the framebuffer/headroom constants
  and TV-out completion-chain classifier tables.
- The TV-out report reads machine state directly rather than generating device
  reads, so enabling diagnostics cannot acknowledge or manufacture an
  interrupt.
- Format-heavy diagnostic paths received targeted compiler and analyzer
  checks, followed by zero-step `bootkernel` and `snapboot` executions.

## VFP11 decoder correction and later test-only coverage

The local decoder now distinguishes the VFP11 cp11 word-transfer encodings from
the genuinely NEON-only scalar forms. It accepts the low/high 32-bit halves of
`d0..d15` (`FMDLR`/`FMDHR` and `FMRDL`/`FMRDH`), including the exact run20
`0xEE274B10`, while retaining fail-closed behavior for 8/16-bit lanes,
`d16..d31`, and malformed/reserved forms. The focused local binaries report:

```text
test_vfp  452 passed, 0 failed
test_arm  810 passed, 0 failed
test_jit  347 passed, 0 failed
```

The exact `debec04` source tree passes the complete local Release suite 23/23 and focused
`-Wall -Wextra -Werror` builds. The privilege regressions also prove that
user-mode FPEXC access enters the guest Undefined vector with state unchanged;
it cannot escape as a fatal emulator status. Hosted core/iOS workflows passed
for that exact commit, and run21 cleared the exact firmware stop to the 2.5 B
cap. The later test-only `0670ab8` exact-path regression passed hosted CI with
VFP 469/0. The latest hosted `657e8d8` helper-sequence expansion passes VFP 488/0
locally and passed hosted core/iOS runs 30097023293/30097023356.

## Reproducing the public subset

The public suite needs no Apple firmware:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The two focused device/state tests can be reproduced with:

```sh
ctest --test-dir build -C Release \
  -R "^(s5l8900_machine|snapshot)$" \
  --output-on-failure
```

The separately recorded preflight can be run with:

```sh
ctest --test-dir build -C Release \
  -R "^external_md_cli_preflight$" \
  --output-on-failure
```

Assertion counts are useful evidence for one revision, not a permanent API.
When tests change, the current executable output and hosted workflow logs are
authoritative.

## Promotion gates

- [x] Hosted core tests, strict warnings, sanitizers, JIT jobs, and the unsigned
  iOS build passed for the pre-run19 baseline
  `afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9`.
- [x] Run19 showed all three TV-out pages reaching the model, validated the
  corrected framebuffer/TOKD layout, and captured the exact `0/5/1` state.
- [x] The surgical mixer+SDO predicate correction passes the local full suite
  **23/23**, `test_soc` **5,504/0**, and `test_snapshot` **469/0**.
- [x] Hosted
  [core run 30091220128](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220128)
  and
  [iOS run 30091220122](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30091220122)
  passed exact surgical-fix commit `590d224`.
- [x] Run20 produced 4 TV-out frames, asserted IRQ 30, entered the shipped
  filter/action, cleared the swap work, woke the gate, and returned the exact
  `IOServiceClose` to PID 20 with `r0=0`.
- [x] Run20 returned from `startWindowServer` with a decoded 320x480 display and
  entered SpringBoard `applicationDidFinishLaunching:`.
- [ ] SpringBoard reaches the `UIController` call and produces recognizable
  live CLCD scanout. Run21 still had zero hits and zero changed pixels.
- [x] The local VFP11 correction passes the targeted VFP/ARM/JIT binaries.
- [x] The current VFP11 correction passes the full local Release suite and
  focused strict builds.
- [x] Hosted
  [core run 30095081111](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081111)
  and
  [iOS run 30095081184](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30095081184)
  pass exact VFP11-correction commit `debec04`.
- [x] Run21 clears `0xEE274B10` in libm `_fmod+0x1a8` and reaches the clean
  2.5 B cap, 562,020,182 instructions beyond run20.
- [x] Run22 proves that the initial copied-in CoreTelephony request reaches a
  saturated `msgcount=qlimit=5` mqueue and that its sender later blocks without
  resuming before the 2.1 B cap; its full-path PCs remain unbound candidates
  because the old recorder omitted the decisive kmsg register.
- [x] Hosted
  [core run 30106957804](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30106957804)
  passes all eight jobs for exact run22 commit `40209b2`.
- [x] The pre-Run23 trace-only ownership, queue-link, per-thread wait, and
  AppleBaseband causal observers pass strict compilation, adversarial startup
  self-checks, and exact 7E18 zero-step code/data gates.
- [x] Exact diagnostic commit `5a40c5e` passes all eight jobs in
  [core run 30143448600](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143448600)
  and the package job in
  [iOS run 30143455036](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143455036).
- [x] The exact committed Run23 cold replay ran to a clean 2.1 B cap at commit
  `777afb4` and established which runtime chains occurred: both send routes are
  **BOUND**, the queue holds **five linked** identical `0x0054b557` kmsgs with
  **zero** reserved slots, and the SpringBoard sender's episode ended
  `OPEN/UNRESOLVED` with no resume observed.
- [x] The active receive-right owner, linked queue entries versus reserved
  slots, CommCenter service thread, and AppleBaseband notification route are
  correlated without union or process-wide-thread assumptions. The owner is
  **launchd (PID 1)** through a validated `ip_receiver_name` discriminator and
  full object graph; CommCenter is PID 24 with six retained threads in
  unresolved waits; and the AppleBaseband route does not exist because the
  reset callback never fired.
- [ ] The reason CommCenter has not checked in for `com.apple.commcenter` is
  identified from exact guest code and runtime evidence, and the relationship
  between CommCenter's blocked receive port `c0dd99d8` and its AppleBaseband
  interest port `c3c59ab0` is established.
- [ ] The shipped AppleBaseband reset event source is resolved to its exact
  trigger, and whether faithful no-modem hardware would fire it is decided from
  the binary rather than assumed.
- [x] The identified `0x3d200000` BasebandSPI register window is named. All
  three device-tree-confirmed SPI windows are now declared stubs; exact
  disassembly of `BasebandSPI+0x1d42` shows the driver stores the four
  registers it reads back into a transfer descriptor without testing or polling
  them, so honest storage is the faithful answer and no autonomous state is
  fabricated. A device model is still required if a later run shows the driver
  waiting on a bit it does not itself write.
- [x] Test-only `0670ab8` passes hosted core/iOS CI with VFP 469/0.
- [x] Latest hosted test-only `657e8d8` helper coverage, VFP 488/0 locally, passes
  hosted
  [core run 30097023293](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023293)
  and
  [iOS run 30097023356](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30097023356).
- [ ] An installable build is tested separately on the iPhone 6s Plus.

The honest claim remains narrow: run20 proves the TV-out correction, run21
firmware-validates the VFP correction through a clean 2.5 B cap, run22 proves a
saturated Mach queue at the initial CTServerConnection send and a subsequent
unresolved sender block, and run23 binds that route, walks the queue, and names
its owner. None renders SpringBoard. What run23 closed is the measurement
question; what it opened is a cleaner engineering question — why the service
has not taken its own port. UI readiness, pixels, touch, and on-device
behavior all remain untouched.

## Run23 real-firmware evidence

Run23 used exact source commit
`777afb4c2350690ecd40cd9e69d12e3967a227cb`, a docs-only descendant of
hosted-green `5a40c5e` whose `CMakeLists.txt`, `core/`, and `tools/` inputs are
byte-identical to it. The tracked launcher
(`tools/run23-cold-replay.ps1`, SHA-256
`FF29B15C09AAEF8EAAC461C5503CC04C4586EA0D52AB08C9B3CA25B174D6596D`) verified
HEAD, refused any drift beneath `CMakeLists.txt`/`core`/`tools`, and pinned the
copied binary at 625,423 bytes, SHA-256
`978987BE339A7C11A3A3CBB87CBE28DB450518AD3AEB8C7CE5E5A6558ACAD67E`.

The 128 MiB display-enabled cold boot exited **0** at the configured
**2,100,000,000-instruction cap** in **1,434.86 seconds**, with empty stderr.
Postflight re-verified the immutable inputs:

```text
kernel.macho    0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C
devicetree.bin  4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57
rootfs.img      C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82
```

### The three run22 candidates that became results

1. **Route binding.** Both send-path PCs are now `BOUND` with the decisive
   register pair `r4=c0d705b8` (mqueue) and `r8=c3d3c000` (kmsg): the
   queue-full slow branch at `c00147ba` @1,966,245,373 and the `fullwaiters=1`
   pre-store at `c00147d6` @1,966,245,387. One candidate each, one bound each,
   zero rejected.
2. **Linked versus reserved.** The bounded reciprocal walk closed on a
   consistent, untruncated, fault-free ring of **five** kmsgs with
   `reserved-or-in-flight=0`. Every entry carries request ID `0x0054b557` and
   size 2,104 to destination `c0d705a0`, with five distinct reply ports
   (`c2bf6ea0`, `c34d2630`, `c2d33d80`, `c31c32d0`, `c31c3cf0`). SpringBoard's
   own message is the sixth against `qlimit=5`.
3. **Ownership.** `ip_receiver_name=0x1b03` is validated before the union is
   read; the space `c0acfe60` is active, task `c0ad7b10`'s space backpointer
   matches, and proc `e0381d68` yields signed **PID 1**. The decode prints
   `AUTHORITATIVE`. launchd holds the receive right.

### The baseband hypothesis, in its delivered form, is retired

The observer's frontier line is explicit:

```text
frontier: event-source enable call was entered, but no reset callback was
          observed since trace start
```

AppleBaseband object `c0c3a700` located its reset platform function
(`c0b6b020`) and created, committed, and enabled an event source
(`c0b6c340`) — then the reset callback never ran. Reset reads, state changes,
`messageClients` dispatches, notification handlers, Mach sends, and queue
routes are all **0**. No baseband notification was delivered, so a delivered
notification cannot be the cause of the saturation.

The same observer produced the run's most suggestive correlation. Of 14
`registerInterest` wrappers, 13 were service-rejected and exactly one accepted:
CommCenter (task `c2ca2760`, proc `e037f890`, PID 24) subscribing to
AppleBaseband on port `c3c59ab0` through thread `e02f5888` at
**931,584,215**. That same thread blocked in `_ipc_mqueue_receive` at
**932,507,189** and was never observed running again. This is a temporal and
identity correlation, not a causal proof: the blocked receive is on port
`c0dd99d8` (task-local name `0x10004001`), not on the interest port, and no
port-set relationship between the two was established.

### What remains unproven

No dequeue, reply, permanent deadlock, or reason for CommCenter's silence is
established. "Last observed unresolved block; no resume observed" is not "still
enqueued at the cap" — no final live wait-state reread exists. The 16
exact-hook attribution omissions and 50 unreadable classifications are reported
rather than hidden; none overlaps the decisive send episode. And the
presentation result did not move: `UIController` **0** hits, live scanout
**0** mutations, and the 460,815-byte PPM byte-identical to the seed at
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB` with **0
changed pixels**. SpringBoard is **not rendered**.

Run23 belongs only to `777afb4`. Its build inputs are identical to `5a40c5e`,
which owns hosted
[core run 30143448600](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143448600)
and
[iOS run 30143455036](https://github.com/j0shua-SYSON/S5LBox/actions/runs/30143455036);
hosted CI contains no private firmware and establishes nothing about this boot.

## 2026-08-12: 2 GiB migration for existing Cydia guests

The initial 2 GiB builder fixed fresh guest installations but did not repair a
machine that already carried the strict v1 install marker: that marker returned
idempotent success before disk capacity was inspected. The current change adds
a second, disjoint disk-replacement journal for capacity maintenance. The v1
install marker remains continuously authoritative while the live image is
cloned, grown, flushed, and atomically replaced. Boot recovery processes this
storage journal first because both transactions share the live image path.

The artifact-backed migration test used a retained, real HFSX work image with a
valid committed install marker. It produced:

| Evidence | Before | After |
|---|---:|---:|
| Host image bytes | 500,375,552 | 2,147,483,648 |
| HFS allocation blocks | 122,162 | 524,288 |
| HFS free blocks | 10,217 | 412,331 |
| HFS free bytes | 41,848,832 | 1,688,907,776 (78.65%) |

The post-migration primary header is `HX`, allocation block size 4,096, and its
`totalBlocks * blockSize` exactly matches the host file length. The builder ran
525 checks with zero failures and a second invocation performed no rewrite.

The follow-up capacity test did not stop at the header. It cloned the migrated
image again and provisioned three 16,777,216-byte files, consuming 12,288
allocation blocks. That is greater than the old volume's entire 10,217-block
free pool. The read-only catalog walker found their extents at blocks
`122174+4096`, `126270+4096`, and `130366+4096`; all three begin beyond the old
volume's 122,162-block boundary. Extracting the first complete 16 MiB file
produced SHA-256
`789AE1E03031156AB4FB0C19892B9197F8097298F66876E59CF1A63DC6CC0DB2`,
exactly matching the deterministic input payload. This proves that the grown
allocator writes and reads newly added space rather than merely advertising it
in the volume header.

Content-preservation evidence was extracted through the read-only HFS catalog
walker before and after migration:

```text
s5lbox-guest-install                 7F825FDBCD2E806D803966A3D4DD89AC79B0C4CAA85968F98AE9B950B1A15DDB
cydia_1.0.3044-66_iphoneos-arm.deb  94769B67E88198012CD1E45163F2F8BD949B4AA927DAB1503A03D62A8EE3DBA9
guest.jailbreak-v1                  57AC94CC21F0F03B2A91284AFCB7C884AF928779F0A127E2395A5DBCA8D6C2E1
```

Each hash matched before and after. The byte-sized transaction suite also
interrupts after journal publication, old-live preservation, new-live
publication, and commit-record publication; every boundary recovers to the new
disk while preserving the original install identity. The complete normal and
static/JIT-gated local suites pass 67/67 and 72/72 respectively.

The exact `96894c9` IPA was then installed on a physical iPhone and exercised
against a retained 500,375,552-byte committed installation. The first attempt
from an ordinary saved session refused with `source volume was not cleanly
unmounted`. That was not a false positive: both on-disk HFS volume headers had
attributes `0x00000000`. A saved emulator checkpoint freezes a live mounted
filesystem; it does not perform the guest's unmount protocol.

The guest was reopened, powered off through its own long-press and slide UI,
and allowed to reach `reboot(RB_HALT)` / PMU standby. Both HFS headers then had
attributes `0x00000100` (cleanly unmounted). Leaving the stopped machine and
retrying the same operation grew the live image to exactly 2,147,483,648 bytes,
published the 112-byte `guest.storage-v1` marker, preserved the existing install
marker byte-for-byte, removed the one-shot resume request, and cold-booted the
guest. This is physical evidence for the migration transaction, not merely a
host-side image test.

The current, not-yet-installed follow-up adds a read-only HFS preflight before
`guest.storage-v1.stage` can exist. Its portable builder regression starts from
a valid committed install, presents a dirty primary and alternate HFS header,
and asserts the shutdown-required result, no staging progress, a byte-identical
live disk, unchanged install authority, and absence of every storage stage,
backup, journal, temporary marker, and committed marker. The copy-time validator
still repeats the checks, so a source race remains fail-closed. Automatic repair
is deliberately absent: guessing repairs on a mounted, unjournaled HFS image is
not a safe migration strategy.

Cydia did complete its first-run `Reorganizing` phase and respring on the grown
physical guest. Its next launch then failed during archive cleanup with
`Could not open lock file /var/cache/apt/archives/lock - open (13 Permission
denied)`. Capacity was therefore fixed, but a real package install remained
blocked. The next section replaces the earlier vague “privilege or sandbox”
description with the first byte-backed cause; it does not yet prove that this is
the only cause.

## 2026-08-12: versioned repair for the older Cydia permission defect

The retained physical guest's own log contained three permission failures:

```text
mktemp: failed to create directory via template `/var/stash/Applications.XXXXXX': Permission denied
cp: cannot create directory `': No such file or directory
E:[Could not open lock file /var/cache/apt/archives/lock - open (13 Permission denied)]
```

Read-only catalog inspection then found the decisive precondition. The guest's
`/Applications/Cydia.app/Cydia_` was uid 0, gid 0, mode `0100755`. Cydia is
launched as mobile, so without the package's setuid/setgid bits it never becomes
root and cannot write `/var/stash` or `/var/cache/apt/archives`. This guest was
created by `069659d`; the explicit first-boot `chown 0:0`, `chmod 6755`, and
setuid/setgid verification arrived later in `330a614`. The old v1 install marker
made subsequent builds idempotent, so the later repair script never migrated
that already-committed machine. This is a versioning defect, not evidence that
the current fresh-install provisioner dropped the bits.

The executable identity was checked from both sides. Extracting `Cydia_` from
the exact pinned `cydia_1.0.3044-66_iphoneos-arm.deb` and extracting it from the
physical guest each produced 320,704 bytes and SHA-256:

```text
4CA3F70FE5CB67737688AB0614C686FE18B124844369848B76846F80A52F6324
```

The implemented migration is intentionally much narrower than a general image
`chmod`:

- the absolute path, regular-file type, 320,704-byte size, and SHA-256 must all
  match;
- current BSD metadata must be exactly root:root `0755` (repair needed) or
  root:root `06755` (already satisfied); any third tuple refuses;
- only owner, group, and mode fields change in an unpublished HFS clone;
  contents, CNID, extents, timestamps, Finder metadata, and resource fork do
  not change;
- `guest.cydia-privileges-v1` has its own backup, stage, journal, temporary
  records, and commit marker because disk capacity and executable privileges
  are independent migrations;
- storage growth and permission repair can share one clone, while dynamic boot
  recovery first resolves whichever maintenance journal currently owns the
  temporarily missing live-disk pathname. Two active maintenance journals fail
  closed.

Portable evidence is green: the catalog suite passes 861 checks including
wrong hash, wrong metadata, missing path, immutable source, and independently
read root:root `06755`; the transaction suite passes 255 checks including all
four durable rename boundaries and both active-maintenance recovery orders. An
exact-binary integration built a legacy root:root `0755` file into a retained
2 GiB HFSX image, ran the real builder, published one repair, independently
re-probed the live result as satisfied, and then proved the next builder call
idempotent: 2,126 checks, zero failures. Full normal and static/JIT-gated suites
pass 67/67 and 72/72.

The exact `aa87afd` iOS artifact was then installed on one physical iPhone 6s
Plus running iOS 15.8.5. After the guest completed its own shutdown, the same
retained machine grew to exactly 2 GiB and published the exact privilege marker.
Cydia completed first-run reorganization, refreshed the manually entered
official `http://apt.saurik.com/cydia/` source, and reported five available
essential upgrades. Those upgrades were deliberately not applied: this guest's
bootstrap is version-pinned for iPhone OS 3 compatibility, and a blanket upgrade
would be a different, destructive experiment.

The smaller `adv-cmds` transaction did complete. Cydia downloaded 61.3 kB,
reported 536.0 kB of disk use, selected and unpacked
`adv-cmds_119-6_iphoneos-arm.deb`, and printed `Setting up adv-cmds (119-6)`.
Returning to its package page changed the action from `Install` to `Modify`.
No `EACCES` or no-space error appeared. That is end-to-end physical evidence
for guest PPP/NAT, the official package archive, APT download, dpkg unpack and
configure, the executable privilege repair, and usable grown storage.

Brutal-honesty boundary: this is one small package transaction on one test
device, observed through Cydia's completion UI; it is not broad package
compatibility, a post-run catalog extraction, or proof that applying the five
bootstrap upgrades is safe. No kernel MAC-policy bypass was added.

## 2026-08-12: official source seed and powered-down checkpoint recovery

Fresh guest rootfs plans now create `/private/etc/apt/sources.list.d/saurik.list`
as exact root:root `0644` data containing:

```text
deb http://apt.saurik.com/cydia/ ./
```

The plan identity was advanced and includes those source bytes. Synthetic
metadata/content checks pass, all 28 real pinned packages merge into a 567-entry
plan, and applying that plan to the exact stock rootfs produced a 466,825,216-byte
HFS image with 556 new and 11 safely reused entries. This seeds only newly
prepared guests. It does not silently modify an already committed installation;
the physical transaction above used a source entered manually.

A separate physical edge case exposed a lifecycle mismatch. After the guest
printed its real shutdown path and set `OOCSHDWN.GO_STANDBY`, the emulation
engine still described itself as running because the powered-down CPU
deliberately loops after quiescing. Saving and reopening that state therefore
restored the same black, non-progressing loop.

The first proposed repair treated the checkpoint as retained-RAM sleep. It also
fixed two real omissions: PCF50635 interrupt events now latch even while masked,
its active-low `INT_N` output drives GPIOIC line 85, and level relatching obeys
the GPIO `INTEN` bit. Exact physical replay reached and returned from the XNU
GPIO handler without livelock. It still did not re-enable the PMU child or CLCD
after another 2,000 M instructions (about 4.85 guest seconds). A separate forced
release of the saved host Power state after 40 M instructions, followed by 500 M
more, also failed: PMU `INT2` remained `0x01`, GPIO line 85 remained disabled,
and the CLCD frame counter remained frozen. The retained-RAM theory is therefore
rejected evidence, not a shipped success claim.

The implemented recovery loads and validates the snapshot and external-media
sidecar, recognizes `GO_STANDBY`, preserves the clean 2 GiB work image, discards
the powered-off CPU/RAM, rebuilds the machine, reapplies the validated engine
controls, and performs normal kernel bring-up. Ordinary running checkpoints
still restore exactly. Normal and static/JIT-gated portable suites pass 67/67
and 72/72. Both the
[iOS build](https://github.com/j0shua-SYSON/S5LBox/actions/runs/31567644467)
and [core tests](https://github.com/j0shua-SYSON/S5LBox/actions/runs/31567644466)
completed successfully for exact commit `9f3d107`.

That exact artifact was installed on one physical iPhone 6s Plus running iOS
15.8.5. A real powered-off checkpoint was consumed once and reported `fresh
boot after powered-off checkpoint`; its saved powered-off CPU state was not
resumed. The fresh path reached the lock screen at about 3,384 M
instructions, accepted slide-to-unlock, and reached the home screen. Back then
atomically published a new checkpoint and marker. Reopening consumed the marker,
reported an exact restore at 3,926.8 M instructions, and immediately displayed
the same unlocked home screen rather than replaying boot.

Brutal-honesty boundary: this is one powered-off recovery and one ordinary
resume on one physical device, not broad lifecycle soak coverage. The fallback
is a cold kernel boot and can take substantially longer than ordinary resume.
The frontend also still lacks a distinct powered-off status: until the user
leaves or presses Power, a shut-down guest is represented by a running engine.
This patch fixes reopen behavior, not that larger UI-state model.
