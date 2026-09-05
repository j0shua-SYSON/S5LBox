"""Offline, exact-identity ARMv6 APT experiment builder; never edits its input.

Accepts two armv6-apple-ios3.0 Mach-O objects containing only __TEXT,__text and
ARM_RELOC_BR24 relocations. Unsupported object layouts fail closed. The result
is a private test artifact, not a default package or an installer action.
"""
import argparse
import hashlib
import json
import pathlib
import struct

PINNED_SHA = "239202453005e090d9176e7d017203adeaa54cd82e0a7a17edd39ef6df9d6075"
WRITE, D1, D2 = 0x65f50, 0x696d4, 0x68994
PROLOGUE = 0xe92d40f0
PAGE = 4096


def align(value, alignment=PAGE):
    return (value + alignment - 1) & -alignment


def unpack(fmt, data, offset):
    if offset < 0 or offset + struct.calcsize(fmt) > len(data):
        raise ValueError("truncated structure")
    return struct.unpack_from(fmt, data, offset)


def commands(data, kind):
    magic, cpu, subtype, actual_kind, count, size, flags = unpack("<7I", data, 0)
    if magic != 0xfeedface or cpu != 12 or actual_kind != kind:
        raise ValueError("wrong Mach-O format, CPU, or file type")
    if 28 + size > len(data) or count > size // 8:
        raise ValueError("invalid load command count/size")
    result, at = [], 28
    for _ in range(count):
        cmd, n = unpack("<II", data, at)
        if n < 8 or at + n > 28 + size:
            raise ValueError("invalid load command")
        result.append((cmd, bytearray(data[at:at + n])))
        at += n
    if at != 28 + size:
        raise ValueError("load command byte count mismatch")
    return result, subtype, flags


def section(raw, at):
    name, segment, va, size, off, alignment, reloc, count, flags, r1, r2 = unpack(
        "<16s16s9I", raw, at)
    return dict(name=name.rstrip(b"\0"), segment=segment.rstrip(b"\0"),
                va=va, size=size, off=off, alignment=alignment,
                reloc=reloc, reloc_count=count, flags=flags)


def object_file(path):
    data = path.read_bytes()
    cmds, subtype, _ = commands(data, 1)
    if subtype != 6:
        raise ValueError("object must be specifically ARMv6")
    sections, symtab = [], None
    for cmd, raw in cmds:
        if cmd == 1:
            count = unpack("<I", raw, 48)[0]
            if len(raw) != 56 + count * 68:
                raise ValueError("invalid object segment")
            sections += [section(raw, 56 + i * 68) for i in range(count)]
        elif cmd == 2:
            symtab = unpack("<4I", raw, 8)
    if len(sections) != 1 or not symtab:
        raise ValueError("object needs exactly one text section and symbols")
    sec = sections[0]
    if (sec["name"], sec["segment"]) != (b"__text", b"__TEXT") or sec["va"] != 0:
        raise ValueError("unsupported object section")
    if sec["alignment"] > 4 or sec["size"] % 4 or sec["off"] + sec["size"] > len(data):
        raise ValueError("invalid A32 text alignment or size")
    symoff, nsyms, stroff, strsize = symtab
    strings = data[stroff:stroff + strsize]
    if len(strings) != strsize:
        raise ValueError("truncated symbol strings")
    symbols = []
    for i in range(nsyms):
        string, typ, sect, desc, value = unpack("<IBBHI", data, symoff + i * 12)
        if string >= len(strings):
            raise ValueError("symbol string outside table")
        name = strings[string:strings.index(b"\0", string)].decode("ascii")
        if desc & 8:
            raise ValueError("Thumb symbols are not supported")
        symbols.append(dict(name=name, type=typ, section=sect, value=value))
    relocations = []
    for i in range(sec["reloc_count"]):
        at, bits = unpack("<II", data, sec["reloc"] + i * 8)
        symbol = bits & 0xffffff
        if at & 0x80000000 or bits >> 24 != 0x5d or symbol >= len(symbols):
            raise ValueError("only external pcrel ARM_RELOC_BR24 is supported")
        if at % 4 or at + 4 > sec["size"]:
            raise ValueError("branch relocation outside aligned text")
        relocations.append((at, symbol))
    return dict(path=str(path), sha256=hashlib.sha256(data).hexdigest(),
                text=bytearray(data[sec["off"]:sec["off"] + sec["size"]]),
                symbols=symbols, relocations=relocations)


