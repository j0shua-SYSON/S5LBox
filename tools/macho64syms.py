"""Resolve addresses in a thin 64-bit little-endian Mach-O.

The slide-reference mode derives the ASLR slide from one runtime symbol
address, then resolves runtime PCs against the exact executable:

  python macho64syms.py <macho> --sections
  python macho64syms.py <macho> --grep <substring>
  python macho64syms.py <macho> <file-address> [file-address ...]
  python macho64syms.py <macho> --slide-reference \
      <symbol> <runtime-address> [runtime-pc ...]

Addresses are hexadecimal, with or without a 0x prefix.
"""

import struct
import sys


MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
N_TYPE = 0x0E
N_SECT = 0x0E


def _hex(value):
    return "0x%016x" % value


def _address(value):
    try:
        return int(value, 16)
    except ValueError as error:
        raise SystemExit("invalid hexadecimal address: %s" % value) from error


class Macho64:
    def __init__(self, path):
        with open(path, "rb") as source:
            self.buf = source.read()
        self._need(0, 32, "Mach-O header")
        magic = struct.unpack_from("<I", self.buf, 0)[0]
        if magic != MH_MAGIC_64:
            raise SystemExit(
                "not a thin 64-bit little-endian Mach-O: %08x" % magic)

        self.sections = []
        self.symtab = None
        ncmds, sizeofcmds = struct.unpack_from("<II", self.buf, 16)
        self._need(32, sizeofcmds, "load commands")
        offset = 32
        command_end = 32 + sizeofcmds
        for _ in range(ncmds):
            self._need(offset, 8, "load command")
            command, size = struct.unpack_from("<II", self.buf, offset)
            if size < 8 or offset + size > command_end:
                raise SystemExit("invalid Mach-O load-command size")
            if command == LC_SEGMENT_64:
                self._parse_segment(offset, size)
            elif command == LC_SYMTAB:
                if size < 24:
                    raise SystemExit("truncated LC_SYMTAB command")
                self.symtab = struct.unpack_from("<IIII", self.buf, offset + 8)
            offset += size
        if offset != command_end:
            raise SystemExit("Mach-O load commands do not match sizeofcmds")
        if self.symtab is None:
            raise SystemExit("Mach-O has no symbol table")
        self._symbols = self._parse_symbols()

    def _need(self, offset, size, label):
        if offset < 0 or size < 0 or offset + size > len(self.buf):
            raise SystemExit("truncated %s" % label)

    def _parse_segment(self, offset, size):
        if size < 72:
            raise SystemExit("truncated LC_SEGMENT_64 command")
        nsects = struct.unpack_from("<I", self.buf, offset + 64)[0]
        if 72 + nsects * 80 > size:
            raise SystemExit("truncated section_64 array")
        section_offset = offset + 72
        for _ in range(nsects):
            name = self._fixed_name(section_offset)
            segment = self._fixed_name(section_offset + 16)
            address, section_size = struct.unpack_from(
                "<QQ", self.buf, section_offset + 32)
            flags = struct.unpack_from("<I", self.buf, section_offset + 64)[0]
            self.sections.append({
                "index": len(self.sections) + 1,
                "segment": segment,
                "name": name,
                "address": address,
                "size": section_size,
                "flags": flags,
            })
            section_offset += 80

    def _fixed_name(self, offset):
        self._need(offset, 16, "Mach-O name")
        return self.buf[offset:offset + 16].split(b"\0", 1)[0].decode(
            "utf-8", "replace")

    def _string(self, string_offset, string_size, index):
        if index >= string_size:
            return None
        start = string_offset + index
        end_limit = string_offset + string_size
        end = self.buf.find(b"\0", start, end_limit)
        if end < 0:
            return None
        return self.buf[start:end].decode("utf-8", "replace")

    def _parse_symbols(self):
        symbol_offset, count, string_offset, string_size = self.symtab
        self._need(symbol_offset, count * 16, "nlist_64 array")
        self._need(string_offset, string_size, "symbol string table")
        symbols = []
        for index in range(count):
            string_index, symbol_type, section, _description, value = \
                struct.unpack_from("<IBBHQ", self.buf,
                                   symbol_offset + index * 16)
            if not value:
                continue
            name = self._string(string_offset, string_size, string_index)
            if not name:
                continue
            symbols.append({
                "value": value,
                "name": name,
                "section": section,
                "is_section": (symbol_type & N_TYPE) == N_SECT,
            })
        symbols.sort(key=lambda symbol: (symbol["value"], symbol["name"]))
        return symbols

    def section_for(self, address):
        for section in self.sections:
            if (section["address"] <= address <
                    section["address"] + section["size"]):
                return section
        return None

    def symbols(self):
        return self._symbols

    def named_symbol(self, requested):
        normalized = requested.lstrip("_")
        exact = [
            symbol for symbol in self._symbols
            if symbol["name"].lstrip("_") == normalized
        ]
        if len(exact) == 1:
            return exact[0]
        if len(exact) > 1:
            values = {symbol["value"] for symbol in exact}
            if len(values) == 1:
                return exact[0]
            raise SystemExit("reference symbol is ambiguous: %s" % requested)
        raise SystemExit("reference symbol not found: %s" % requested)

    def resolve(self, address):
        section = self.section_for(address)
        if section is None:
            return "%s  <outside every section>" % _hex(address)
        candidates = [
            symbol for symbol in self._symbols
            if symbol["is_section"] and
            symbol["section"] == section["index"] and
            symbol["value"] <= address
        ]
        location = "<no symbol>"
        if candidates:
            symbol = max(candidates, key=lambda candidate: candidate["value"])
            location = "%s+0x%x" % (
                symbol["name"], address - symbol["value"])
        return "%s  %s,%s  %s" % (
            _hex(address), section["segment"], section["name"], location)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    macho = Macho64(sys.argv[1])
    operation = sys.argv[2]
    if operation == "--sections":
        for section in macho.sections:
            print("%-16s %-16s %s..%s" % (
                section["segment"], section["name"],
                _hex(section["address"]),
                _hex(section["address"] + section["size"])))
        return
    if operation == "--grep":
        if len(sys.argv) != 4:
            raise SystemExit("--grep requires one substring")
        needle = sys.argv[3].lower()
        for symbol in macho.symbols():
            if needle in symbol["name"].lower():
                print("%s  %s" % (_hex(symbol["value"]), symbol["name"]))
        return
    if operation == "--slide-reference":
        if len(sys.argv) < 5:
            raise SystemExit(
                "--slide-reference requires a symbol and runtime address")
        reference = macho.named_symbol(sys.argv[3])
        runtime_reference = _address(sys.argv[4])
        slide = runtime_reference - reference["value"]
        print("reference %s file=%s runtime=%s slide=%s" % (
            reference["name"], _hex(reference["value"]),
            _hex(runtime_reference), _hex(slide)))
        for raw_address in sys.argv[5:]:
            runtime_address = _address(raw_address)
            file_address = runtime_address - slide
            print("runtime %s -> %s" % (
                _hex(runtime_address), macho.resolve(file_address)))
        return
    for raw_address in sys.argv[2:]:
        print(macho.resolve(_address(raw_address)))


if __name__ == "__main__":
    main()
