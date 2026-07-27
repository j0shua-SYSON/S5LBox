"""Read-only exact disassembly helper for the 7E18 prelinked kernelcache.

Maps kernel VM addresses to file offsets using the __PRELINK_TEXT and __TEXT
segment mappings printed by machoinfo, then disassembles with Capstone.
Nothing here writes to the firmware; it only reads bytes.
"""
import sys
import os

REPO = r"F:\JOSHUA_1st_2021\projects\S5LBox"
sys.path.insert(0, os.path.join(REPO, "work", "tools", "capstone-python"))

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN

KERNEL = os.path.join(REPO, "firmware", "kernel.macho")

# (vm_start, vm_end, file_offset) from machoinfo.
SEGS = [
    (0xC0008000, 0xC020D000, 0x00000000),  # __TEXT
    (0xC020D000, 0xC0260000, 0x00205000),  # __DATA
    (0xC0000000, 0xC0005000, 0x0021D000),  # __HIB
    (0xC02CD000, 0xC0795000, 0x0028F000),  # __PRELINK_TEXT
    (0xC0795000, 0xC07D1000, 0x00757000),  # __PRELINK_INFO
    (0xC0261000, 0xC02CC5A4, 0x00223000),  # __LINKEDIT
]


def vm_to_off(va):
    for lo, hi, off in SEGS:
        if lo <= va < hi:
            return off + (va - lo)
    raise ValueError("va 0x%08x is outside every mapped segment" % va)


def read(va, n):
    with open(KERNEL, "rb") as f:
        f.seek(vm_to_off(va))
        return f.read(n)


def dis(va, n, thumb=True):
    va &= ~1
    code = read(va, n)
    md = Cs(CS_ARCH_ARM,
            (CS_MODE_THUMB if thumb else CS_MODE_ARM) | CS_MODE_LITTLE_ENDIAN)
    md.detail = False
    out = []
    for i in md.disasm(code, va):
        out.append("%08x  %-10s %s %s" % (
            i.address, i.bytes.hex(), i.mnemonic, i.op_str))
    return out


if __name__ == "__main__":
    mode_thumb = True
    args = sys.argv[1:]
    if args and args[0] == "--arm":
        mode_thumb = False
        args = args[1:]
    if args and args[0] == "--words":
        base = int(args[1], 0)
        count = int(args[2], 0)
        data = read(base & ~1, count * 4)
        for i in range(count):
            w = int.from_bytes(data[i * 4:i * 4 + 4], "little")
            print("%08x: %08x" % (base + i * 4, w))
        sys.exit(0)
    start = int(args[0], 0)
    length = int(args[1], 0) if len(args) > 1 else 0x60
    for line in dis(start, length, mode_thumb):
        print(line)
