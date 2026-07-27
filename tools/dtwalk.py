"""Read-only Apple device-tree walker for the supplied 7E18 devicetree.bin.

Prints the path, and for matching nodes the raw properties, so a node's
`reg`, `interrupts`, `interrupt-parent`, and `compatible` can be read exactly
instead of guessed. It never writes the file.
"""
import struct
import sys
import os

# Derived from this file's own location rather than hardcoded: these
# scripts live in <repo>/tools/, and an absolute path here is a path a
# project rename silently rewrites. That is exactly what happened -- the
# S5LBox rename replaced the folder name inside the string and left six
# tools pointing at a directory that has never existed.
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DT = os.path.join(REPO, "firmware", "devicetree.bin")

WANT = [w.lower() for w in sys.argv[1:]] or ["baseband"]


def fmt(name, data):
    if all(32 <= b < 127 or b == 0 for b in data) and len(data) and data[0] != 0:
        s = data.split(b"\x00")[0].decode("ascii", "replace")
        if len(s) >= 2:
            return "'%s'" % s
    words = [struct.unpack_from("<I", data, i)[0]
             for i in range(0, len(data) - 3, 4)]
    if words:
        return "{" + ", ".join("0x%08x" % w for w in words) + "}"
    return data.hex()


def walk(buf, off, path, out):
    nprops, nchildren = struct.unpack_from("<II", buf, off)
    off += 8
    props = []
    name = None
    for _ in range(nprops):
        pname = buf[off:off + 32].split(b"\x00")[0].decode("ascii", "replace")
        off += 32
        (length,) = struct.unpack_from("<I", buf, off)
        off += 4
        length &= 0x7FFFFFFF
        data = buf[off:off + length]
        off += (length + 3) & ~3
        props.append((pname, data))
        if pname == "name":
            name = data.split(b"\x00")[0].decode("ascii", "replace")
    here = path + "/" + (name or "?")
    out.append((here, props))
    for _ in range(nchildren):
        off = walk(buf, off, here, out)
    return off


with open(DT, "rb") as f:
    buf = f.read()

nodes = []
walk(buf, 0, "", nodes)
print("device tree: %d bytes, %d nodes" % (len(buf), len(nodes)))
for path, props in nodes:
    if any(w in path.lower() for w in WANT):
        print("\n%s" % path)
        for pname, data in props:
            print("    %-24s %s" % (pname, fmt(pname, data)))