def branch(at, target, original=0xea000000):
    delta = target - at - 8
    if delta % 4 or not -(1 << 25) <= delta < (1 << 25):
        raise ValueError("unaligned or out-of-range ARM branch")
    if original & 0x0e000000 != 0x0a000000 or original >> 28 == 15:
        raise ValueError("not an ordinary ARM B/BL instruction")
    return (original & 0xff000000) | ((delta >> 2) & 0xffffff)


def link(objects, base):
    at, defined = base, {}
    for obj in objects:
        obj["base"] = at
        at = align(at + len(obj["text"]), 16)
        for sym in obj["symbols"]:
            if sym["section"] == 1 and sym["type"] & 1:
                if sym["name"] in defined:
                    raise ValueError("duplicate exported symbol")
                defined[sym["name"]] = obj["base"] + sym["value"]
    original = at
    d1, d2 = at + 8, at + 28
    imports = {"_guest_apt_original_write": original,
               "_guest_apt_pool_allocate": 0x1750,
               "_guest_apt_pool_write_string": 0x1a00,
               "_guest_apt_mmap": 0xd1764, "_guest_apt_munmap": 0xd1784}
    payload = bytearray(at + 48 - base)
    for obj in objects:
        code = bytearray(obj["text"])
        for offset, symbol in obj["relocations"]:
            sym = obj["symbols"][symbol]
            if sym["section"] == 1:
                target = obj["base"] + sym["value"]
            elif sym["section"] == 0 and (sym["type"] & 0xe) == 0:
                target = defined.get(sym["name"], imports.get(sym["name"]))
                if target is None:
                    raise ValueError("unresolved symbol: " + sym["name"])
            else:
                raise ValueError("unsupported symbol kind")
            word = unpack("<I", code, offset)[0]
            imm = word & 0xffffff
            if imm & 0x800000:
                imm -= 0x1000000
            # r_extern branches encode their addend against address zero,
            # including references to defined local symbols in this object.
            addend = offset + 8 + (imm << 2)
            if addend:
                raise ValueError("nonzero branch addend: %s offset=%#x word=%#x symbol=%#x addend=%#x" %
                                 (sym["name"], offset, word, sym["value"], addend))
            struct.pack_into("<I", code, offset, branch(obj["base"] + offset, target, word))
        start = obj["base"] - base
        payload[start:start + len(code)] = code
    struct.pack_into("<2I", payload, original - base, PROLOGUE, branch(original + 4, WRITE + 4))
    release = defined["_guest_apt_pinned_release"]
    for start, destination in ((d1, D1), (d2, D2)):
        # Save all incoming argument/scratch registers and LR on a 24-byte
        # frame, call cleanup, restore, then execute the displaced prologue.
        words = [0xe92d500f, branch(start + 4, release, 0xeb000000),
                 0xe8bd500f, PROLOGUE, branch(start + 16, destination + 4)]
        struct.pack_into("<5I", payload, start - base, *words)
    return payload, {WRITE: defined["_guest_apt_pinned_write"], D1: d1, D2: d2}, defined


