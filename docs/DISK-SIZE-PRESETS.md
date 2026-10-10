# Disk-size presets: candidate implementation

Status on 2026-10-10: implemented on `feature/audio-disk-presets`, not merged
into main or released. The original guarded geometry foundation is retained.
New Machine offers 2 (default), 4 and 8 GiB; a strict `disk-size-v1` record
travels with each machine. Fresh provisioning and jailbreak honor it, duplicates
copy the choice, and missing records retain legacy behavior. Existing disks are
never resized at boot. Malformed records refuse before install transaction work.

Maintenance cloning now preserves zero extents while hashing every logical byte,
so choosing a larger capacity does not inherently allocate that capacity on the
host. This is sparse allocation, not a promise that deleting guest files always
reclaims host space. Snapshot retention and actual guest data still consume space.

## Why raising the cap alone is unsafe

The host file adapter, raw bridge and physical-copy bridge already carry
64-bit offsets. The supported 7E18 kernel's memory-disk driver does not
consistently do so:

- The device-tree RAMDisk property has a 32-bit byte count.
- The strategy routine computes a 64-bit block offset, saves both halves on
  its stack, then discards the high half when constructing the media token.
- Bounds checks and both block-count ioctls shift a 32-bit page count into a
  32-bit byte count. At 4 GiB that wraps to zero.

The read-only disassembly of the project's exact authenticated kernel is
the authority for addresses and register contracts. Apple's corresponding
[XNU memory-disk source](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-1456.1.26/bsd/dev/memdev.c)
helps identify the operations; it is not substituted for exact-build proof.

## Implemented, but kept off the RC

`ios3_bringup_gate_configure` selects the extended manifest only when its
already-assigned root medium exceeds 2 GiB. Smaller media retain all original
patch bytes and the desktop harness's existing manifest remains unchanged.
Reconfigure after changing media; never mutate a running medium's geometry.

The existing whole-kernel SHA-256, segment and loaded-RAM checks still precede
the atomic patch transaction. Five additional two-byte sites are authorized:

| Address | Operation |
| --- | --- |
| `c01a1b66` | Register the real page count; retain a bounded 2 GiB bootstrap device-tree length |
| `c0074088` | Compare the complete 64-bit offset and select native map/trim/EOF/error paths |
| `c00740e2` | Load the saved high offset instead of replacing it with zero |
| `c0073eb4` | Compute the 64-bit ioctl's block count without byte-count overflow |
| `c0073f22` | Same for the legacy 32-bit block-count ioctl |

`md_geometry_bridge` accepts only the configured privileged Thumb sites,
validates the live md0 page count/base/flags/sector size and translates every
metadata word through the guest MMU into bounded RAM. It performs no disk I/O
and writes no guest memory. Native registration, buffer mapping/unmapping,
trimming, completion, error handling and ioctl output stores remain native.

The high-offset instruction, stack slots and continuation register contracts
are covered by executing the actual authenticated kernel instructions in
`test_bringup`, not just by calling the host arithmetic helper.

## Foundation evidence and limits (2026-10-06)

- Windows Release build and CTest: **74/74 passed**, with JIT disabled.
- Geometry helper: 297 checks passed, including 2/4/8 GiB, both ioctl variants,
  4 GiB crossing, 8 GiB tail, EOF, clipping, negative offsets, malformed
  metadata, MMU failures, unchanged failure state and real SVC retirement.
- Existing physical-copy bridge: 1,278 checks passed.
- Existing raw bridge: 357 checks passed.
- Exact-kernel manifest suite: 1,496 checks passed with the private kernel;
  includes optional-site guards and preservation of the legacy manifest.
- Private `test_bringup`: 245 checks passed; runs the real registration, capacity-store, trim and
  token-construction instructions with 4 and 8 GiB geometries. Its ordinary
  boot smoke test still runs only 200,000 instructions.

