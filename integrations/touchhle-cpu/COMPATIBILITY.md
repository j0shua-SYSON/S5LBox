# Additional touchHLE compatibility targets

This matrix covers 20 additional utility apps and 20 additional games. It is a
validation plan, not a list of supported apps. The original 20-app investigation
remains documented in [the integration notes](README.md).

A pass requires the listed functions, readable graphics, responsive input,
sustained use without a crash or stall, and save/relaunch checks where applicable.
Record startup and interaction delays separately. Installation or a splash screen
alone is never a pass. Results apply only to the exact version and tested build.

The initial device baseline uses build `d9052b50`, an iPhone 6s Plus running iOS
15.8.5, and an offline iPhone OS 3.1.3 profile. The guest OS is not booted.
Archives and private test logs are not distributed in this repository.
The desktop startup rows use `23a5d292` with a shared OpenAL library on Windows;
they establish the next failure or a 25-second observation, not functionality.

## Utilities

| App | Version | Required functional checks | Latest result |
| --- | --- | --- | --- |
| PCalc RPN Calculator | 2.3.3 | Arithmetic, scientific and RPN operations; settings persist | Desktop startup: missing CFNumberFormatterCreate. |
| Convertbot — The Amazing Unit Converter | 1.6.1 | Convert units and switch categories; saved choices persist | Device build 39dfa4bd: passes sound initialization, then fails on missing NSNumberFormatter. |
| Units - Unit Converter for your iPhone | 1.9.1 | Convert length, mass and temperature; navigate categories | Blocked: black display after more than two minutes; input did not reveal usable UI. |
| Flashlight RGB | 1.0.2 | Change RGB controls and restore saved color | Blocked: missing UINavigationBar. |
| iHandy Level Free | 1.60.0 | Tilt response, calibration and saved calibration | Desktop startup: missing NSCalendar. |
| Clock | 2.0.2 | Clock updates, settings and timer behavior | Device build 3ffc8cb7: passes key-value getters and setters, then fails on missing change-observer registration. |
| Notes | 1.0 | Create, edit, delete and reopen a saved note | Desktop startup: missing WebPreferences. |
| ToDo | 2.0 | Create, complete and edit tasks; relaunch persistence | Desktop startup: missing NSPersistentStoreCoordinator. |
| SketchBook Mobile | 1.5 | Draw, undo, save and reopen a sketch | Redraw, archives, NIB toolbars and geometry dictionaries pass ARMv6 checks. Desktop startup now reaches missing UINavigationController setNavigationBarHidden:animated:. Drawing and persistence remain unverified. |
| Good Reader (large PDF viewer) - read big PDF files with reflow | 3.8.1 | Open a local document, navigate pages and remember position | Desktop startup: guest CPU memory fault; cause unconfirmed. |
| Stanza | 3.2 | Open an offline book, turn pages and save position | Desktop startup: missing UITabBarController. |
| Brushes - iPhone Edition | 2.2.2 | Draw, undo and reopen saved artwork | Desktop startup: missing CGImageMaskCreate. |
| Color Splash | 1.6.2 | Import a test image, edit and save the result | Desktop startup: missing UITabBar. |
| Recorder | 1.1.1 | Record, play back, rename and reopen a recording | Desktop startup: missing UINavigationBar. |
| iTalk Recorder Premium | 1.0.4 | Record, play back and reopen saved audio | Desktop startup blocked: missing navigationItem. Replaces the non-Premium archive, which failed duplicate-path validation. |
| Merriam-Webster Dictionary | 1.2.1 | Offline word lookup, definitions and navigation | Bundle enumeration fixed; desktop startup now reaches missing NSOperationQueue. |
| Batterie Gratuite HD | 2.24 | Battery state updates and settings navigation | Desktop startup: missing CALayer setContentsScale:. |
| Ruler | 1.1 | Switch units and measure using the ruler | Desktop startup: missing CGContextSetShouldAntialias. |
| Convert ~ the unit calculator | 1.0.1 | Arithmetic and unit conversion; history or choices persist | Desktop startup: missing NSNumberFormatter. |
| WiFi HD FREE (Wireless Hard Disk Drive) | 1.2 | Transfer a local file and reopen it | Desktop startup: missing UINavigationBar. |

