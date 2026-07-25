"""Encoding-directed call-site scan for a 32-bit ARM Mach-O.

A linear disassembly of __text desyncs on inline data and on ARM/Thumb mixing,
which silently loses call sites. This instead tests every candidate encoding at
every offset and keeps only those whose computed target matches, so a miss is a
real absence rather than a decoder artifact.

Covers the ARMv6 Thumb BL/BLX halfword pair and the ARM BL/BLX word forms.
"""
import struct
import sys


def sign_extend(value, bits):
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def main():
    path, target = sys.argv[1], int(sys.argv[2], 16) & ~1
    buf = open(path, "rb").read()

    ncmds = struct.unpack_from("<I", buf, 16)[0]
    off, text = 28, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, off)
        if cmd == 0x1:
            nsects = struct.unpack_from("<I", buf, off + 48)[0]
            so = off + 56
            for _s in range(nsects):
                name = buf[so:so + 16].rstrip(b"\0").decode()
                addr, ssize, foff = struct.unpack_from("<III", buf, so + 32)
                if name == "__text":
                    text = (addr, ssize, foff)
                so += 68
        off += size
    if not text:
        raise SystemExit("no __text section")
    base, size, foff = text
    code = buf[foff:foff + size]
    print("__text vm %08x..%08x (%d bytes); target %08x"
          % (base, base + size, size, target))

    hits = []

    # Thumb BL / BLX pair: H1 = 11110 S imm10, H2 = 11111 imm11 (BL)
    # or 11101 imm11 (BLX). offset = SignExtend(S:imm10:imm11:0), 23 bits.
    for i in range(0, len(code) - 3, 2):
        h1 = struct.unpack_from("<H", code, i)[0]
        if (h1 & 0xF800) != 0xF000:
            continue
        h2 = struct.unpack_from("<H", code, i + 2)[0]
        if (h2 & 0xF800) == 0xF800:
            kind, blx = "bl", False
        elif (h2 & 0xF800) == 0xE800:
            kind, blx = "blx", True
        else:
            continue
        offset = sign_extend(((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1), 23)
        dest = (base + i + 4 + offset) & 0xFFFFFFFF
        if blx:
            dest &= ~3
        if (dest & ~1) == target:
            hits.append((base + i, "thumb " + kind, dest))

    # ARM BL: cond 1011 imm24 ; ARM BLX(imm): 1111101H imm24
    for i in range(0, len(code) - 3, 4):
        word = struct.unpack_from("<I", code, i)[0]
        top = word >> 24
        if (top & 0x0F) == 0x0B and (top >> 4) != 0xF:
            kind = "arm bl"
        elif (word >> 25) == 0x7D:
            kind = "arm blx"
        else:
            continue
        offset = sign_extend(word & 0xFFFFFF, 24) << 2
        dest = (base + i + 8 + offset) & 0xFFFFFFFF
        if (dest & ~1) == target:
            hits.append((base + i, kind, dest))

    hits.sort()
    print("%d candidate call site(s)" % len(hits))
    for a, k, d in hits[:60]:
        print("  %08x  %-10s -> %08x" % (a, k, d))


main()
