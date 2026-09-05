"""Read-only layout, unchanged-region, symbol-table, and page-signature checks."""
import hashlib
import json
import pathlib
import struct
import sys
from build_guest_apt_index import PINNED_SHA, WRITE, D1, D2, commands, unpack


def verify(original, candidate):
    if hashlib.sha256(original).hexdigest() != PINNED_SHA:
        raise ValueError("wrong original identity")
    old, old_subtype, old_flags = commands(original, 6)
    new, new_subtype, new_flags = commands(candidate, 6)
    if old_subtype != new_subtype or old_flags != new_flags or len(new) != len(old) + 1:
        raise ValueError("unexpected header change")
    def segments(cmds):
        result = []
        for cmd, raw in cmds:
            if cmd == 1:
                result.append((raw[8:24].rstrip(b"\0"), *unpack("<6I", raw, 24)))
        return result
    before, after = segments(old), segments(new)
    if [x[0] for x in after] != [b"__TEXT", b"__DATA", b"__S5L_APT", b"__LINKEDIT"]:
        raise ValueError("unexpected segment order")
    if before[:2] != after[:2]:
        raise ValueError("existing TEXT/DATA mapping or protections changed")
    for previous, current in zip(after, after[1:]):
        if previous[1] + previous[2] > current[1] or previous[3] + previous[4] > current[3]:
            raise ValueError("overlapping or out-of-order virtual/file segments")
    for name, vm, vs, off, fs, maximum, initial in after:
        if (vm | vs | off) & 4095 or fs > vs or off + fs > len(candidate):
            raise ValueError("invalid segment alignment or extent")
        if maximum & 6 == 6 or initial & 6 == 6:
            raise ValueError("writable executable segment")
    payload = after[2]
    if payload[5:] != (5, 5):
        raise ValueError("payload is not read/execute only")
    for pc in (WRITE, D1, D2):
        word = unpack("<I", candidate, pc)[0]
        if word >> 24 != 0xea:
            raise ValueError("entry is not an unconditional ARM branch")
        displacement = word & 0xffffff
        if displacement & 0x800000:
            displacement -= 0x1000000
        target = pc + 8 + displacement * 4
        if not payload[1] <= target < payload[1] + payload[4]:
            raise ValueError("entry branch misses payload")
    restored = bytearray(candidate[0x1680:before[-1][3]])
    for pc in (WRITE, D1, D2):
        restored[pc - 0x1680:pc - 0x1680 + 4] = original[pc:pc + 4]
    if restored != original[0x1680:before[-1][3]]:
        raise ValueError("original TEXT/DATA changed outside three entries")
    def one(cmds, kind):
        matches = [raw for cmd, raw in cmds if cmd == kind]
        if len(matches) != 1:
            raise ValueError("missing or duplicate required command")
        return matches[0]
    old_sig, _ = unpack("<2I", one(old, 0x1d), 8)
    new_sig, new_sig_size = unpack("<2I", one(new, 0x1d), 8)
    shift = after[-1][3] - before[-1][3]
    if new_sig != old_sig + shift or new_sig + new_sig_size != len(candidate):
        raise ValueError("signature or LINKEDIT shift mismatch")
    if original[before[-1][3]:old_sig] != candidate[after[-1][3]:new_sig]:
        raise ValueError("existing LINKEDIT contents changed")
    for kind, fields in ((2, (8, 16)), (0xb, (32, 40, 48, 56, 64, 72))):
        was, now = one(old, kind), one(new, kind)
        normalized = bytearray(now)
        for field in fields:
            a = unpack("<I", was, field)[0]
            b = unpack("<I", now, field)[0]
            if b != (a + shift if a else 0):
                raise ValueError("linker table offset did not move consistently")
            struct.pack_into("<I", normalized, field, a)
        if normalized != was:
            raise ValueError("unexpected linker table metadata change")
    # Independent CodeDirectory parsing: check every on-disk SHA-1 code slot.
    sig = candidate[new_sig:]
    magic, length, count, slot, cd_at = unpack(">5I", sig, 0)
    if (magic, length, count, slot, cd_at) != (0xfade0cc0, len(sig), 1, 0, 20):
        raise ValueError("invalid signature SuperBlob")
    cd_magic, cd_size, version, flags, hashes, ident, special, slots, limit = unpack(">9I", sig, cd_at)
    hs, ht, platform, log_page = unpack(">4B", sig, cd_at + 36)
    if (cd_magic, version, flags, special, limit, hs, ht, platform, log_page) != (
            0xfade0c02, 0x20001, 2, 0, new_sig, 20, 1, 0, 12):
        raise ValueError("unexpected CodeDirectory contract")
    if cd_size + cd_at != len(sig) or slots != (limit + 4095) // 4096:
        raise ValueError("CodeDirectory length/slot count mismatch")
    if ident != 44 or hashes <= ident or hashes + slots * hs != cd_size:
        raise ValueError("invalid CodeDirectory tables")
    if sig[cd_at + hashes - 1] != 0:
        raise ValueError("unterminated signing identifier")
    for i in range(slots):
        digest = hashlib.sha1(candidate[i * 4096:min((i + 1) * 4096, limit)]).digest()
        at = cd_at + hashes + i * hs
        if sig[at:at + hs] != digest:
            raise ValueError("signature hash mismatch at page %d" % i)
    return dict(status="offline structure and signature PASS", code_pages=slots,
                output_sha256=hashlib.sha256(candidate).hexdigest(),
                original_code_data_unchanged_except_entries=True,
                original_linkedit_contents_unchanged=True,
                loader_and_physical_execution="not verified by this tool")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_guest_apt_index.py ORIGINAL CANDIDATE")
    print(json.dumps(verify(pathlib.Path(sys.argv[1]).read_bytes(),
                            pathlib.Path(sys.argv[2]).read_bytes()), indent=2))