## Games

| App | Version | Required functional checks | Latest result |
| --- | --- | --- | --- |
| Super Monkey Ball | 1.01 | Steer through a complete stage; restart and save progress | Device build b065e116: menus respond and the first level renders; gameplay is slow. Tilt control, stage completion and persistence remain unverified. |
| Crash Bandicoot Nitro Kart 3D | 1.0 | Complete a race, pause/resume and retain progress | Device build 23a5d292: Quick Race renders and continues running; pause and options respond. Tilt steering, race completion and persistence remain unverified. |
| Rolando | 1.9 | Complete a level using touch and tilt; resume saved progress | Desktop startup: guest CPU memory fault; cause unconfirmed. |
| Labyrinth | 1.9.1 | Tilt through a maze, calibrate and restart | Desktop startup: missing UINavigationController setNavigationBarHidden:animated:. |
| Cro-Mag Rally | 3.0.3 | Complete a race, pause/resume and retain settings | Device build 1f8d4c8e: race renders; pause labels are upright and the paused frame remains visible. Resume works after the delegate lifetime fix; a second pause and End Game return to the main menu. Tilt steering, race completion, sustained play and persistence remain unverified. |
| R.Thunder | 1.0 | Complete a race; responsive steering and pause/resume | Desktop startup: no crash during 25 seconds; functionality unverified. |
| Enigmo | 4.1 | Solve a level, manipulate tools and reopen saved progress | NIB progress views now decode and draw; ARMv6 state and pixel checks pass. Desktop startup advances to missing UIPickerView dataSource support. Functionality remains unverified. |
| Touchgrind | 1.4.1 | Multitouch tricks, scoring, pause/resume and score persistence | Desktop startup: no crash during 25 seconds; functionality unverified. |
| Edge | 1.62 | Complete a level, precise controls and saved progress | Device build 60bd8788: offline install passes; blank display persists beyond three minutes. |
| Canabalt | 1.7 | Sustained run, jumping, restart and saved high score | Device build b065e116: offline install passes; missing NSOperationQueue prevents launch. |
| Fieldrunners | 1.6.2 | Build and upgrade towers through several waves; resume | Device build cf7c00bf: startup exhausts host memory and iOS terminates S5LBox. Scanner, thread dispatch, PNG export and nil dictionary-key fixes advance desktop startup to a stale default-file-manager reference during resource lookup. Its cause is under investigation. Gameplay and a device retest remain pending. |
| Minigore | 7.0 | Movement and firing through sustained combat; restart | Desktop startup: no crash during 25 seconds; functionality unverified. |
| iCopter | 1.4 | Sustained flight, restart and saved high score | Device build cf7c00bf: transitions now complete; short flights render and touch holds/releases work. Collisions return to the title normally. Score 122 survives Home and relaunch. Longer play and frame pacing remain unverified. |
| Flight Control | 1.9.2 | Draw landing paths through a session; saved score | Desktop startup: missing glRenderbufferStorageMultisampleAPPLE. |
| Tap Tap Revenge | 1.7 | Play a bundled song, timing/scoring and restart | Desktop startup: missing NSNetServiceBrowser. |
| Trism | 1.4 | Complete a puzzle session and retain progress | Desktop startup: missing UIButton setTitleShadowColor:forState:. |
| Aqua Forest | 1.1.3 | Draw and interact with a physics scene; complete a level | Device build b6082ad3: category and level menus work; touch-tutorial scenes respond to swipes, and a drawing tutorial displays water drawn with a sustained stroke. The material picker opens but clips some labels. Level completion, saved progress, sustained frame pacing and audio remain unverified. |
| Chopper | 1.3.1 | Complete a mission, steer/fire and pause/resume | Desktop startup: no crash during 25 seconds; functionality unverified. |
| SPACE DEADBEEF | 1.01 | Sustained gameplay, firing, pause/resume and restart | Desktop startup: no crash during 25 seconds; functionality unverified. |
| Trace | 1.3 | Draw paths, complete a stage and retain progress | Device build 23a5d292: orientation corrected, first two levels completed, platform drawing and jumping work. First-level completion time survives relaunch and app reinstall; longer play remains untested. |

