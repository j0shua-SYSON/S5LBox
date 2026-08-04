#!/usr/bin/env python3
"""Generate the firmware-independent signed AArch64 handler table.

The output is ordinary assembly compiled and signed with the executable. The
generator enumerates ISA operand combinations only; it consumes no guest image,
profile, opcode stream, or other Apple-derived input.
"""

from __future__ import annotations

import argparse
from pathlib import Path


HOST = tuple(range(19, 27))  # guest r0-r7 stay pinned in x19-x26
PINNED = {reg: host for reg, host in enumerate(HOST)}
PINNED[13] = 27
CONDITIONS = (
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le",
)
EXPECTED_HANDLERS = 24646

READ_KINDS = (
    ("word", "ldr", 4),
    ("byte", "ldrb", 1),
    ("half", "ldrh", 2),
    ("signed_byte", "ldrsb", 1),
    ("signed_half", "ldrsh", 2),
)


def next_dispatch() -> list[str]:
    return [
        # Decode has already resolved the portable handler id through the
        # signed table below. The hot record carries that exact relative
        # offset, removing one dependent table load from every uop while the
        # target remains confined to ordinary build-time-signed text.
        "    ldrsw x16, [x13], #16",
        "    add x16, x8, x16",
        "    br x16",
    ]


def terminal_branch_body(condition: str, link: bool) -> list[str]:
    body: list[str] = []
    if condition:
        body.extend([
            f"    b.{condition} 1f",
            # A failed condition exits at the instruction after B/BL and must
            # leave LR untouched even when the link bit is set.
            "    ldur w12, [x13, #-8]",
            "    b .La64s_terminal_exit",
            "1:",
        ])
    if link:
        body.extend([
            "    ldur w9, [x13, #-8]",
            "    str w9, [x0, #56]",
        ])
    body.extend([
        "    ldur w12, [x13, #-12]",
        "    b .La64s_terminal_exit",
    ])
    return body


def read_guest_register(reg: int, scratch: int) -> tuple[list[str], str]:
    if reg in PINNED:
        return [], f"w{PINNED[reg]}"
    if reg == 15:
        return [f"    ldur w{scratch}, [x13, #-8]"], f"w{scratch}"
    return [f"    ldr w{scratch}, [x0, #{reg * 4}]"], f"w{scratch}"


def result_register(rd: int) -> tuple[str, list[str]]:
    if rd in PINNED:
        return f"w{PINNED[rd]}", []
    return "w12", [f"    str w12, [x0, #{rd * 4}]"]


def write_guest_register(reg: int, value: str) -> list[str]:
    if reg in PINNED:
        return [f"    mov w{PINNED[reg]}, {value}"]
    return [f"    str {value}, [x0, #{reg * 4}]"]


def logic_flags(result: str) -> list[str]:
    return [
        # metadata: 0 preserves C, 1 clears C, 2 sets C. V is preserved.
        "    ldur w16, [x13, #-4]",
        "    mrs x10, nzcv",
        f"    ands wzr, {result}, {result}",
        "    mrs x9, nzcv",
        "    and w9, w9, #0xc0000000",
        "    cmp w16, #0",
        "    b.ne 1f",
        "    and w10, w10, #0x30000000",
        "    b 3f",
        "1:",
        "    and w10, w10, #0x10000000",
        "    cmp w16, #2",
        "    b.ne 3f",
        "    orr w10, w10, #0x20000000",
        "3:",
        "    orr w9, w9, w10",
        "    msr nzcv, x9",
    ]


def dp_immediate_body(opcode: int, set_flags: bool,
                      rd: int, rn: int) -> list[str]:
    body = ["    ldur w9, [x13, #-12]"]
    if opcode not in (13, 15):
        loads, source = read_guest_register(rn, 10)
        body.extend(loads)
    else:
        source = "wzr"

    writes = opcode < 8 or opcode >= 12
    if writes:
        result, stores = result_register(rd)
    else:
        result, stores = "w12", []

    if opcode == 0:       # AND
        body.append(f"    and {result}, {source}, w9")
    elif opcode == 1:     # EOR
        body.append(f"    eor {result}, {source}, w9")
    elif opcode == 2:     # SUB
        body.append(f"    {'subs' if set_flags else 'sub'} {result}, {source}, w9")
    elif opcode == 3:     # RSB
        body.append(f"    {'subs' if set_flags else 'sub'} {result}, w9, {source}")
    elif opcode == 4:     # ADD
        body.append(f"    {'adds' if set_flags else 'add'} {result}, {source}, w9")
    elif opcode == 5:     # ADC
        body.append(f"    {'adcs' if set_flags else 'adc'} {result}, {source}, w9")
    elif opcode == 6:     # SBC
        body.append(f"    {'sbcs' if set_flags else 'sbc'} {result}, {source}, w9")
    elif opcode == 7:     # RSC
        body.append(f"    {'sbcs' if set_flags else 'sbc'} {result}, w9, {source}")
    elif opcode == 8:     # TST
        body.append(f"    and {result}, {source}, w9")
    elif opcode == 9:     # TEQ
        body.append(f"    eor {result}, {source}, w9")
    elif opcode == 10:    # CMP
        body.append(f"    subs wzr, {source}, w9")
    elif opcode == 11:    # CMN
        body.append(f"    adds wzr, {source}, w9")
    elif opcode == 12:    # ORR
        body.append(f"    orr {result}, {source}, w9")
    elif opcode == 13:    # MOV
        body.append(f"    mov {result}, w9")
    elif opcode == 14:    # BIC
        body.append(f"    bic {result}, {source}, w9")
    else:                 # MVN
        body.append(f"    mvn {result}, w9")

    if (opcode in (0, 1, 8, 9, 12, 13, 14, 15) and
            (set_flags or not writes)):
        body.extend(logic_flags(result))
    body.extend(stores)
    body.extend(next_dispatch())
    return body


def logic_flags_from_shifter(result: str) -> list[str]:
    return [
        # w6 is the exact ARM shifter carry-out. Preserve the old V flag.
        "    mrs x10, nzcv",
        f"    ands wzr, {result}, {result}",
        "    mrs x9, nzcv",
        "    and w10, w10, #0x10000000",
        "    and w9, w9, #0xc0000000",
        "    cbz w6, 1f",
        "    orr w10, w10, #0x20000000",
        "1:",
        "    orr w9, w9, w10",
        "    msr nzcv, x9",
    ]


