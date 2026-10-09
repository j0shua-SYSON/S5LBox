# Native package manager

Work branch: `feature/native-package-manager`. Not merged to main.

The host app handles repository download/decompression, search, dependency
planning and package downloads. The running guest executes its own `dpkg` and
maintainer scripts. This avoids interpreting repository processing inside the
emulator without pretending that arbitrary guest scripts can run on the host.

## Using it

New jailbreak preparations include the guest helper. For an existing jailbroken
machine, return to Machines and choose **App Settings > Set up package manager**.
This explicitly discards the saved running state, validates the stopped disk,
and adds the helper using the existing crash-safe disk-publication transaction.
It does not rerun Cydia repair. Important: historical snapshots block setup;
back up important machine data first. Strict filesystem errors are not repaired
or ignored by helper setup.

Then open the machine and use **Machine Settings > Packages**. A dedicated
full-screen, five-tab workspace opens; it does not redesign the rest of S5LBox:

- **Packages:** connection status, all packages, and category browsing.
- **Sources:** iOS 3 Party and Saurik initially. Open a source to browse its
  packages; add flat HTTP/HTTPS repositories or explicitly add legacy BigBoss
  HTTP. Removing a source never uninstalls packages.
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
checks remain enabled; an expired certificate is not bypassed. Legacy HTTP is
an explicit source choice with an interception warning. SHA-256 and exact size
checks protect against corrupt downloads, not a malicious or intercepted index.
Older sources such as Saurik publish only MD5. SHA-1/MD5 are accepted solely
over verified HTTPS, with a warning in the transaction preview, and are described
as weak corruption checks rather than authenticity guarantees. A present but bad
SHA-256 never falls back to a weaker digest. Only add trusted sources: packages may execute root scripts inside the guest.
These privileges do not extend to the host iPhone.

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
operation. The later compact-title/visible-version adjustments require their
own layout check; they do not change the executor.

The same test guest did not have Substrate installed. Planning iOS3 Folders 1.2
refused the Substrate / Safe Mode dependency cycle before download or mutation,
as designed. Cycle support remains a real limitation of this first installer;
it is not yet a complete Cydia replacement for tweak bootstrapping.

iOS build 37901568166 at `1c96798` passed the explicit iOS 13 API-baseline check
and the 55 native / 15 synthetic package checks. Its app sources equal
`def62a7`; only CI and documentation changed. Runtime testing on other host
iOS versions remains outstanding.

The first physical setup attempt exposed a pre-existing Cydia-repair assumption:
after Cydia reorganizes Applications into a symlink, its old repair probe reports
`path component 0 of /Applications/Cydia.app/Cydia_ is not a directory`.
Existing-machine package setup is now separate from that repair path. No raw
filesystem patch was used to bypass the refusal.

Protocol/dependency references:
[Debian relationships](https://www.debian.org/doc/debian-policy/ch-relationships.html)
and [control/version fields](https://www.debian.org/doc/debian-policy/ch-controlfields.html).
