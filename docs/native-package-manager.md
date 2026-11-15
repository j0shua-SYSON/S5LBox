# Native package manager

The host app handles repository download/decompression, search, dependency
planning and package downloads. The running guest executes its own `dpkg` and
maintainer scripts. This avoids interpreting repository processing inside the
emulator without pretending that arbitrary guest scripts can run on the host.

## Using it

Open **Machine Settings > Packages**. If setup is needed, choose **Set Up
Packages**; confirm the restart before the running guest is powered off. A stock
machine offers **Jailbreak & Set Up**, with a separate confirmation. Jailbreak
includes helper setup by default. Turn off **App Settings > Advanced > Packages
with jailbreak** to skip it on subsequent jailbreak preparations. This does not
remove an existing helper. Explicit **Set Up Packages** (including **Jailbreak &
Set Up**) still installs it regardless of that preference. A successful
jailbreak opens the machine automatically, without another prompt. Explicit
**Set Up Packages** requests return to Packages after the machine starts.
For a stopped machine, the same setup
is available under **App Settings > Advanced > Set up package manager**.

After preparation, Packages remembers that this machine has not yet connected
to its helper. Opening Packages manually gets the same first-start wait as the
explicit setup flow, even after closing the screen or relaunching S5LBox. The
hint clears only when Packages receives and accepts a live installed-package
list; it is separate from the stopped-disk transaction marker. The current
helper withholds that list until its first-boot finalizer finishes, so Packages
does not offer installations that setup will reject. Older helpers may expose
the list earlier. A startup connection timeout offers **Try Again**, retains the connection error, and
does not suggest reinstalling an already-prepared helper.

Failed USB enumeration or mux handshakes reconnect the virtual cable automatically, with
bounded backoff and time for the guest to observe the disconnect. Pausing the
machine does not consume negotiation or retry time. Recovery closes stale
streams; it never replays an installation or other package command. Canceled
connection attempts release their sockets and channel slots even while the
handshake is unfinished. A single refused service does not reset a healthy link.
Resuming a saved machine also gives the guest an observable cable-disconnect
interval before attaching a new host session.

Setup shows the current step and real, centered copy progress. Keep S5LBox open
while it prepares the disk; the app prevents Auto-Lock during preparation.
Errors offer retry or, for an interrupted shutdown, a path back to the machine
to shut down iPhone OS normally. Setup discards the saved running state,
validates the stopped disk, and uses the crash-safe disk-publication transaction.
Helper-only setup does not rerun Cydia repair. Historical snapshots still block
setup, and strict filesystem errors are not ignored.

