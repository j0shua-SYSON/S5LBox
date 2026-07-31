"""Read-only static caller search inside an iPhone OS dyld shared cache.

dscmap.py answers "what is at this address". This answers the other half:
"who calls it". Walking a call chain upward with --call-probe costs one
~35-minute emulator run per level, and a run only sees the callers that a
particular boot happened to exercise. A BL is in the image whether or not it
executed, so the static answer is both cheaper and more complete -- at the
cost of not knowing which of the sites it finds actually ran.

Use both: this narrows the candidates, a probe confirms which one is live.

The cache maps linearly: file_offset == va - 0x30000000, verified against two
images at different addresses before this was relied upon. A caller in a
DIFFERENT image than the callee is possible in principle but not through a BL,
which is why the default scan window is the callee's own neighbourhood -- a
cross-image call goes through a stub, and the stub is what this will find.

Nothing here writes to the cache or to the image it came from.

Usage:
  python dscxref.py <cache> <hex-target-va> [--from VA] [--to VA] [--thumb]
"""

import sys

CACHE_VA_BASE = 0x30000000


def sign_extend(value, bits):
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def arm_bl_target(word, va):
    """Return the target of an ARM BL/BLX(imm) at `va`, or None.

    BL  is cond != 1111 with bits[27:24] == 1011.
    BLX(imm) is cond == 1111 with bits[27:25] == 101; bit24 is the H bit,
    which adds a halfword so it can reach a Thumb destination.
    """
    cond = word >> 28
    if cond == 0xF:
        if (word >> 25) & 0x7 != 0x5:
            return None
        h = (word >> 24) & 1
        imm = sign_extend(word & 0xFFFFFF, 24)
        return (va + 8 + (imm << 2) + (h << 1)) & 0xFFFFFFFF
    if (word >> 24) & 0xF != 0xB:
        return None
    imm = sign_extend(word & 0xFFFFFF, 24)
    return (va + 8 + (imm << 2)) & 0xFFFFFFFF


def thumb_bl_target(half1, half2, va):
    """Return the target of a Thumb-2 BL/BLX pair at `va`, or None.

    The first halfword carries the high 11 bits, the second the low 11.
    0xF800 is BL (stays Thumb); 0xE800 is BLX (switches to ARM and forces
    the target even).
    """
    if half1 & 0xF800 != 0xF000:
        return None
    kind = half2 & 0xF800
    if kind not in (0xF800, 0xE800):
        return None
    imm = sign_extend(((half1 & 0x7FF) << 11) | (half2 & 0x7FF), 22) << 1
    target = (va + 4 + imm) & 0xFFFFFFFF
    if kind == 0xE800:
        target &= ~0x3
    return target


def scan(blob, target, lo, hi, thumb):
    hits = []
    start = lo - CACHE_VA_BASE
    end = min(hi - CACHE_VA_BASE, len(blob))
    if start < 0 or start >= end:
        return hits

    for off in range(start, end - 3, 4):
        word = int.from_bytes(blob[off:off + 4], "little")
        if arm_bl_target(word, off + CACHE_VA_BASE) == target:
            hits.append((off + CACHE_VA_BASE, "arm"))

    if thumb:
        for off in range(start, end - 3, 2):
            half1 = int.from_bytes(blob[off:off + 2], "little")
            half2 = int.from_bytes(blob[off + 2:off + 4], "little")
            if thumb_bl_target(half1, half2, off + CACHE_VA_BASE) == target:
                hits.append((off + CACHE_VA_BASE, "thumb"))

    hits.sort()
    return hits


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    cache_path = argv[1]
    target = int(argv[2], 16) & ~1

    lo, hi, thumb = target - 0x80000, target + 0x80000, False
    i = 3
    while i < len(argv):
        if argv[i] == "--from" and i + 1 < len(argv):
            lo = int(argv[i + 1], 16)
            i += 2
        elif argv[i] == "--to" and i + 1 < len(argv):
            hi = int(argv[i + 1], 16)
            i += 2
        elif argv[i] == "--thumb":
            thumb = True
            i += 1
        else:
            print("unrecognised argument: %s" % argv[i])
            return 2

    with open(cache_path, "rb") as handle:
        blob = handle.read()

    hits = scan(blob, target, lo, hi, thumb)
    print("target : 0x%08x" % target)
    print("window : 0x%08x..0x%08x  (%s)"
          % (lo, hi, "arm+thumb" if thumb else "arm only"))
    print("callers: %u" % len(hits))
    for va, kind in hits:
        print("    0x%08x  %s" % (va, kind))
    if not hits:
        print("    (none -- widen with --from/--to, or the call is indirect)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
