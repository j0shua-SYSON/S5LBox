"""Name the Objective-C selector behind a PC-relative literal load in a 32-bit
Mach-O.

SpringBoard is stripped, so a run log can only say "objc_msgSend at 0xa7be" and
the interesting question -- WHICH message -- is unanswerable from the trace. The
selector is reachable statically: the compiler emits

    ldr r0, [pc, #imm]     ; r0 = &selref
    ldr r1, [r0]           ; r1 = selector (a pointer into __objc_methname)
    blx objc_msgSend

so following literal -> selref -> methname recovers the name.

Usage:
  python objcsel.py <macho> <hex-va-of-the-ldr> [--arm]

The address is the `ldr rX, [pc, #imm]` itself. Thumb is assumed; pass --arm for
ARM-state code. Read-only.
"""
import struct
import sys


def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def segments(blob):
    """(vmaddr, vmsize, fileoff) for every LC_SEGMENT in a 32-bit Mach-O."""
    ncmds = u32(blob, 16)
    off, segs = 28, []
    for _ in range(ncmds):
        cmd, size = u32(blob, off), u32(blob, off + 4)
        if cmd == 0x1:  # LC_SEGMENT
            segs.append((u32(blob, off + 24), u32(blob, off + 28),
                         u32(blob, off + 32)))
        off += size
    return segs


def va_to_off(segs, va):
    for vmaddr, vmsize, fileoff in segs:
        if vmaddr <= va < vmaddr + vmsize:
            return fileoff + (va - vmaddr)
    return None


def cstring(blob, off, limit=256):
    end = blob.index(b"\0", off, off + limit)
    return blob[off:end].decode("utf-8", "replace")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    blob = open(sys.argv[1], "rb").read()
    va = int(sys.argv[2], 16)
    thumb = "--arm" not in sys.argv
    segs = segments(blob)

    off = va_to_off(segs, va)
    if off is None:
        sys.exit("VA %08x is outside every segment" % va)
    insn = struct.unpack_from("<H", blob, off)[0] if thumb else u32(blob, off)

    # Thumb LDR (literal), T1: 01001 Rt imm8, offset = imm8*4 from
    # Align(PC,4) where PC is the instruction address + 4.
    if thumb:
        if (insn & 0xf800) != 0x4800:
            sys.exit("not a Thumb LDR literal at %08x (halfword %04x)"
                     % (va, insn))
        imm = (insn & 0xff) * 4
        pool = ((va + 4) & ~3) + imm
    else:
        if (insn & 0x0f7f0000) != 0x051f0000:
            sys.exit("not an ARM LDR literal at %08x (word %08x)" % (va, insn))
        imm = insn & 0xfff
        pool = (va + 8) + (imm if (insn & (1 << 23)) else -imm)

    selref = u32(blob, va_to_off(segs, pool))
    print("literal pool  %08x -> selref %08x" % (pool, selref))
    name_ptr = u32(blob, va_to_off(segs, selref))
    print("selref        %08x -> methname %08x" % (selref, name_ptr))
    print("selector      %s" % cstring(blob, va_to_off(segs, name_ptr)))


main()
