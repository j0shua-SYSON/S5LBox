# Disk-size presets

Status on 2026-10-10: verified for prerelease integration into main; no release
has been published. The original guarded geometry foundation is retained.
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

## Guarded geometry implementation

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

## Device and regression validation (2026-10-10)

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

A subsequent ARMv6 guest fixture on the physical phone exercised real HFS I/O
beyond 4 GiB. Through ordinary guest file APIs it preallocated only 64 KiB using
`F_PREALLOCATE` / `F_VOLPOSMODE`, wrote a deterministic pattern, called `fsync`,
closed/reopened with caching/read-ahead disabled, and compared all 65,536 bytes.
The guest returned a pass and `F_LOG2PHYS` reported `0x110010000`. The image's
HFS catalog independently placed the file at allocation block 1,114,128 for
16 blocks (4 KiB each), the same physical offset. Read-only host extraction
matched every byte; SHA-256:
`e9667a3592507f860c75e37d3563b5769c541ef90110adb5b8e230feb188ba7b`.
The corresponding truncated 32-bit location `0x10010000` did not contain the
marker. This proves the tested high-offset write/read, not every disk boundary
or an unchanged hash of the entire lower 4 GiB. The first fixture queried an
unwritten HFS extent and received device offset -512; checking placement after
the write corrected the fixture, not the emulator. Test data stayed in the
disposable guest's app Documents directory; no raw guest disk writes were used.
Normal shutdown returned to Machines. After a fresh boot, the fixture reopened
the existing file read-only, repeated the uncached read/compare, and again
reported `PASS @ 110010000`. It did not rewrite the marker on that path.

Current Windows evidence (2026-10-10): **82/82 CTest tests passed**;
`test_rootfs_work --large-disks`: **28 checks passed** for synthetic 4/8 GiB
images, sparse maintenance copies and high-offset marker preservation.
The private authenticated-kernel suites were rerun: **1,496 manifest checks**
and **245 bring-up checks** passed. Their short boot smoke remains only 200,000
instructions, not a completed large-disk boot.

The final picker pass also created a fresh default 2 GiB machine, booted it to
SpringBoard, saved/reopened it and completed normal shutdown. Guest Settings >
General > About showed 2.0, 4.0 and 8.0 GB for the three presets (the legacy
guest UI labels binary capacities as GB). Both HFS volume headers agreed with
each logical image size. Host allocation was approximately 413, 415 and 488 MiB
respectively at that checkpoint, not the full selected capacity. The 4/8 GiB
machines restored saved state; the existing high-offset marker still matched.
The disposable 2 GiB machine was removed after the check.

An 8 GiB duplicate retained its capacity record. A malformed `s5lbox-disk-v1 3`
record on a disposable fixture refused provisioning and duplication without
creating a work image or another list entry. A duplicate is a fresh machine
with copied configuration, not a clone of the source's installed apps or disk.

### Persistence failure fix

Device fault injection exposed a real bug: duplication ignored a failed machine
list save, showed a successful row and left an orphan directory. Create,
duplicate, rename and delete now report save failure and roll back the in-memory
edit. Deletion saves the updated list **before** removing any machine files.
No success notification is emitted for an edit that could not be persisted.

`app/Tests/test_vminstancestore.m` runs the real Foundation store in macOS CI,
not a reimplementation. It covers all three capacities, duplicate/reload,
rename/delete, a forced save failure and an actual failed atomic write to a path
whose parent is a regular file. It checks unchanged serialized bytes, rows and
directories, absence of success notifications and preservation of a sentinel
work image on refused deletion.

The corrected `1d8f2de` iPhone build also passed one-shot save-failure injection
through the actual Duplicate, Rename and Delete UI actions. Each showed its error,
left the serialized list byte-identical and preserved existing machine files;
failed duplication left no orphan directory. Normal duplication retained 8 GiB.
The same build restored the 8 GiB guest and displayed its correct capacity.

### Remaining validation limits

These checks do not certify a completely filled 8 GiB filesystem, physical
power-loss behavior during publication, every disk boundary or every host iOS
version. Sparse maintenance copies are implemented and tested; reclaiming host
allocation after arbitrary guest file deletion is not promised. Existing user
machines are never resized automatically. Integration into main does not turn
these bounded device and host results into those broader guarantees.

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
