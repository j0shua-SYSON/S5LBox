"""Read-only address lookup inside an iPhone OS dyld shared cache.

Run logs report shared-cache PCs as bare addresses because the cache is a
single blob covering 273 libraries, so a hot loop at 0x3145ad4c reads as
"userspace" and nothing more. That is the difference between "SpringBoard is
stuck somewhere" and "SpringBoard is in Security.framework's giant-integer
multiply", which is what actually decides the next move.

This maps a guest VA to its owning image, then to the nearest symbol at or
below it. Cache-embedded dylibs keep their own LC_SYMTAB with symoff/stroff
expressed as offsets into the cache file, so the nlist table can be read
directly once the dylib's Mach-O header is located.

Extract the cache from a retained work image first:
  python hfsx_extract.py <work.img> dyld_shared_cache_armv6 <out>

Usage:
  python dscmap.py <cache> <hex-va> [--count N] [--thumb]

Nothing here writes to the cache or to the image it came from.
"""
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "work", "tools", "capstone-python"))


def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]


def va_to_file_offset(blob, va):
    """Cache mappings are (address, size, fileOffset) triples of 32 bytes."""
    map_off, map_cnt = u32(blob, 16), u32(blob, 20)
    for i in range(map_cnt):
        o = map_off + i * 32
        addr, size, foff = u64(blob, o), u64(blob, o + 8), u64(blob, o + 16)
        if addr <= va < addr + size:
            return foff + (va - addr)
    return None


def owning_image(blob, va):
    """The image whose load address is the greatest one at or below va."""
    img_off, img_cnt = u32(blob, 24), u32(blob, 28)
    best = None
    for i in range(img_cnt):
        o = img_off + i * 32
        addr, path_off = u64(blob, o), u32(blob, o + 24)
        if addr <= va and (best is None or addr > best[0]):
            end = blob.index(b"\0", path_off)
            best = (addr, blob[path_off:end].decode("utf-8", "replace"))
    return best


def nearest_symbol(blob, image_va, target):
    header = va_to_file_offset(blob, image_va)
    if header is None:
        return None
    ncmds = u32(blob, header + 16)
    off = header + 28
    for _ in range(ncmds):
        cmd, size = u32(blob, off), u32(blob, off + 4)
        if cmd == 0x02:  # LC_SYMTAB
            symoff, nsyms = u32(blob, off + 8), u32(blob, off + 12)
            stroff = u32(blob, off + 16)
            best = None
            for i in range(nsyms):
                e = symoff + i * 12
                strx, typ, value = u32(blob, e), blob[e + 4], u32(blob, e + 8)
                # N_STAB entries are debugger notes, not code addresses.
                if typ & 0xe0 or not value:
                    continue
                if value <= target and (best is None or value > best[0]):
                    end = blob.index(b"\0", stroff + strx)
                    best = (value,
                            blob[stroff + strx:end].decode("utf-8", "replace"))
            return best
        off += size
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cache, va = sys.argv[1], int(sys.argv[2], 16)
    count = 32
    if "--count" in sys.argv:
        count = int(sys.argv[sys.argv.index("--count") + 1])
    thumb = "--thumb" in sys.argv

    blob = open(cache, "rb").read()
    print("cache: %s" % blob[:16].split(b"\0")[0].decode())

    file_off = va_to_file_offset(blob, va)
    if file_off is None:
        sys.exit("VA %08x is not inside any cache mapping" % va)

    image = owning_image(blob, va)
    print("image: %s" % image[1])
    print("       loaded at %08x, va is +0x%x, file offset %08x"
          % (image[0], va - image[0], file_off))

    sym = nearest_symbol(blob, image[0], va)
    if sym:
        print("symbol: %s at %08x (va is +0x%x)"
              % (sym[1], sym[0], va - sym[0]))
    else:
        print("symbol: none at or below this address")

    try:
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
    except ImportError:
        print("capstone unavailable; skipping disassembly")
        return
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
    for ins in md.disasm(blob[file_off:file_off + count * 4], va):
        print("  %08x  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))


main()