## Additional reported games

| App | Version | Latest device result |
| --- | --- | --- |
| Angry Birds | 1.0 | Build 3322b6fd: one menu load finished within 34 seconds, versus more than 56 seconds on 425e7f36 in the same offline profile. A repeat finished between 30 and 70 seconds. First level completed with score 7830; aiming and pause/resume respond. Relaunch preserves the star and unlocks level 2. Timings are observation bounds, not exact durations or a universal speedup. |
| Super Monkey Ball Lite | 1.0 | Build 3322b6fd: title reached within 40 seconds; mode selection, level flyover and tilt-calibration screens respond. Physical calibration and stage completion remain unverified. One delayed close recovered without Force Close. This is separate from the full version above. |
| Beer Pong U | 1.2 | Build 46f9c6f9: the oversized-read crash is fixed on the iPhone; Time Attack and CPU Challenge render, and shot-style, help and quit controls respond. Relaunch works. Build 6fde244b confirms landscape presentation with no local override. Build cf7c00bf again passes Time Attack rendering, advancing timer, Lob selection and quit-to-menu. The game uses physical tilt to throw; throwing, scoring and saved results remain unverified. |

## Runtime changes under validation

Thread exit now runs Darwin cleanup handlers and thread-local destructors before
waking joiners. Normal return and explicit `pthread_exit` use the same cleanup;
exiting the main thread leaves other workers running. `NSThread` remains retained
until completion, including explicit exit. ARMv6 regression fixtures pass locally.

Double floating-point classification is implemented; `nan()` uses the double
return ABI, and `hypot()` avoids premature overflow and underflow. ARMv6 math
checks pass locally. These checks do not yet establish PCalc functionality.

Objective-C method lookup and replacement, current-app preference conversion,
hexadecimal scanning, run-loop cancellation, and checked formatting now have
passing ARMv6 regression fixtures. A targeted desktop startup check progresses
past the original failures in PCalc, Convertbot, Rolando, Labyrinth, and
Fieldrunners, but each still has a later failure. None is a functional pass.
Host method type encodings and preference domains for other applications remain
unsupported. The desktop graphics failures require separate device diagnosis.

Link-time strings and booleans no longer invoke guest class initializers before
`+load`. First instance messages now initialize constant-object classes; reentrant
and concurrent initialization are covered by the ARMv6 startup fixture. Runtime
class lookup returns nil for unavailable classes, and nil selector conversion is
handled. The Core Foundation version constant has the correct double layout.
Screen-mode size and identity have regression coverage. These checks pass locally;
the corresponding app paths still require device verification.

On device build `60bd8788`, Angry Birds 1.0 reaches its first level and Home returns
to the launcher in 3.5 seconds after loading. Closing during loading took much
longer in a separate attempt; that remains unresolved. This is a regression check
for an original app, not one of the 40 additional compatibility passes.

Instance-variable lookup, type encodings, offsets, inherited lookup, list ownership,
and manual-reference-counted object ivar access pass ARMv6 regression checks and
CI on `b065e116`. On the physical iPhone, Clock 2.0.2 now reaches its splash screen
and subsequently fails on `Preferences valueForKey:`. Convertbot 1.6.1 on
`af788f74` progresses to missing `AudioServicesAddSystemSoundCompletion`.
Neither observation establishes functional compatibility.

