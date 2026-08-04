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
EXPECTED_HANDLERS = 24005


def next_dispatch() -> list[str]:
    return [
        "    ldr w16, [x13], #16",
        "    ldrsw x16, [x8, w16, uxtw #2]",
        "    add x16, x8, x16",
        "    br x16",
    ]


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


def direct_read_body(byte: bool, rd: int) -> list[str]:
    result, stores = result_register(rd)
    body = [
        # Every comparison below uses host NZCV. Preserve the guest flags even
        # when the access misses and exits in the middle of a block.
        "    mrs x7, nzcv",
        "    cbnz x3, 1f",
        "    b .La64s_direct_miss",
        "1:",
    ]
    if not byte:
        # The observed boot window had no unaligned candidate. Refusing it is
        # nevertheless essential: arm_step owns SCTLR.A/U faults and legacy
        # rotate semantics, while an aligned word cannot cross a 1 KiB block.
        body.extend([
            "    tst w17, #3",
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
        f"    {'ldrb' if byte else 'ldr'} {result}, [x16]",
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


def build_handlers() -> list[tuple[str, list[str]]]:
    handlers: list[tuple[str, list[str]]] = []

    handlers.append((".La64s_end", [
        "    ldur w12, [x13, #-12]",
        "    sub x15, x15, #1",
        # Keep the conditional target local: CBZ/CBNZ reaches only +/-1 MiB,
        # while the expanded signed handler text is intentionally larger.
        "    cbnz x15, 1f",
        "    b .La64s_exit",
        "1:",
        "    mov x13, x14",
        *next_dispatch(),
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

    # Loads to PC remain literal. Byte/word and r0-r14 need only thirty direct
    # handlers because the address record has already produced the exact VA.
    for byte in (False, True):
        for rd in range(15):
            label = f".La64s_direct_read_{int(byte)}_{rd}"
            handlers.append((label, direct_read_body(byte, rd)))

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
        "    stp x29, x30, [sp, #-96]!",
        "    mov x29, sp",
        "    stp x19, x20, [sp, #16]",
        "    stp x21, x22, [sp, #32]",
        "    stp x23, x24, [sp, #48]",
        "    stp x25, x26, [sp, #64]",
        "    stp x27, x28, [sp, #80]",
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
        # The ninth AAPCS64 argument lives at the entry SP. The 96-byte
        # prologue moves that slot to [sp,#96]; it is NULL for the flat proof.
        "    ldr x3, [sp, #96]",
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
        "    ldp x29, x30, [sp], #96",
        "    mov w0, w17",
        "    ret",
        "#if !defined(__APPLE__)",
        ".size A64S_CSYM(a64_static_execute), .-A64S_CSYM(a64_static_execute)",
        "#endif",
        "",
        ".p2align 2",
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