def signature_size(limit, identifier):
    return 20 + 44 + len(identifier) + 20 * ((limit + PAGE - 1) // PAGE)


def sign(data, identifier):
    slots = (len(data) + PAGE - 1) // PAGE
    hash_offset = 44 + len(identifier)
    cd_size = hash_offset + 20 * slots
    cd = struct.pack(">9I4BI", 0xfade0c02, cd_size, 0x20001, 2,
                     hash_offset, 44, 0, slots, len(data), 20, 1, 0, 12, 0) + identifier
    cd += b"".join(hashlib.sha1(data[i:i + PAGE]).digest() for i in range(0, len(data), PAGE))
    return struct.pack(">5I", 0xfade0cc0, 20 + cd_size, 1, 0, 20) + cd


def build(original, objects):
    if hashlib.sha256(original).hexdigest() != PINNED_SHA:
        raise ValueError("input is not the exact pinned publisher library")
    cmds, subtype, flags = commands(original, 6)
    linkedit = next(raw for cmd, raw in cmds if cmd == 1 and raw[8:24].rstrip(b"\0") == b"__LINKEDIT")
    old_vm, old_vs, old_off, old_fs = unpack("<4I", linkedit, 24)
    sigcmd = next(raw for cmd, raw in cmds if cmd == 0x1d)
    old_sig, old_sigsize = unpack("<2I", sigcmd, 8)
    if old_sig + old_sigsize != len(original) or old_off + old_fs != len(original):
        raise ValueError("unexpected signature or LINKEDIT extent")
    # Insert executable pages before LINKEDIT in BOTH file and virtual order.
    # Keep TEXT/DATA addresses fixed and leave LINKEDIT as the final segment;
    # loaders may derive the reserved image extent from that final segment.
    # LINKEDIT has no sections or original code/data symbol addresses.
    if unpack("<I", linkedit, 48)[0] != 0 or old_vm != old_off:
        raise ValueError("unexpected pinned LINKEDIT mapping")
    base = old_vm
    payload, detours, symbols = link(objects, base)
    payload_size = align(len(payload))
    identifier = b"libapt-pkg.s5lbox.ordered-index-experiment\0"
    signature_at = align(old_sig + payload_size, 16)
    sigsize = signature_size(signature_at, identifier)
    final_size = signature_at + sigsize
    linkedit_size = final_size - (old_off + payload_size)
    new_segment = bytearray(struct.pack("<II16s8I", 1, 124, b"__S5L_APT",
                                       base, payload_size, old_off, payload_size,
                                       5, 5, 1, 0))
    new_segment += struct.pack("<16s16s9I", b"__text", b"__S5L_APT", base,
                               len(payload), old_off, 2, 0, 0, 0x80000400, 0, 0)
    output_commands = []
    for cmd, raw in cmds:
        if raw is linkedit:
            output_commands.append(new_segment)
            struct.pack_into("<4I", raw, 24, old_vm + payload_size, align(linkedit_size),
                             old_off + payload_size, linkedit_size)
        elif cmd == 2:
            for field in (8, 16):
                value = unpack("<I", raw, field)[0]
                if value < old_off:
                    raise ValueError("symbol table unexpectedly outside LINKEDIT")
                struct.pack_into("<I", raw, field, value + payload_size)
        elif cmd == 0xb:
            for field in (32, 40, 48, 56, 64, 72):
                value = unpack("<I", raw, field)[0]
                if value:
                    if value < old_off:
                        raise ValueError("dynamic table unexpectedly outside LINKEDIT")
                    struct.pack_into("<I", raw, field, value + payload_size)
        elif cmd == 0x1d:
            struct.pack_into("<2I", raw, 8, signature_at, sigsize)
        output_commands.append(raw)
    new_commands = b"".join(output_commands)
    header = struct.pack("<7I", 0xfeedface, 12, subtype, 6,
                         len(output_commands), len(new_commands), flags)
    if 28 + len(new_commands) > 0x1680 or any(original[28 + sum(len(r) for _, r in cmds):0x1680]):
        raise ValueError("insufficient zero header padding")
    output = bytearray(original[:old_off])
    output += payload + bytes(payload_size - len(payload))
    output += original[old_off:old_sig]
    output += bytes(signature_at - len(output))
    output[:len(header + new_commands)] = header + new_commands
    for address, target in detours.items():
        if unpack("<I", original, address)[0] != PROLOGUE:
            raise ValueError("unexpected original entry instruction")
        struct.pack_into("<I", output, address, branch(address, target))
    output += sign(output, identifier)
    if len(output) != final_size:
        raise ValueError("signature extent mismatch")
    return bytes(output), dict(input_sha256=PINNED_SHA,
                               output_sha256=hashlib.sha256(output).hexdigest(),
                               output_bytes=len(output), payload_vm=base,
                               payload_bytes=len(payload), payload_fileoff=old_off,
                               linkedit_vm=old_vm + payload_size,
                               signature_offset=signature_at, signature_bytes=sigsize,
                               detours={hex(k): hex(v) for k, v in detours.items()},
                               symbols={k: hex(v) for k, v in symbols.items()},
                               objects=[dict(path=o["path"], sha256=o["sha256"]) for o in objects],
                               status="offline artifact; not a runtime validation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--index-object", type=pathlib.Path, required=True)
    parser.add_argument("--adapter-object", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.manifest.exists() or args.output.resolve() == args.manifest.resolve():
        raise SystemExit("outputs must be distinct new files")
    output, manifest = build(args.input.read_bytes(),
                             [object_file(args.index_object), object_file(args.adapter_object)])
    # Generated binary and provenance are build outputs; all source edits are
    # separate. Exclusive creation protects the input and earlier experiments.
    with args.output.open("xb") as file:
        file.write(output)
    with args.manifest.open("x", encoding="utf-8") as file:
        json.dump(manifest, file, indent=2)
        file.write("\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