Trace 1.3's device stack overflow was traced to MP3 files being exposed as linear
PCM: its streaming callback computed a zero packet count and recursively retried
the read. MP3 packet handling and audio completion callbacks were added in
`6fb1e3b7`. The ARMv6 audio fixture on `39dfa4bd` passes playback, pause/resume,
packet boundaries, EOF, reset, completion callbacks, PCM conversion, and invalid
AVAudioPlayer input checks in macOS CI. Device build `39dfa4bd` reaches Trace's
world and level menus, but selecting a level exposes unsupported asynchronous
queue disposal. A deferred disposal path now drains active queues and suppresses
all subsequent callbacks; its regression fixture passes CI, and device build
`dd948eb4` loads the first level. Callback file reads now free guest scratch memory,
accept partial reads, and report malformed input through error codes. These
checks also pass CI and a Windows build with an isolated shared OpenAL library.
The Windows desktop audio fixture separately crashes in OpenAL initialization's
native `std::call_once`/`pthread_once` path, before playback decoding. Desktop
access violations must not be treated as proof of the same failure on iOS. The
isolated shared-library build avoids that local toolchain failure.

Basic key-value getters, setters, and nested key paths pass local ARMv6 tests,
including inherited ivars, numeric boxing/unboxing, common Core Graphics structs,
typed host accessors, nil and undefined-key overrides, setter precedence, and
retain/release ownership when replacing object ivars. Collection proxies and
arbitrary struct encodings remain unsupported. Clock's device retest confirms
progress to the separate missing key-value observation API.

Client vertex/color/normal/texture-coordinate pointers are now translated at draw
time, preserving disabled pointers, VBO offsets, per-texture-unit state, and
context isolation. Invalid array parameters and empty draws no longer panic.
An ARMv6 rendering fixture verifies pixels, fixed-point arrays, pointer queries,
and state restoration on the Windows OpenGL backend and in macOS CI. Both CI
workflows pass. On the iPhone, Crash Nitro Kart's Quick Race now renders and runs
past its previous crash, including working pause and options menus.

The `rand48` family now implements Darwin's 48-bit generator, independent global
and caller-owned seeds, configurable coefficients, and the static `seed48`
return buffer. ARMv6 checks pass for known sequences, signed and floating-point
returns, seed aliasing, coefficient reset, and independence from `rand()`.
Both CI workflows pass, and Cro-Mag Rally reaches race rendering on the iPhone.

Filesystem-stat queries now accept existing paths outside Documents and report
invalid paths, pointers and descriptors through errno. `fstatvfs` shares the
descriptor path, maps Darwin mount flags to POSIX flags, and reports count
overflow. ARMv6 regression checks pass locally. Volume capacity remains a
virtual compatibility value; the filesystem service does not yet supply actual
volume statistics.

System queries distinguish legacy numeric OIDs from named 64-bit properties,
support Darwin's four-byte numeric output conversion, and return errno for
unknown queries, invalid buffers, read-only writes and insufficient space.
Page size, memory size, CPU frequency and kernel-revision ABI checks pass in
the ARMv6 fixture. The hardware values describe the emulated environment.

Guest alerts now create a modal window with tappable buttons, copied titles,
weak delegates, and presentation/dismissal callbacks. ARMv6 tests cover variadic
buttons, real control events, nested windows, and callbacks that dismiss or
reopen the alert. Presentation animations and text-entry alert styles remain
unsupported; Cro-Mag Rally's pause menu still needs a device retest.

Bundle enumeration includes the main bundle and instantiated resource bundles,
excludes frameworks, and returns an independent array. The ARMv6 resource and
ownership fixture passes locally; Merriam-Webster still needs other APIs.

Repeated input now releases temporary touch sets and event objects, including
duplicate presses and cancelled drags. Hidden windows no longer intercept
touches. A local 200-cycle ownership test checks event destruction and stable
view reference counts; physical-device sustained-use checks remain pending.

The touchHLE CPU adapter now supplies checked plain-RAM mappings to the core's
existing instruction/read/write caches. All mappings are invalidated when a CPU
batch returns, so framework calls can unmap or replace memory safely. Callback
fallback remains available for refused mappings. C and Rust checks cover faults,
permissions, changed backing and self-modifying code. This uses no generated
code or executable allocation. All three CI workflows pass. The first device
comparison improves Angry Birds loading; other apps still need timed checks.

