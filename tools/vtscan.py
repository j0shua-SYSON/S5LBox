"""Find which vtable slot holds a given kernel function pointer.

`ldr pc, [r3, #N]` only names a slot; the callee is whatever the vtable holds.
This scans the kernelcache for words equal to each candidate accessor and
reports the implied vtable base for the slot under test, so the slot can be
resolved instead of guessed.
"""
import os
import struct
import sys

# Derived from this file's own location rather than hardcoded: these
# scripts live in <repo>/tools/, and an absolute path here is a path a
# project rename silently rewrites. That is exactly what happened -- the
# S5LBox rename replaced the folder name inside the string and left six
# tools pointing at a directory that has never existed.
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "firmware", "kernel.macho")
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
