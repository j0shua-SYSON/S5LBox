"""Independent byte-region and code-signature checks for the pinned HTTP trial."""
import hashlib
import pathlib
import struct
import sys
from patch_guest_apt_http import PINNED_SHA, patch


def verify(original, candidate):
    if hashlib.sha256(original).hexdigest() != PINNED_SHA:
        raise ValueError("wrong original identity")
    if len(candidate) != len(original):
        raise ValueError("HTTP file size changed")
    # The pinned CodeDirectory starts at 0x13b6c, with code hashes at +89.
    # Only text pages 0 and 10 change. No header, signature metadata,
    # requirement, symbol, import, or other code/data byte may change.
    hashes = 0x13b6c + 89
    ranges = ((0xf00, 0xf38), (0xa25c, 0xa260),
              (hashes, hashes + 20), (hashes + 200, hashes + 220))
    restored = bytearray(candidate)
    for start, end in ranges:
        restored[start:end] = original[start:end]
    if restored != original:
        raise ValueError("HTTP changed outside the detour and its two page hashes")
    for i in range(20):
        digest = hashlib.sha1(candidate[i * 4096:min((i + 1) * 4096, 0x13b50)]).digest()
        if candidate[hashes + i * 20:hashes + (i + 1) * 20] != digest:
            raise ValueError("invalid HTTP page signature")
    for offset, pc, target in ((0xa25c, 0xb25c, 0x1f00), (0xf34, 0x1f34, 0xb260)):
        word, = struct.unpack_from('<I', candidate, offset)
        delta = word & 0xffffff
        if delta & 0x800000:
            delta -= 0x1000000
        if word >> 24 != 0xea or pc + 8 + 4 * delta != target:
            raise ValueError("incorrect HTTP detour/rejoin")
    if patch(original) != candidate:
        raise ValueError("HTTP candidate is not reproducible")
    for at in (0, 0xf00, 0xa25c, 0x13b50, len(original) - 1):
        bad = bytearray(original)
        bad[at] ^= 1
        try:
            patch(bad)
        except ValueError:
            continue
        raise ValueError("accepted an altered HTTP input")
    print("HTTP byte preservation, 20 signature pages, branches, reproducibility, input refusal: PASS")


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_guest_apt_http.py ORIGINAL CANDIDATE")
    verify(pathlib.Path(sys.argv[1]).read_bytes(), pathlib.Path(sys.argv[2]).read_bytes())