File reads now handle short resources without requiring the unused destination
range to be mapped. Reads and writes also support adjacent guest mappings backed
by separate host allocations, reject invalid ranges through errno, and preserve
position and errors for pread/pwrite. Local unit and ARMv6 checks pass for EOF,
oversized fread, invalid buffers and descriptors, split mappings and positioning.
Both CI workflows pass. Beer Pong U now starts Time Attack on the iPhone.

Landscape alerts now counter-rotate their contents. Showing a window captures
the preceding fullscreen GL frame once for composition, including paused games.
The ARMv6 graphics fixture checks context switching, preserved framebuffer and
renderbuffer bindings, unchanged pixels, and deleted-buffer handling. Normal
fullscreen presentation retains its direct path. Cro-Mag's appearance needs a
device retest. The subsequent 6fde244b device check confirms the upright dialog
and preserved race background, but exposes a separate failure on Resume.

Sort descriptors now compare key paths or whole objects with a chosen selector,
reverse ordering, and sort arrays by multiple keys. ARMv6 checks pass locally for
numeric and case-insensitive comparisons, nested keys, nil values, copied keys,
immutable results, ties, empty descriptor lists and reversed order. iCopter's
desktop startup advances to the separate missing toolbar API. Block comparators
and descriptor archiving are not implemented.

Programmatic toolbars now lay out title/image buttons, custom views, fixed spaces
and flexible spaces, resize their contents, and deliver actions with the bar item
as sender. ARMv6 tests cover touch activation, disabled and nil actions, explicit
targets, responder-chain dispatch, callbacks that remove items, and items that
outlive the toolbar. System items use text labels; original system artwork,
insertion animations and NIB item decoding are not implemented. The three app
startup retests above establish progress, not functional compatibility.

Cro-Mag's Resume failure was reproduced on the iPhone and traced to dismissal
notifying a delegate released by the click callback. Alerts now retain the
assigned delegate only for the active callback sequence. An ARMv6 fixture
reproduces the original failure and passes after the fix, including release
during programmatic dismissal. Both CI workflows pass on 1f8d4c8e. The device
retest confirms Resume, a second pause, and End Game without the original panic.

The single-precision hypotf entry point now uses a scaled hypotenuse calculation.
ARMv6 checks pass for ordinary values, large and tiny inputs, signed zero,
infinity and NaN. Aqua Forest progresses beyond this missing function but still
fails later during guest execution.

Rectangular redraw invalidation now schedules the existing deferred full backing
store redraw for nonempty regions intersecting view/layer bounds. ARMv6 checks
cover coalescing, empty and outside rectangles, partial overlap and direct layer
invalidation. Transition action and direction string constants are also exported
and checked. SketchBook progresses to its archive handling, and Aqua Forest to
implicit transaction handling. Partial backing-store redraw and transition
animation rendering are not implemented by these changes.

Animation transaction property access now creates an implicit transaction when
needed. Flush defers while an explicit transaction is open, and timing functions
are retained for the transaction lifetime. ARMv6 checks cover default and changed
settings, custom values, replacement, nested flush, reset and ownership. Aqua
Forest progresses to the separate unsupported transition rendering path.

Keyed archives now support sequential object entries alongside named keys,
including escaped dollar-prefixed keys, per-object ordering, nil, shared identity
and cycles. Unarchiving releases its internal reader while preserving returned
objects. Number encoding covers floating-point and signed/unsigned integer
variants. ARMv6 round-trip checks cover nested objects, mixed keys, lifetime,
cycles, large integers and fractional values. SketchBook advances to NIB toolbar
item decoding; this does not yet establish working sketch persistence.

Programmatically initialized image views and labels now ignore touches by
default, as UIKit specifies. Explicitly enabling interaction remains supported.
An ARMv6 test verifies that an image-view subclass with empty touch handlers no
longer blocks the underlying receiver, and that opting in restores hit testing.
iCopter uses this overlay pattern. Device build cc5d2bb6 confirms that input now
reaches game-start code, exposing missing CATransition subtype support.

