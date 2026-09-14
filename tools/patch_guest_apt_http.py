"""Close the pinned Darwin APT HTTP output before setting its modification time.

The publisher's CFReadStream success path leaves File open across utime and
URIDone. Closing that writable vnode later can change its HFS modification
time again. The decompression method then copies that changed time, defeating
conditional requests even when the repository has not changed.

This offline transformation changes only that success-path ordering. It uses
the existing virtual deleting destructor, clears File, and rejoins the original
timestamp/hash/publication path. Downloading, verification and failures remain
original code. It adds no imports, segments, runtime options or dependencies.
"""
import argparse
import hashlib
import pathlib
import struct

from build_guest_apt_index import branch, commands, unpack

PINNED_SHA = "742c6bf2d39dc69b35cc4737bd403d717b545d7af9f5196e240112a12d9c2c9f"
SITE, CAVE, TEXT_BIAS = 0xb25c, 0x1f00, 0x1000


def patch(original):
    if hashlib.sha256(original).hexdigest() != PINNED_SHA:
        raise ValueError("not the pinned apt7-lib 0.7.20.2-1 HTTP method")
    cmds, _, _ = commands(original, 2)
    text = next(raw for cmd, raw in cmds
                if cmd == 1 and raw[8:24].rstrip(b"\0") == b"__TEXT")
    if unpack("<4I", text, 24) != (0x1000, 0x10000, 0, 0x10000):
        raise ValueError("unexpected HTTP text mapping")
    # r7 is Loop's frame pointer; [r7-0x2d70] is this and this->File is +0x1c.
    # The original code already invokes this same deleting destructor on the
    # failure path at 0xb314. No stack frame or imported address is invented.
    words = [0xe247ca02, 0xe51c3d70, 0xe593001c, 0xe3500000,
             branch(CAVE + 16, CAVE + 32, 0x0a000000),
             0xe5902000, 0xe1a0e00f, 0xe592f004,
             0xe247ca02, 0xe51c3d70, 0xe3a02000, 0xe583201c,
             0xe24710e8, branch(CAVE + 52, SITE + 4)]
    start = CAVE - TEXT_BIAS
    if any(original[start:start + 4 * len(words)]):
        raise ValueError("HTTP code placement is not unused zero padding")
    if unpack("<I", original, SITE - TEXT_BIAS)[0] != 0xe24710e8:
        raise ValueError("unexpected HTTP success-path instruction")
    output = bytearray(original)
    struct.pack_into("<%dI" % len(words), output, start, *words)
    struct.pack_into("<I", output, SITE - TEXT_BIAS, branch(SITE, CAVE))

    # Keep the existing identifier, requirements and special-slot hashes.
    # Refresh only its page hashes; preserve the publisher's flags as well.
    sig = next(raw for cmd, raw in cmds if cmd == 0x1d)
    sigoff, sigsize = unpack("<2I", sig, 8)
    magic, total, count = unpack(">3I", original, sigoff)
    if magic != 0xfade0cc0 or total > sigsize or sigoff + sigsize != len(original):
        raise ValueError("unexpected HTTP signature extent")
    directories = [sigoff + rel for slot, rel in
                   (unpack(">2I", original, sigoff + 12 + i * 8)
                    for i in range(count)) if slot == 0]
    if len(directories) != 1:
        raise ValueError("expected one HTTP CodeDirectory")
    cd = directories[0]
    magic, length, version, flags, hashes, ident, special, slots, limit = unpack(
        ">9I", original, cd)
    hashsize, hashtype, platform, page = unpack("4B", original, cd + 36)
    if (magic != 0xfade0c02 or version != 0x20001 or flags != 0 or
            hashsize != 20 or hashtype != 1 or platform != 0 or page != 12 or
            limit != sigoff or slots != (limit + 4095) // 4096 or
            hashes + slots * 20 != length or cd + length > sigoff + total or
            hashes < special * 20 + 44 or not 44 <= ident < hashes):
        raise ValueError("unsupported HTTP CodeDirectory layout")
    for i in range(slots):
        digest = hashlib.sha1(original[i * 4096:min((i + 1) * 4096, limit)]).digest()
        at = cd + hashes + i * 20
        if original[at:at + 20] != digest:
            raise ValueError("invalid original HTTP code hash")
        output[at:at + 20] = hashlib.sha1(
            output[i * 4096:min((i + 1) * 4096, limit)]).digest()
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    result = patch(args.input.read_bytes())
    with args.output.open("xb") as output:
        output.write(result)
    print("%s  %s" % (hashlib.sha256(result).hexdigest(), args.output))


if __name__ == "__main__":
    main()
