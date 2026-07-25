"""Resolve addresses in a 32-bit ARM Mach-O to symbols, including lazy stubs.

Guest PCs below 0x10000000 belong to the process's own image, and the tail of
__TEXT is normally symbol stubs, so a raw PC there is usually "it called an
external function" rather than "it was executing its own code". Resolving the
stub through the indirect symbol table turns that into the callee's name.

Usage:
  python machosyms.py <macho> <addr> [addr ...]
  python machosyms.py <macho> --sections
  python machosyms.py <macho> --grep <substring>
"""
import struct
import sys

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB

S_SYMBOL_STUBS = 0x8
S_LAZY_SYMBOL_POINTERS = 0x7
S_NON_LAZY_SYMBOL_POINTERS = 0x6
INDIRECT_ABS = 0x40000000
INDIRECT_LOCAL = 0x80000000


class Macho:
    def __init__(self, path):
        self.buf = open(path, "rb").read()
        magic = struct.unpack_from("<I", self.buf, 0)[0]
        if magic != 0xFEEDFACE:
            raise SystemExit("not a 32-bit little-endian Mach-O: %08x" % magic)
        self.ncmds = struct.unpack_from("<I", self.buf, 16)[0]
        self.sections = []
        self.symtab = None
        self.dysymtab = None
        off = 28
        for _ in range(self.ncmds):
            cmd, size = struct.unpack_from("<II", self.buf, off)
            if cmd == LC_SEGMENT:
                nsects = struct.unpack_from("<I", self.buf, off + 48)[0]
                so = off + 56
                for _s in range(nsects):
                    name = self.buf[so:so + 16].rstrip(b"\0").decode()
                    seg = self.buf[so + 16:so + 32].rstrip(b"\0").decode()
                    addr, ssize, foff = struct.unpack_from(
                        "<III", self.buf, so + 32)
                    flags, r1, r2 = struct.unpack_from(
                        "<III", self.buf, so + 56)
                    self.sections.append(
                        dict(seg=seg, name=name, addr=addr, size=ssize,
                             off=foff, flags=flags, reserved1=r1,
                             reserved2=r2))
                    so += 68
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack_from(
                    "<IIII", self.buf, off + 8)
                self.symtab = (symoff, nsyms, stroff, strsize)
            elif cmd == LC_DYSYMTAB:
                # dysymtab_command after cmd/cmdsize: ilocalsym, nlocalsym,
                # iextdefsym, nextdefsym, iundefsym, nundefsym, tocoff, ntoc,
                # modtaboff, nmodtab, extrefsymoff, nextrefsyms,
                # indirectsymoff, nindirectsyms, extreloff, nextrel,
                # locreloff, nlocrel  -> indirectsymoff is index 12.
                vals = struct.unpack_from("<18I", self.buf, off + 8)
                self.dysymtab = dict(indirectsymoff=vals[12],
                                     nindirectsyms=vals[13])
            off += size

    def sym_name(self, index):
        symoff, nsyms, stroff, _ = self.symtab
        if index >= nsyms:
            return None
        n_strx = struct.unpack_from("<I", self.buf, symoff + index * 12)[0]
        end = self.buf.index(b"\0", stroff + n_strx)
        return self.buf[stroff + n_strx:end].decode("utf-8", "replace")

    def symbols(self):
        symoff, nsyms, stroff, _ = self.symtab
        out = []
        for i in range(nsyms):
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from(
                "<IBBHI", self.buf, symoff + i * 12)
            if not n_value:
                continue
            end = self.buf.index(b"\0", stroff + n_strx)
            name = self.buf[stroff + n_strx:end].decode("utf-8", "replace")
            out.append((n_value & ~1, name, n_type))
        out.sort()
        return out

    def section_for(self, addr):
        for s in self.sections:
            if s["addr"] <= addr < s["addr"] + s["size"]:
                return s
        return None

    def resolve(self, addr):
        addr &= ~1
        sec = self.section_for(addr)
        if not sec:
            return "0x%08x  <outside every section>" % addr
        tag = "%s,%s" % (sec["seg"], sec["name"])
        stype = sec["flags"] & 0xFF
        if stype in (S_SYMBOL_STUBS, S_LAZY_SYMBOL_POINTERS,
                     S_NON_LAZY_SYMBOL_POINTERS) and self.dysymtab:
            width = sec["reserved2"] if stype == S_SYMBOL_STUBS else 4
            if width:
                slot = (addr - sec["addr"]) // width
                idx = sec["reserved1"] + slot
                iso = self.dysymtab["indirectsymoff"]
                if idx < self.dysymtab["nindirectsyms"]:
                    val = struct.unpack_from("<I", self.buf, iso + idx * 4)[0]
                    if val & (INDIRECT_ABS | INDIRECT_LOCAL):
                        return "0x%08x  %s  stub[%d] -> <local/abs>" % (
                            addr, tag, slot)
                    name = self.sym_name(val)
                    return "0x%08x  %s  stub[%d] -> %s" % (
                        addr, tag, slot, name)
        best = None
        for value, name, _t in self.symbols():
            if value <= addr:
                best = (value, name)
            else:
                break
        if best:
            return "0x%08x  %s  %s+0x%x" % (
                addr, tag, best[1], addr - best[0])
        return "0x%08x  %s  <no symbol>" % (addr, tag)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    m = Macho(sys.argv[1])
    if sys.argv[2] == "--sections":
        for s in m.sections:
            print("%-10s %-18s vm %08x..%08x  type=%02x r1=%d r2=%d"
                  % (s["seg"], s["name"], s["addr"], s["addr"] + s["size"],
                     s["flags"] & 0xFF, s["reserved1"], s["reserved2"]))
        return
    if sys.argv[2] == "--grep":
        needle = sys.argv[3].lower()
        for value, name, _t in m.symbols():
            if needle in name.lower():
                print("%08x  %s" % (value, name))
        return
    if sys.argv[2] == "--stubs":
        # Every imported function, in stub order. Undefined symbols have no
        # value, so they never appear in the defined-symbol listing above;
        # this is the process's actual external dependency surface.
        needle = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for s in m.sections:
            if (s["flags"] & 0xFF) != S_SYMBOL_STUBS or not s["reserved2"]:
                continue
            count = s["size"] // s["reserved2"]
            for slot in range(count):
                idx = s["reserved1"] + slot
                if idx >= m.dysymtab["nindirectsyms"]:
                    break
                val = struct.unpack_from(
                    "<I", m.buf,
                    m.dysymtab["indirectsymoff"] + idx * 4)[0]
                if val & (INDIRECT_ABS | INDIRECT_LOCAL):
                    continue
                name = m.sym_name(val) or "<none>"
                if needle and needle not in name.lower():
                    continue
                print("%08x  stub[%d]  %s"
                      % (s["addr"] + slot * s["reserved2"], slot, name))
        return
    for a in sys.argv[2:]:
        print(m.resolve(int(a, 16)))


main()
