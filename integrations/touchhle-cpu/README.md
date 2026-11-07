# touchHLE integration

This crate exposes S5LBox's ARMv6 user-mode interpreter to Rust. It executes
ARM and Thumb instructions without generating host code or requesting
executable memory. The existing full-system emulator remains unchanged.

`Cpu` owns one interpreter on one worker thread. A `Memory` provider supplies
bounded reads and writes during execution; its callbacks cannot unwind into C.
Execution returns on budget exhaustion, SVC, breakpoint, undefined instruction,
or memory failure. Thread contexts include general registers, VFP state, and
thread-local registers. Faults remain latched until a context is restored.

Run `cargo test --manifest-path integrations/touchhle-cpu/Cargo.toml --release`
from the repository root. The CMake `arm_user` test exercises the C boundary.

## Using the feature build

The feature branch includes a separate-container iPhone test app, built with
`app/project-touchhle.yml`. It does not replace the normal S5LBox app or its
machines, and is not included in release 1.1.0.
The iPhone app links the optimized Rust release library. Panic unwinding is
retained so guest faults and stop requests return through the host boundary
instead of terminating S5LBox.

Use a jailbroken iPhone OS 3.1.3 guest with Packages connected. Install apps
through the existing IPA library or Legacy Store. Machine Settings → touchHLE
discovers those installations through `installation_proxy`; there is no second
touchHLE app library to maintain.

Set Up installs a small guest support package and resprings iPhone OS. Choose
**Use touchHLE** for an app, then open its ordinary SpringBoard icon. Other apps
continue to open in iPhone OS. **Use iPhone OS** restores the native choice.
If support is missing when an app is selected, setup waits for the guest to
reconnect and applies that choice automatically.
Home closes the interpreted app and returns to the guest; a failed launch
offers **Try iPhone OS** without restarting the host app.

App binaries, resources, documents and preferences use the running guest's
file service, not a host-side sandbox or a disk-image editor. Installation and
updates still belong to the guest's normal installer. No per-app re-signing or
executable-memory allocation is introduced by the interpreter.

## Verified behavior

On 2026-10-20, the integrated build passed CI and the following path passed on
an A9 iPhone running iOS 15.8.5 with a freshly jailbroken 3.1.3 guest:

- Install the ARMv6 `HLEVisual` fixture through virtual USB and discover it.
- Install guest support through the package helper and launch with native
  graphics and touch input.
- Save a document in touchHLE, read and change it from the native guest app,
  then read the updated value in touchHLE.
- Reject a touchHLE launch while the native app is running, then allow it
  after the native app exits.
- Return with Home and preserve preference writes made by termination
  callbacks across both runtimes.
- Save/close the guest, update the host test app, resume the guest, and reopen
  the same app and saved data through a newly connected file session.
- Pause without replaying taps on resume; preserve manual pause across host
  background/foreground transitions.
- Interrupt a held button press by switching host apps, resume without firing
  the button, and accept the next ordinary tap.
- Close with Home while paused, reopen the app, and read the saved document.
- Upgrade the fixture through live IPA installation without losing its document.
- Start OpenAL playback in both the native guest and touchHLE; observe advancing
  playback positions in touchHLE after pause/resume and host background/foreground
  transitions, then close and reopen normally. This checks backend operation,
  not audible quality or audio-route/interruption recovery.
- Recover from a real app's CPU fault in the same host process and reopen the
  fixture with its saved data intact.
- Choose a runtime per app, launch through its actual SpringBoard icon, and
  retain that choice across a host update and guest save/resume.
- Recover from an unsupported Geekbench launch with **Try iPhone OS**, opening
  the native app's system-information screen in the same host process.
- Read physical accelerometer samples, stop sensing while paused, and resume
  after a host background/foreground transition.
- Leave an interpreted app open for two minutes with guest Auto-Lock set to
  one minute, then return directly to an awake SpringBoard. Guest auto-lock
  subsequently resumes after the interpreted app closes.
- Enter a multi-character string in one keyboard event; honor the text-field
  delegate's rejected edit; delete a composed accent and a joined emoji as
  whole characters; deliver editing-change actions and Return.
- Use Home while the host keyboard is visible, then launch the same app
  natively and read the exact saved UTF-8 text from its guest document.
- Remove guest support through Packages, respring, select **Use touchHLE**,
  and complete the offered setup. The app list refreshes with the original
  selection applied, without closing and reopening the screen.
- Sleep the guest with interpreted apps selected, leave it asleep for over
  one minute, then wake/unlock and launch through its icon. Background routing
  does not wake the sleeping guest.