An interrupted boot can leave the volume's next catalog ID behind records
already written to disk. Stopped-disk setup now advances that hint above every
observed catalog identity in the unpublished copy, after the full catalog
audit. It does not reuse IDs, repair tree structure, or mark the disk clean.
Both volume headers are updated together; the source disk stays unchanged until
the transaction commits. Invalid structure or exhausted IDs still stop setup.
The distinction between a catalog ID and its allocation hint follows
[Apple's HFS Plus format specification](https://developer.apple.com/library/archive/technotes/tn/tn1150.html).

Stopped-disk setup also rebuilds stale directory child counts from the complete
catalog. This addresses the interrupted-create case where `/var/run/utmpx`
exists but `/var/run` still claims to be empty: iPhone OS 3 then loops between
"not found" and "already exists" during startup. Apple's later
[HFS lookup implementation](https://raw.githubusercontent.com/apple-oss-distributions/hfs/main/core/hfs_lookup.c)
documents that same failure. Recovery requires unique IDs, exact object/thread
pairs, valid directory parents and a tree rooted at the volume root. It changes
only derived counts in the private transaction, re-audits after writing, and
never deletes the existing file. A read-only preflight keeps repeated helper
setup fast when neither files nor counters need changing. This is stopped setup
recovery, not a general filesystem repair tool or a clean-shutdown substitute.

A pending marker keeps interrupted helper setup from appearing ready merely
because a credential was created. Retries retain the same private credential;
only exact verification or successful disk publication clears the marker.
Existing completed installations without a marker remain compatible.

Validation on 2026-10-12: the default-on jailbreak, explicit setup with the
preference off, and ordinary opt-out paths were exercised in the isolated
SetupTest app on an iPhone 6s Plus (iOS 15.8.5). At `46410b0`, the previously
stalled disposable machine completed recovery and its first post-jailbreak
boot without an extra restart. Independent catalog readback found `/var/run`
valence corrected from zero to one, with the same `utmpx` identity and no
mismatched object/thread pairs. Packages read 29 installed packages, including
Cydia Installer, and refreshed both default sources (375 available versions).
Normal guest shutdown returned to the machine list. First-start service
availability was observed after roughly eight minutes, including manual
wake/unlock and inspection; this is not a minimum boot-time measurement.
All 83 local CTests, all nine core CI jobs, and the iOS build/API baseline passed.
No original user machine was changed; physical host coverage is iOS 15.8.5 only.

The Controls restart path now requests guest shutdown and waits for the PMU
power-off witness before replacing the machine, instead of resetting a running
kernel during filesystem writes. Force Power Off remains available if a guest
cannot complete shutdown; it is not claimed to be a clean unmount.

An interrupted first-boot installation exposed two separate HFS consistency
problems: a stale nextCatalogID/directory counts, and referenced file blocks
marked free. A complete, collision-free owner audit can now protect those blocks
on an unpublished cold-boot clone, retaining every existing allocation. It does
not free orphan blocks, relocate file content, reconstruct ambiguous forks, or
mark the disk clean. Saved running checkpoints remain untouched. The same
protection runs before stopped jailbreak/helper file allocation and before disk
growth can relocate allocation metadata.

Physical recovery at `462300a` used a preserved copy of the affected Test disk.
Read-only comparison of a local recovery candidate found 200 changed metadata
bytes across the 2 GiB image and no changed file content. On the iPhone 6s Plus,
the restored original disk booted through guest package configuration, published
the Cydia icon, and connected Packages with 29 installed records and 375 available
versions. Cydia then completed its first-launch reorganization and reached its
user-role selection screen. This is recovery evidence, not a first-boot speed
benchmark. The initial counter-only attempt was insufficient; protecting missing
allocation bits was also required. Both the original disk and that failed retry
were retained during testing.

The same device then passed Controls > Restart: console evidence recorded
`guest power-off confirmed; saving shutdown witness`, followed by a fresh boot
from that powered-off checkpoint. The guest accepted unlock/Home navigation,
and Packages reconnected without Save & close or a host-app relaunch, reading
30 installed packages after Cydia's initial configuration and 375 source versions.
All 83 local CTests passed at `db9bbd6`; its iOS build and iOS 13 API checks passed.

The machine starts and Packages reopens automatically after setup. The one-time
post-jailbreak startup can take several minutes on older hosts; that connection
wait is bounded to ten minutes. Regular connection attempts stop after two
minutes. **Try Again**, **Repair Setup**, and **Show iPhone OS** provide recovery
without an endless spinner. Empty browsing tabs remain disabled until connected
or a cached catalog is available.
If the guest is asleep or locked, use **Show iPhone OS** to wake and unlock it,
then return to Packages. The host stays awake during the bounded connection wait.

The full-screen workspace has five tabs:

- **Packages:** connection status, all packages, and category browsing.
- **Sources:** iOS 3 Party, Saurik and BigBoss initially. Open a source to browse
  its packages or add a flat HTTPS repository. BigBoss uses its published legacy
  HTTP endpoint, with a domain-specific network exception. Existing installations
  receive BigBoss once; removing it afterwards keeps it removed. Removing a
  source never uninstalls packages.
- **Changes:** available newer versions of installed, nonprotected packages.
  Choose one to check its dependencies; an update listing alone is not a
  compatibility promise. There is deliberately no automatic update-all.
- **Installed:** the running guest's dpkg database. Remove a nonessential
  package only if its reverse dependencies remain satisfied; keep conffiles.
- **Search:** native name, author, identifier and description search.

Lists open a package detail page with description, author, version, source,
download size and dependencies. **Other versions** retains access to older
releases. Install or Remove prepares one dependency-resolved transaction;
**Confirm changes** shows every affected package and download size. There is
no hidden background installation or persistent multi-operation queue.
The activity screen shows the current stage and a verified result; raw guest
installer output is behind **Show details**. Native Dynamic Type, system
light/dark colors and local category icons keep the interface readable without
loading repository web depictions or remote tracking artwork.
Navigation titles are compact throughout; the transaction screen omits the
redundant generic title and gives the actual progress/result first. Dynamic
Type still controls body text instead of forcing a tiny fixed font.

Host compatibility follows S5LBox's **iOS 13.0 and later** deployment target,
not just the iOS 15 lab phone. CI rejects unguarded newer API calls in all four
native package components against the iOS 13 baseline. SF Symbol lookup has a
baseline fallback. Workspace creation is gated on completed initialization:
iOS 15 was observed loading the tab controller's view during `super init`.
API checks are not proof of runtime behavior on every supported iOS version;
physical validation below names the OS actually tested. Guest package/tweak
compatibility is a separate concern; this feature targets the iPhone OS 3 lane.

Close Cydia during package operations. Keep S5LBox foregrounded. Stop waiting
cancels host work, not a running guest dpkg process. A disconnect after submission
has an unknown outcome; refresh Installed before retrying. Do not force power off
while dpkg is working. Some tweaks require a guest restart. Compatibility is not
implied by a successful installation.

## Trust and supported scope

This first version **does not verify OpenPGP repository signatures**. HTTPS
checks remain enabled; an expired certificate is not bypassed. BigBoss is the
only built-in HTTP exception. SHA-256 and exact size
checks protect against corrupt downloads, not a malicious or intercepted index.
Older sources such as Saurik publish only MD5. SHA-1/MD5 are accepted solely
over verified HTTPS, with a warning in the transaction preview, and are described
as weak corruption checks rather than authenticity guarantees. A present but bad
SHA-256 never falls back to a weaker digest. Only add trusted sources: packages may execute root scripts inside the guest.
These privileges do not extend to the host iPhone.

Source refreshes are independent: an unavailable source retains its cached
records while healthy sources update. Errors identify the affected source.
Repository records with identifiers unsupported by the guest helper (including
legacy mixed-case identifiers) are skipped with a count, rather than rejecting
the entire source. Installed-state parsing remains strict. Malformed fields,
duplicate fields and invalid compressed indexes still fail validation.

The planner understands Debian epoch/upstream/revision ordering, Depends,
Pre-Depends, alternatives, unversioned Provides, Conflicts and Breaks. It checks
the complete effective installed set before offering a plan. It refuses core
or held package changes, downgrades, automatic dependency removals, dependency
cycles, unsupported relationship syntax and overly large searches. This is a
conservative installer, not a full SAT replacement for APT; use guest Cydia for
plans it declines. Recommends/Suggests and automatic update-all are not included.

Limits: 32 MiB expanded index, 100,000 records per index, 128 packages / 256 MiB
per transaction, 64 MiB per archive, 4 MiB installed status, thirty-minute host
execution wait. Packages without a supported checksum are refused. Successful transfer is not
success: the UI requires a terminal guest response and matching installed state.

## Guest boundary

`tools/guest_package_service.c` is an ARMv6 guest executable. It binds only guest
loopback port 64321 and is reached through the emulated USB multiplexer. Each
connection requires a random per-machine 256-bit capability, provisioned root-
only in the guest and kept in the host's private Application Support directory.
There is no physical USB requirement, host daemon, SSH password, JIT permission,
or host jailbreak dependency. Moving a machine alone to another host does not
move its private capability; automatic rekeying is deliberately not implemented.

Only status, install and remove exist on the wire. Clients cannot provide shell
commands or guest file paths. Uploads use root-owned unique staging directories,
are bounded and hashed again inside the guest, and complete before any dpkg
execution. The guest status hash is checked before and after upload. dpkg retains
its own lock/dependency enforcement; concurrent Cydia changes can still make a
plan fail, and no force-dependency flag is used. Guest operation logs are at
`/private/var/lib/s5lbox-package-manager-v1/last-operation.log`.

dpkg/script failures are **not rolled back**. Dependencies already installed or
an unpacked/partially configured package may remain, exactly as with a failed
guest package operation. The UI reports that state instead of claiming atomicity.
Downloaded host archives from an attempt are discarded after it finishes; the
metadata cache remains for browsing.

## Build and evidence

The included 50 KiB ARMv6 helper is built from the checked-in C source and
`tools/sha256.c`, using the project's portable LLVM compiler and open-source
`common-3.0.sdk`. `tools/build_guest_package_service.ps1` records the build
command and enforces project-local intermediate outputs. It links to the guest's
libSystem; omitted SDK stub imports use dynamic lookup. It is not a host binary.
The SDK's unmodified Apple Csu startup object has its own APSL license; the app
ships [notices and source links](../app/Resources/GuestPackages/GuestPackageNotices.txt)
and [APSL 2.0](../app/Resources/GuestPackages/APSL-2.0.txt). It is not MIT code.

At `655b9ef`: 45 native planning checks, 15 synthetic executor protocol checks,
77 Windows CTests, and iOS build 37896428525 passed. Synthetic executor tests
substitute a fake dpkg, so they do not prove firmware execution. Subsequent
device acceptance and final-SHA results are recorded below when established.

Physical iPhone 6s Plus / iOS 15.8.5, disposable `rc-ipa-validation` guest:
the ARMv6 helper was provisioned through App Settings, then read live installed
state over virtual USB. Both sources loaded 375 versions. At `e1de61b`, the
native screen downloaded and installed Saurik's `p7zip` 4.57-3p (1,595,788 bytes),
with a verified guest-database result observed within 11 seconds of Install.
Removal then exposed a legitimate versionless dpkg selection tombstone, which
the parser incorrectly rejected. `6dbf077` fixes that with five regression
checks: 55 native checks and 15 synthetic protocol checks passed in iOS build
37899384197. Core matrix 37898385090 at `e1de61b` passed all nine jobs. These
results predate the five-tab UI and do not yet establish its device acceptance.

At `def62a7`, physical acceptance of the five-tab workspace covered opening,
search, source-specific browsing, package details, dependency review, and the
activity/result screens. `p7zip` was reinstalled with a matching guest-database
result observed within 11 seconds, then removed with a verified result in under
4 seconds. The versionless removal record no longer breaks Installed.
ImageMagick 6.4.3-6-1p plus four dependencies (`libxml2-lib` 2.6.32-3,
`libxml2` 2.6.32-7, `png` 1.2.24-3, `tiff` 3.8.2-2p) installed as one reviewed
five-package transaction; all five final versions were verified within
55 seconds. This proves package installation, not execution of every ImageMagick
operation. The later compact-title/visible-version adjustments do not change
the executor; their separate acceptance is recorded below.

The same test guest did not have Substrate installed. Planning iOS3 Folders 1.2
refused the Substrate / Safe Mode dependency cycle before download or mutation,
as designed. Cycle support remains a real limitation of this first installer;
it is not yet a complete Cydia replacement for tweak bootstrapping.

iOS build 37901568166 at `1c96798` passed the explicit iOS 13 API-baseline check
and the 55 native / 15 synthetic package checks. Its app sources equal
`def62a7`; only CI and documentation changed. Runtime testing on other host
iOS versions remains outstanding.

Final compact UI candidate `82ed924`: iOS build 37902474803 passed, including
the iOS 13 API baseline, 55 native package checks, 15 synthetic executor checks,
and 41 native USB checks. It was installed on the same iOS 15.8.5 phone. The
compact home, search, detail, confirmation, categories and result screens were
visually checked; the generic activity heading is gone and list versions remain
visible. A further real `p7zip` install/remove round trip passed on this exact
candidate. The host download cache contained only the 187 KiB metadata plist
after the earlier five-package operation, not retained archives. Core matrix
37902474600 subsequently completed with all nine jobs successful.

The first physical setup attempt exposed a pre-existing Cydia-repair assumption:
after Cydia reorganizes Applications into a symlink, its old repair probe reports
`path component 0 of /Applications/Cydia.app/Cydia_ is not a directory`.
Existing-machine package setup is now separate from that repair path. No raw
filesystem patch was used to bypass the refusal.

Protocol/dependency references:
[Debian relationships](https://www.debian.org/doc/debian-policy/ch-relationships.html)
and [control/version fields](https://www.debian.org/doc/debian-policy/ch-controlfields.html).

## Package finishing actions (October 9)

The native workspace now understands Cydia's ordered finish requests: return,
reopen, restart, reload, reboot. A dedicated inherited descriptor is advertised
as `CYDIA="<fd> 1"`; maintainer scripts write `finish:<action>\n` to that descriptor.
Ordinary stdout cannot request a restart. The highest request wins across the
whole transaction. This is automatic selection of a completion button, not an
unannounced restart while packages are still being applied.

As fallbacks, changed packages (including the previous release on upgrade and
the removed release) that declare a Substrate dependency request a respring.
The guest hashes its SpringBoard launch job and `/etc/notify.conf` before and
after dpkg: changes request reload and reboot respectively. These are bounded,
specific heuristics, not universal detection of every tweak's runtime needs.
The reference is [Cydia's source](https://github.com/sbingner/cydia/blob/master/MobileCydia.mm),
not a copied implementation.

An authenticated `SPM2` response carries the pending action and whether the
transaction completed. The root-private state survives helper/app disconnects.
A changed kernel boot timestamp expires old requests, but the emulator can
reuse that timestamp. Explicitly accepted reboots therefore consume the request
durably before executing the reboot command, restoring it on command failure.
An unrelated force-off/cold boot with the same timestamp may conservatively
leave a deferred request visible; it never executes one automatically. Failed
dpkg does not enable the finish action. The native UI additionally checks that all dpkg
records are configured or removed. The user can finish immediately or return to
the action on the Packages tab after refreshing. A status hash and dpkg lock
guard the finish request against stale state/concurrent package work.

Only the guest helper executes the fixed guest commands: launchctl stop for a
respring, unload/load of the SpringBoard job for reload, and `/sbin/reboot` for a
reboot. The host app never runs host restart commands. A reboot acknowledgment
means requested, not proven boot completion. Synthetic builds substitute a
fixture executable and cannot run these real commands.

Existing v1/v2/v3/v4 setups require App Settings > Advanced > Set up package manager again. Setup
stages a new immutable `service-finish-v5` executable and rewrites only an exact
recognized v1/v2/v3/v4 launch job in the existing crash-safe image transaction. It does
not overwrite an unknown helper, resize its fork, or edit a running disk. The
old approximately 50 KiB executable generations are retained but no longer
launched. The capability is kept.

New jailbreak setups run Cydia's bundled `firmware.sh` before configuring the
packages. This records the guest's actual firmware version and GraphicsServices
capabilities in dpkg, so native dependency resolution need not wait for Cydia's
first launch. A durable `packages-configured` checkpoint avoids repeating that
database rewrite when only icon publication or cache preparation needs a retry.
The v5 helper waits for the first-boot completion marker before exposing status
or accepting installation, removal and finish actions. Setup-in-progress replies
keep the automatic connection wait active; they are not treated as a ready
database or a reason to reinstall the helper.
An app update alone does not rewrite an already-completed guest's first-boot
script. If an older guest has no `firmware` record, let its setup finish, open
Cydia once, then refresh Installed in Packages.

Physical validation on 2026-10-17, iPhone 6s Plus / iOS 15.8.5, disposable
`Bootstrap test`, build `ab99d1e` (v4 helper): without opening Cydia or manually
rebooting, Packages saw the generated `firmware` 3.1.3 record. An early AppSync
attempt was correctly refused while first-boot APT caches were still being
prepared. After the guest log recorded completion, AppSync for OS 3.1 1.0.2
installed successfully through virtual USB. The v5 readiness change addresses
the premature install affordance observed in that test.

The same phone subsequently passed a fresh v5 test on `9cce79c`, disposable
`Cancellation test`: Packages stayed in its finishing-setup state, then changed
to Connected automatically. No Cydia launch, guest unlock or manual reboot was
needed. The guest's log confirmed Cydia registration, repository HTTP/DNS
activity and completed APT cache preparation. AppSync for OS 3.1 1.0.2 then
installed successfully. This does not establish a first-boot duration on other
host devices; cache preparation still took several minutes on this phone.

ARMv6 cross-compilation passes. At `4e3d2fb`, 60 host planning checks, 48
synthetic executor checks, 41 USB checks, the iOS 13 availability gate and the
app build passed. Its nine-job core matrix also completed successfully. The
availability gate now includes virtual USB pairing; an unguarded iOS 14 trust
API was given an iOS 13 fallback.

Physical acceptance on the iOS 15.8.5 lab phone, only in `rc-ipa-validation`:

- Upgraded the original helper through the native stopped-machine setup.
- Installed a disposable package whose real guest postinst writes Cydia finish
  requests; the native result selected Restart SpringBoard. The button actually
  restarted SpringBoard and returned the guest to its lock screen.
- A second fixture requested restart, reboot and return. Reboot won, and the
  pending action survived a saved guest and a host-app update/relaunch.
- Reboot Guest reached `[vm] guest watchdog reboot 1: fresh firmware boot
  started`, then the real guest lock screen. The host phone was not rebooted.
- That test exposed the repeated-boot-timestamp bug above. `da3d87c` fixes it;
  `e0cabbd` additionally checks the status witness under the dpkg lock and adds
  reboot-failure/lock regression coverage. Native stopped-machine setup upgraded
  the same guest to v3. Its extracted executable matched the bundled SHA-256
  `21b200d39ba4588f403ed7b055afb7a3a557845d27a00c9cc15a5631d583dc0a`.
- On `e0cabbd`, a second real reboot cleared the durable pending action. However,
  package connections did not recover until Save & close / reopen of the same
  guest. No helper or disk change was needed for that recovery, pointing to
  stale host USB session state rather than a missing finish request.
- A real removal script also requested restart; its completion button restarted
  SpringBoard. Removing the reboot fixture correctly requested no restart.

`43bc10a` adds an explicit host USB session reset at the firmware owner's
successful watchdog reboot generation change. Old service connections and mux
state are discarded before attaching to the replacement guest controller.
Physical acceptance on that exact build passed: the reboot fixture installed,
selected Reboot Guest, and booted to the guest lock screen. An early refresh
while booting timed out; after waking/unlocking and allowing startup to finish,
the package manager read all 37 installed records with no pending reboot, without
Save & close or a host-app restart. Successful readback was observed about
190 seconds after the action, including manual navigation and retries, not a
measured minimum boot time. The same session then removed the fixture through
real guest dpkg and verified the result with no restart requested. Both finish
fixtures and their temporary LAN source were removed; the original repositories
and unrelated guest packages were retained.

At `43bc10a`, iOS build 37909087066 and all nine jobs of core matrix 37909087028
passed. The iOS gate includes 60 native planning checks, 56 synthetic executor
checks, 41 USB checks, and the iOS 13 API baseline. Locally, all 77 Windows CTests
passed at `e0cabbd`; the changed firmware-boot test was rebuilt and passed again
at `43bc10a`. Its optional real-firmware reboot fixture is separate from the
ordinary unit-test pass. Physical host coverage remains iOS 15.8.5 only.

The signal fixtures contain no injected tweak code. They prove the finish
protocol and actions, not compatibility of arbitrary real-world tweaks.

## Default AppSync for new jailbreaks (development builds)

Advanced Settings includes **AppSync with jailbreak**, enabled unless explicitly
disabled. It is independent of **Packages with jailbreak**. The selection is
captured when preparation starts; disabling it neither downloads/stages AppSync
nor removes an existing installation. Repairing an already jailbroken machine
does not add or replace AppSync; use Packages for those machines.

The optional manifest pins `us.hackulo.appsync31` **1.0.2** from the
[iOS 3 Party repository](https://ios3.party/Packages):
`https://ios3.party/debs/appsync31-1.0.2.deb`, 248082 bytes, SHA-256
`c9ded74192d0e3a78855700f65be3834c53364782343338f336f2315a4e60e89`.
The repository and archive control record both declare firmware 3.1.x support,
including 3.1.3. This is not AppSync Unified or a package for other guest profiles.
Only metadata is bundled; the archive is fetched over HTTPS and verified before
the jailbreak disk transaction. The base package set/cache remains unchanged.

The guest's first-boot job installs it with ordinary `dpkg --install`, after
firmware registration and base package configuration, preserving its firmware
Pre-Depends and original maintainer scripts. It restarts any running `installd`
before Cydia registration/respring. A failed install retains the archive and
blocks the completion marker, allowing the existing job to retry. The setting
does not disable package checksums, change the host iPhone, or guarantee that
an arbitrary IPA is compatible with the emulator.
