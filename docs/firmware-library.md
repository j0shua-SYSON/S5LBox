# Firmware library

Settings > Advanced > Firmware retains imported firmware sets. Use **+** to
import another IPSW, tap a row to choose the default, and long-press to remove
an unused set. When several sets are available, creating a machine offers a
firmware choice. Existing machines and their duplicates keep their original
firmware, independently of the default.

## Profiles and validation

`tools/ios3_firmware_profiles.h` contains component sizes and SHA-256 hashes for:

| Firmware | Device | Build | Distribution |
| --- | --- | --- | --- |
| iPhone OS 3.1.3 | iPhone 3G (`iPhone1,2`) | 7E18 | Stock IPSW |
| Whited00r 7.1 | iPhone 3G (`iPhone1,2`) | 7E18 | Normal |

The Whited00r profile identifies the three decoded components from the tested
Normal distribution. It does not cover Unlocker, iPhone 2G, or other releases.
Import recognition alone is not a claim that every bundled tweak works.

The importer checks manifest identity, decodes the kernel, device tree and
root filesystem, and compares all three against one complete profile. Matching
component sizes alone does not authenticate firmware. Public decryption keys
remain separate in `VMFirmwareKeys.c`; no Apple firmware is included.

Advanced checksum skipping affects the next import only. Each prepared set
retains its own policy. Manifest and format checks, bounded extraction, HFS
validation and boot-time kernel layout checks still apply. The library can
retain multiple supported sets; it does not itself add support for another OS
or hardware model. Future OS support needs its own importer and boot validation.

## Storage and publication

Each import creates `firmware/prepared-<UUID>/` containing the three prepared
files, `import-policy-v1`, and display metadata in `firmware-info.plist`.
`import-in-progress` keeps incomplete output out of the library. Successful
publication removes the marker and atomically updates `current-firmware.txt`.
Normal failures and cancellations remove that attempt's output. A process kill
can leave an unpublished directory, which remains unselectable.

`Machines/<id>/firmware-source.txt` stores a relative generation identifier.
Older machines are pinned before the default changes. Malformed or missing
pinned sources fail explicitly; a machine must not silently switch to a
different firmware. Legacy loose files use the identifier `legacy` and remain
managed through Files. The default and sets referenced by machines cannot be
deleted through the library.

Firmware is shared read-only between machines. Each bootable machine has its
own writable work disk. Importing the same IPSW again currently creates another
generation; imports are not deduplicated.

## Custom boot preparation

App disk preparation resolves the active `/etc/fstab` through the HFS catalog,
including the `/etc` symlink, validates its file extents, and rewrites that file
to mount `/dev/md0`. It preserves the source image and unrelated bytes. A raw
search is insufficient: a custom image can retain an obsolete stock file while
using a different active filesystem table.

New app disks with networking enabled receive a dedicated
`/System/Library/LaunchDaemons/com.s5lbox.ppp.plist`. This avoids depending on an
unused Apple job being present in the active LaunchDaemons directory. Existing
prepared disks are not silently rewritten. The older desktop harness's
explicit patch options remain available.

## Verification

Portable tests cover decoded component checks, kernel gates, active-catalog
filesystem-table rewriting, and production disk preparation. Native tests cover
publication, source pinning, default changes, duplication, damaged records,
staging visibility, and removal protection. Full IPSW import and guest boot
checks use private firmware and are separate from synthetic public CI fixtures.

Physical-device testing of the Whited00r Normal profile covered strict import,
an 8 GB machine reaching SpringBoard, touch navigation, Settings, guest unlock,
and an automatic Restart returning to a usable guest. The restart test confirmed
guest power-off before saving, followed by a fresh boot. Both observed variants
of Whited00r's themed power-off screen are recognized; stock shutdown detection
remains covered separately.

The same test installation retained three stock machines. Changing the default
did not change their firmware pins; duplicating a stock machine while Whited00r
was the default retained its stock source. Removal of firmware used by machines
was disabled. PPP negotiation, guest USB enumeration and clock synchronization
also completed, and guest Safari loaded an HTTP page after the restart. These
checks do not establish compatibility for every bundled
app or tweak, or validate Packages and live IPA installation on Whited00r.