- Install the upstream TestApp through the live IPA library, select touchHLE,
  and open its actual guest icon. Its animated menu, second window, 20 layer
  screens, eight glyph screens and seven blend-mode screens run. The blend
  colors match the displayed expectations; this is not a pixel-exact visual
  comparison of every layer and glyph case.
- Repeatedly return Home and reopen TestApp, then save/close and resume the
  guest and launch it again through its selected icon.

The keyboard checks used integrated build `15e4bee4`; the subsequent setup,
sleep/wake and upstream GUI checks used `7fe721a5`, all with guest support
package 1.1.1. All three CI workflows passed for both builds. No new host
crash report appeared during these checks. These are observations, not a
claim that every UIKit API or every app is supported.

On 2026-11-07, optimized build `c1ca099a` passed the following additional
checks on the same device and guest support version:

- Run an ARMv6 fixture with an endless CPU loop, request Home, and use Force
  Close after the graceful-close deadline. The host process stays alive.
- Immediately launch another interpreted app, then its native version, and
  read the same saved UTF-8 document. The stopped runtime releases its file
  lease; neither a host restart nor a guest reboot is required.
- Recover from Geekbench's unsupported class through **Try iPhone OS** in the
  same host process using the optimized, unwinding-enabled Rust library.
- Uninstall the hanging fixture through SpringBoard and verify that it
  disappears from touchHLE's authoritative installed-app list.

The optimized build's three CI workflows passed. Its Windows native-window
unit suite passed 62 tests; three audio tests were skipped on Windows and
tested with a null backend in macOS CI. The phone also recorded a CPU-resource
report during cold boot, with no termination action. Cold-boot app discovery
can wait over a minute for the guest USB driver. After boot and unlock, a
fresh query completed; subsequent warm queries took about four seconds.
This is a startup limitation, not a measured discovery speedup.

The pre-existing upstream Objective-C TestApp also passed **107/107 CLI
tests** through the portable interpreter on Windows. Those cover additional
Foundation, Objective-C and C-library behavior; they are desktop framework
tests, not physical-iPhone results.

The host tests additionally cover file isolation and locks, launch races,
installer validation/removal, runtime exits/faults, and audio resource cleanup.
Routing tests exercise bounded requests, stale sessions, idempotent
acknowledgements, native fallback, real file locks and unauthorized/replayed
requests. Native transport tests distinguish passive polling from connections
that are allowed to wake a sleeping guest.
Audio tests use a null output device to verify teardown, frozen playback
positions during pause, resume, and devices opened while paused. They do not
verify audible output on the iPhone.

The Mach-O loader selects ARMv6-compatible slices for apps and libraries. It
rejects ARMv7-only binaries before execution; the portable CPU does not provide
upstream touchHLE's ARMv7 execution support.
Bundled zlib, SQLite and libxml2 are rebuilt for ARMv6. The library smoke test
executes compression/decompression, database creation and queries, and XML
parsing through the interpreter. The loader follows transitive library
dependencies and deduplicates aliases before running library initializers.
The same fixture initializes the bundled C++ runtime and exercises its
allocation/deallocation entry points.

The user-supplied Geekbench 2.2.8 test reaches nib loading after the ARMv6
selection fix, on both the desktop test runtime and the iPhone. It then stops
at the unimplemented `UINavigationBar` class. This is not a supported app;
the test establishes progress past the original CPU fault, not compatibility.

Presentation ABI 2 brackets each native GLES borrow with a host graphics gate.
Context creation, destruction and frame presentation use the same gate. On
pause or loss of host activity, it excludes the worker and drains every live
context before returning; startup and cleanup also wait until the gate reopens.
Raw GL calls must not escape the GLES borrow. The device checks above exercise
the integrated lifecycle but are not a long-duration interruption stress test.

## Boundaries

Guest support currently targets the verified iPhone OS 3.1.3 SpringBoard
selectors. It never hooks the physical host's SpringBoard. Native launch
exclusion and authenticated, bounded file access are required for interpreted
execution; the host does not silently fall back to editing a mounted disk.

This remains a feature-branch build, not a claim of universal compatibility.
The device evidence uses open-source fixtures, the upstream framework demo,
and a negative real-app compatibility check, not a representative game suite.
Upstream framework gaps, ARMv7-only apps, audible quality across audio-route
changes, and host iOS versions other than the tested device must not be
presented as verified. The keyboard bridge covers insertion, deletion and
Return; full text selection, marked-text composition and secure-entry behavior
are not established by these tests.
