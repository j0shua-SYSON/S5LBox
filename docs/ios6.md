# iPhone 3GS / iOS 6 bring-up

The initial iOS 6 target is iPhone 3GS running 6.1.6. The current change adds
an instruction-profile boundary for its Cortex-A8. A complete S5L8920 machine,
kernel boot, and SpringBoard have not been demonstrated. The existing iPhone
OS 3 machine and application defaults remain ARM1176/S5L8900.

## Target evidence

[Apple's iOS 6.1.6 bulletin](https://support.apple.com/en-us/103607) confirms
the device/version pairing. `BuildManifest.plist` and `Restore.plist` inside
the [Apple restore archive](https://secure-appldnld.apple.com/iOS6.1/091-3457.20140221.Btt3e/iPhone2,1_6.1.6_10B500_Restore.ipsw)
were retrieved with validated HTTP 206 ranges and ZIP member CRC checks.
They identify:

| Property | Manifest value |
| --- | --- |
| Product | `iPhone2,1` |
| Version / build | `6.1.6` / `10B500` |
| Board / board ID | `n88ap` / `0x00` |
| Platform / chip ID | `s5l8920x` / `0x8920` |
| Kernel | `kernelcache.release.n88` |
| Device tree | `Firmware/all_flash/all_flash.n88ap.production/DeviceTree.n88ap.img3` |
| System image | `048-2955-001.dmg` |

The archive is 822,970,962 bytes; it has not been downloaded in full or
independently hashed. SHA-256 of the retrieved manifest is
`87175db6ed8043e215add63f16e774d63f4d2792b2bbb852cd1d966156b7cf00`.
The decrypted device tree has 89 nodes and parses to its exact 57,856-byte
length. Its CPU compatibility is `ARM,cortex-a8` / `ARM,v7`; the CPU revision,
memory size, and clock properties are zero placeholders filled during boot.
They must not be treated as measured hardware values. The peripheral mapping
also differs from S5L8900: its UART0 maps to `0x82500000` and the PL192 VIC
window to `0xbf200000`, using the tree's parent ranges.

## CPU boundary

`arm_arch_t` values are identifiers, not ordered architecture levels.
ARM1176 remains zero and Swift remains one; Cortex-A8 has its own identifier.
Explicit predicates allow A32 MOVW/MOVT on Swift and Cortex-A8, while A32
SDIV/UDIV require Swift. Unknown profiles cannot execute or reset CPU state.
Arm documents Cortex-A8 as having no integer division in either instruction
set in [DDI0344K, section 3.2.15](https://documentation-service.arm.com/static/5e8e1ac688295d1e18d35fde)
and its [division support table](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/divide-and-conquer).

`arm_reset()` explicitly selects ARM1176 and is safe on uninitialized storage.
`arm_reset_profile()` resets the implemented state with a validated explicit
profile. Board reset/wake paths must select their own profile when a new board
is introduced. Directly setting the field does not create an S5L8920 machine.

The legacy snapshot format describes S5L8900 and has no architecture field.
Its bytes and version remain unchanged; save/load now reject a non-ARM1176
machine before dropping the profile or changing live state. A future S5L8920
format must explicitly identify its CPU and board.

Signed-static fast paths continue to require ARM1176. A Cortex-A8 fixture
checks interpreter fallback, including refusal of unsupported divide, across
the basic, persistent, graph, and compact configurations where available.
Native handler execution requires an AArch64 host; an x86 Windows build cannot
establish that result.

## Remaining architecture and boot gaps

- The CP15 identification, cache/TLB controls, reset values, exception state,
  and memory translation paths still need a Cortex-A8 audit and implementation.
  The instruction profile is not a complete system-register model.
- Thumb decode currently handles Thumb-1 and legacy BL/BLX pairs. Thumb-2,
  IT state, and the required ARMv7 instruction families remain to implement.
- VFP stores only d0-d15 and models VFP11/VFPv2. Cortex-A8 VFPv3 and NEON,
  including the wider register file and context-switch semantics, are absent.
- The SoC, interrupt wiring, storage, graphics, input and power devices are
  currently S5L8900-specific. Build them from the N88 firmware requirements.
- Boot arguments, device-tree relocation, importer/storage selection, and
  any compatibility patches need explicit target/version guards. Existing
  iPhone OS 3 patches are not evidence of iOS 6 compatibility.
- The current IMG3 helper copies a partial final AES block as plaintext. It
  produces a 36-byte decompression shortfall and an Adler-32 mismatch on this
  kernelcache. A separate host probe decrypting the entire final block,
  including its one byte of DATA-tag padding, produces all 11,821,056 bytes
  with matching Adler-32 `c4bee74e`. The resulting kernel SHA-256 is
  `ec4787ac012567f9c10ba2d5ab611058545bce7e17cd95ef310eba94bfca9b78`.
  The production extraction helper still needs that fix and regression tests.

Host tests, exact-commit builds, firmware analysis, guest boot traces, and
physical app behavior are separate evidence. No iOS 6 boot or usability claim
follows from passing instruction tests.