def dp_register_body(opcode: int, set_flags: bool,
                     rd: int, rn: int) -> list[str]:
    body: list[str] = []
    if opcode not in (13, 15):
        loads, source = read_guest_register(rn, 10)
        body.extend(loads)
    else:
        source = "wzr"

    writes = opcode < 8 or opcode >= 12
    if writes:
        result, stores = result_register(rd)
    else:
        result, stores = "w12", []

    if opcode == 0:
        body.append(f"    and {result}, {source}, w17")
    elif opcode == 1:
        body.append(f"    eor {result}, {source}, w17")
    elif opcode == 2:
        body.append(f"    {'subs' if set_flags else 'sub'} {result}, {source}, w17")
    elif opcode == 3:
        body.append(f"    {'subs' if set_flags else 'sub'} {result}, w17, {source}")
    elif opcode == 4:
        body.append(f"    {'adds' if set_flags else 'add'} {result}, {source}, w17")
    elif opcode == 5:
        body.append(f"    {'adcs' if set_flags else 'adc'} {result}, {source}, w17")
    elif opcode == 6:
        body.append(f"    {'sbcs' if set_flags else 'sbc'} {result}, {source}, w17")
    elif opcode == 7:
        body.append(f"    {'sbcs' if set_flags else 'sbc'} {result}, w17, {source}")
    elif opcode == 8:
        body.append(f"    and {result}, {source}, w17")
    elif opcode == 9:
        body.append(f"    eor {result}, {source}, w17")
    elif opcode == 10:
        body.append(f"    subs wzr, {source}, w17")
    elif opcode == 11:
        body.append(f"    adds wzr, {source}, w17")
    elif opcode == 12:
        body.append(f"    orr {result}, {source}, w17")
    elif opcode == 13:
        body.append(f"    mov {result}, w17")
    elif opcode == 14:
        body.append(f"    bic {result}, {source}, w17")
    else:
        body.append(f"    mvn {result}, w17")

    if (opcode in (0, 1, 8, 9, 12, 13, 14, 15) and
            (set_flags or not writes)):
        body.extend(logic_flags_from_shifter(result))
    body.extend(stores)
    body.extend(next_dispatch())
    return body


def carry_bit(source: str, bit: int) -> list[str]:
    return [f"    ubfx w6, {source}, #{bit}, #1"]


def shift_immediate_body(needs_carry: bool, shift_type: int,
                         rm: int, amount: int) -> list[str]:
    body, source = read_guest_register(rm, 10)
    body = list(body)

    if shift_type == 0:  # LSL
        if amount == 0:
            body.append(f"    mov w17, {source}")
            if needs_carry:
                body.append("    cset w6, cs")
        else:
            body.append(f"    lsl w17, {source}, #{amount}")
            if needs_carry:
                body.extend(carry_bit(source, 32 - amount))
    elif shift_type == 1:  # LSR; encoded zero means 32
        if amount == 0:
            body.append("    mov w17, wzr")
            if needs_carry:
                body.extend(carry_bit(source, 31))
        else:
            body.append(f"    lsr w17, {source}, #{amount}")
            if needs_carry:
                body.extend(carry_bit(source, amount - 1))
    elif shift_type == 2:  # ASR; encoded zero means 32
        body.append(f"    asr w17, {source}, #{31 if amount == 0 else amount}")
        if needs_carry:
            body.extend(carry_bit(source, 31 if amount == 0 else amount - 1))
    else:  # ROR; encoded zero is RRX through the old C flag
        if amount == 0:
            body.extend([
                "    cset w12, cs",
                f"    lsr w17, {source}, #1",
                "    orr w17, w17, w12, lsl #31",
            ])
            if needs_carry:
                body.extend(carry_bit(source, 0))
        else:
            body.append(f"    ror w17, {source}, #{amount}")
            if needs_carry:
                body.extend(carry_bit(source, amount - 1))

    body.extend(next_dispatch())
    return body


def shift_register_body(needs_carry: bool, shift_type: int,
                        rm: int, rs: int) -> list[str]:
    body, source = read_guest_register(rm, 10)
    amount_loads, amount_source = read_guest_register(rs, 9)
    body = list(body)
    body.extend(amount_loads)
    body.append(f"    and w9, {amount_source}, #0xff")

    if shift_type == 0:  # LSL
        body.extend([
            "    cbz w9, 1f",
            "    lsr w12, w9, #5",
            "    cbz w12, 2f",
            "    eor w12, w9, #32",
            "    cbz w12, 3f",
            "    mov w17, wzr",
        ])
        if needs_carry:
            body.append("    mov w6, wzr")
        body.extend(["    b 9f", "1:", f"    mov w17, {source}"])
        if needs_carry:
            body.append("    cset w6, cs")
        body.extend(["    b 9f", "2:", f"    lslv w17, {source}, w9"])
        if needs_carry:
            body.extend([
                "    mov w12, #32",
                "    sub w12, w12, w9",
                f"    lsrv w6, {source}, w12",
                "    and w6, w6, #1",
            ])
        body.extend(["    b 9f", "3:", "    mov w17, wzr"])
        if needs_carry:
            body.extend(carry_bit(source, 0))
    elif shift_type == 1:  # LSR
        body.extend([
            "    cbz w9, 1f",
            "    lsr w12, w9, #5",
            "    cbz w12, 2f",
            "    eor w12, w9, #32",
            "    cbz w12, 3f",
            "    mov w17, wzr",
        ])
        if needs_carry:
            body.append("    mov w6, wzr")
        body.extend(["    b 9f", "1:", f"    mov w17, {source}"])
        if needs_carry:
            body.append("    cset w6, cs")
        body.extend(["    b 9f", "2:", f"    lsrv w17, {source}, w9"])
        if needs_carry:
            body.extend([
                "    sub w12, w9, #1",
                f"    lsrv w6, {source}, w12",
                "    and w6, w6, #1",
            ])
        body.extend(["    b 9f", "3:", "    mov w17, wzr"])
        if needs_carry:
            body.extend(carry_bit(source, 31))
    elif shift_type == 2:  # ASR
        body.extend([
            "    cbz w9, 1f",
            "    lsr w12, w9, #5",
            "    cbz w12, 2f",
            "3:",
            f"    asr w17, {source}, #31",
        ])
        if needs_carry:
            body.extend(carry_bit(source, 31))
        body.extend(["    b 9f", "1:", f"    mov w17, {source}"])
        if needs_carry:
            body.append("    cset w6, cs")
        body.extend(["    b 9f", "2:", f"    asrv w17, {source}, w9"])
        if needs_carry:
            body.extend([
                "    sub w12, w9, #1",
                f"    lsrv w6, {source}, w12",
                "    and w6, w6, #1",
            ])
    else:  # ROR
        body.extend([
            "    cbz w9, 1f",
            "    and w12, w9, #31",
            "    cbz w12, 3f",
            f"    rorv w17, {source}, w12",
        ])
        if needs_carry:
            body.extend([
                "    sub w12, w12, #1",
                f"    lsrv w6, {source}, w12",
                "    and w6, w6, #1",
            ])
        body.extend(["    b 9f", "1:", f"    mov w17, {source}"])
        if needs_carry:
            body.append("    cset w6, cs")
        body.extend(["    b 9f", "3:", f"    mov w17, {source}"])
        if needs_carry:
            body.extend(carry_bit(source, 31))

    body.append("9:")
    body.extend(next_dispatch())
    return body


def address_body(up: bool, rn: int, register_offset: bool) -> list[str]:
    body: list[str] = []
    if not register_offset:
        body.append("    ldur w9, [x13, #-12]")
    loads, source = read_guest_register(rn, 10)
    body.extend(loads)
    offset = "w17" if register_offset else "w9"
    body.append(
        f"    {'add' if up else 'sub'} w17, {source}, {offset}"
    )
    body.extend(next_dispatch())
    return body


