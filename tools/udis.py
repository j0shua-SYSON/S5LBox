"""Disassemble a range of a 32-bit ARM Mach-O by virtual address."""
import os
import struct
import sys

REPO = r"F:\JOSHUA_1st_2021\projects\S5LBox"
sys.path.insert(0, os.path.join(REPO, "work", "tools", "capstone-python"))
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_ARM, \
    CS_MODE_LITTLE_ENDIAN

path = sys.argv[1]
start = int(sys.argv[2], 16) & ~1
length = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x60
arm = len(sys.argv) > 4 and sys.argv[4] == "arm"
buf = open(path, "rb").read()

ncmds = struct.unpack_from("<I", buf, 16)[0]
off, segs = 28, []
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", buf, off)
    if cmd == 0x1:
        vmaddr, vmsize, fileoff = struct.unpack_from("<III", buf, off + 24)
        segs.append((vmaddr, vmsize, fileoff))
    off += size


def vm_to_off(va):
    for vmaddr, vmsize, fileoff in segs:
        if vmaddr <= va < vmaddr + vmsize:
            return fileoff + (va - vmaddr)
    raise SystemExit("va %08x outside every segment" % va)


code = buf[vm_to_off(start):vm_to_off(start) + length]
md = Cs(CS_ARCH_ARM,
        (CS_MODE_ARM if arm else CS_MODE_THUMB) | CS_MODE_LITTLE_ENDIAN)
for i in md.disasm(code, start):
    print("%08x  %-10s %s %s" % (i.address, i.bytes.hex(), i.mnemonic,
                                 i.op_str))
