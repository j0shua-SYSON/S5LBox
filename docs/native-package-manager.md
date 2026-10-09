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

Then open the machine and use **Machine Settings > Packages**:

- **Browse:** search names/descriptions; select a version and review every
  dependency before Install. Merely browsing/downloading never installs a tweak.
- **Installed:** read the running guest's dpkg status; remove one nonessential
  package only if its reverse dependencies remain satisfied. Configuration
  files are retained.
- **Sources:** iOS 3 Party and Saurik initially; add flat HTTP/HTTPS repository
  directories, remove a source, or explicitly add BigBoss's legacy HTTP source.
  Removing a source never uninstalls guest packages.

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
Only add trusted sources: packages may execute root scripts inside the guest.
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
execution wait. A package without SHA-256 is refused. Successful transfer is not
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

At `655b9ef`: 45 native planning checks, 15 synthetic executor protocol checks,
77 Windows CTests, and iOS build 37896428525 passed. Synthetic executor tests
substitute a fake dpkg, so they do not prove firmware execution. Subsequent
device acceptance and final-SHA results are recorded below when established.

The first physical setup attempt exposed a pre-existing Cydia-repair assumption:
after Cydia reorganizes Applications into a symlink, its old repair probe reports
`path component 0 of /Applications/Cydia.app/Cydia_ is not a directory`.
Existing-machine package setup is now separate from that repair path. No raw
filesystem patch was used to bypass the refusal.

Protocol/dependency references:
[Debian relationships](https://www.debian.org/doc/debian-policy/ch-relationships.html)
and [control/version fields](https://www.debian.org/doc/debian-policy/ch-controlfields.html).
