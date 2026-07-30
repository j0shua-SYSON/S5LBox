"""Search an iPhone OS dyld shared cache for symbols by NAME.

dscmap.py answers "what is at this address". This answers the other direction,
which is what you need when you know what a function is called but not where it
lives -- the case every time a new layer of the touch or graphics stack has to
be read. Written while chasing which layer applies the Y flip between
MultitouchSupport's normalised contact and a screen pixel; finding
_GSEventGetLocationInWindow by name took one call where a linear hunt through
273 images would have taken many.

Extract the cache from a retained work image first:
  python hfsx_extract.py <work.img> dyld_shared_cache_armv6 <out>

Usage:
  python dscsym.py <cache> <name-substring> [image-substring]

Both needles are case-insensitive. The image filter matches the full install
path, so 'multitouch' and 'PrivateFrameworks' both work.

Note that a cache lists some images at more than one load address, so the same
symbol can legitimately appear twice with different values; both are printed
rather than silently de-duplicated, because which one a process used is a fact
about that process and not about the cache.

Nothing here writes to the cache or to the image it came from.
"""
import struct
import sys

PRINT_LIMIT = 400


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


def images(blob):
    img_off, img_cnt = u32(blob, 24), u32(blob, 28)
    out = []
    for i in range(img_cnt):
        o = img_off + i * 32
        addr, path_off = u64(blob, o), u32(blob, o + 24)
        end = blob.index(b"\0", path_off)
        out.append((addr, blob[path_off:end].decode("utf-8", "replace")))
    return out


def symbols(blob, image_va):
    """Every non-STAB, non-zero symbol in the image's own LC_SYMTAB.

    Cache-embedded dylibs keep symoff/stroff as offsets into the cache file, so
    the nlist table reads directly once the Mach-O header is located.
    """
    header = va_to_file_offset(blob, image_va)
    if header is None:
        return
    ncmds = u32(blob, header + 16)
    off = header + 28
    for _ in range(ncmds):
        cmd, size = u32(blob, off), u32(blob, off + 4)
        if cmd == 0x02:  # LC_SYMTAB
            symoff, nsyms = u32(blob, off + 8), u32(blob, off + 12)
            stroff = u32(blob, off + 16)
            for i in range(nsyms):
                e = symoff + i * 12
                strx, typ, value = u32(blob, e), blob[e + 4], u32(blob, e + 8)
                # N_STAB entries are debugger notes, not code addresses.
                if typ & 0xe0 or not value:
                    continue
                end = blob.index(b"\0", stroff + strx)
                yield value, blob[stroff + strx:end].decode("utf-8", "replace")
            return
        off += size


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cache, needle = sys.argv[1], sys.argv[2].lower()
    img_needle = sys.argv[3].lower() if len(sys.argv) > 3 else None
    blob = open(cache, "rb").read()

    hits = 0
    for addr, path in images(blob):
        if img_needle and img_needle not in path.lower():
            continue
        for value, name in symbols(blob, addr):
            if needle in name.lower():
                print("%08x  %-58s %s" % (value, name, path.split("/")[-1]))
                hits += 1
                if hits >= PRINT_LIMIT:
                    # Say so rather than stopping quietly: a truncated list that
                    # looks complete is how a "no such symbol" conclusion gets
                    # drawn from a search that simply stopped early.
                    print("... stopped at %d matches; narrow the needle or pass "
                          "an image filter" % PRINT_LIMIT)
                    return
    print("(%d match(es))" % hits)


main()