NIB toolbars now decode bar-item titles, images, enabled state, tags, styles,
widths, system items and custom views, along with toolbar appearance and items.
NIB event connections assign bar-button targets/actions and release their
retained references on destruction. ARMv6 checks cover decoding, layout,
disabled actions, enabled actions and connection ownership. SketchBook advances
to geometry dictionary serialization; this is startup progress, not a drawing
or persistence pass.

Core Graphics geometry dictionaries now serialize and restore points, sizes and
rectangles with numeric components and CF Create ownership. Native iOS 15.8.5
probes confirmed the flat key format and sequential decoding of missing fields;
ARMv6 checks cover negative dimensions, ownership, archive round trips, mixed
numeric values and incomplete dictionaries. SketchBook advances to navigation
controller configuration. These checks do not establish its drawing support.

Transition animations now have separate slide/fade rendering, direction and
progress properties, and copies with independent playback state. The compositor
retains outgoing render trees and freezes their texture versions when content
changes. It releases unused textures after those trees are no longer referenced.
An ARMv6 desktop visual fixture completes push, move-in, reveal and fade, then
checks cancellation; completion callbacks remove and replace their own animation.
Foundation checks also cover copied properties and retained-value ownership.
iCopter's iPhone retest on cf7c00bf reaches flight and restart; a high score of
122 survives Home and relaunch. This is short interaction and persistence
coverage, not a sustained-play or frame-pacing measurement.

Transition visuals are not pixel-identical to Core Animation: translucent
subtrees use per-layer alpha, arbitrary rotations use rectangular clip bounds,
and the direct fullscreen OpenGL path does not supply an outgoing cached tree.
Opaque, compositor-rendered scenes have the outgoing texture available. Custom
transition filters and keyframe animation rendering remain unsupported.

Progress views now decode their NIB state, clamp progress to the documented
range, and draw default/bar tracks that update when progress decreases or the
view resizes. ARMv6 checks cover properties, missing archive keys and actual
empty/partial/full track pixels. Shading approximates the legacy controls;
pixel-identical UIKit assets and newer animated progress APIs are not provided.

Table cells retain normal and selected background views, keep them behind the
content, and size them during layout. Selected backgrounds enter and leave the
hierarchy with selection/highlighting; reuse clears those states. ARMv6 checks
cover replacement, nil, self-assignment, layering, resizing and retained-view
cleanup. These changes address Aqua Forest's observed menu-construction failure;
selection fades and complete legacy cell styling remain unimplemented.

String scanners now fail at end-of-input without replacing the output string;
scanning an empty match also fails. ARMv6 checks cover a bounded replacement
loop, absent and adjacent delimiters, skipped whitespace, null output pointers
and UTF-16 positions. This fixes Fieldrunners' unbounded startup replacement
loop; it does not establish the game's functionality.

NSThread main-thread lookup shares the same object as currentThread on the main
thread, including when a worker requests it first. Thread-targeted selectors use
the target's existing run loop, with synchronous same-thread delivery and
queued asynchronous delivery. Delayed-request cancellation leaves these
cross-thread messages intact. ARMv6 checks cover identity, FIFO delivery,
worker-to-main and main-to-worker waiting, class targets and retained-object
cleanup; the existing cancellation/exit suite also passes. Custom run-loop
modes and dispatch to threads that have not started or have exited remain
unsupported.

UIImagePNGRepresentation encodes the existing CGImage pixels as ordinary PNG
and returns autoreleased NSData. It converts premultiplied alpha without
changing the source image. Rust and ARMv6 fixtures check round-trip pixels,
transparency, row order, and nil/empty-image handling. The 71 active Rust unit
tests pass; four pre-existing tests remain ignored. This covers the runtime's
existing 8-bit RGBA image representation, not additional UIImage orientation
or bitmap-format support.

NSDictionary valueForKey: now forwards nil keys to objectForKey: without trying
to convert nil into a host string. ARMv6 tests cover empty, populated and mutable
dictionaries. Native Foundation on the iOS 15 test device also returns nil for
the empty and populated cases; that reference is not an iPhone OS 3 measurement.