def direct_read_body(mnemonic: str, width: int, rd: int) -> list[str]:
    result, stores = result_register(rd)
    body = [
        # Every comparison below uses host NZCV. Preserve the guest flags even
        # when the access misses and exits in the middle of a block.
        "    mrs x7, nzcv",
        "    cbnz x3, 1f",
        "    b .La64s_direct_miss",
        "1:",
    ]
    if width > 1:
        # Refusing unaligned accesses is essential: arm_step owns SCTLR.A/U
        # faults, legacy word rotate semantics and legacy halfword
        # UNPREDICTABLE behavior. An aligned word or halfword cannot cross a
        # 1 KiB cache block.
        body.extend([
            f"    tst w17, #{width - 1}",
            "    b.eq 1f",
            "    b .La64s_direct_miss",
            "1:",
        ])
    body.extend([
        # slot = ((va >> 10) + (priv ? 32 : 0)) & 63
        "    ldr w4, [x3, #20]",
        "    lsr w5, w17, #10",
        "    add w5, w5, w4, lsl #5",
        "    and w5, w5, #63",
        "    ldr x6, [x3, #0]",
        "    add x6, x6, w5, uxtw #4",
        "    ldr x16, [x6, #0]",
        "    cbnz x16, 1f",
        "    b .La64s_direct_miss",
        "1:",
        # tag = 1 KiB-aligned VA | privilege
        "    lsr w4, w17, #10",
        "    lsl w4, w4, #10",
        "    ldr w5, [x3, #20]",
        "    orr w4, w4, w5",
        "    ldr w5, [x6, #8]",
        "    cmp w5, w4",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldr w4, [x6, #12]",
        "    ldr w5, [x3, #16]",
        "    cmp w4, w5",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    and w4, w17, #0x3ff",
        "    add x16, x16, w4, uxtw",
        f"    {mnemonic} {result}, [x16]",
        *stores,
        # Match dread_hit(): one hit for each successful direct access and no
        # counter change at all on a miss left for the literal slow path.
        "    ldr x4, [x3, #8]",
        "    ldr x5, [x4]",
        "    add x5, x5, #1",
        "    str x5, [x4]",
        "    msr nzcv, x7",
        *next_dispatch(),
    ])
    return body


def vfp_gate(kind: str) -> list[str]:
    """Preserve guest NZCV and fail before any VFP-visible state change.

    Access is precomputed from the live CPACR and privilege at each block run.
    FPEXC is read through its pointer for every instruction because VMSR FPEXC
    can enable or disable the following VFP instruction in the same block.
    """
    body = [
        "    mrs x7, nzcv",
        "    cbnz x3, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldr w4, [x3, #48]",
        "    cbnz w4, 1f",
        "    b .La64s_direct_miss",
        "1:",
    ]
    if kind == "priv":
        body.extend([
            "    ldr w4, [x3, #20]",
            "    cbnz w4, 1f",
            "    b .La64s_direct_miss",
            "1:",
        ])
    elif kind == "fpsid":
        body.extend([
            "    ldr x4, [x3, #32]",
            "    ldr w4, [x4]",
            "    tbnz w4, #30, 1f",
            "    ldr w4, [x3, #20]",
            "    cbnz w4, 1f",
            "    b .La64s_direct_miss",
            "1:",
        ])
    else:
        body.extend([
            "    ldr x4, [x3, #32]",
            "    ldr w4, [x4]",
            "    tbnz w4, #30, 1f",
            "    b .La64s_direct_miss",
            "1:",
        ])
        if kind == "exact":
            body.extend([
                "    ldr x4, [x3, #40]",
                "    ldr w4, [x4]",
                "    tst w4, #0x70000",
                "    b.eq 1f",
                "    b .La64s_direct_miss",
                "1:",
            ])
        elif kind == "values":
            body.extend([
                "    ldr x4, [x3, #40]",
                "    ldr w4, [x4]",
                "    mov w5, #0x9f00",
                "    movk w5, #0x7, lsl #16",
                "    tst w4, w5",
                "    b.eq 1f",
                "    b .La64s_direct_miss",
                "1:",
            ])
    return body


def vfp_finish() -> list[str]:
    return ["    msr nzcv, x7", *next_dispatch()]


def vfp_core_to_s_body(rt: int) -> list[str]:
    body = vfp_gate("enabled")
    loads, source = read_guest_register(rt, 10)
    body.extend(loads)
    body.extend([
        "    ldur w9, [x13, #-12]",
        "    ldr x4, [x3, #24]",
        f"    str {source}, [x4, w9, uxtw #2]",
        *vfp_finish(),
    ])
    return body


def vfp_s_to_core_body(rt: int) -> list[str]:
    result, stores = result_register(rt)
    return [
        *vfp_gate("enabled"),
        "    ldur w9, [x13, #-12]",
        "    ldr x4, [x3, #24]",
        f"    ldr {result}, [x4, w9, uxtw #2]",
        *stores,
        *vfp_finish(),
    ]


def vfp_core_to_pair_body(rt: int, rt2: int) -> list[str]:
    body = vfp_gate("enabled")
    loads, first = read_guest_register(rt, 4)
    body.extend(loads)
    loads, second = read_guest_register(rt2, 5)
    body.extend(loads)
    body.extend([
        "    ldur w9, [x13, #-12]",
        "    ldr x6, [x3, #24]",
        "    add x6, x6, w9, uxtw #2",
        f"    stp {first}, {second}, [x6]",
        *vfp_finish(),
    ])
    return body


def vfp_pair_to_core_body(rt: int, rt2: int) -> list[str]:
    return [
        *vfp_gate("enabled"),
        "    ldur w9, [x13, #-12]",
        "    ldr x6, [x3, #24]",
        "    add x6, x6, w9, uxtw #2",
        "    ldp w4, w5, [x6]",
        *write_guest_register(rt, "w4"),
        *write_guest_register(rt2, "w5"),
        *vfp_finish(),
    ]


def vfp_system_read_body(kind: str, rt: int) -> list[str]:
    result, stores = result_register(rt)
    gate = "fpsid" if kind == "fpsid" else (
        "priv" if kind == "fpexc" else "enabled"
    )
    body = vfp_gate(gate)
    if kind == "fpsid":
        body.extend([
            f"    mov {result}, #0x20b4",
            f"    movk {result}, #0x4101, lsl #16",
        ])
    else:
        offset = 32 if kind == "fpexc" else 40
        body.extend([
            f"    ldr x4, [x3, #{offset}]",
            f"    ldr {result}, [x4]",
        ])
    body.extend([*stores, *vfp_finish()])
    return body


def vfp_system_write_body(kind: str, rt: int) -> list[str]:
    gate = "priv" if kind == "fpexc" else "enabled"
    body = vfp_gate(gate)
    loads, source = read_guest_register(rt, 10)
    body.extend(loads)
    offset = 32 if kind == "fpexc" else 40
    body.append(f"    ldr x6, [x3, #{offset}]")
    if kind == "fpscr":
        body.extend([
            "    mov w5, #0x9f9f",
            "    movk w5, #0xf3f7, lsl #16",
            f"    and w4, {source}, w5",
            "    str w4, [x6]",
        ])
    else:
        body.append(f"    str {source}, [x6]")
    body.extend(vfp_finish())
    return body


def vfp_unary_body(operation: str, width: int) -> list[str]:
    reg = "w5" if width == 4 else "x5"
    body = [
        *vfp_gate("exact"),
        "    ldur w9, [x13, #-12]",
        "    and w10, w9, #0xff",
        "    ubfx w9, w9, #8, #8",
        "    ldr x4, [x3, #24]",
        "    add x6, x4, w9, uxtw #2",
        f"    ldr {reg}, [x6]",
    ]
    if operation == "abs":
        mask = "0x7fffffff" if width == 4 else "0x7fffffffffffffff"
        body.append(f"    and {reg}, {reg}, #{mask}")
    elif operation == "neg":
        mask = "0x80000000" if width == 4 else "0x8000000000000000"
        body.append(f"    eor {reg}, {reg}, #{mask}")
    body.extend([
        "    add x6, x4, w10, uxtw #2",
        f"    str {reg}, [x6]",
        *vfp_finish(),
    ])
    return body


def vfp_compare_body(width: int) -> list[str]:
    """Implement VFPv2 FPCompare without using the host floating-point unit."""
    bits = width * 8
    reg = "w" if width == 4 else "x"
    zero = "wzr" if width == 4 else "xzr"
    exp_lsb = 23 if width == 4 else 52
    exp_bits = 8 if width == 4 else 11
    exp_all = "#0xff" if width == 4 else "#0x7ff"
    frac_shift = 9 if width == 4 else 12
    quiet_bit = 22 if width == 4 else 51
    sign_mask = "#0x80000000" if width == 4 else "#0x8000000000000000"
    p = f".La64s_vfp_compare_{bits}"
    return [
        *vfp_gate("values"),
        "    ldur w9, [x13, #-12]",
        "    and w16, w9, #0xff",
        "    ubfx w17, w9, #8, #8",
        "    ubfx w12, w9, #17, #1",
        "    ldr x4, [x3, #24]",
        "    add x16, x4, w16, uxtw #2",
        f"    ldr {reg}5, [x16]",
        f"    tbnz w9, #16, {p}_zero",
        "    add x16, x4, w17, uxtw #2",
        f"    ldr {reg}6, [x16]",
        f"    b {p}_operands",
        f"{p}_zero:",
        f"    mov {reg}6, {zero}",
        f"{p}_operands:",
        "    ldr x17, [x3, #40]",
        "    ldr w4, [x17]",
        f"    tbz w4, #24, {p}_classify",
        # FZ replaces each input denormal by a same-sign zero and sets IDC.
        f"    ubfx {reg}16, {reg}5, #{exp_lsb}, #{exp_bits}",
        f"    cbnz {reg}16, {p}_fz_a_done",
        f"    lsl {reg}16, {reg}5, #{frac_shift}",
        f"    cbz {reg}16, {p}_fz_a_done",
        f"    and {reg}5, {reg}5, {sign_mask}",
        "    orr w4, w4, #0x80",
        f"{p}_fz_a_done:",
        f"    ubfx {reg}16, {reg}6, #{exp_lsb}, #{exp_bits}",
        f"    cbnz {reg}16, {p}_classify",
        f"    lsl {reg}16, {reg}6, #{frac_shift}",
        f"    cbz {reg}16, {p}_classify",
        f"    and {reg}6, {reg}6, {sign_mask}",
        "    orr w4, w4, #0x80",
        f"{p}_classify:",
        # w9 is any-NaN, w10 is any-signalling-NaN.
        "    mov w9, wzr",
        "    mov w10, wzr",
        f"    ubfx {reg}16, {reg}5, #{exp_lsb}, #{exp_bits}",
        f"    cmp {reg}16, {exp_all}",
        f"    b.ne {p}_class_b",
        f"    lsl {reg}16, {reg}5, #{frac_shift}",
        f"    cbz {reg}16, {p}_class_b",
        "    mov w9, #1",
        f"    tbnz {reg}5, #{quiet_bit}, {p}_class_b",
        "    mov w10, #1",
        f"{p}_class_b:",
        f"    ubfx {reg}16, {reg}6, #{exp_lsb}, #{exp_bits}",
        f"    cmp {reg}16, {exp_all}",
        f"    b.ne {p}_classified",
        f"    lsl {reg}16, {reg}6, #{frac_shift}",
        f"    cbz {reg}16, {p}_classified",
        "    mov w9, #1",
        f"    tbnz {reg}6, #{quiet_bit}, {p}_classified",
        "    mov w10, #1",
        f"{p}_classified:",
        f"    cbz w9, {p}_ordered",
        "    mov w6, #0x30000000",
        f"    cbnz w12, {p}_invalid",
        f"    cbz w10, {p}_write",
        f"{p}_invalid:",
        "    orr w4, w4, #1",
        f"    b {p}_write",
        f"{p}_ordered:",
        f"    cmp {reg}5, {reg}6",
        f"    b.eq {p}_equal",
        # +0.0 and -0.0 compare equal even though their encodings differ.
        f"    lsl {reg}16, {reg}5, #1",
        f"    cbnz {reg}16, {p}_key",
        f"    lsl {reg}16, {reg}6, #1",
        f"    cbz {reg}16, {p}_equal",
        f"{p}_key:",
        f"    asr {reg}9, {reg}5, #{bits - 1}",
        f"    orr {reg}9, {reg}9, {sign_mask}",
        f"    eor {reg}5, {reg}5, {reg}9",
        f"    asr {reg}9, {reg}6, #{bits - 1}",
        f"    orr {reg}9, {reg}9, {sign_mask}",
        f"    eor {reg}6, {reg}6, {reg}9",
        f"    cmp {reg}5, {reg}6",
        f"    b.lo {p}_less",
        "    mov w6, #0x20000000",
        f"    b {p}_write",
        f"{p}_less:",
        "    mov w6, #0x80000000",
        f"    b {p}_write",
        f"{p}_equal:",
        "    mov w6, #0x60000000",
        f"{p}_write:",
        "    bic w4, w4, #0xf0000000",
        "    orr w4, w4, w6",
        "    str w4, [x17]",
        *vfp_finish(),
    ]


def vfp_widen_body() -> list[str]:
    """Expand one binary32 encoding into binary64 without host FP state."""
    p = ".La64s_vfp_widen_32"
    return [
        *vfp_gate("values"),
        "    ldur w9, [x13, #-12]",
        "    and w12, w9, #0xff",
        "    ubfx w10, w9, #8, #8",
        "    ldr x6, [x3, #24]",
        "    add x16, x6, w10, uxtw #2",
        "    ldr w5, [x16]",
        "    ldr x17, [x3, #40]",
        "    ldr w4, [x17]",
        "    ubfx x9, x5, #31, #1",
        "    lsl x9, x9, #63",
        "    ubfx w10, w5, #23, #8",
        "    and w6, w5, #0x7fffff",
        f"    cbz w10, {p}_zero_exp",
        "    cmp w10, #0xff",
        f"    b.eq {p}_special",
        # Normal input: rebias 127 to 1023 and move the fraction exactly.
        "    add w10, w10, #896",
        "    lsl x10, x10, #52",
        "    lsl x6, x6, #29",
        "    orr x5, x9, x10",
        "    orr x5, x5, x6",
        f"    b {p}_store",
        f"{p}_zero_exp:",
        f"    cbz w6, {p}_signed_zero",
        f"    tbz w4, #24, {p}_subnormal",
        # FZ consumes the denormal and preserves its sign as zero.
        "    orr w4, w4, #0x80",
        f"    b {p}_signed_zero",
        f"{p}_subnormal:",
        # A binary32 subnormal is still normal in binary64. clz gives both the
        # normalization shift and the exact rebased exponent.
        "    clz w16, w6",
        "    add w10, w16, #21",
        "    lsl x6, x6, x10",
        "    and x6, x6, #0x000fffffffffffff",
        "    mov w10, #905",
        "    sub w10, w10, w16",
        "    lsl x10, x10, #52",
        "    orr x5, x9, x10",
        "    orr x5, x5, x6",
        f"    b {p}_store",
        f"{p}_special:",
        f"    cbnz w6, {p}_nan",
        "    mov x10, #0x7ff0000000000000",
        "    orr x5, x9, x10",
        f"    b {p}_store",
        f"{p}_nan:",
        # FPConvert quiets signalling NaNs and raises IOC before DN chooses
        # whether the payload remains observable.
        f"    tbnz w6, #22, {p}_quiet_nan",
        "    orr w4, w4, #1",
        "    orr w6, w6, #0x400000",
        f"{p}_quiet_nan:",
        f"    tbnz w4, #25, {p}_default_nan",
        "    mov x10, #0x7ff0000000000000",
        "    lsl x6, x6, #29",
        "    orr x5, x9, x10",
        "    orr x5, x5, x6",
        f"    b {p}_store",
        f"{p}_default_nan:",
        "    mov x5, #0x7ff8000000000000",
        f"    b {p}_store",
        f"{p}_signed_zero:",
        "    mov x5, x9",
        f"{p}_store:",
        "    ldr x6, [x3, #24]",
        "    add x6, x6, w12, uxtw #2",
        "    str x5, [x6]",
        "    str w4, [x17]",
        *vfp_finish(),
    ]


def vfp_direct_read_body(width: int) -> list[str]:
    reg = "w5" if width == 4 else "x5"
    body = [
        *vfp_gate("enabled"),
        # The interpreter applies coprocessor word alignment independently to
        # each word. Refuse every misaligned case so it retains sole ownership
        # of SCTLR.A/U faults and legacy align-down behavior.
        "    tst w17, #3",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
    ]
    if width == 8:
        body.extend([
            # Both words must reside in the same already-proved 1 KiB host
            # block. Otherwise the second word needs its own translation,
            # fault and cache accounting and stays literal.
            "    and w4, w17, #0x3ff",
            "    cmp w4, #1016",
            "    b.ls 1f",
            "    b .La64s_direct_miss",
            "1:",
        ])
    body.extend([
        "    ldr w4, [x3, #20]",
        "    lsr w5, w17, #10",
        "    add w5, w5, w4, lsl #5",
        "    and w5, w5, #63",
        "    ldr x6, [x3, #0]",
        "    add x6, x6, w5, uxtw #4",
        "    ldr x16, [x6, #0]",
        "    cbnz x16, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    lsr w4, w17, #10",
        "    lsl w4, w4, #10",
        "    ldr w5, [x3, #20]",
        "    orr w4, w4, w5",
        "    ldr w5, [x6, #8]",
        "    cmp w5, w4",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldr w4, [x6, #12]",
        "    ldr w5, [x3, #16]",
        "    cmp w4, w5",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    and w4, w17, #0x3ff",
        "    add x16, x16, w4, uxtw",
        "    ldur w9, [x13, #-12]",
        "    ldr x4, [x3, #24]",
        "    add x4, x4, w9, uxtw #2",
        f"    ldr {reg}, [x16]",
        f"    str {reg}, [x4]",
        # A double VLDR is architecturally two read32 calls. Both are hits
        # under the same-block guard, so preserve the interpreter's counters.
        "    ldr x4, [x3, #8]",
        "    ldr x5, [x4]",
        f"    add x5, x5, #{width // 4}",
        "    str x5, [x4]",
        *vfp_finish(),
    ])
    return body


def build_handlers() -> list[tuple[str, list[str]]]:
    handlers: list[tuple[str, list[str]]] = []

    handlers.append((".La64s_end", [
        "    ldur w12, [x13, #-12]",
        "    sub x15, x15, #1",
        # Keep the conditional target local: CBZ/CBNZ reaches only +/-1 MiB,
        # while the expanded signed handler text is intentionally larger.
        "    cbnz x15, 1f",
        "    ldr x9, [sp, #96]",
        "    cbnz x9, 2f",
        "    b .La64s_exit",
        "1:",
        "    mov x13, x14",
        *next_dispatch(),
        "2:",
        "    b .La64s_chain_advance",
    ]))

    for name, mnemonic in (
        ("add_rrr", "add"),
        ("sub_rrr", "sub"),
        ("eor_rrr", "eor"),
        ("orr_rrr", "orr"),
    ):
        for rd, hd in enumerate(HOST):
            for rn, hn in enumerate(HOST):
                for rm, hm in enumerate(HOST):
                    label = f".La64s_{name}_{rd}_{rn}_{rm}"
                    handlers.append((label, [
                        f"    {mnemonic} w{hd}, w{hn}, w{hm}",
                        *next_dispatch(),
                    ]))

    for name, mnemonic in (
        ("add_imm", "add"),
        ("sub_imm", "sub"),
        ("eor_imm", "eor"),
        ("adds_imm", "adds"),
        ("subs_imm", "subs"),
    ):
        for rd, hd in enumerate(HOST):
            for rn, hn in enumerate(HOST):
                label = f".La64s_{name}_{rd}_{rn}"
                handlers.append((label, [
                    "    ldur w9, [x13, #-12]",
                    f"    {mnemonic} w{hd}, w{hn}, w9",
                    *next_dispatch(),
                ]))

    for rd, hd in enumerate(HOST):
        for rm, hm in enumerate(HOST):
            label = f".La64s_eors_rr_{rd}_{rm}"
            handlers.append((label, [
                # Thumb EOR updates N/Z but preserves C/V. AArch64 has no EORS,
                # so retain C/V explicitly around a flag-setting self-test.
                "    mrs x10, nzcv",
                f"    eor w{hd}, w{hd}, w{hm}",
                f"    ands wzr, w{hd}, w{hd}",
                "    mrs x9, nzcv",
                "    and w10, w10, #0x30000000",
                "    and w9, w9, #0xc0000000",
                "    orr w9, w9, w10",
                "    msr nzcv, x9",
                *next_dispatch(),
            ]))

    for rd, hd in enumerate(HOST):
        for rm, hm in enumerate(HOST):
            label = f".La64s_muls_rr_{rd}_{rm}"
            handlers.append((label, [
                # Thumb MUL updates N/Z and preserves C/V on ARMv6.
                "    mrs x10, nzcv",
                f"    mul w{hd}, w{hd}, w{hm}",
                f"    ands wzr, w{hd}, w{hd}",
                "    mrs x9, nzcv",
                "    and w10, w10, #0x30000000",
                "    and w9, w9, #0xc0000000",
                "    orr w9, w9, w10",
                "    msr nzcv, x9",
                *next_dispatch(),
            ]))

    for name, mnemonic in (("ldr", "ldr"), ("str", "str")):
        for rd, hd in enumerate(HOST):
            for rn, hn in enumerate(HOST):
                label = f".La64s_{name}_{rd}_{rn}"
                memory_op = (
                    f"    ldr w{hd}, [x28, w9, uxtw]"
                    if mnemonic == "ldr"
                    else f"    str w{hd}, [x28, w9, uxtw]"
                )
                handlers.append((label, [
                    "    ldur w9, [x13, #-12]",
                    f"    add w9, w{hn}, w9",
                    "    and w9, w9, w11",
                    memory_op,
                    *next_dispatch(),
                ]))

    for name, mnemonic in (("ldr_sp", "ldr"), ("str_sp", "str")):
        for rd, hd in enumerate(HOST):
            label = f".La64s_{name}_{rd}"
            memory_op = (
                f"    ldr w{hd}, [x28, w9, uxtw]"
                if mnemonic == "ldr"
                else f"    str w{hd}, [x28, w9, uxtw]"
            )
            handlers.append((label, [
                "    ldur w9, [x13, #-12]",
                "    add w9, w27, w9",
                "    and w9, w9, w11",
                memory_op,
                *next_dispatch(),
            ]))

    # A failed ARM condition skips the record count stored in its metadata.
    # Immediate forms use one semantic record; register forms use a shifter
    # record followed by an ALU record. AL has no guard and cond=0xf is
    # rejected by the decoder.
    for condition in CONDITIONS:
        handlers.append((f".La64s_cond_{condition}", [
            f"    b.{condition} 1f",
            "    ldur w16, [x13, #-4]",
            "    add x13, x13, x16, lsl #4",
            "1:",
            *next_dispatch(),
        ]))

    # Uniform dimensions keep the C decoder's handler arithmetic auditable.
    # Several comparison/Rn combinations are unreachable but retaining their
    # ordinary signed text is cheaper and safer than a generated lookup map.
    for opcode in range(16):
        for set_flags in (False, True):
            for rd in range(15):
                for rn in range(16):
                    label = (
                        f".La64s_dp_imm_{opcode}_{int(set_flags)}_{rd}_{rn}"
                    )
                    handlers.append((
                        label,
                        dp_immediate_body(opcode, set_flags, rd, rn),
                    ))

    # Register Operand2 is split into an exact barrel-shifter record and a
    # data-processing record. Keeping those dimensions independent avoids a
    # firmware-derived table and holds signed text to a tractable size.
    for needs_carry in (False, True):
        for shift_type in range(4):
            for rm in range(16):
                for amount in range(32):
                    label = (
                        f".La64s_shift_imm_{int(needs_carry)}_"
                        f"{shift_type}_{rm}_{amount}"
                    )
                    handlers.append((
                        label,
                        shift_immediate_body(
                            needs_carry, shift_type, rm, amount
                        ),
                    ))

    # R15 is architecturally UNPREDICTABLE in every register-specified shift
    # operand field on ARM1176, so only r0-r14 are enumerated here.
    for needs_carry in (False, True):
        for shift_type in range(4):
            for rm in range(15):
                for rs in range(15):
                    label = (
                        f".La64s_shift_reg_{int(needs_carry)}_"
                        f"{shift_type}_{rm}_{rs}"
                    )
                    handlers.append((
                        label,
                        shift_register_body(
                            needs_carry, shift_type, rm, rs
                        ),
                    ))

    for opcode in range(16):
        for set_flags in (False, True):
            for rd in range(15):
                for rn in range(16):
                    label = (
                        f".La64s_dp_reg_{opcode}_{int(set_flags)}_{rd}_{rn}"
                    )
                    handlers.append((
                        label,
                        dp_register_body(opcode, set_flags, rd, rn),
                    ))

    # The product read path splits address generation from the cache access.
    # U and Rn are ordinary ISA dimensions; immediates remain in data records.
    for up in (False, True):
        for rn in range(16):
            label = f".La64s_addr_imm_{int(up)}_{rn}"
            handlers.append((label, address_body(up, rn, False)))

    for up in (False, True):
        for rn in range(16):
            label = f".La64s_addr_reg_{int(up)}_{rn}"
            handlers.append((label, address_body(up, rn, True)))

    # Loads to PC remain literal. Five result widths/sign modes across r0-r14
    # need seventy-five direct handlers because the address record has already
    # produced the exact VA.
    for kind, mnemonic, width in READ_KINDS:
        for rd in range(15):
            label = f".La64s_direct_read_{kind}_{rd}"
            handlers.append((label,
                             direct_read_body(mnemonic, width, rd)))

    # VFP register and system-state operations are ordinary signed text too.
    # Only core-register operands need enumerated handlers; VFP register
    # numbers remain data-record immediates because the register file is not
    # pinned in host registers.
    for rt in range(15):
        handlers.append((f".La64s_vfp_core_to_s_{rt}",
                         vfp_core_to_s_body(rt)))
    for rt in range(15):
        handlers.append((f".La64s_vfp_s_to_core_{rt}",
                         vfp_s_to_core_body(rt)))
    for rt in range(15):
        for rt2 in range(15):
            handlers.append((f".La64s_vfp_core_to_pair_{rt}_{rt2}",
                             vfp_core_to_pair_body(rt, rt2)))
    for rt in range(15):
        for rt2 in range(15):
            handlers.append((f".La64s_vfp_pair_to_core_{rt}_{rt2}",
                             vfp_pair_to_core_body(rt, rt2)))
    for kind in ("fpsid", "fpscr", "fpexc"):
        for rt in range(15):
            handlers.append((f".La64s_vfp_vmrs_{kind}_{rt}",
                             vfp_system_read_body(kind, rt)))
    handlers.append((".La64s_vfp_vmrs_apsr", [
        *vfp_gate("enabled"),
        "    ldr x4, [x3, #40]",
        "    ldr w4, [x4]",
        # This instruction intentionally replaces guest NZCV instead of
        # restoring x7; all other CPSR bits stay in the C-side cpsr word.
        "    msr nzcv, x4",
        *next_dispatch(),
    ]))
    for kind in ("fpscr", "fpexc"):
        for rt in range(15):
            handlers.append((f".La64s_vfp_vmsr_{kind}_{rt}",
                             vfp_system_write_body(kind, rt)))
    for operation in ("mov", "abs", "neg"):
        handlers.append((f".La64s_vfp_{operation}_32",
                         vfp_unary_body(operation, 4)))
    for operation in ("mov", "abs", "neg"):
        handlers.append((f".La64s_vfp_{operation}_64",
                         vfp_unary_body(operation, 8)))
    handlers.append((".La64s_vfp_compare_32", vfp_compare_body(4)))
    handlers.append((".La64s_vfp_compare_64", vfp_compare_body(8)))
    handlers.append((".La64s_vfp_widen_32", vfp_widen_body()))
    handlers.append((".La64s_vfp_direct_read_32",
                     vfp_direct_read_body(4)))
    handlers.append((".La64s_vfp_direct_read_64",
                     vfp_direct_read_body(8)))

    # Terminal A32 immediate branches leave the threaded block directly. An
    # unconditional B already uses the compact END record; these fourteen
    # conditional B handlers and fifteen conditional/AL BL handlers carry the
    # distinct taken and fallthrough PCs without generating runtime code.
    for condition in CONDITIONS:
        handlers.append((f".La64s_branch_{condition}",
                         terminal_branch_body(condition, False)))
    for condition in (*CONDITIONS, ""):
        name = condition if condition else "al"
        handlers.append((f".La64s_branch_link_{name}",
                         terminal_branch_body(condition, True)))

    if len(handlers) != EXPECTED_HANDLERS:
        raise RuntimeError(
            f"generated {len(handlers)} handlers, expected {EXPECTED_HANDLERS}"
        )
    return handlers


def render() -> str:
    handlers = build_handlers()
    lines = [
        "/* Generated by tools/gen_a64_static.py. Do not edit. */",
        "#if defined(__APPLE__)",
        "# define A64S_CSYM_INNER(name) _##name",
        "# define A64S_CSYM(name) A64S_CSYM_INNER(name)",
        "#else",
        "# define A64S_CSYM(name) name",
        "#endif",
        "",
        ".text",
        ".p2align 2",
        ".globl A64S_CSYM(a64_static_execute)",
        "#if !defined(__APPLE__)",
        ".type A64S_CSYM(a64_static_execute), %function",
        "#endif",
        "A64S_CSYM(a64_static_execute):",
        "    stp x29, x30, [sp, #-144]!",
        "    mov x29, sp",
        "    stp x19, x20, [sp, #16]",
        "    stp x21, x22, [sp, #32]",
        "    stp x23, x24, [sp, #48]",
        "    stp x25, x26, [sp, #64]",
        "    stp x27, x28, [sp, #80]",
        "    str x0, [sp, #104]",
        "    str x1, [sp, #112]",
        "    str x2, [sp, #120]",
        "",
        "    ldp w19, w20, [x0, #0]",
        "    ldp w21, w22, [x0, #8]",
        "    ldp w23, w24, [x0, #16]",
        "    ldp w25, w26, [x0, #24]",
        "    ldr w27, [x0, #52]",
        "    mov x28, x5",
        "    mov w11, w6",
        "    mov x14, x3",
        "    mov x13, x3",
        "    mov x15, x4",
        # The ninth and tenth AAPCS64 arguments live at the entry SP. The
        # 144-byte prologue moves them to [sp,#144] and [sp,#152]. The former
        # carries DREAD/VFP live-state pointers; the latter is NULL for the
        # legacy boundary or carries the persistent product-chain context.
        "    ldr x3, [sp, #144]",
        "    ldr x9, [sp, #152]",
        "    str x9, [sp, #96]",
        "    str x3, [sp, #128]",
        # ADR also reaches only +/-1 MiB. Use the platform's page-relative
        # relocation spelling so handler growth cannot invalidate the entry.
        "#if defined(__APPLE__)",
        "    adrp x8, .La64s_table@PAGE",
        "    add x8, x8, .La64s_table@PAGEOFF",
        "#else",
        "    adrp x8, .La64s_table",
        "    add x8, x8, :lo12:.La64s_table",
        "#endif",
        "",
        "    mul x9, x4, x7",
        "    ldr x10, [x2]",
        "    add x10, x10, x9",
        "    str x10, [x2]",
        "    ldr w9, [x1]",
        "    msr nzcv, x9",
        *next_dispatch(),
        "",
    ]

    for label, body in handlers:
        lines.append(f"{label}:")
        lines.extend(body)

    lines.extend([
        "",
        ".La64s_direct_miss:",
        # metadata low byte is the unretired suffix (including this load); the
        # next byte is completed+1 so zero can continue to mean full success.
        "    ldur w16, [x13, #-4]",
        "    and w9, w16, #0xff",
        "    ldr x10, [x2]",
        "    sub x10, x10, x9",
        "    str x10, [x2]",
        "    ldur w12, [x13, #-8]",
        "    ubfx w17, w16, #8, #8",
        "    msr nzcv, x7",
        "    ldr x9, [sp, #96]",
        "    cbz x9, .La64s_save",
        "    mrs x10, nzcv",
        "    str x10, [sp, #136]",
        "    mov x0, x9",
        "    sub w1, w17, #1",
        "    mov w2, w12",
        "    bl A64S_CSYM(a64_static_chain_partial)",
        "    ldr x9, [sp, #96]",
        "    ldr w12, [x9, #60]",
        "    ldr x0, [sp, #104]",
        "    ldr x1, [sp, #112]",
        "    ldr x10, [sp, #136]",
        "    msr nzcv, x10",
        "    mov w17, wzr",
        "    b .La64s_save",
        "",
        ".La64s_terminal_exit:",
        "    ldr x9, [sp, #96]",
        "    cbnz x9, .La64s_chain_advance",
        "    b .La64s_exit",
        "",
        ".La64s_chain_advance:",
        "    mrs x9, nzcv",
        "    str x9, [sp, #136]",
        "    ldr x0, [sp, #96]",
        "    ldr x10, [x0, #72]",
        "    cbnz x10, .La64s_graph_advance",
        "    mov w1, w12",
        "    bl A64S_CSYM(a64_static_chain_advance)",
        "    cbz x0, .La64s_chain_stop",
        "    ldr x13, [x0, #0]",
        "    mov x14, x13",
        "    mov x15, #1",
        "    ldr x9, [sp, #96]",
        "    ldr w11, [x9, #40]",
        "    ldr x0, [sp, #104]",
        "    ldr x1, [sp, #112]",
        "    ldr x2, [sp, #120]",
        "    ldr x3, [sp, #128]",
        "    ldr x9, [sp, #136]",
        "    msr nzcv, x9",
        "#if defined(__APPLE__)",
        "    adrp x8, .La64s_table@PAGE",
        "    add x8, x8, .La64s_table@PAGEOFF",
        "#else",
        "    adrp x8, .La64s_table",
        "    add x8, x8, :lo12:.La64s_table",
        "#endif",
        *next_dispatch(),
        "",
        ".La64s_graph_advance:",
        # Account the block that just completed before selecting another. All
        # quantities are bounded to sixteen, but validate before adding so a
        # corrupted descriptor can only stop, never expand caller retirement.
        "    str w12, [x0, #60]",
        "    ldr w4, [x0, #52]",
        "    ldr x5, [x0, #8]",
        "    cbz x5, .La64s_chain_stop",
        "    ldr w6, [x0, #48]",
        "    cmp w4, w6",
        "    b.hs .La64s_chain_stop",
        "    sub w7, w6, w4",
        "    cmp w5, w7",
        "    b.hi .La64s_chain_stop",
        "    add w4, w4, w5",
        "    str w4, [x0, #52]",
        "    ldr w6, [x0, #56]",
        "    add w6, w6, #1",
        "    str w6, [x0, #56]",
        "    sub w6, w7, w5",
        "    cbz w6, .La64s_chain_stop",
        # Chaining never leaves the fetch block proved by the outer C entry.
        "    ldr w5, [x0, #88]",
        "    lsr w7, w12, #10",
        "    lsr w16, w5, #10",
        "    cmp w7, w16",
        "    b.ne .La64s_chain_stop",
        "    sub w4, w12, w5",
        # One 128-byte descriptor exists per halfword offset. A32 uses the even
        # quarter of those slots. Full PC/Thumb checks below catch every alias.
        "    ldrb w7, [x0, #64]",
        "    cbz w7, .La64s_graph_a32_index",
        "    lsr w4, w4, #1",
        "    b .La64s_graph_indexed",
        ".La64s_graph_a32_index:",
        "    lsr w4, w4, #2",
        ".La64s_graph_indexed:",
        "    lsl x4, x4, #7",
        "    add x10, x10, x4",
        # Validate the data-only node. Owner replacement clears valid before
        # reusing inline uops, and the complete executing-block byte witness
        # catches SMC without repeatedly comparing unrelated candidate tail.
        "    ldrb w4, [x10, #106]",
        "    cbz w4, .La64s_chain_stop",
        "    ldrb w4, [x10, #107]",
        "    cbz w4, .La64s_chain_stop",
        "    ldr x4, [x10, #8]",
        "    ldr x5, [x0, #80]",
        "    cmp x4, x5",
        "    b.ne .La64s_chain_stop",
        "    ldr w4, [x10, #24]",
        "    cmp w4, w12",
        "    b.ne .La64s_chain_stop",
        "    ldr w4, [x10, #28]",
        "    ldr w5, [x0, #92]",
        "    cmp w4, w5",
        "    b.ne .La64s_chain_stop",
        "    ldrb w4, [x10, #104]",
        "    ldr w5, [x0, #96]",
        "    cmp w4, w5",
        "    b.ne .La64s_chain_stop",
        "    ldrb w4, [x10, #105]",
        "    ldrb w5, [x0, #64]",
        "    cmp w4, w5",
        "    b.ne .La64s_chain_stop",
        "    ldr w5, [x10, #32]",
        "    cbz w5, .La64s_chain_stop",
        "    cmp w5, w6",
        "    b.hi .La64s_chain_stop",
        "    ldr w7, [x10, #36]",
        "    cbz w7, .La64s_chain_stop",
        "    cmp w7, #64",
        "    b.hi .La64s_chain_stop",
        "    tst w7, #1",
        "    b.ne .La64s_chain_stop",
        "    ldr w4, [x0, #88]",
        "    sub w4, w12, w4",
        "    ldr x5, [x0, #80]",
        "    add x4, x5, w4, uxtw",
        "    add x5, x10, #40",
        ".La64s_graph_raw8:",
        "    cmp w7, #8",
        "    b.lo .La64s_graph_raw4",
        "    ldr x16, [x4], #8",
        "    ldr x17, [x5], #8",
        "    cmp x16, x17",
        "    b.ne .La64s_chain_stop",
        "    sub w7, w7, #8",
        "    b .La64s_graph_raw8",
        ".La64s_graph_raw4:",
        "    tbz w7, #2, .La64s_graph_raw2",
        "    ldr w16, [x4], #4",
        "    ldr w17, [x5], #4",
        "    cmp w16, w17",
        "    b.ne .La64s_chain_stop",
        ".La64s_graph_raw2:",
        "    tbz w7, #1, .La64s_graph_accept",
        "    ldrh w16, [x4]",
        "    ldrh w17, [x5]",
        "    cmp w16, w17",
        "    b.ne .La64s_chain_stop",
        ".La64s_graph_accept:",
        "    ldr x13, [x10, #16]",
        "    cbz x13, .La64s_chain_stop",
        "    ldr w5, [x10, #32]",
        "    str x13, [x0, #0]",
        "    str x5, [x0, #8]",
        "    ldr x4, [sp, #120]",
        "    ldr x6, [x4]",
        "    add x6, x6, x5",
        "    str x6, [x4]",
        "    mov x14, x13",
        "    mov x15, #1",
        "    ldr w11, [x0, #40]",
        "    ldr x0, [sp, #104]",
        "    ldr x1, [sp, #112]",
        "    ldr x2, [sp, #120]",
        "    ldr x3, [sp, #128]",
        "    ldr x9, [sp, #136]",
        "    msr nzcv, x9",
        "#if defined(__APPLE__)",
        "    adrp x8, .La64s_table@PAGE",
        "    add x8, x8, .La64s_table@PAGEOFF",
        "#else",
        "    adrp x8, .La64s_table",
        "    add x8, x8, :lo12:.La64s_table",
        "#endif",
        *next_dispatch(),
        "",
        ".La64s_chain_stop:",
        "    ldr x9, [sp, #96]",
        "    ldr w12, [x9, #60]",
        "    ldr x0, [sp, #104]",
        "    ldr x1, [sp, #112]",
        "    ldr x9, [sp, #136]",
        "    msr nzcv, x9",
        "    mov w17, wzr",
        "    b .La64s_save",
        "",
        ".La64s_exit:",
        "    mov w17, wzr",
        "",
        ".La64s_save:",
        "    stp w19, w20, [x0, #0]",
        "    stp w21, w22, [x0, #8]",
        "    stp w23, w24, [x0, #16]",
        "    stp w25, w26, [x0, #24]",
        "    str w27, [x0, #52]",
        "    str w12, [x0, #60]",
        "    mrs x9, nzcv",
        "    ldr w10, [x1]",
        "    ubfx w9, w9, #28, #4",
        "    bfi w10, w9, #28, #4",
        "    str w10, [x1]",
        "",
        "    ldp x27, x28, [sp, #80]",
        "    ldp x25, x26, [sp, #64]",
        "    ldp x23, x24, [sp, #48]",
        "    ldp x21, x22, [sp, #32]",
        "    ldp x19, x20, [sp, #16]",
        "    ldp x29, x30, [sp], #144",
        "    mov w0, w17",
        "    ret",
        "#if !defined(__APPLE__)",
        ".size A64S_CSYM(a64_static_execute), .-A64S_CSYM(a64_static_execute)",
        "#endif",
        "",
        ".p2align 2",
        ".globl A64S_CSYM(a64_static_handler_offsets)",
        "A64S_CSYM(a64_static_handler_offsets):",
        ".La64s_table:",
    ])
    lines.extend(f"    .long {label} - .La64s_table" for label, _ in handlers)
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render(), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
