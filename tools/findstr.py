"""Locate a string in the kernelcache and every word that points at it."""
import struct
import sys

KERNEL = r"F:\JOSHUA_1st_2021\projects\iOS3-VM\firmware\kernel.macho"
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
needle = sys.argv[1].encode()
start = 0
addrs = []
while True:
    i = buf.find(needle, start)
    if i < 0:
        break
    start = i + 1
    vm = off_to_vm(i)
    if vm is None:
        continue
    # back up to the start of the C string
    j = i
    while j > 0 and buf[j - 1] not in (0, 10):
        j -= 1
    svm = off_to_vm(j)
    text = buf[j:buf.index(b"\0", j)].decode("ascii", "replace")
    print("string @ vm %08x (file %08x): %r" % (svm, j, text[:110]))
    addrs.append(svm)

for a in set(addrs):
    ref = struct.pack("<I", a)
    s = 0
    n = 0
    while True:
        k = buf.find(ref, s)
        if k < 0:
            break
        s = k + 1
        if k & 3:
            continue
        vm = off_to_vm(k)
        if vm is None:
            continue
        print("  referenced by word at vm %08x (file %08x)" % (vm, k))
        n += 1
        if n > 10:
            break