These are **not** proof of an 8 GiB HFS boot, a completed jailbreak, snapshot
restore, a filled filesystem or physical-phone execution of the new branch.
No existing machine, firmware input, installed app or disk was modified.

## Release validation still required

Physical iPhone 6s Plus / iOS 15.8.5 (2026-10-10): new disposable machines
selected through the native picker provisioned exact 4/8 GiB images, each
initially allocating about 414 MiB on APFS. Both booted to SpringBoard and
saved/reopened; the 8 GiB guest also cold-booted again and created a Voice Memos
recording. Primary and alternate 8 GiB HFS headers both report 4 KiB blocks and
2,097,152 blocks. Existing user machines were not modified. This does not yet
prove guest I/O beyond 4 GiB.

The 8 GiB guest subsequently completed normal UI shutdown (the app's PMU-witness
path returned to Machines). With `6eb0652` installed, the native Jailbreak flow
completed both the jailbreak transaction and package-service preparation, then
automatically started the guest. The live image stayed exactly 8 GiB; its
recorded Voice Memos file survived maintenance. The whole machine directory,
including its retained 119 MiB snapshot, allocated about 573 MiB. The two
staging directories were removed by successful publication. This is physical
maintenance/space evidence, not proof of high-offset guest data I/O.

Current Windows evidence (2026-10-10): **81/81 CTest tests passed**;
`test_rootfs_work --large-disks`: **28 checks passed** for synthetic 4/8 GiB
images, sparse maintenance copies and high-offset marker preservation.
The private authenticated-kernel suites were rerun: **1,496 manifest checks**
and **245 bring-up checks** passed. Their short boot smoke remains only 200,000
instructions, not a completed large-disk boot.

The implementation now includes the picker, persistence, provisioning and sparse
maintenance copy described below. Host regression tests exercise synthetic HFS
growth, high-offset data preservation and allocated size; `test_rootfs_work
--large-disks` covers actual 4/8 GiB logical images. This remains distinct from
guest boot/jailbreak/save/reopen and physical iOS allocation checks.

The original validation checklist is retained to make the outstanding device
gates explicit; implementation of a checkbox is not proof of its live outcome.

1. Preserve sparse space during maintenance copies. `rootfs_work.c` creates
   sparse growth, but `copy_source` subsequently writes every source chunk.
   A mostly empty 8 GiB source can therefore become an allocated 8 GiB clone.
   Verify logical bytes, actual allocation and crash-safe publication on both
   Windows and iOS. Do not promise that capacity equals physical usage.
2. Create disposable real 4/8 GiB HFS images; validate primary/alternate headers,
   allocation bitmap growth, reads and writes beyond 4 GiB, and no low-offset
   aliasing. Do not edit imported firmware or an existing user's machine.
3. Boot those images with the app's actual boot owner and verify guest-reported
   capacity, file creation, clean shutdown, reopen and saved-state restore.
4. Persist the selected size per new machine, using a strict versioned record
   or a deliberate schema migration. Missing records must preserve legacy
   behavior; malformed records must not silently choose a different size.
5. Make new-image creation and fresh jailbreak rebuilding honor the same
   choice. Maintenance must retain larger existing volumes, never shrink them.
   Validate duplicates and failure/rollback paths too.
6. Only then add the small native creation picker, with 2 GiB as the default,
   and physically test creation plus jailbreak on disposable machines.

Do not merge this experiment into the RC merely because the boundary tests pass.

## Reproducing the private checks

Build in a project-local directory with `S5LBOX_JIT=OFF`. Set TEMP/TMP to the
project's work directory. Run the normal CTest suite, then:

```powershell
$env:S5LBOX_TEST_FIRMWARE_DIR = '<project>\firmware'
& '<build>\core\test_bringup.exe'
& '<build>\core\test_ios3_kernel_patch.exe' '<project>\firmware\kernel.macho'
```

The firmware directory is read-only input. Public CI has no Apple firmware;
its skipped private cases must not be reported as real-kernel execution.
