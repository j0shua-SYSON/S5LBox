# S5LBox

Run real **iPhone OS 3.1.3** inside an app on a modern iPhone.

S5LBox emulates the S5L8900 hardware used by the iPhone 3G. It runs Apple's
actual operating system—not a recreation of its interface. You supply the
firmware; no Apple firmware or decryption keys are included.

[![core-tests](https://github.com/j0shua-SYSON/S5LBox/actions/workflows/core-tests.yml/badge.svg)](https://github.com/j0shua-SYSON/S5LBox/actions/workflows/core-tests.yml)
[![ios-build](https://github.com/j0shua-SYSON/S5LBox/actions/workflows/ios-build.yml/badge.svg)](https://github.com/j0shua-SYSON/S5LBox/actions/workflows/ios-build.yml)

## Status

An initial release candidate is being prepared. This is still experimental.

- Boots the real guest OS, including its lock screen, home screen and apps.
- Supports touch, physical-button controls and saving/resuming the last session.
- Includes optional guest networking and a Cydia installation flow.
- Uses a build-time-generated ARM64 engine with interpreter fallback. No runtime
  code generation or host jailbreak is required.

Animations, keyboard input and heavier tasks can still be slow. Original-device
responsiveness and consistent 30 fps are **not** established. Audio playback is
not available, MBX graphics remain experimental, and not every app or tweak works.
Old websites and package repositories may also be unavailable or incompatible.

## Get started

You need an ARM64 iPhone running **iOS 13 or later**, your own supported
**iPhone 3G / iPhone1,2, iPhone OS 3.1.3 (7E18)** firmware and its required keys.
The deployment target is not a guarantee that every device/version has been
tested. Keep several gigabytes of storage free for import, guest disks and
temporary installation copies.

1. Obtain `S5LBox.ipa` from a successful
   [ios-build workflow run](https://github.com/j0shua-SYSON/S5LBox/actions/workflows/ios-build.yml)
   for the branch you intend to test. The Actions artifact is only ad-hoc signed:
   **re-sign it with your own valid provisioning profile before installing on
   stock iOS**.
2. Open S5LBox. From **Machines**, tap the gear and choose **Import from an IPSW**.
   Select your firmware and provide the keys requested by the importer.
3. New machines default to **MBX** graphics (experimental), with the CPU
   software-renderer override off. **CPU software** remains a compatibility
   option. Return to Machines and open a machine. Initial disk preparation
   and a cold boot take longer than reopening a saved session.
4. Use **Back** and choose **Save & close** to resume later, or **Shut down** to
   power off iPhone OS and cold-boot next time. Keep S5LBox open until it returns
   to Machines before force-quitting or copying machine files.

Without supported firmware, the app runs a built-in test program—not iPhone OS.

The Machines gear opens **App Settings** for defaults, firmware and jailbreak.
Inside a machine, **Settings** opens **Machine Settings** for that session's
pause behavior, snapshots and power controls. Session changes do not alter
defaults for other machines; Developer Mode adds session diagnostics.

### Networking and Cydia

Guest networking and internet routing are **on by default for new machines**.
To opt out before the first open, disable **Guest networking (PPP over uart4)**
under **App Settings → Developer Mode**. Explicit saved choices are preserved;
these defaults do not retrofit an existing offline disk.

To install Cydia, prepare a machine first, return to Machines, then open
**Settings → Jailbreak** and follow its prompts. This affects the **guest only**,
not your host iPhone. Packages are downloaded separately; they are not bundled.
S5LBox shuts down the selected guest before installing. If shutdown cannot be
confirmed, it leaves the machine open and does not start installation.
Back up guest data first: a new installation replaces the selected guest disk,
while supported upgrades use a separate migration path.

Put `.ipa` files in **Files → On My iPhone → S5LBox → IPAs**, or use **Import**
in **App Settings → Install IPA**. The library has a separate **Install** button
for each app; adding files never installs everything. Compatible, unencrypted
ARMv6 apps install into the selected jailbroken guest, not your host iPhone.
Newer-iOS apps, encrypted apps and replacing existing apps are not supported.

### Keep your data safe

Each machine has its own writable disk. **Duplicate copies configuration, not
guest data.** Deleting a machine also deletes its files. Cydia machines use a
2 GiB guest volume, and installation/migration backups can require additional
host storage. Copy a stopped machine's complete folder from
**Files → On My iPhone → S5LBox → Machines** before experimenting; do not mix a
saved state from one moment with a disk from another. Named snapshots are not
yet a supported replacement for this backup.

## Build and test

The portable core needs a C compiler and CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Public tests do not require Apple firmware. The iOS app is built with Xcode;
the [iOS workflow](.github/workflows/ios-build.yml) contains the build and
packaging steps.

## More detail

- [Detailed README](docs/README-DETAILED.md) — the previous long-form overview,
  technical explanations and historical measurements.
- [Architecture](docs/ARCHITECTURE.md) and [firmware preparation](docs/BOOT_CHAIN.md).
- [Validation](docs/QUALITY.md), [development history](docs/BOOTLOG.md) and
  [performance notes](docs/hotpath.md). These include historical results, not
  blanket compatibility guarantees.

## License

[MIT](LICENSE). Created by [j0shua-SYSON](https://github.com/j0shua-SYSON).
Supply only firmware you are entitled to use. S5LBox is independent of Apple
and is not affiliated with or endorsed by Apple.
