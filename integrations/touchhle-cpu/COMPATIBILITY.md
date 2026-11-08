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
| SketchBook Mobile | 1.5 | Draw, undo, save and reopen a sketch | Filesystem-stat path restriction fixed; desktop startup runs for 25 seconds. Drawing and persistence need device testing. |
| Good Reader (large PDF viewer) - read big PDF files with reflow | 3.8.1 | Open a local document, navigate pages and remember position | Desktop startup: guest CPU memory fault; cause unconfirmed. |
| Stanza | 3.2 | Open an offline book, turn pages and save position | Desktop startup: missing UITabBarController. |
| Brushes - iPhone Edition | 2.2.2 | Draw, undo and reopen saved artwork | Desktop startup: missing CGImageMaskCreate. |
| Color Splash | 1.6.2 | Import a test image, edit and save the result | Desktop startup: missing UITabBar. |
| Recorder | 1.1.1 | Record, play back, rename and reopen a recording | Desktop startup: missing UINavigationBar. |
| iTalk Recorder Premium | 1.0.4 | Record, play back and reopen saved audio | Desktop startup blocked: missing navigationItem. Replaces the non-Premium archive, which failed duplicate-path validation. |
| Merriam-Webster Dictionary | 1.2.1 | Offline word lookup, definitions and navigation | Desktop startup: missing NSBundle allBundles. |
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
| Cro-Mag Rally | 3.0.3 | Complete a race, pause/resume and retain settings | Device build 4efcbc30: offline install, track/vehicle/character selection and race rendering work. Pause requests an unimplemented UIAlertView. Driving, completion and persistence remain unverified. |
| R.Thunder | 1.0 | Complete a race; responsive steering and pause/resume | Desktop startup: no crash during 25 seconds; functionality unverified. |
| Enigmo | 4.1 | Solve a level, manipulate tools and reopen saved progress | Desktop startup: missing UIToolbar. |
| Touchgrind | 1.4.1 | Multitouch tricks, scoring, pause/resume and score persistence | Desktop startup: no crash during 25 seconds; functionality unverified. |
| Edge | 1.62 | Complete a level, precise controls and saved progress | Device build 60bd8788: offline install passes; blank display persists beyond three minutes. |
| Canabalt | 1.7 | Sustained run, jumping, restart and saved high score | Device build b065e116: offline install passes; missing NSOperationQueue prevents launch. |
| Fieldrunners | 1.6.2 | Build and upgrade towers through several waves; resume | Desktop startup: no crash during 25 seconds; functionality unverified. |
| Minigore | 7.0 | Movement and firing through sustained combat; restart | Desktop startup: no crash during 25 seconds; functionality unverified. |
| iCopter | 1.4 | Sustained flight, restart and saved high score | Desktop startup: missing NSSortDescriptor. |
| Flight Control | 1.9.2 | Draw landing paths through a session; saved score | Desktop startup: missing glRenderbufferStorageMultisampleAPPLE. |
| Tap Tap Revenge | 1.7 | Play a bundled song, timing/scoring and restart | Desktop startup: missing NSNetServiceBrowser. |
| Trism | 1.4 | Complete a puzzle session and retain progress | Desktop startup: missing UIButton setTitleShadowColor:forState:. |
| Aqua Forest | 1.1.3 | Draw and interact with a physics scene; complete a level | Desktop startup: missing UIToolbar. |
| Chopper | 1.3.1 | Complete a mission, steer/fire and pause/resume | Desktop startup: no crash during 25 seconds; functionality unverified. |
| SPACE DEADBEEF | 1.01 | Sustained gameplay, firing, pause/resume and restart | Desktop startup: no crash during 25 seconds; functionality unverified. |
| Trace | 1.3 | Draw paths, complete a stage and retain progress | Device build 23a5d292: orientation corrected, first two levels completed, platform drawing and jumping work. First-level completion time survives relaunch and app reinstall; longer play remains untested. |

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
