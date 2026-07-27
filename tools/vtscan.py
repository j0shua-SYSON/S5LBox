"""Find which vtable slot holds a given kernel function pointer.

`ldr pc, [r3, #N]` only names a slot; the callee is whatever the vtable holds.
This scans the kernelcache for words equal to each candidate accessor and
reports the implied vtable base for the slot under test, so the slot can be
resolved instead of guessed.
"""
import struct
import sys

KERNEL = r"F:\JOSHUA_1st_2021\projects\S5LBox\firmware\kernel.macho"
SEGS = [
    (0xC0008000, 0xC020D000, 0x00000000),
    (0xC020D000, 0xC0260000, 0x00205000),
    (0xC0000000, 0xC0005000, 0x0021D000),
    (0xC02CD000, 0xC0795000, 0x0028F000),
    (0xC0795000, 0xC07D1000, 0x00757000),
    (0xC0261000, 0xC02CC5A4, 0x00223000),
]


def off_to_vm(off):
    for lo, hi, base in SEGS:
        if base <= off < base + (hi - lo):
            return lo + (off - base)
    return None


buf = open(KERNEL, "rb").read()
targets = [int(a, 16) for a in sys.argv[1:-1]]
slot = int(sys.argv[-1], 16)

for t in targets:
    needle = struct.pack("<I", t)
    start = 0
    found = 0
    while True:
        i = buf.find(needle, start)
        if i < 0:
            break
        start = i + 1
        if i & 3:
            continue
        vm = off_to_vm(i)
        if vm is None:
            continue
        found += 1
        print("%08x found at file %08x vm %08x -> implied vtable base %08x"
              % (t, i, vm, vm - slot))
        if found > 8:
            break
    if not found:
        print("%08x: not found in any mapped segment" % t)
