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
EXPECTED_HANDLERS = 26509

READ_KINDS = (
    ("word", "ldr", 4),
    ("byte", "ldrb", 1),
    ("half", "ldrh", 2),
    ("signed_byte", "ldrsb", 1),
    ("signed_half", "ldrsh", 2),
)

WRITE_KINDS = (
    ("word", "str", 4),
    ("byte", "strb", 1),
    ("half", "strh", 2),
)


def next_dispatch() -> list[str]:
    return [
        "    ldr w16, [x13], #16",
        "    ldrsw x16, [x8, w16, uxtw #2]",
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


def indirect_branch_body(link: bool, thumb: bool, rm: int) -> list[str]:
    body = ["    mrs x7, nzcv"]
    if rm in PINNED:
        body.append(f"    mov w12, w{PINNED[rm]}")
    elif rm == 15:
        body.extend([
            "    ldur w12, [x13, #-8]",
            f"    add w12, w12, #{4 if thumb else 8}",
        ])
    else:
        body.append(f"    ldr w12, [x0, #{rm * 4}]")

    body.extend([
        # An even halfword target cannot be represented in ARM state. Refuse
        # before changing LR, CPSR.T or the persistent chain-state byte.
        "    and w16, w12, #3",
        "    cmp w16, #2",
        "    b.ne 1f",
        "    b .La64s_direct_miss",
        "1:",
    ])
    if link:
        body.extend([
            "    ldur w9, [x13, #-8]",
            f"    add w9, w9, #{2 if thumb else 4}",
        ])
        if thumb:
            body.append("    orr w9, w9, #1")
        body.append("    str w9, [x0, #56]")

    body.extend([
        "    and w16, w12, #1",
        "    ldr w9, [x1]",
        "    bfi w9, w16, #5, #1",
        "    str w9, [x1]",
        "    ldr x10, [sp, #96]",
        "    cbz x10, 2f",
        "    strb w16, [x10, #64]",
        "2:",
        "    bic w12, w12, #1",
        "    msr nzcv, x7",
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


def post_address_body(up: bool, rn: int,
                      register_offset: bool) -> list[str]:
    body: list[str] = []
    if not register_offset:
        body.append("    ldur w9, [x13, #-12]")
    loads, source = read_guest_register(rn, 10)
    body.extend(loads)
    offset = "w17" if register_offset else "w9"
    # w17 is the transfer VA; w9 retains the post-index writeback value until
    # the guarded store has succeeded. No guest register changes on a miss.
    body.extend([
        f"    {'add' if up else 'sub'} w9, {source}, {offset}",
        f"    mov w17, {source}",
        *next_dispatch(),
    ])
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


def direct_write_body(mnemonic: str, width: int, rd: int,
                      rn: int | None, unprivileged: bool) -> list[str]:
    body = [
        # Preserve guest NZCV across every host-side cache comparison and every
        # miss. x3 is non-NULL for product runs, but a flat runner must fail
        # closed before a direct store can touch guest state.
        "    mrs x7, nzcv",
        "    cbnz x3, 1f",
        "    b .La64s_direct_miss",
        "1:",
    ]
    if width > 1:
        # arm_step owns SCTLR.A/U behavior and all legacy unaligned semantics.
        # An aligned word/halfword also cannot cross this cache's 1 KiB block.
        body.extend([
            f"    tst w17, #{width - 1}",
            "    b.eq 1f",
            "    b .La64s_direct_miss",
            "1:",
        ])

    # dwrite is present only while the frontend's separate write-pointer
    # consent callback is live. The entry itself proves the exact translated
    # VA block, privilege and MMU generation resolved by the literal path.
    body.extend([
        "    ldr x6, [x3, #56]",
        "    cbnz x6, 1f",
        "    b .La64s_direct_miss",
        "1:",
    ])
    if unprivileged:
        body.append("    mov w4, wzr")
    else:
        body.append("    ldr w4, [x3, #20]")
    body.extend([
        "    lsr w5, w17, #10",
        "    add w5, w5, w4, lsl #5",
        "    and w5, w5, #63",
        "    add x6, x6, w5, uxtw #4",
        "    ldr x16, [x6, #0]",
        "    cbnz x16, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    lsr w4, w17, #10",
        "    lsl w4, w4, #10",
    ])
    if unprivileged:
        body.append("    mov w5, wzr")
    else:
        body.append("    ldr w5, [x3, #20]")
    body.extend([
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
    ])

    if rd == 15:
        # Runtime-guard metadata makes this record's pc_value the instruction
        # address. ARM STR pc stores that address plus twelve, not PC+8.
        source_loads = [
            "    ldur w10, [x13, #-8]",
            "    add w10, w10, #12",
        ]
        source = "w10"
    else:
        source_loads, source = read_guest_register(rd, 10)
    body.extend(source_loads)
    body.append(f"    {mnemonic} {source}, [x16]")

    if rn is not None:
        # immediate==0 selects the pre-index effective address in w17;
        # immediate==1 selects the post-index update retained in w9.
        body.extend([
            "    ldur w4, [x13, #-12]",
            "    cbz w4, 1f",
            "    mov w4, w9",
            "    b 2f",
            "1:",
            "    mov w4, w17",
            "2:",
            *write_guest_register(rn, "w4"),
        ])

    body.extend([
        # Match dwrite_hit(): a successful signed access is one hit; a miss is
        # counted exactly once later by the literal fallback that fills it.
        "    ldr x4, [x3, #64]",
        "    ldr x5, [x4]",
        "    add x5, x5, #1",
        "    str x5, [x4]",
        "    msr nzcv, x7",
        *next_dispatch(),
    ])
    return body


def stm_preflight_body(pre: bool, up: bool, rn: int) -> list[str]:
    """Prove one aligned DWRITE block before an ordinary STM commits.

    x7 retains guest NZCV, w9 the optional writeback value, w10 PC+12 and x17
    the advancing host pointer across the source handlers. No architectural
    state changes until every runtime proof below has succeeded.
    """
    body = [
        "    mrs x7, nzcv",
        "    cbnz x3, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldur w5, [x13, #-12]",
        "    cbnz w5, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    cmp w5, #16",
        "    b.ls 1f",
        "    b .La64s_direct_miss",
        "1:",
    ]
    loads, base = read_guest_register(rn, 4)
    body.extend(loads)
    if up:
        body.append(f"    {'add w17, ' + base + ', #4' if pre else 'mov w17, ' + base}")
        body.append(f"    add w9, {base}, w5, lsl #2")
    else:
        body.append(f"    sub w9, {base}, w5, lsl #2")
        body.append("    mov w17, w9" if pre else "    add w17, w9, #4")
    body.extend([
        # arm_step owns SCTLR.A/U and legacy align-down behavior.
        "    tst w17, #3",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        # A single validated DWRITE entry covers exactly one 1 KiB block. This
        # check also rejects a wrapped 32-bit run at the top of guest VA space.
        "    and w4, w17, #0x3ff",
        "    add w4, w4, w5, lsl #2",
        "    cmp w4, #1024",
        "    b.ls 1f",
        "    b .La64s_direct_miss",
        "1:",
        # STM's R15 source is the instruction address plus twelve.
        "    ldur w10, [x13, #-8]",
        "    add w10, w10, #12",
        # The table exists only under the frontend's explicit write consent.
        "    ldr x6, [x3, #56]",
        "    cbnz x6, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldr w4, [x3, #20]",
        "    lsr w5, w17, #10",
        "    add w5, w5, w4, lsl #5",
        "    and w5, w5, #63",
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
        "    add x17, x16, w4, uxtw",
        *next_dispatch(),
    ])
    return body


def stm_commit_body(rd: int) -> list[str]:
    if rd == 15:
        body = ["    str w10, [x17], #4"]
    else:
        loads, source = read_guest_register(rd, 4)
        body = [*loads, f"    str {source}, [x17], #4"]
    body.extend(next_dispatch())
    return body


def stm_finish_body(rn: int | None) -> list[str]:
    body: list[str] = []
    if rn is not None:
        body.extend(write_guest_register(rn, "w9"))
    body.extend([
        # Match one dwrite_hit() for each architectural write32 call.
        "    ldur w6, [x13, #-12]",
        "    ldr x4, [x3, #64]",
        "    ldr x5, [x4]",
        "    add x5, x5, x6",
        "    str x5, [x4]",
        "    msr nzcv, x7",
        *next_dispatch(),
    ])
    return body


def ldm_preflight_body(pre: bool, up: bool, rn: int) -> list[str]:
    """Prove one aligned DREAD block before an ordinary LDM commits.

    x7 retains guest NZCV, w9 the optional writeback value and x17 the
    advancing host pointer across destination handlers. No architectural
    register changes until the complete transfer has been proved cache-local.
    """
    body = [
        "    mrs x7, nzcv",
        "    cbnz x3, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldur w5, [x13, #-12]",
        "    cbnz w5, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    cmp w5, #15",
        "    b.ls 1f",
        "    b .La64s_direct_miss",
        "1:",
    ]
    loads, base = read_guest_register(rn, 4)
    body.extend(loads)
    if up:
        body.append(
            f"    {'add w17, ' + base + ', #4' if pre else 'mov w17, ' + base}"
        )
        body.append(f"    add w9, {base}, w5, lsl #2")
    else:
        body.append(f"    sub w9, {base}, w5, lsl #2")
        body.append("    mov w17, w9" if pre else "    add w17, w9, #4")
    body.extend([
        # The signed contract is deliberately narrower than legacy ARMv6
        # align-down behavior. arm_step retains every unaligned transfer.
        "    tst w17, #3",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        # One DREAD entry proves every word and rejects both cache-block and
        # 32-bit address-space wrapping before any destination changes.
        "    and w4, w17, #0x3ff",
        "    add w4, w4, w5, lsl #2",
        "    cmp w4, #1024",
        "    b.ls 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldr x6, [x3, #0]",
        "    cbnz x6, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldr w4, [x3, #20]",
        "    lsr w5, w17, #10",
        "    add w5, w5, w4, lsl #5",
        "    and w5, w5, #63",
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
        "    add x17, x16, w4, uxtw",
        *next_dispatch(),
    ])
    return body


def ldm_commit_body(rd: int) -> list[str]:
    if rd in PINNED:
        body = [f"    ldr w{PINNED[rd]}, [x17], #4"]
    else:
        body = [
            "    ldr w4, [x17], #4",
            *write_guest_register(rd, "w4"),
        ]
    body.extend(next_dispatch())
    return body


def ldm_finish_body(rn: int | None) -> list[str]:
    body: list[str] = []
    if rn is not None:
        body.extend(write_guest_register(rn, "w9"))
    body.extend([
        # Match one dread_hit() for each architectural read32 call.
        "    ldur w6, [x13, #-12]",
        "    ldr x4, [x3, #8]",
        "    ldr x5, [x4]",
        "    add x5, x5, x6",
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
        elif kind == "live_arith":
            body.extend([
                "    ldr x4, [x3, #40]",
                "    ldr w4, [x4]",
                # Accept exactly the live RunFast control mode: RN, FZ, DN,
                # scalar Len and all exception enables clear.
                "    mov w5, #0x9f00",
                "    movk w5, #0x3c7, lsl #16",
                "    and w6, w4, w5",
                "    mov w5, #3",
                "    lsl w5, w5, #24",
                "    cmp w6, w5",
                "    b.eq 1f",
                "    b .La64s_direct_miss",
                "1:",
                # Host IXC can be ignored only because it is already sticky in
                # the guest. Refuse every other pre-existing cumulative flag.
                "    mov w5, #0x9f",
                "    and w4, w4, w5",
                "    cmp w4, #0x10",
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


def vfp_narrow_body() -> list[str]:
    """Round binary64 to binary32 inside the audited RunFast contract."""
    p = ".La64s_vfp_narrow_64"
    return [
        *vfp_gate("live_arith"),
        "    ldur w9, [x13, #-12]",
        "    and w12, w9, #0xff",
        "    ubfx w10, w9, #8, #8",
        "    ldr x6, [x3, #24]",
        "    add x16, x6, w10, uxtw #2",
        "    ldr x5, [x16]",
        *vfp_simple_classify(
            8, "x5", "x9", f"{p}_input", ".La64s_direct_miss"
        ),
        "    fmov d1, x5",
        "    bl .La64s_fp_session_begin",
        "    msr fpsr, xzr",
        "    fcvt s0, d1",
        "    fmov w5, s0",
        *vfp_simple_classify(
            4, "w5", "w6", f"{p}_result", ".La64s_direct_miss"
        ),
        *vfp_arithmetic_flags(
            4, "w5", f"{p}_result", ".La64s_direct_miss"
        ),
        "    ldr x6, [x3, #24]",
        "    add x6, x6, w12, uxtw #2",
        "    str w5, [x6]",
        *vfp_finish(),
    ]


def vfp_simple_classify(width: int, value: str, scratch: str,
                        prefix: str, failure: str) -> list[str]:
    """Accept only a signed zero or a finite normal IEEE encoding."""
    exp_lsb = 23 if width == 4 else 52
    exp_bits = 8 if width == 4 else 11
    exp_all = "#0xff" if width == 4 else "#0x7ff"
    return [
        f"    ubfx {scratch}, {value}, #{exp_lsb}, #{exp_bits}",
        f"    cbz {scratch}, {prefix}_zero",
        f"    cmp {scratch}, {exp_all}",
        f"    b.ne {prefix}_ok",
        f"    b {failure}",
        f"{prefix}_zero:",
        f"    lsl {scratch}, {value}, #1",
        f"    cbz {scratch}, {prefix}_ok",
        f"    b {failure}",
        f"{prefix}_ok:",
    ]


def vfp_arithmetic_flags(width: int, value: str, prefix: str,
                         failure: str) -> list[str]:
    """Reject exceptions the sticky-IXC contract cannot make invisible."""
    body = [
        "    mrs x17, fpsr",
        "    mov w9, #0x9f",
        "    and w9, w17, w9",
        "    cmp w9, #0x10",
        f"    b.eq {prefix}_flags_ok",
        f"    cbz w9, {prefix}_flags_ok",
        f"    b {failure}",
        f"{prefix}_flags_ok:",
    ]
    if width == 4:
        body.extend([
            f"    and w17, {value}, #0x7fffffff",
            "    mov w10, #0x800000",
        ])
    else:
        body.extend([
            f"    and x17, {value}, #0x7fffffffffffffff",
            "    mov x10, #1",
            "    lsl x10, x10, #52",
        ])
    body.extend([
        f"    cmp {'w17' if width == 4 else 'x17'}, "
        f"{'w10' if width == 4 else 'x10'}",
        f"    b.ne {prefix}_boundary_ok",
        f"    tbz w9, #4, {prefix}_boundary_ok",
        # In guest FZ mode an inexact value rounded to the smallest normal is
        # the interpreter's explicit pre-rounding-boundary ambiguity.
        f"    b {failure}",
        f"{prefix}_boundary_ok:",
    ])
    return body


def vfp_arithmetic_body(operation: str, width: int) -> list[str]:
    """Execute the traced scalar VFPv2 arithmetic contract exactly.

    VMLA is deliberately two host instructions. Its rounded product is
    classified and its flags are sampled before a separately-rounded add, so
    neither AArch64 FMA contraction nor cumulative FPSR state can hide the
    guest's FZ boundary refusal.
    """
    if operation not in (
        "vmla", "vmls", "vnmls", "vnmla", "vmul", "vnmul",
        "vadd", "vsub", "vdiv",
    ) or width not in (4, 8):
        raise ValueError("invalid VFP arithmetic handler shape")

    bits = width * 8
    ireg = "w16" if width == 4 else "x16"
    iscratch = "w17" if width == 4 else "x17"
    fp = "s" if width == 4 else "d"
    p = f".La64s_vfp_{operation}_{bits}"
    pre_failure = ".La64s_direct_miss"
    state_failure = f"{p}_state_fail"
    mla = operation in ("vmla", "vmls", "vnmls", "vnmla")

    body = [
        *vfp_gate("live_arith"),
        "    ldur w9, [x13, #-12]",
        "    and w12, w9, #0xff",
        "    ubfx w10, w9, #8, #8",
        "    ubfx w9, w9, #16, #8",
        "    ldr x6, [x3, #24]",
    ]

    def load_operand(index: str, fp_reg: int, name: str) -> list[str]:
        if width == 4:
            result = [f"    ldr {ireg}, [x6, {index}, uxtw #2]"]
        else:
            result = [
                f"    add x16, x6, {index}, uxtw #2",
                "    ldr x16, [x16]",
            ]
        result.append(f"    fmov {fp}{fp_reg}, {ireg}")
        result.extend(vfp_simple_classify(
            width, ireg, iscratch, f"{p}_{name}", pre_failure
        ))
        return result

    body.extend(load_operand("w10", 1, "n"))
    body.extend(load_operand("w9", 2, "m"))
    if mla:
        body.extend(load_operand("w12", 0, "d"))

    body.extend([
        # Product execution normally owns one lazy host-FP session. The
        # context switch at +52 also exposes an exact same-binary control:
        # false uses the former inline per-operation save/restore sequence,
        # while true saves once and reuses the state until an exit or C call.
        # FPSR remains cleared and sampled per guest instruction in both arms.
        "    ldr w17, [x3, #52]",
        f"    cbz w17, {p}_unbatched_begin",
        "    bl .La64s_fp_session_begin",
        f"    b {p}_fp_ready",
        f"{p}_unbatched_begin:",
        "    mrs x4, fpcr",
        "    mrs x5, fpsr",
        f"    cbz x4, {p}_unbatched_fpcr_ready",
        "    msr fpcr, xzr",
        f"{p}_unbatched_fpcr_ready:",
        f"{p}_fp_ready:",
        "    msr fpsr, xzr",
    ])

    if mla:
        body.append(f"    fmul {fp}3, {fp}1, {fp}2")
        body.append(f"    fmov {ireg}, {fp}3")
        body.extend(vfp_simple_classify(
            width, ireg, iscratch, f"{p}_product", state_failure
        ))
        body.extend(vfp_arithmetic_flags(
            width, ireg, f"{p}_product", state_failure
        ))
        body.append("    msr fpsr, xzr")
        if operation in ("vmls", "vnmla"):
            body.append(f"    fneg {fp}3, {fp}3")
        if operation in ("vnmla", "vnmls"):
            body.append(f"    fneg {fp}0, {fp}0")
        body.append(f"    fadd {fp}0, {fp}0, {fp}3")
    elif operation in ("vmul", "vnmul"):
        body.append(f"    fmul {fp}0, {fp}1, {fp}2")
        if operation == "vnmul":
            body.append(f"    fneg {fp}0, {fp}0")
    elif operation == "vadd":
        body.append(f"    fadd {fp}0, {fp}1, {fp}2")
    elif operation == "vsub":
        body.append(f"    fsub {fp}0, {fp}1, {fp}2")
    else:
        body.append(f"    fdiv {fp}0, {fp}1, {fp}2")

    body.extend([
        f"    fmov {ireg}, {fp}0",
        *vfp_simple_classify(
            width, ireg, iscratch, f"{p}_result", state_failure
        ),
        *vfp_arithmetic_flags(
            width, ireg, f"{p}_result", state_failure
        ),
        # The control arm restores exactly where the pre-session handler did.
        # The product arm deliberately carries the session into an adjacent
        # arithmetic handler; the common exit still restores caller state.
        "    ldr w17, [x3, #52]",
        f"    cbnz w17, {p}_state_ready",
        "    msr fpsr, x5",
        f"    cbz x4, {p}_state_ready",
        "    msr fpcr, x4",
        f"{p}_state_ready:",
    ])
    if width == 4:
        body.append("    str w16, [x6, w12, uxtw #2]")
    else:
        body.extend([
            "    add x6, x6, w12, uxtw #2",
            "    str x16, [x6]",
        ])
    body.extend([
        *vfp_finish(),
        f"{state_failure}:",
        # A batched failure defers restoration to the common direct-miss
        # boundary. The unbatched control owns its inline saved x4/x5 values.
        "    ldr w17, [x3, #52]",
        f"    cbnz w17, {p}_failure_ready",
        "    msr fpsr, x5",
        f"    cbz x4, {p}_failure_ready",
        "    msr fpcr, x4",
        f"{p}_failure_ready:",
        "    b .La64s_direct_miss",
    ])
    return body


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


def vfp_direct_write_body(width: int) -> list[str]:
    """Store one VFP register through already-proved DWRITE mappings.

    VSTR D is two architectural write32 calls. Validate both translated words
    before touching RAM so a miss can return the whole instruction to the
    literal path without partially committing it. This also covers the legal
    1 KiB-boundary case, where the two words occupy distinct cache entries.
    """
    body = [
        *vfp_gate("enabled"),
        # The interpreter owns SCTLR.A/U and legacy align-down behavior.
        "    tst w17, #3",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        # The write table exists only while the frontend explicitly consents
        # to bypassing write observers.
        "    ldr x6, [x3, #56]",
        "    cbnz x6, 1f",
        "    b .La64s_direct_miss",
        "1:",
        # Validate the first word and retain its exact host address in x16.
        "    ldr w4, [x3, #20]",
        "    lsr w5, w17, #10",
        "    add w5, w5, w4, lsl #5",
        "    and w5, w5, #63",
        "    add x9, x6, w5, uxtw #4",
        "    ldr x16, [x9, #0]",
        "    cbnz x16, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    lsr w4, w17, #10",
        "    lsl w4, w4, #10",
        "    ldr w5, [x3, #20]",
        "    orr w4, w4, w5",
        "    ldr w5, [x9, #8]",
        "    cmp w5, w4",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldr w4, [x9, #12]",
        "    ldr w5, [x3, #16]",
        "    cmp w4, w5",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    and w4, w17, #0x3ff",
        "    add x16, x16, w4, uxtw",
    ]
    if width == 8:
        body.extend([
            # Preserve the second wrapped 32-bit VA in w10 until its host
            # pointer is final. Adjacent blocks use adjacent cache slots; the
            # 0xffffffff -> 0 wrap follows the interpreter's uint32_t address.
            "    add w10, w17, #4",
            "    ldr w4, [x3, #20]",
            "    lsr w5, w10, #10",
            "    add w5, w5, w4, lsl #5",
            "    and w5, w5, #63",
            "    add x9, x6, w5, uxtw #4",
            "    ldr x12, [x9, #0]",
            "    cbnz x12, 1f",
            "    b .La64s_direct_miss",
            "1:",
            "    lsr w4, w10, #10",
            "    lsl w4, w4, #10",
            "    ldr w5, [x3, #20]",
            "    orr w4, w4, w5",
            "    ldr w5, [x9, #8]",
            "    cmp w5, w4",
            "    b.eq 1f",
            "    b .La64s_direct_miss",
            "1:",
            "    ldr w4, [x9, #12]",
            "    ldr w5, [x3, #16]",
            "    cmp w4, w5",
            "    b.eq 1f",
            "    b .La64s_direct_miss",
            "1:",
            "    and w4, w10, #0x3ff",
            "    add x10, x12, w4, uxtw",
        ])
    body.extend([
        "    ldur w9, [x13, #-12]",
        "    ldr x4, [x3, #24]",
        "    add x4, x4, w9, uxtw #2",
    ])
    if width == 4:
        body.extend([
            "    ldr w5, [x4]",
            "    str w5, [x16]",
        ])
    else:
        body.extend([
            "    ldr x5, [x4]",
            "    str w5, [x16]",
            "    lsr x5, x5, #32",
            "    str w5, [x10]",
        ])
    body.extend([
        # Match the interpreter's one dwrite_hit() per write32 call.
        "    ldr x4, [x3, #64]",
        "    ldr x5, [x4]",
        f"    add x5, x5, #{width // 4}",
        "    str x5, [x4]",
        *vfp_finish(),
    ])
    return body


def vstm_direct_write_body(mode: int, rn: int) -> list[str]:
    """Commit one architectural VSTM through one proved DWRITE block.

    mode 0 is increment-after without writeback, mode 1 is increment-after
    with writeback, and mode 2 is decrement-before with writeback. The record
    immediate packs the first S-word index in bits 0..5 and the transfer word
    count in bits 8..13. VSTM D is the same contiguous word stream as VSTM S;
    the C decoder rejects deprecated odd-count FSTMX before selecting this
    handler.
    """
    if mode not in (0, 1, 2) or rn not in range(15):
        raise ValueError("invalid VSTM handler shape")

    body = [
        *vfp_gate("enabled"),
        "    ldur w12, [x13, #-12]",
        "    and w4, w12, #0x3f",
        "    ubfx w5, w12, #8, #6",
        # Refuse malformed data records even on the decoded-cache fast path.
        "    orr w6, w4, w5, lsl #8",
        "    cmp w6, w12",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    cbnz w5, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    cmp w5, #32",
        "    b.ls 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    add w6, w4, w5",
        "    cmp w6, #32",
        "    b.ls 1f",
        "    b .La64s_direct_miss",
        "1:",
    ]
    loads, base = read_guest_register(rn, 4)
    body.extend(loads)
    if mode == 0:
        body.append(f"    mov w17, {base}")
    elif mode == 1:
        body.extend([
            f"    mov w17, {base}",
            f"    add w9, {base}, w5, lsl #2",
        ])
    else:
        body.extend([
            f"    sub w17, {base}, w5, lsl #2",
            "    mov w9, w17",
        ])

    body.extend([
        # arm_step owns SCTLR.A/U and every legacy unaligned behavior.
        "    tst w17, #3",
        "    b.eq 1f",
        "    b .La64s_direct_miss",
        "1:",
        # Prove the complete transfer stays in one 1 KiB cache block before
        # writing its first word. This also rejects a top-of-VA wrap.
        "    and w4, w17, #0x3ff",
        "    add w4, w4, w5, lsl #2",
        "    cmp w4, #1024",
        "    b.ls 1f",
        "    b .La64s_direct_miss",
        "1:",
        # DWRITE exists only while the frontend separately consents to direct
        # callback-free RAM writes.
        "    ldr x6, [x3, #56]",
        "    cbnz x6, 1f",
        "    b .La64s_direct_miss",
        "1:",
        "    ldr w4, [x3, #20]",
        "    lsr w5, w17, #10",
        "    add w5, w5, w4, lsl #5",
        "    and w5, w5, #63",
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
        "    add x17, x16, w4, uxtw",
        # Both S and D lists are contiguous 32-bit words in vfp_s[].
        "    and w4, w12, #0x3f",
        "    ubfx w6, w12, #8, #6",
        "    ldr x10, [x3, #24]",
        "    add x10, x10, w4, uxtw #2",
        "2:",
        "    ldr w4, [x10], #4",
        "    str w4, [x17], #4",
        "    subs w6, w6, #1",
        "    b.ne 2b",
    ])
    if mode != 0:
        body.extend(write_guest_register(rn, "w9"))
    body.extend([
        # Match one interpreter dwrite_hit() for every architectural write32.
        "    ubfx w6, w12, #8, #6",
        "    ldr x4, [x3, #64]",
        "    ldr x5, [x4]",
        "    add x5, x5, x6",
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

    # Post-indexed stores transfer through the original base in w17 and defer
    # the updated base in w9 until the guarded write has actually succeeded.
    for up in (False, True):
        for rn in range(16):
            label = f".La64s_post_addr_imm_{int(up)}_{rn}"
            handlers.append((label, post_address_body(up, rn, False)))

    for up in (False, True):
        for rn in range(16):
            label = f".La64s_post_addr_reg_{int(up)}_{rn}"
            handlers.append((label, post_address_body(up, rn, True)))

    # Loads to PC remain literal. Five result widths/sign modes across r0-r14
    # need seventy-five direct handlers because the address record has already
    # produced the exact VA.
    for kind, mnemonic, width in READ_KINDS:
        for rd in range(15):
            label = f".La64s_direct_read_{kind}_{rd}"
            handlers.append((label,
                             direct_read_body(mnemonic, width, rd)))

    # Store sources include R15 because A32 STR pc has a precise PC+12 value;
    # byte-to-PC encodings are rejected by the decoder. Writeback bases exclude
    # R15 exactly as the ARMv6 interpreter does. Translation stores have their
    # own family so privilege is forced to user for both slot and tag checks.
    for kind, mnemonic, width in WRITE_KINDS:
        for rd in range(16):
            label = f".La64s_direct_write_{kind}_{rd}"
            handlers.append((label,
                             direct_write_body(mnemonic, width, rd,
                                               None, False)))

    for kind, mnemonic, width in WRITE_KINDS:
        for rd in range(16):
            for rn in range(15):
                label = f".La64s_direct_write_wb_{kind}_{rd}_{rn}"
                handlers.append((label,
                                 direct_write_body(mnemonic, width, rd,
                                                   rn, False)))

    for kind, mnemonic, width in WRITE_KINDS:
        for rd in range(16):
            for rn in range(15):
                label = (
                    f".La64s_direct_write_wb_unpriv_{kind}_{rd}_{rn}"
                )
                handlers.append((label,
                                 direct_write_body(mnemonic, width, rd,
                                                   rn, True)))

    # Ordinary A32 STM separates its all-or-nothing one-block proof from the
    # ordered source commits. Addressing mode and base select sixty preflights;
    # source and optional writeback registers add thirty-two compact handlers.
    for pre in (False, True):
        for up in (False, True):
            for rn in range(15):
                label = f".La64s_stm_preflight_{int(pre)}_{int(up)}_{rn}"
                handlers.append((label, stm_preflight_body(pre, up, rn)))
    for rd in range(16):
        handlers.append((f".La64s_stm_commit_{rd}",
                         stm_commit_body(rd)))
    handlers.append((".La64s_stm_finish", stm_finish_body(None)))
    for rn in range(15):
        handlers.append((f".La64s_stm_finish_wb_{rn}",
                         stm_finish_body(rn)))

    # Ordinary no-PC A32 LDM uses the same four address modes but reads from a
    # completely proved DREAD block before committing up to fifteen ascending
    # destination registers. Unlike stores, a successful LDM may be followed
    # by more instructions in the same signed head.
    for pre in (False, True):
        for up in (False, True):
            for rn in range(15):
                label = f".La64s_ldm_preflight_{int(pre)}_{int(up)}_{rn}"
                handlers.append((label, ldm_preflight_body(pre, up, rn)))
    for rd in range(15):
        handlers.append((f".La64s_ldm_commit_{rd}",
                         ldm_commit_body(rd)))
    handlers.append((".La64s_ldm_finish", ldm_finish_body(None)))
    for rn in range(15):
        handlers.append((f".La64s_ldm_finish_wb_{rn}",
                         ldm_finish_body(rn)))

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
    handlers.append((".La64s_vfp_narrow_64", vfp_narrow_body()))
    for operation in (
        "vmla", "vmls", "vnmls", "vnmla", "vmul", "vnmul",
        "vadd", "vsub", "vdiv",
    ):
        handlers.append((f".La64s_vfp_{operation}_32",
                         vfp_arithmetic_body(operation, 4)))
    for operation in (
        "vmla", "vmls", "vnmls", "vnmla", "vmul", "vnmul",
        "vadd", "vsub", "vdiv",
    ):
        handlers.append((f".La64s_vfp_{operation}_64",
                         vfp_arithmetic_body(operation, 8)))
    handlers.append((".La64s_vfp_direct_read_32",
                     vfp_direct_read_body(4)))
    handlers.append((".La64s_vfp_direct_read_64",
                     vfp_direct_read_body(8)))
    handlers.append((".La64s_vfp_direct_write_32",
                     vfp_direct_write_body(4)))
    handlers.append((".La64s_vfp_direct_write_64",
                      vfp_direct_write_body(8)))
    # VSTM has three architectural address/writeback forms and fifteen legal
    # base registers. One handler loops over the already-bounded contiguous
    # VFP word slice after an all-or-nothing one-block DWRITE proof.
    for mode in range(3):
        for rn in range(15):
            handlers.append((f".La64s_vstm_direct_write_{mode}_{rn}",
                             vstm_direct_write_body(mode, rn)))

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

    # Register-indirect branches are terminal, runtime-guarded exits. ARM
    # conditions reuse the ordinary guard record, so only the register, link
    # and source-state dimensions need distinct signed text here.
    for rm in range(16):
        handlers.append((f".La64s_arm_bx_{rm}",
                         indirect_branch_body(False, False, rm)))
    for rm in range(15):
        handlers.append((f".La64s_arm_blx_{rm}",
                         indirect_branch_body(True, False, rm)))
    for rm in range(16):
        handlers.append((f".La64s_thumb_bx_{rm}",
                         indirect_branch_body(False, True, rm)))
    for rm in range(15):
        handlers.append((f".La64s_thumb_blx_{rm}",
                         indirect_branch_body(True, True, rm)))

    if len(handlers) != EXPECTED_HANDLERS:
        raise RuntimeError(
            f"generated {len(handlers)} handlers, expected {EXPECTED_HANDLERS}"
        )
    return handlers


def compact_vfp_compare_body(width: int) -> list[str]:
    """Return exact integer-only VCMP/VCMPE semantics for the live loop."""
    if width not in (4, 8):
        raise ValueError("invalid compact VFP compare width")
    bits = width * 8
    r = "w" if width == 4 else "x"
    zero = "wzr" if width == 4 else "xzr"
    exp_lsb = 23 if width == 4 else 52
    exp_bits = 8 if width == 4 else 11
    exp_all = "#0xff" if width == 4 else "#0x7ff"
    frac_shift = 9 if width == 4 else 12
    quiet_bit = 22 if width == 4 else 51
    sign_mask = "#0x80000000" if width == 4 else "#0x8000000000000000"
    p = f".La64cr_vfp_compare_{bits}"
    body = [
        f"{p}:",
        # VCM(P/E) #0 requires the encoded Vm/M fields to be zero.
        "    ubfx w4, w9, #16, #4",
        "    cmp w4, #5",
        f"    b.ne {p}_register",
        "    and w4, w9, #0xf",
        "    cbnz w4, .La64cr_fallback",
        "    tbnz w9, #5, .La64cr_fallback",
        "    mov w4, #1",
        f"    b {p}_shape",
        f"{p}_register:",
        "    mov w4, wzr",
        f"{p}_shape:",
    ]
    if width == 8:
        body.extend([
            "    tbnz w9, #22, .La64cr_fallback",
            "    tbnz w9, #5, .La64cr_fallback",
            "    ubfx w2, w9, #12, #4",
            "    lsl w2, w2, #1",
            "    and w3, w9, #0xf",
            "    lsl w3, w3, #1",
        ])
    else:
        body.extend([
            "    ubfx w2, w9, #12, #4",
            "    ubfx w3, w9, #22, #1",
            "    orr w2, w3, w2, lsl #1",
            "    and w3, w9, #0xf",
            "    ubfx w10, w9, #5, #1",
            "    orr w3, w10, w3, lsl #1",
        ])
    body.extend([
        "    ubfx w5, w9, #7, #1",
        "    bl .La64cr_vfp_guard_values",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #104]",
        "    add x10, x0, w2, uxtw #2",
        f"    ldr {r}6, [x10]",
        f"    cbnz w4, {p}_zero",
        "    add x10, x0, w3, uxtw #2",
        f"    ldr {r}7, [x10]",
        f"    b {p}_operands",
        f"{p}_zero:",
        f"    mov {r}7, {zero}",
        f"{p}_operands:",
        "    ldr x1, [x27, #120]",
        "    ldr w8, [x1]",
        f"    tbz w8, #24, {p}_classify",
        # FZ consumes an input denormal as a same-sign zero and sets IDC.
        f"    ubfx {r}10, {r}6, #{exp_lsb}, #{exp_bits}",
        f"    cbnz {r}10, {p}_fz_b",
        f"    lsl {r}10, {r}6, #{frac_shift}",
        f"    cbz {r}10, {p}_fz_b",
        f"    and {r}6, {r}6, {sign_mask}",
        "    orr w8, w8, #0x80",
        f"{p}_fz_b:",
        f"    ubfx {r}10, {r}7, #{exp_lsb}, #{exp_bits}",
        f"    cbnz {r}10, {p}_classify",
        f"    lsl {r}10, {r}7, #{frac_shift}",
        f"    cbz {r}10, {p}_classify",
        f"    and {r}7, {r}7, {sign_mask}",
        "    orr w8, w8, #0x80",
        f"{p}_classify:",
        "    mov w11, wzr",
        "    mov w12, wzr",
        f"    ubfx {r}10, {r}6, #{exp_lsb}, #{exp_bits}",
        f"    cmp {r}10, {exp_all}",
        f"    b.ne {p}_class_b",
        f"    lsl {r}10, {r}6, #{frac_shift}",
        f"    cbz {r}10, {p}_class_b",
        "    mov w11, #1",
        f"    tbnz {r}6, #{quiet_bit}, {p}_class_b",
        "    mov w12, #1",
        f"{p}_class_b:",
        f"    ubfx {r}10, {r}7, #{exp_lsb}, #{exp_bits}",
        f"    cmp {r}10, {exp_all}",
        f"    b.ne {p}_classified",
        f"    lsl {r}10, {r}7, #{frac_shift}",
        f"    cbz {r}10, {p}_classified",
        "    mov w11, #1",
        f"    tbnz {r}7, #{quiet_bit}, {p}_classified",
        "    mov w12, #1",
        f"{p}_classified:",
        f"    cbz w11, {p}_ordered",
        "    mov w13, #0x30000000",
        f"    cbnz w5, {p}_invalid",
        f"    cbz w12, {p}_write",
        f"{p}_invalid:",
        "    orr w8, w8, #1",
        f"    b {p}_write",
        f"{p}_ordered:",
        f"    cmp {r}6, {r}7",
        f"    b.eq {p}_equal",
        f"    lsl {r}10, {r}6, #1",
        f"    cbnz {r}10, {p}_key",
        f"    lsl {r}10, {r}7, #1",
        f"    cbz {r}10, {p}_equal",
        f"{p}_key:",
        f"    asr {r}10, {r}6, #{bits - 1}",
        f"    orr {r}10, {r}10, {sign_mask}",
        f"    eor {r}6, {r}6, {r}10",
        f"    asr {r}10, {r}7, #{bits - 1}",
        f"    orr {r}10, {r}10, {sign_mask}",
        f"    eor {r}7, {r}7, {r}10",
        f"    cmp {r}6, {r}7",
        f"    b.lo {p}_less",
        "    mov w13, #0x20000000",
        f"    b {p}_write",
        f"{p}_less:",
        "    mov w13, #0x80000000",
        f"    b {p}_write",
        f"{p}_equal:",
        "    mov w13, #0x60000000",
        f"{p}_write:",
        "    bic w8, w8, #0xf0000000",
        "    orr w8, w8, w13",
        "    str w8, [x1]",
        "    b .La64cr_vfp_done",
    ])
    return body


def compact_vfp_arithmetic_flags(width: int, value: str, prefix: str,
                                 failure: str) -> list[str]:
    """Reject host exceptions that sticky guest IXC cannot hide."""
    body = [
        "    mrs x7, fpsr",
        "    mov w8, #0x9f",
        "    and w8, w7, w8",
        "    cmp w8, #0x10",
        f"    b.eq {prefix}_flags_ok",
        f"    cbz w8, {prefix}_flags_ok",
        f"    b {failure}",
        f"{prefix}_flags_ok:",
    ]
    if width == 4:
        body.extend([
            f"    and w7, {value}, #0x7fffffff",
            "    mov w10, #0x800000",
        ])
    else:
        body.extend([
            f"    and x7, {value}, #0x7fffffffffffffff",
            "    mov x10, #1",
            "    lsl x10, x10, #52",
        ])
    body.extend([
        f"    cmp {'w7' if width == 4 else 'x7'}, "
        f"{'w10' if width == 4 else 'x10'}",
        f"    b.ne {prefix}_boundary_ok",
        f"    tbz w8, #4, {prefix}_boundary_ok",
        # The interpreter explicitly refuses this pre-rounding ambiguity in
        # guest FZ mode: an inexact result at the smallest normal encoding.
        f"    b {failure}",
        f"{prefix}_boundary_ok:",
    ])
    return body


def compact_vfp_arithmetic_body(width: int) -> list[str]:
    """Return exact live scalar VFPv2 arithmetic for one guest width."""
    if width not in (4, 8):
        raise ValueError("invalid compact VFP arithmetic width")

    bits = width * 8
    integer = "w6" if width == 4 else "x6"
    scratch = "w7" if width == 4 else "x7"
    fp = "s" if width == 4 else "d"
    p = f".La64cr_vfp_arith_{bits}"
    state_failure = f"{p}_state_fail"
    body = [
        f"{p}:",
        "    bl .La64cr_vfp_guard_arith",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #104]",
    ]

    def load_operand(index: str, fp_reg: int, name: str) -> list[str]:
        if width == 4:
            result = [f"    ldr {integer}, [x0, {index}, uxtw #2]"]
        else:
            result = [
                f"    add x1, x0, {index}, uxtw #2",
                f"    ldr {integer}, [x1]",
            ]
        result.extend([
            f"    fmov {fp}{fp_reg}, {integer}",
            *vfp_simple_classify(
                width, integer, scratch, f"{p}_{name}",
                ".La64cr_fallback"
            ),
        ])
        return result

    body.extend(load_operand("w10", 1, "n"))
    body.extend(load_operand("w11", 2, "m"))
    body.extend([
        # Operation IDs 0..3 are the four multiply-accumulate variants and
        # alone consume the old destination as a third input.
        "    cmp w14, #4",
        f"    b.hs {p}_operands_ready",
    ])
    body.extend(load_operand("w12", 0, "d"))
    body.extend([
        f"{p}_operands_ready:",
        "    bl .La64cr_fp_session_begin",
        # FPSR is cumulative architecturally, but this contract samples each
        # guest instruction independently while guest IXC is already sticky.
        "    msr fpsr, xzr",
        "    cmp w14, #4",
        f"    b.lo {p}_accumulate",
        "    cmp w14, #4",
        f"    b.eq {p}_vmul",
        "    cmp w14, #5",
        f"    b.eq {p}_vnmul",
        "    cmp w14, #6",
        f"    b.eq {p}_vadd",
        "    cmp w14, #7",
        f"    b.eq {p}_vsub",
        f"    fdiv {fp}0, {fp}1, {fp}2",
        f"    b {p}_result",
        f"{p}_vmul:",
        f"    fmul {fp}0, {fp}1, {fp}2",
        f"    b {p}_result",
        f"{p}_vnmul:",
        f"    fmul {fp}0, {fp}1, {fp}2",
        f"    fneg {fp}0, {fp}0",
        f"    b {p}_result",
        f"{p}_vadd:",
        f"    fadd {fp}0, {fp}1, {fp}2",
        f"    b {p}_result",
        f"{p}_vsub:",
        f"    fsub {fp}0, {fp}1, {fp}2",
        f"    b {p}_result",
        f"{p}_accumulate:",
        # VFPv2 VMLA is two separately rounded operations, never an FMA.
        f"    fmul {fp}3, {fp}1, {fp}2",
        f"    fmov {integer}, {fp}3",
        *vfp_simple_classify(
            width, integer, scratch, f"{p}_product", state_failure
        ),
        *compact_vfp_arithmetic_flags(
            width, integer, f"{p}_product", state_failure
        ),
        "    msr fpsr, xzr",
        "    cmp w14, #1",
        f"    b.eq {p}_negate_product",
        "    cmp w14, #3",
        f"    b.ne {p}_product_sign_ready",
        f"{p}_negate_product:",
        f"    fneg {fp}3, {fp}3",
        f"{p}_product_sign_ready:",
        "    cmp w14, #2",
        f"    b.eq {p}_negate_destination",
        "    cmp w14, #3",
        f"    b.ne {p}_destination_sign_ready",
        f"{p}_negate_destination:",
        f"    fneg {fp}0, {fp}0",
        f"{p}_destination_sign_ready:",
        f"    fadd {fp}0, {fp}0, {fp}3",
        f"{p}_result:",
        f"    fmov {integer}, {fp}0",
        *vfp_simple_classify(
            width, integer, scratch, f"{p}_result", state_failure
        ),
        *compact_vfp_arithmetic_flags(
            width, integer, f"{p}_result", state_failure
        ),
        "    ldr x0, [x27, #104]",
    ])
    if width == 4:
        body.append("    str w6, [x0, w12, uxtw #2]")
    else:
        body.extend([
            "    add x0, x0, w12, uxtw #2",
            "    str x6, [x0]",
        ])
    body.extend([
        "    b .La64cr_vfp_done",
        f"{state_failure}:",
        # Guest state is still untouched. The common boundary restores the
        # caller's complete FP environment before stopping or entering C.
        "    b .La64cr_fallback",
    ])
    return body


def compact_vfp_narrow_body() -> list[str]:
    """Return transactional VCVT.F32.F64 for the audited live contract."""
    p = ".La64cr_vfp_narrow_64"
    return [
        f"{p}:",
        "    ubfx w2, w9, #12, #4",
        "    ubfx w3, w9, #22, #1",
        "    orr w2, w3, w2, lsl #1",
        "    and w3, w9, #0xf",
        "    lsl w3, w3, #1",
        "    bl .La64cr_vfp_guard_arith",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #104]",
        "    add x1, x0, w3, uxtw #2",
        "    ldr x6, [x1]",
        *vfp_simple_classify(
            8, "x6", "x7", f"{p}_input", ".La64cr_fallback"
        ),
        "    fmov d1, x6",
        "    bl .La64cr_fp_session_begin",
        "    msr fpsr, xzr",
        "    fcvt s0, d1",
        "    fmov w6, s0",
        *vfp_simple_classify(
            4, "w6", "w7", f"{p}_result", ".La64cr_fallback"
        ),
        *compact_vfp_arithmetic_flags(
            4, "w6", f"{p}_result", ".La64cr_fallback"
        ),
        "    ldr x0, [x27, #104]",
        "    str w6, [x0, w2, uxtw #2]",
        "    b .La64cr_vfp_done",
    ]


def compact_vfp_memory_body() -> list[str]:
    """Return transactional witnessed VLDR/VSTR/VSTM live semantics."""
    return [
        ".La64cr_vfp_memory_decode:",
        "    ubfx w10, w9, #24, #1",
        "    ubfx w11, w9, #21, #1",
        "    cmp w10, #1",
        "    b.ne .La64cr_vfp_vstm_decode",
        "    cbnz w11, .La64cr_vfp_vstm_decode",
        # One VSTR/VLDR. D16-D31 are absent on this VFPv2 target.
        "    ubfx w12, w9, #16, #4",
        "    cmp w12, #15",
        "    b.eq .La64cr_vfp_memory_pc_base",
        "    ldr w10, [x19, w12, uxtw #2]",
        "    b .La64cr_vfp_memory_base_ready",
        ".La64cr_vfp_memory_pc_base:",
        "    add w10, w26, #8",
        ".La64cr_vfp_memory_base_ready:",
        "    and w11, w9, #0xff",
        "    lsl w11, w11, #2",
        "    tbz w9, #23, .La64cr_vfp_memory_sub",
        "    add w10, w10, w11",
        "    b .La64cr_vfp_memory_address_ready",
        ".La64cr_vfp_memory_sub:",
        "    sub w10, w10, w11",
        ".La64cr_vfp_memory_address_ready:",
        "    tst w10, #3",
        "    b.ne .La64cr_fallback",
        "    ubfx w15, w9, #20, #1",
        "    tbnz w9, #8, .La64cr_vfp_memory_double",
        "    ubfx w13, w9, #12, #4",
        "    ubfx w12, w9, #22, #1",
        "    orr w13, w12, w13, lsl #1",
        "    mov w14, #1",
        "    b .La64cr_vfp_memory_guard",
        ".La64cr_vfp_memory_double:",
        "    tbnz w9, #22, .La64cr_fallback",
        "    ubfx w13, w9, #12, #4",
        "    lsl w13, w13, #1",
        "    mov w14, #2",
        ".La64cr_vfp_memory_guard:",
        "    bl .La64cr_vfp_guard_enabled",
        "    cbz w0, .La64cr_fallback",
        # Validate every required translation before any read, write, counter
        # or VFP-register mutation. This makes a second-word miss transactional.
        "    bl .La64cr_memory_lookup",
        "    cbz w2, .La64cr_fallback",
        "    mov x6, x0",
        "    mov x7, x1",
        "    cmp w14, #2",
        "    b.ne .La64cr_vfp_memory_commit",
        "    add w10, w10, #4",
        "    bl .La64cr_memory_lookup",
        "    cbz w2, .La64cr_fallback",
        "    mov x8, x0",
        "    mov x9, x1",
        ".La64cr_vfp_memory_commit:",
        "    ldr x0, [x27, #104]",
        "    add x0, x0, w13, uxtw #2",
        "    cbz w15, .La64cr_vfp_memory_store",
        "    ldr w2, [x6]",
        "    cmp w14, #2",
        "    b.ne .La64cr_vfp_memory_load_single",
        "    ldr w3, [x8]",
        "    stp w2, w3, [x0]",
        "    b .La64cr_vfp_memory_count",
        ".La64cr_vfp_memory_load_single:",
        "    str w2, [x0]",
        "    b .La64cr_vfp_memory_count",
        ".La64cr_vfp_memory_store:",
        "    ldr w2, [x0]",
        "    str w2, [x6]",
        "    cmp w14, #2",
        "    b.ne .La64cr_vfp_memory_count",
        "    ldr w3, [x0, #4]",
        "    str w3, [x8]",
        ".La64cr_vfp_memory_count:",
        "    cbz x7, .La64cr_vfp_memory_count_second",
        "    ldr x2, [x7]",
        "    add x2, x2, #1",
        "    str x2, [x7]",
        ".La64cr_vfp_memory_count_second:",
        "    cmp w14, #2",
        "    b.ne .La64cr_vfp_done",
        "    cbz x9, .La64cr_vfp_done",
        "    ldr x2, [x9]",
        "    add x2, x2, #1",
        "    str x2, [x9]",
        "    b .La64cr_vfp_done",
        "",
        ".La64cr_vfp_vstm_decode:",
        # All loads remain literal. Stores admit IA, IA! and DB! only.
        "    tbnz w9, #20, .La64cr_fallback",
        "    ubfx w12, w9, #16, #4",
        "    cmp w12, #15",
        "    b.eq .La64cr_fallback",
        "    and w14, w9, #0xff",
        "    cbz w14, .La64cr_fallback",
        "    ubfx w10, w9, #24, #1",
        "    ubfx w11, w9, #23, #1",
        "    ubfx w15, w9, #21, #1",
        "    cbnz w10, .La64cr_vfp_vstm_pre",
        "    cbz w11, .La64cr_fallback",
        "    mov w11, wzr",
        "    cbz w15, .La64cr_vfp_vstm_mode_ready",
        "    mov w11, #1",
        "    b .La64cr_vfp_vstm_mode_ready",
        ".La64cr_vfp_vstm_pre:",
        "    cbnz w11, .La64cr_fallback",
        "    cbz w15, .La64cr_fallback",
        "    mov w11, #2",
        ".La64cr_vfp_vstm_mode_ready:",
        "    tbnz w9, #8, .La64cr_vfp_vstm_double",
        "    ubfx w13, w9, #12, #4",
        "    ubfx w10, w9, #22, #1",
        "    orr w13, w10, w13, lsl #1",
        "    add w10, w13, w14",
        "    cmp w10, #32",
        "    b.hi .La64cr_fallback",
        "    b .La64cr_vfp_vstm_shape_ready",
        ".La64cr_vfp_vstm_double:",
        "    tbnz w9, #22, .La64cr_fallback",
        "    tbnz w14, #0, .La64cr_fallback",
        "    ubfx w13, w9, #12, #4",
        "    lsl w13, w13, #1",
        "    add w10, w13, w14",
        "    cmp w10, #32",
        "    b.hi .La64cr_fallback",
        ".La64cr_vfp_vstm_shape_ready:",
        "    ldr w10, [x19, w12, uxtw #2]",
        "    cmp w11, #2",
        "    b.ne .La64cr_vfp_vstm_start_ready",
        "    sub w10, w10, w14, lsl #2",
        ".La64cr_vfp_vstm_start_ready:",
        "    tst w10, #3",
        "    b.ne .La64cr_fallback",
        # The shipped exact VSTM contract proves the complete word stream in
        # one DWRITE block before committing its first store.
        "    and w6, w10, #0x3ff",
        "    add w6, w6, w14, lsl #2",
        "    cmp w6, #1024",
        "    b.hi .La64cr_fallback",
        # Flat RAM additionally must not wrap its host allocation mid-stream.
        "    ldr x6, [x27, #0]",
        "    cbz x6, .La64cr_vfp_vstm_lookup",
        "    ldr w6, [x27, #8]",
        "    and w7, w10, w6",
        "    add w7, w7, w14, lsl #2",
        "    add w6, w6, #1",
        "    cmp w7, w6",
        "    b.hi .La64cr_fallback",
        ".La64cr_vfp_vstm_lookup:",
        "    bl .La64cr_vfp_guard_enabled",
        "    cbz w0, .La64cr_fallback",
        "    mov w15, wzr",
        "    bl .La64cr_memory_lookup",
        "    cbz w2, .La64cr_fallback",
        "    mov x6, x0",
        "    mov x7, x1",
        "    ldr x0, [x27, #104]",
        "    add x0, x0, w13, uxtw #2",
        "    mov w2, w14",
        ".La64cr_vfp_vstm_copy:",
        "    ldr w3, [x0], #4",
        "    str w3, [x6], #4",
        "    subs w2, w2, #1",
        "    b.ne .La64cr_vfp_vstm_copy",
        "    cbz w11, .La64cr_vfp_vstm_count",
        "    cmp w11, #2",
        "    b.eq .La64cr_vfp_vstm_writeback_db",
        "    ldr w2, [x19, w12, uxtw #2]",
        "    add w2, w2, w14, lsl #2",
        "    str w2, [x19, w12, uxtw #2]",
        "    b .La64cr_vfp_vstm_count",
        ".La64cr_vfp_vstm_writeback_db:",
        "    str w10, [x19, w12, uxtw #2]",
        ".La64cr_vfp_vstm_count:",
        "    cbz x7, .La64cr_vfp_done",
        "    ldr x2, [x7]",
        "    add x2, x2, w14, uxtw",
        "    str x2, [x7]",
        "    b .La64cr_vfp_done",
        "",
        # Input w10 is a word VA and w15 selects DREAD (1) or DWRITE (0).
        # Return x0=host word, x1=architectural hit counter and w2=success.
        # Only x0-x5 are clobbered so callers can retain transactional state.
        ".La64cr_memory_lookup:",
        "    ldr x0, [x27, #0]",
        "    cbz x0, .La64cr_memory_lookup_cache",
        "    ldr w1, [x27, #8]",
        "    and w2, w10, w1",
        "    add x0, x0, w2, uxtw",
        "    mov x1, xzr",
        "    mov w2, #1",
        "    ret",
        ".La64cr_memory_lookup_cache:",
        "    tbz w15, #0, .La64cr_memory_lookup_write",
        "    ldr x0, [x27, #16]",
        "    ldr x1, [x27, #32]",
        "    b .La64cr_memory_lookup_common",
        ".La64cr_memory_lookup_write:",
        "    ldr x0, [x27, #24]",
        "    ldr x1, [x27, #40]",
        ".La64cr_memory_lookup_common:",
        "    cbz x0, .La64cr_memory_lookup_fail",
        "    cbz x1, .La64cr_memory_lookup_fail",
        "    ldr w5, [x27, #84]",
        "    tbz w15, #1, .La64cr_memory_lookup_priv_ready",
        "    mov w5, wzr",
        ".La64cr_memory_lookup_priv_ready:",
        "    lsr w3, w10, #10",
        "    add w3, w3, w5, lsl #5",
        "    and w3, w3, #63",
        "    add x0, x0, w3, uxtw #4",
        "    ldr x2, [x0, #0]",
        "    cbz x2, .La64cr_memory_lookup_fail",
        "    lsr w3, w10, #10",
        "    lsl w3, w3, #10",
        "    orr w3, w3, w5",
        "    ldr w4, [x0, #8]",
        "    cmp w4, w3",
        "    b.ne .La64cr_memory_lookup_fail",
        "    ldr w4, [x0, #12]",
        "    ldr w3, [x27, #80]",
        "    cmp w4, w3",
        "    b.ne .La64cr_memory_lookup_fail",
        "    and w3, w10, #0x3ff",
        "    add x0, x2, w3, uxtw",
        "    mov w2, #1",
        "    ret",
        ".La64cr_memory_lookup_fail:",
        "    mov w2, wzr",
        "    ret",
    ]


def compact_vfp_nonarith_body() -> list[str]:
    """Return broad exact VFPv2 register/compare/widen live semantics."""
    return [
        ".La64cr_vfp_decode:",
        # MCR/MRC core/system transfers.
        "    mov w10, #0x0e10",
        "    movk w10, #0x0f00, lsl #16",
        "    and w11, w9, w10",
        "    mov w12, #0x0a10",
        "    movk w12, #0x0e00, lsl #16",
        "    cmp w11, w12",
        "    b.eq .La64cr_vfp_mcr",
        # MCRR/MRRC two-core transfers.
        "    mov w10, #0x0e00",
        "    movk w10, #0x0fe0, lsl #16",
        "    and w11, w9, w10",
        "    mov w12, #0x0a00",
        "    movk w12, #0x0c40, lsl #16",
        "    cmp w11, w12",
        "    b.eq .La64cr_vfp_mcrr",
        # LDC/STC scalar and multiple memory forms.
        "    mov w10, #0x0e00",
        "    movk w10, #0x0e00, lsl #16",
        "    and w11, w9, w10",
        "    mov w12, #0x0a00",
        "    movk w12, #0x0c00, lsl #16",
        "    cmp w11, w12",
        "    b.eq .La64cr_vfp_memory_decode",
        # CDP scalar arithmetic/other group. Arithmetic deliberately remains
        # on the literal path until the host-FP session tranche lands.
        "    mov w10, #0x0e10",
        "    movk w10, #0x0f00, lsl #16",
        "    and w11, w9, w10",
        "    mov w12, #0x0a00",
        "    movk w12, #0x0e00, lsl #16",
        "    cmp w11, w12",
        "    b.eq .La64cr_vfp_cdp",
        "    b .La64cr_fallback",
        "",
        ".La64cr_vfp_mcr:",
        "    ubfx w10, w9, #21, #3",
        "    ubfx w11, w9, #8, #1",
        "    ubfx w12, w9, #16, #4",
        "    ubfx w13, w9, #12, #4",
        "    cbnz w11, .La64cr_vfp_mcr_cp11",
        "    cbnz w10, .La64cr_vfp_mcr_system",
        "    mov w14, #0x6f",
        "    tst w9, w14",
        "    b.ne .La64cr_fallback",
        "    cmp w13, #15",
        "    b.eq .La64cr_fallback",
        "    lsl w12, w12, #1",
        "    ubfx w14, w9, #7, #1",
        "    add w12, w12, w14",
        "    b .La64cr_vfp_core_word",
        ".La64cr_vfp_mcr_cp11:",
        "    cmp w10, #1",
        "    b.hi .La64cr_fallback",
        "    tbnz w9, #7, .La64cr_fallback",
        "    mov w14, #0x6f",
        "    tst w9, w14",
        "    b.ne .La64cr_fallback",
        "    cmp w13, #15",
        "    b.eq .La64cr_fallback",
        "    lsl w12, w12, #1",
        "    add w12, w12, w10",
        ".La64cr_vfp_core_word:",
        "    bl .La64cr_vfp_guard_enabled",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #104]",
        "    add x0, x0, w12, uxtw #2",
        "    tbnz w9, #20, .La64cr_vfp_core_word_load",
        "    ldr w1, [x19, w13, uxtw #2]",
        "    str w1, [x0]",
        "    b .La64cr_vfp_done",
        ".La64cr_vfp_core_word_load:",
        "    ldr w1, [x0]",
        "    str w1, [x19, w13, uxtw #2]",
        "    b .La64cr_vfp_done",
        "",
        ".La64cr_vfp_mcr_system:",
        "    cmp w10, #7",
        "    b.ne .La64cr_fallback",
        "    mov w14, #0xef",
        "    tst w9, w14",
        "    b.ne .La64cr_fallback",
        "    tbz w9, #20, .La64cr_vfp_system_write",
        "    cmp w12, #1",
        "    b.ne .La64cr_vfp_system_read_not_apsr",
        "    cmp w13, #15",
        "    b.eq .La64cr_vfp_vmrs_apsr",
        ".La64cr_vfp_system_read_not_apsr:",
        "    cmp w13, #15",
        "    b.eq .La64cr_fallback",
        "    cbz w12, .La64cr_vfp_vmrs_fpsid",
        "    cmp w12, #1",
        "    b.eq .La64cr_vfp_vmrs_fpscr",
        "    cmp w12, #8",
        "    b.eq .La64cr_vfp_vmrs_fpexc",
        "    b .La64cr_fallback",
        ".La64cr_vfp_vmrs_apsr:",
        "    bl .La64cr_vfp_guard_enabled",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #120]",
        "    ldr w1, [x0]",
        "    ldr w2, [x20]",
        "    ubfx w1, w1, #28, #4",
        "    bfi w2, w1, #28, #4",
        "    str w2, [x20]",
        "    b .La64cr_vfp_done",
        ".La64cr_vfp_vmrs_fpsid:",
        "    bl .La64cr_vfp_guard_fpsid",
        "    cbz w0, .La64cr_fallback",
        "    mov w1, #0x20b4",
        "    movk w1, #0x4101, lsl #16",
        "    str w1, [x19, w13, uxtw #2]",
        "    b .La64cr_vfp_done",
        ".La64cr_vfp_vmrs_fpscr:",
        "    bl .La64cr_vfp_guard_enabled",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #120]",
        "    ldr w1, [x0]",
        "    str w1, [x19, w13, uxtw #2]",
        "    b .La64cr_vfp_done",
        ".La64cr_vfp_vmrs_fpexc:",
        "    bl .La64cr_vfp_guard_priv",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #112]",
        "    ldr w1, [x0]",
        "    str w1, [x19, w13, uxtw #2]",
        "    b .La64cr_vfp_done",
        ".La64cr_vfp_system_write:",
        "    cmp w13, #15",
        "    b.eq .La64cr_fallback",
        "    cmp w12, #1",
        "    b.eq .La64cr_vfp_vmsr_fpscr",
        "    cmp w12, #8",
        "    b.eq .La64cr_vfp_vmsr_fpexc",
        "    b .La64cr_fallback",
        ".La64cr_vfp_vmsr_fpscr:",
        "    bl .La64cr_vfp_guard_enabled",
        "    cbz w0, .La64cr_fallback",
        "    ldr w1, [x19, w13, uxtw #2]",
        "    mov w2, #0x9f9f",
        "    movk w2, #0xf3f7, lsl #16",
        "    and w1, w1, w2",
        "    ldr x0, [x27, #120]",
        "    str w1, [x0]",
        "    b .La64cr_vfp_done",
        ".La64cr_vfp_vmsr_fpexc:",
        "    bl .La64cr_vfp_guard_priv",
        "    cbz w0, .La64cr_fallback",
        "    ldr w1, [x19, w13, uxtw #2]",
        "    ldr x0, [x27, #112]",
        "    str w1, [x0]",
        "    b .La64cr_vfp_done",
        "",
        ".La64cr_vfp_mcrr:",
        "    mov w10, #0xc0",
        "    tst w9, w10",
        "    b.ne .La64cr_fallback",
        "    ubfx w10, w9, #16, #4",
        "    ubfx w11, w9, #12, #4",
        "    cmp w10, #15",
        "    b.eq .La64cr_fallback",
        "    cmp w11, #15",
        "    b.eq .La64cr_fallback",
        "    and w12, w9, #0xf",
        "    lsl w12, w12, #1",
        "    tbz w9, #8, .La64cr_vfp_mcrr_single",
        "    tbnz w9, #5, .La64cr_fallback",
        "    b .La64cr_vfp_mcrr_index_ready",
        ".La64cr_vfp_mcrr_single:",
        "    ubfx w13, w9, #5, #1",
        "    add w12, w12, w13",
        "    cmp w12, #31",
        "    b.eq .La64cr_fallback",
        ".La64cr_vfp_mcrr_index_ready:",
        "    bl .La64cr_vfp_guard_enabled",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #104]",
        "    add x0, x0, w12, uxtw #2",
        "    tbnz w9, #20, .La64cr_vfp_mcrr_load",
        "    ldr w1, [x19, w11, uxtw #2]",
        "    ldr w2, [x19, w10, uxtw #2]",
        "    stp w1, w2, [x0]",
        "    b .La64cr_vfp_done",
        ".La64cr_vfp_mcrr_load:",
        "    ldp w1, w2, [x0]",
        "    str w1, [x19, w11, uxtw #2]",
        "    str w2, [x19, w10, uxtw #2]",
        "    b .La64cr_vfp_done",
        "",
        ".La64cr_vfp_cdp:",
        "    ubfx w10, w9, #23, #1",
        "    ubfx w11, w9, #21, #1",
        "    orr w10, w11, w10, lsl #1",
        "    ubfx w11, w9, #20, #1",
        "    orr w10, w11, w10, lsl #1",
        "    cmp w10, #7",
        "    b.eq .La64cr_vfp_cdp_other",
        # Scalar arithmetic families 0..4 map to the nine VFPv2 operations.
        # Families 5/6 are later fused operations and remain literal.
        "    cmp w10, #4",
        "    b.hi .La64cr_fallback",
        "    ubfx w11, w9, #6, #1",
        "    cmp w10, #4",
        "    b.ne .La64cr_vfp_arith_operation",
        "    cbnz w11, .La64cr_fallback",
        ".La64cr_vfp_arith_operation:",
        "    lsl w14, w10, #1",
        "    add w14, w14, w11",
        "    tbnz w9, #8, .La64cr_vfp_arith_decode_64",
        "    ubfx w12, w9, #12, #4",
        "    ubfx w13, w9, #22, #1",
        "    orr w12, w13, w12, lsl #1",
        "    ubfx w10, w9, #16, #4",
        "    ubfx w13, w9, #7, #1",
        "    orr w10, w13, w10, lsl #1",
        "    and w11, w9, #0xf",
        "    ubfx w13, w9, #5, #1",
        "    orr w11, w13, w11, lsl #1",
        "    b .La64cr_vfp_arith_32",
        ".La64cr_vfp_arith_decode_64:",
        "    tbnz w9, #22, .La64cr_fallback",
        "    tbnz w9, #7, .La64cr_fallback",
        "    tbnz w9, #5, .La64cr_fallback",
        "    ubfx w12, w9, #12, #4",
        "    lsl w12, w12, #1",
        "    ubfx w10, w9, #16, #4",
        "    lsl w10, w10, #1",
        "    and w11, w9, #0xf",
        "    lsl w11, w11, #1",
        "    b .La64cr_vfp_arith_64",
        ".La64cr_vfp_cdp_other:",
        "    tbz w9, #6, .La64cr_fallback",
        "    ubfx w10, w9, #16, #4",
        "    cmp w10, #4",
        "    b.eq .La64cr_vfp_compare_select",
        "    cmp w10, #5",
        "    b.eq .La64cr_vfp_compare_select",
        "    cmp w10, #7",
        "    b.eq .La64cr_vfp_convert_select",
        "    cbz w10, .La64cr_vfp_unary_mov_abs",
        "    cmp w10, #1",
        "    b.ne .La64cr_fallback",
        "    tbnz w9, #7, .La64cr_fallback",
        "    mov w10, #2",
        "    b .La64cr_vfp_unary",
        ".La64cr_vfp_unary_mov_abs:",
        "    ubfx w10, w9, #7, #1",
        "    b .La64cr_vfp_unary",
        ".La64cr_vfp_compare_select:",
        "    tbnz w9, #8, .La64cr_vfp_compare_64",
        "    b .La64cr_vfp_compare_32",
        ".La64cr_vfp_convert_select:",
        "    tbz w9, #7, .La64cr_fallback",
        "    tbnz w9, #8, .La64cr_vfp_narrow_select",
        "    tbnz w9, #22, .La64cr_fallback",
        "    b .La64cr_vfp_widen_32",
        ".La64cr_vfp_narrow_select:",
        "    tbnz w9, #5, .La64cr_fallback",
        "    b .La64cr_vfp_narrow_64",
        "",
        ".La64cr_vfp_unary:",
        "    tbnz w9, #8, .La64cr_vfp_unary_64",
        "    ubfx w12, w9, #12, #4",
        "    ubfx w13, w9, #22, #1",
        "    orr w12, w13, w12, lsl #1",
        "    and w13, w9, #0xf",
        "    ubfx w14, w9, #5, #1",
        "    orr w13, w14, w13, lsl #1",
        "    bl .La64cr_vfp_guard_exact",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #104]",
        "    ldr w1, [x0, w13, uxtw #2]",
        "    cmp w10, #1",
        "    b.eq .La64cr_vfp_unary_32_abs",
        "    cmp w10, #2",
        "    b.ne .La64cr_vfp_unary_32_store",
        "    eor w1, w1, #0x80000000",
        "    b .La64cr_vfp_unary_32_store",
        ".La64cr_vfp_unary_32_abs:",
        "    and w1, w1, #0x7fffffff",
        ".La64cr_vfp_unary_32_store:",
        "    str w1, [x0, w12, uxtw #2]",
        "    b .La64cr_vfp_done",
        ".La64cr_vfp_unary_64:",
        "    tbnz w9, #22, .La64cr_fallback",
        "    tbnz w9, #5, .La64cr_fallback",
        "    ubfx w12, w9, #12, #4",
        "    lsl w12, w12, #1",
        "    and w13, w9, #0xf",
        "    lsl w13, w13, #1",
        "    bl .La64cr_vfp_guard_exact",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #104]",
        "    add x1, x0, w13, uxtw #2",
        "    ldr x1, [x1]",
        "    cmp w10, #1",
        "    b.eq .La64cr_vfp_unary_64_abs",
        "    cmp w10, #2",
        "    b.ne .La64cr_vfp_unary_64_store",
        "    eor x1, x1, #0x8000000000000000",
        "    b .La64cr_vfp_unary_64_store",
        ".La64cr_vfp_unary_64_abs:",
        "    and x1, x1, #0x7fffffffffffffff",
        ".La64cr_vfp_unary_64_store:",
        "    add x0, x0, w12, uxtw #2",
        "    str x1, [x0]",
        "    b .La64cr_vfp_done",
        "",
        *compact_vfp_compare_body(4),
        "",
        *compact_vfp_compare_body(8),
        "",
        *compact_vfp_arithmetic_body(4),
        "",
        *compact_vfp_arithmetic_body(8),
        "",
        ".La64cr_vfp_widen_32:",
        "    ubfx w2, w9, #12, #4",
        "    lsl w2, w2, #1",
        "    and w3, w9, #0xf",
        "    ubfx w4, w9, #5, #1",
        "    orr w3, w4, w3, lsl #1",
        "    bl .La64cr_vfp_guard_values",
        "    cbz w0, .La64cr_fallback",
        "    ldr x0, [x27, #104]",
        "    ldr w6, [x0, w3, uxtw #2]",
        "    ldr x1, [x27, #120]",
        "    ldr w8, [x1]",
        "    ubfx x12, x6, #31, #1",
        "    lsl x12, x12, #63",
        "    ubfx w10, w6, #23, #8",
        "    and w11, w6, #0x7fffff",
        "    cbz w10, .La64cr_vfp_widen_zero_exp",
        "    cmp w10, #0xff",
        "    b.eq .La64cr_vfp_widen_special",
        "    add w10, w10, #896",
        "    lsl x10, x10, #52",
        "    lsl x11, x11, #29",
        "    orr x7, x12, x10",
        "    orr x7, x7, x11",
        "    b .La64cr_vfp_widen_store",
        ".La64cr_vfp_widen_zero_exp:",
        "    cbz w11, .La64cr_vfp_widen_signed_zero",
        "    tbnz w8, #24, .La64cr_vfp_widen_flush",
        "    clz w13, w11",
        "    add w10, w13, #21",
        "    lsl x11, x11, x10",
        "    and x11, x11, #0x000fffffffffffff",
        "    mov w10, #905",
        "    sub w10, w10, w13",
        "    lsl x10, x10, #52",
        "    orr x7, x12, x10",
        "    orr x7, x7, x11",
        "    b .La64cr_vfp_widen_store",
        ".La64cr_vfp_widen_flush:",
        "    orr w8, w8, #0x80",
        "    b .La64cr_vfp_widen_signed_zero",
        ".La64cr_vfp_widen_special:",
        "    cbnz w11, .La64cr_vfp_widen_nan",
        "    mov x10, #0x7ff0000000000000",
        "    orr x7, x12, x10",
        "    b .La64cr_vfp_widen_store",
        ".La64cr_vfp_widen_nan:",
        "    tbnz w11, #22, .La64cr_vfp_widen_quiet_nan",
        "    orr w8, w8, #1",
        "    orr w11, w11, #0x400000",
        ".La64cr_vfp_widen_quiet_nan:",
        "    tbnz w8, #25, .La64cr_vfp_widen_default_nan",
        "    mov x10, #0x7ff0000000000000",
        "    lsl x11, x11, #29",
        "    orr x7, x12, x10",
        "    orr x7, x7, x11",
        "    b .La64cr_vfp_widen_store",
        ".La64cr_vfp_widen_default_nan:",
        "    mov x7, #0x7ff8000000000000",
        "    b .La64cr_vfp_widen_store",
        ".La64cr_vfp_widen_signed_zero:",
        "    mov x7, x12",
        ".La64cr_vfp_widen_store:",
        "    add x0, x0, w2, uxtw #2",
        "    str x7, [x0]",
        "    str w8, [x1]",
        "    b .La64cr_vfp_done",
        "",
        *compact_vfp_narrow_body(),
        "",
        *compact_vfp_memory_body(),
        "",
        ".La64cr_vfp_done:",
        "    add w26, w26, #4",
        "    b .La64cr_retire",
        "",
        # These helpers touch only x0/x1 and x30. No guest state changes before
        # a zero return, so every failure can safely enter the exact fallback.
        ".La64cr_vfp_guard_enabled:",
        "    ldr w0, [x27, #128]",
        "    cbz w0, .La64cr_vfp_guard_fail",
        "    ldr x1, [x27, #112]",
        "    cbz x1, .La64cr_vfp_guard_fail",
        "    ldr w0, [x1]",
        "    tbz w0, #30, .La64cr_vfp_guard_fail",
        "    mov w0, #1",
        "    ret",
        ".La64cr_vfp_guard_exact:",
        "    ldr w0, [x27, #128]",
        "    cbz w0, .La64cr_vfp_guard_fail",
        "    ldr x1, [x27, #112]",
        "    ldr w0, [x1]",
        "    tbz w0, #30, .La64cr_vfp_guard_fail",
        "    ldr x1, [x27, #120]",
        "    ldr w0, [x1]",
        "    tst w0, #0x70000",
        "    b.ne .La64cr_vfp_guard_fail",
        "    mov w0, #1",
        "    ret",
        ".La64cr_vfp_guard_values:",
        "    ldr w0, [x27, #128]",
        "    cbz w0, .La64cr_vfp_guard_fail",
        "    ldr x1, [x27, #112]",
        "    ldr w0, [x1]",
        "    tbz w0, #30, .La64cr_vfp_guard_fail",
        "    ldr x1, [x27, #120]",
        "    ldr w0, [x1]",
        "    mov w1, #0x9f00",
        "    movk w1, #0x7, lsl #16",
        "    tst w0, w1",
        "    b.ne .La64cr_vfp_guard_fail",
        "    mov w0, #1",
        "    ret",
        ".La64cr_vfp_guard_arith:",
        "    ldr w0, [x27, #128]",
        "    cbz w0, .La64cr_vfp_guard_fail",
        "    ldr x1, [x27, #112]",
        "    cbz x1, .La64cr_vfp_guard_fail",
        "    ldr w0, [x1]",
        "    tbz w0, #30, .La64cr_vfp_guard_fail",
        "    ldr x1, [x27, #120]",
        "    cbz x1, .La64cr_vfp_guard_fail",
        "    ldr w0, [x1]",
        # Accept exactly RN/FZ/DN, scalar Len, no enables. NZCV is outside
        # this mask and remains available to guest compare/condition logic.
        "    mov w1, #0x9f00",
        "    movk w1, #0x3c7, lsl #16",
        "    and w0, w0, w1",
        "    mov w1, #3",
        "    lsl w1, w1, #24",
        "    cmp w0, w1",
        "    b.ne .La64cr_vfp_guard_fail",
        "    ldr x1, [x27, #120]",
        "    ldr w0, [x1]",
        "    mov w1, #0x9f",
        "    and w0, w0, w1",
        "    cmp w0, #0x10",
        "    b.ne .La64cr_vfp_guard_fail",
        "    mov w0, #1",
        "    ret",
        ".La64cr_vfp_guard_priv:",
        "    ldr w0, [x27, #128]",
        "    cbz w0, .La64cr_vfp_guard_fail",
        "    ldr w0, [x27, #84]",
        "    cbz w0, .La64cr_vfp_guard_fail",
        "    mov w0, #1",
        "    ret",
        ".La64cr_vfp_guard_fpsid:",
        "    ldr w0, [x27, #128]",
        "    cbz w0, .La64cr_vfp_guard_fail",
        "    ldr x1, [x27, #112]",
        "    ldr w0, [x1]",
        "    tbnz w0, #30, .La64cr_vfp_guard_success",
        "    ldr w0, [x27, #84]",
        "    cbz w0, .La64cr_vfp_guard_fail",
        ".La64cr_vfp_guard_success:",
        "    mov w0, #1",
        "    ret",
        ".La64cr_vfp_guard_fail:",
        "    mov w0, wzr",
        "    ret",
        "",
        ".La64cr_fp_session_begin:",
        "    ldr w0, [x27, #132]",
        "    cbnz w0, .La64cr_fp_session_begin_done",
        "    mrs x0, fpcr",
        "    mrs x1, fpsr",
        "    str x0, [x27, #136]",
        "    str x1, [x27, #144]",
        "    cbz x0, .La64cr_fp_session_fpcr_ready",
        "    msr fpcr, xzr",
        ".La64cr_fp_session_fpcr_ready:",
        "    mov w0, #1",
        "    str w0, [x27, #132]",
        ".La64cr_fp_session_begin_done:",
        "    ret",
        "",
        ".La64cr_fp_session_restore:",
        "    ldr w0, [x27, #132]",
        "    cbz w0, .La64cr_fp_session_restore_done",
        "    ldr x0, [x27, #144]",
        "    msr fpsr, x0",
        "    ldr x0, [x27, #136]",
        "    msr fpcr, x0",
        "    str wzr, [x27, #132]",
        ".La64cr_fp_session_restore_done:",
        "    ret",
    ]


def compact_system_coprocessor_body() -> list[str]:
    """Return exact callback-free CP14 and safe CP15 scalar semantics."""
    return [
        ".La64cr_coprocessor_decode:",
        # Only the MCR/MRC form reaches exec_coprocessor() in arm_step().
        # Other class-3 encodings retain the existing VFP decoder/fallback.
        "    ubfx w10, w9, #24, #4",
        "    cmp w10, #0xe",
        "    b.ne .La64cr_vfp_decode",
        "    tbz w9, #4, .La64cr_vfp_decode",
        "    ubfx w10, w9, #8, #4",
        "    cmp w10, #14",
        "    b.eq .La64cr_cp14",
        "    cmp w10, #15",
        "    b.eq .La64cr_cp15",
        "    b .La64cr_vfp_decode",
        "",
        # The modeled ARM1176 has no debug unit. Privileged CP14 reads return
        # zero and writes are ignored; User accesses remain undefined.
        ".La64cr_cp14:",
        "    ldr w10, [x27, #84]",
        "    cbz w10, .La64cr_fallback",
        "    tbz w9, #20, .La64cr_system_done",
        "    ubfx w10, w9, #12, #4",
        "    cmp w10, #15",
        "    b.eq .La64cr_system_done",
        "    str wzr, [x19, w10, uxtw #2]",
        "    b .La64cr_system_done",
        "",
        ".La64cr_cp15:",
        "    ubfx w10, w9, #16, #4",
        "    cmp w10, #7",
        "    b.eq .La64cr_cp15_c7",
        "    cmp w10, #13",
        "    b.eq .La64cr_cp15_c13",
        "    b .La64cr_fallback",
        "",
        # Every c7 read returns zero and every ordinary cache/barrier write is
        # a no-op in the interpreter. The exact WFI form must leave the native
        # loop before any retirement so the platform wait callback remains
        # synchronous with the device graph.
        ".La64cr_cp15_c7:",
        "    tbnz w9, #20, .La64cr_cp15_c7_read",
        "    ubfx w10, w9, #21, #3",
        "    cbnz w10, .La64cr_system_done",
        "    and w10, w9, #0xf",
        "    cbnz w10, .La64cr_system_done",
        "    ubfx w10, w9, #5, #3",
        "    cmp w10, #4",
        "    b.eq .La64cr_fallback",
        "    b .La64cr_system_done",
        ".La64cr_cp15_c7_read:",
        "    ubfx w10, w9, #12, #4",
        "    cmp w10, #15",
        "    b.eq .La64cr_system_done",
        "    str wzr, [x19, w10, uxtw #2]",
        "    b .La64cr_system_done",
        "",
        # c13 opc2 2..4 are the software thread-ID words. User mode may
        # read/write TPIDRURW and read TPIDRURO with CRm=0; all three are
        # available in privileged modes. Context/FCSE writes still fall back
        # because they can change translation identity or require a TLB flush.
        ".La64cr_cp15_c13:",
        "    ubfx w11, w9, #5, #3",
        "    cmp w11, #2",
        "    b.lo .La64cr_fallback",
        "    cmp w11, #4",
        "    b.hi .La64cr_fallback",
        "    ldr w10, [x27, #84]",
        "    cbnz w10, .La64cr_cp15_c13_access",
        "    and w10, w9, #0xf",
        "    cbnz w10, .La64cr_fallback",
        "    cmp w11, #2",
        "    b.eq .La64cr_cp15_c13_access",
        "    cmp w11, #3",
        "    b.ne .La64cr_fallback",
        "    tbz w9, #20, .La64cr_fallback",
        ".La64cr_cp15_c13_access:",
        "    ldr x0, [x27, #152]",
        "    cbz x0, .La64cr_fallback",
        "    add x0, x0, #44",
        "    add x0, x0, w11, uxtw #2",
        "    ubfx w10, w9, #12, #4",
        "    tbz w9, #20, .La64cr_cp15_c13_write",
        "    cmp w10, #15",
        "    b.eq .La64cr_system_done",
        "    ldr w12, [x0]",
        "    str w12, [x19, w10, uxtw #2]",
        "    b .La64cr_system_done",
        ".La64cr_cp15_c13_write:",
        "    cmp w10, #15",
        "    b.eq .La64cr_cp15_c13_write_pc",
        "    ldr w12, [x19, w10, uxtw #2]",
        "    b .La64cr_cp15_c13_write_commit",
        ".La64cr_cp15_c13_write_pc:",
        "    add w12, w26, #8",
        ".La64cr_cp15_c13_write_commit:",
        "    str w12, [x0]",
        ".La64cr_system_done:",
        "    add w26, w26, #4",
        "    b .La64cr_retire",
        "",
    ]


def compact_raw_function() -> list[str]:
    """Return the mixed A32/Thumb live-byte loop used by the feasibility gate.

    Unlike the static handler engine above, this loop consumes live guest
    instruction bytes directly and keeps the architectural PC plus the flat
    RAM base resident across instructions.  It deliberately accepts only a
    bounded, exactly testable subset: conditional A32 data-processing with
    complete immediate/register-immediate-shift NZCV semantics, the broad
    proven Thumb arithmetic/control/load-store family, aligned witnessed
    memory, and direct branches. A callback-free invocation stops before an
    unsupported or out-of-window instruction. A resident invocation may hand
    that instruction to one exact architectural fallback; continuation must
    publish the proven live window containing the next PC. The return value is
    always the exact retired prefix, with no runtime code generation.
    """
    return [
        "",
        ".p2align 2",
        ".globl A64S_CSYM(a64_compact_raw_execute)",
        "#if !defined(__APPLE__)",
        ".type A64S_CSYM(a64_compact_raw_execute), %function",
        "#endif",
        "A64S_CSYM(a64_compact_raw_execute):",
        # Export zero-cost text boundaries for the opt-in statistical
        # profiler. They are aliases only: ordinary execution gains no
        # instruction, load, branch or writable executable state.
        ".globl A64S_CSYM(a64_compact_raw_profile_entry)",
        "A64S_CSYM(a64_compact_raw_profile_entry):",
        # Eight AAPCS64 arguments. x7 names a caller-owned context containing
        # the flat-RAM oracle, optional resident fallback and exact split
        # counters. Preserve every callee-saved register used by the loop; x29
        # is the native-retirement count not yet committed across a fallback.
        "    stp x29, x30, [sp, #-112]!",
        "    stp x19, x20, [sp, #16]",
        "    stp x21, x22, [sp, #32]",
        "    stp x23, x24, [sp, #48]",
        "    stp x25, x26, [sp, #64]",
        "    stp x27, x28, [sp, #80]",
        "    str w6, [sp, #96]", # original budget; w28 is live width
        "    mov x19, x0",       # architectural register file
        "    mov x20, x1",       # CPSR (read for ADC/SBC/RSC carry)
        "    mov x21, x2",       # cycle counter
        "    mov x22, x3",       # live code bytes at code_base
        "    mov w23, w4",       # code_base
        "    mov w24, w5",       # code_bytes
        "    mov w25, w6",       # remaining instruction budget
        "    mov x27, x7",       # compact raw execution context
        "    mov w28, #4",       # current A32/Thumb instruction width
        "    mov w29, wzr",      # native retirements pending cycle commit
        "    ldr w26, [x19, #60]", # architectural PC
        # Pin the semantic jump table once rather than resolving it per
        # instruction.  The table contains relative offsets and is ordinary
        # signed text on every platform.
        "#if defined(__APPLE__)",
        "    adrp x16, .La64cr_dp_table@PAGE",
        "    add x16, x16, .La64cr_dp_table@PAGEOFF",
        "#else",
        "    adrp x16, .La64cr_dp_table",
        "    add x16, x16, :lo12:.La64cr_dp_table",
        "#endif",
        "#if defined(__APPLE__)",
        "    adrp x17, .La64cr_cond_table@PAGE",
        "    add x17, x17, .La64cr_cond_table@PAGEOFF",
        "#else",
        "    adrp x17, .La64cr_cond_table",
        "    add x17, x17, :lo12:.La64cr_cond_table",
        "#endif",
        "    cbz w25, .La64cr_exit",
        "",
        ".La64cr_loop:",
        # The code pointer names code_base. Unsigned subtraction rejects both
        # below-base and past-end PCs. A resident invocation hands the first
        # instruction beyond the window to the exact fallback; continuation
        # must publish the next proven window before another native fetch.
        "    sub w8, w26, w23",
        "    cmp w8, w24",
        "    b.hs .La64cr_window_miss",
        # Select width and fetch from live CPSR.T every iteration. A fallback
        # or native BX/BLX may change state without leaving this invocation.
        "    ldr w10, [x20]",
        "    tbnz w10, #5, .La64cr_thumb_fetch",
        "    mov w28, #4",
        "    tst w8, #3",
        "    b.ne .La64cr_exit",
        "    add w11, w8, #4",
        "    cmp w11, w24",
        "    b.hi .La64cr_fallback",
        "    ldr w9, [x22, w8, uxtw]",
        # ARM and AArch64 share the fourteen ordinary condition predicates.
        # Keep AL on a direct fast path; other predicates use a tiny signed
        # branch table after loading the guest's NZCV.  A failed condition
        # still retires and advances PC without decoding the instruction.
        "    lsr w10, w9, #28",
        "    cmp w10, #14",
        "    b.eq .La64cr_condition_pass",
        "    b.hi .La64cr_fallback",
        "    ldr w11, [x20]",
        "    msr nzcv, x11",
        "    ldrsw x15, [x17, w10, uxtw #2]",
        "    add x15, x17, x15",
        "    br x15",
        ".La64cr_condition_skip:",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        ".La64cr_condition_pass:",
        # A32 BX/BLX(register) lives in the data-processing miscellaneous
        # space. Recognize the complete encoding before ordinary DP decode.
        "    bic w11, w9, #0xf0000000",
        "    bic w11, w11, #0xf",
        "    mov w10, #0xff10",
        "    movk w10, #0x012f, lsl #16",
        "    cmp w11, w10",
        "    b.eq .La64cr_indirect_bx",
        "    add w10, w10, #0x20",
        "    cmp w11, w10",
        "    b.eq .La64cr_indirect_blx",
        "    ubfx w10, w9, #25, #3",
        "    cmp w10, #5",
        "    b.eq .La64cr_branch",
        "    ubfx w10, w9, #26, #2",
        "    cbz w10, .La64cr_dp",
        "    cmp w10, #1",
        "    b.eq .La64cr_memory",
        "    cmp w10, #2",
        "    b.eq .La64cr_block",
        "    cmp w10, #3",
        "    b.eq .La64cr_coprocessor_decode",
        "    b .La64cr_fallback",
        "",
        ".La64cr_thumb_fetch:",
        "    mov w28, #2",
        "    tst w8, #1",
        "    b.ne .La64cr_exit",
        "    add w11, w8, #2",
        "    cmp w11, w24",
        "    b.hi .La64cr_fallback",
        "    ldrh w9, [x22, w8, uxtw]",
        "    b .La64cr_thumb_decode",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_dp)",
        "A64S_CSYM(a64_compact_raw_profile_dp):",
        ".La64cr_dp:",
        # PC operands/destinations remain outside this tranche. Immediate
        # rotation and both immediate/register-specified shifts feed every
        # exact S/comparison opcode, including architectural shifter carry.
        "    ubfx w13, w9, #12, #4",
        "    cmp w13, #15",
        "    b.eq .La64cr_fallback",
        "    ubfx w11, w9, #16, #4",
        "    cmp w11, #15",
        "    b.eq .La64cr_fallback",
        "    ldr w11, [x19, w11, uxtw #2]",
        "    ldr w8, [x20]",
        "    ubfx w8, w8, #29, #1",
        "    tbnz w9, #25, .La64cr_dp_immediate",
        "    and w10, w9, #0xf",
        "    cmp w10, #15",
        "    b.eq .La64cr_fallback",
        "    ldr w10, [x19, w10, uxtw #2]",
        "    tbnz w9, #4, .La64cr_dp_register_shift",
        # Preserve the old zero-shift fast path. Every other immediate-shift
        # shape follows ARM's special amount-zero rules rather than AArch64's
        # modulo-32 variable-shift behavior.
        "    tst w9, #0xff0",
        "    b.eq .La64cr_dp_operand_ready",
        "    ubfx w14, w9, #7, #5",
        "    ubfx w15, w9, #5, #2",
        "    cbz w15, .La64cr_shift_lsl",
        "    cmp w15, #1",
        "    b.eq .La64cr_shift_lsr",
        "    cmp w15, #2",
        "    b.eq .La64cr_shift_asr",
        "    b .La64cr_shift_ror",
        ".La64cr_shift_lsl:",
        "    mov w15, #32",
        "    sub w15, w15, w14",
        "    lsrv w15, w10, w15",
        "    and w8, w15, #1",
        "    lslv w10, w10, w14",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_shift_lsr:",
        "    cbz w14, .La64cr_shift_lsr_32",
        "    sub w15, w14, #1",
        "    lsrv w15, w10, w15",
        "    and w8, w15, #1",
        "    lsrv w10, w10, w14",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_shift_lsr_32:",
        "    lsr w8, w10, #31",
        "    mov w10, wzr",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_shift_asr:",
        "    cbz w14, .La64cr_shift_asr_32",
        "    sub w15, w14, #1",
        "    lsrv w15, w10, w15",
        "    and w8, w15, #1",
        "    asrv w10, w10, w14",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_shift_asr_32:",
        "    lsr w8, w10, #31",
        "    asr w10, w10, #31",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_shift_ror:",
        "    cbz w14, .La64cr_shift_rrx",
        "    rorv w10, w10, w14",
        "    lsr w8, w10, #31",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_shift_rrx:",
        "    and w15, w10, #1",
        "    lsr w10, w10, #1",
        "    orr w10, w10, w8, lsl #31",
        "    mov w8, w15",
        "    b .La64cr_dp_operand_ready",
        "",
        # Register-specified shifts consume only Rs[7:0]. AArch64 masks
        # variable shift counts modulo 32, while ARM distinguishes 32, >32,
        # and nonzero ROR multiples of 32, so every boundary is explicit.
        # Bit 7 separates this encoding from multiply/extra-transfer space;
        # ARMv6 forbids PC in Rs just as the common path forbids it in Rm.
        ".La64cr_dp_register_shift:",
        "    tbnz w9, #7, .La64cr_fallback",
        "    ubfx w14, w9, #8, #4",
        "    cmp w14, #15",
        "    b.eq .La64cr_fallback",
        "    ldr w14, [x19, w14, uxtw #2]",
        "    and w14, w14, #0xff",
        "    cbz w14, .La64cr_dp_operand_ready",
        "    ubfx w15, w9, #5, #2",
        "    cbz w15, .La64cr_dp_register_lsl",
        "    cmp w15, #1",
        "    b.eq .La64cr_dp_register_lsr",
        "    cmp w15, #2",
        "    b.eq .La64cr_dp_register_asr",
        "    b .La64cr_dp_register_ror",
        ".La64cr_dp_register_lsl:",
        "    cmp w14, #32",
        "    b.hi .La64cr_dp_register_zero",
        "    b.eq .La64cr_dp_register_lsl_32",
        "    mov w15, #32",
        "    sub w15, w15, w14",
        "    lsrv w15, w10, w15",
        "    and w8, w15, #1",
        "    lslv w10, w10, w14",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_dp_register_lsl_32:",
        "    and w8, w10, #1",
        "    mov w10, wzr",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_dp_register_lsr:",
        "    cmp w14, #32",
        "    b.hi .La64cr_dp_register_zero",
        "    b.eq .La64cr_dp_register_lsr_32",
        "    sub w15, w14, #1",
        "    lsrv w15, w10, w15",
        "    and w8, w15, #1",
        "    lsrv w10, w10, w14",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_dp_register_lsr_32:",
        "    lsr w8, w10, #31",
        "    mov w10, wzr",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_dp_register_zero:",
        "    mov w8, wzr",
        "    mov w10, wzr",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_dp_register_asr:",
        "    cmp w14, #32",
        "    b.hs .La64cr_dp_register_asr_32",
        "    sub w15, w14, #1",
        "    lsrv w15, w10, w15",
        "    and w8, w15, #1",
        "    asrv w10, w10, w14",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_dp_register_asr_32:",
        "    lsr w8, w10, #31",
        "    asr w10, w10, #31",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_dp_register_ror:",
        "    and w15, w14, #31",
        "    cbz w15, .La64cr_dp_register_ror_multiple_32",
        "    rorv w10, w10, w15",
        "    lsr w8, w10, #31",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_dp_register_ror_multiple_32:",
        "    lsr w8, w10, #31",
        "    b .La64cr_dp_operand_ready",
        ".La64cr_dp_immediate:",
        "    and w10, w9, #0xff",
        "    ubfx w14, w9, #8, #4",
        "    lsl w14, w14, #1",
        "    rorv w10, w10, w14",
        "    cbz w14, .La64cr_dp_operand_ready",
        "    lsr w8, w10, #31",
        ".La64cr_dp_operand_ready:",
        "    ubfx w14, w9, #21, #4",
        "    tbnz w9, #20, .La64cr_dp_flag_dispatch",
        ".La64cr_dp_plain_dispatch:",
        "    ldrsw x15, [x16, w14, uxtw #2]",
        "    add x15, x16, x15",
        "    br x15",
        ".La64cr_dp_flag_dispatch:",
        "    add x15, x16, #64",
        "    ldrsw x14, [x15, w14, uxtw #2]",
        "    add x15, x15, x14",
        "    br x15",
        "",
        ".La64cr_dp_and:",
        "    and w10, w11, w10",
        "    b .La64cr_dp_write",
        ".La64cr_dp_eor:",
        "    eor w10, w11, w10",
        "    b .La64cr_dp_write",
        ".La64cr_dp_sub:",
        "    sub w10, w11, w10",
        "    b .La64cr_dp_write",
        ".La64cr_dp_rsb:",
        "    sub w10, w10, w11",
        "    b .La64cr_dp_write",
        ".La64cr_dp_add:",
        "    add w10, w11, w10",
        "    b .La64cr_dp_write",
        ".La64cr_dp_adc:",
        "    ldr w15, [x20]",
        "    ubfx w15, w15, #29, #1",
        "    add w10, w11, w10",
        "    add w10, w10, w15",
        "    b .La64cr_dp_write",
        ".La64cr_dp_sbc:",
        "    ldr w15, [x20]",
        "    ubfx w15, w15, #29, #1",
        "    sub w10, w11, w10",
        "    sub w10, w10, #1",
        "    add w10, w10, w15",
        "    b .La64cr_dp_write",
        ".La64cr_dp_rsc:",
        "    ldr w15, [x20]",
        "    ubfx w15, w15, #29, #1",
        "    sub w10, w10, w11",
        "    sub w10, w10, #1",
        "    add w10, w10, w15",
        "    b .La64cr_dp_write",
        ".La64cr_dp_orr:",
        "    orr w10, w11, w10",
        "    b .La64cr_dp_write",
        ".La64cr_dp_mov:",
        "    b .La64cr_dp_write",
        ".La64cr_dp_bic:",
        "    bic w10, w11, w10",
        "    b .La64cr_dp_write",
        ".La64cr_dp_mvn:",
        "    mvn w10, w10",
        ".La64cr_dp_write:",
        "    str w10, [x19, w13, uxtw #2]",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        "",
        # Flag-writing arithmetic maps directly to the AArch64 32-bit flag
        # rules. ADC/SBC/RSC first install the guest carry. Comparisons share
        # the same flag capture but deliberately omit the destination write.
        ".La64cr_dp_flag_and:",
        "    and w10, w11, w10",
        "    b .La64cr_dp_logic_flags_write",
        ".La64cr_dp_flag_eor:",
        "    eor w10, w11, w10",
        "    b .La64cr_dp_logic_flags_write",
        ".La64cr_dp_flag_sub:",
        "    subs w10, w11, w10",
        "    b .La64cr_dp_arith_flags_write",
        ".La64cr_dp_flag_rsb:",
        "    subs w10, w10, w11",
        "    b .La64cr_dp_arith_flags_write",
        ".La64cr_dp_flag_add:",
        "    adds w10, w11, w10",
        "    b .La64cr_dp_arith_flags_write",
        ".La64cr_dp_flag_adc:",
        "    ldr w15, [x20]",
        "    msr nzcv, x15",
        "    adcs w10, w11, w10",
        "    b .La64cr_dp_arith_flags_write",
        ".La64cr_dp_flag_sbc:",
        "    ldr w15, [x20]",
        "    msr nzcv, x15",
        "    sbcs w10, w11, w10",
        "    b .La64cr_dp_arith_flags_write",
        ".La64cr_dp_flag_rsc:",
        "    ldr w15, [x20]",
        "    msr nzcv, x15",
        "    sbcs w10, w10, w11",
        "    b .La64cr_dp_arith_flags_write",
        ".La64cr_dp_flag_tst:",
        "    and w10, w11, w10",
        "    b .La64cr_dp_logic_flags",
        ".La64cr_dp_flag_teq:",
        "    eor w10, w11, w10",
        "    b .La64cr_dp_logic_flags",
        ".La64cr_dp_flag_cmp:",
        "    subs w10, w11, w10",
        "    b .La64cr_dp_arith_flags",
        ".La64cr_dp_flag_cmn:",
        "    adds w10, w11, w10",
        "    b .La64cr_dp_arith_flags",
        ".La64cr_dp_flag_orr:",
        "    orr w10, w11, w10",
        "    b .La64cr_dp_logic_flags_write",
        ".La64cr_dp_flag_mov:",
        "    b .La64cr_dp_logic_flags_write",
        ".La64cr_dp_flag_bic:",
        "    bic w10, w11, w10",
        "    b .La64cr_dp_logic_flags_write",
        ".La64cr_dp_flag_mvn:",
        "    mvn w10, w10",
        ".La64cr_dp_logic_flags_write:",
        "    str w10, [x19, w13, uxtw #2]",
        ".La64cr_dp_logic_flags:",
        # AArch64 logical flag instructions supply exact N/Z but clear C/V.
        # Restore ARM's shifter C and preserved V before publishing NZCV.
        "    ands wzr, w10, w10",
        "    mrs x15, nzcv",
        "    ldr w14, [x20]",
        "    and w9, w14, #0x10000000",
        "    orr w15, w15, w9",
        "    orr w15, w15, w8, lsl #29",
        "    ubfx w15, w15, #28, #4",
        "    bfi w14, w15, #28, #4",
        "    str w14, [x20]",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        ".La64cr_dp_arith_flags_write:",
        "    str w10, [x19, w13, uxtw #2]",
        ".La64cr_dp_arith_flags:",
        "    mrs x15, nzcv",
        "    ldr w14, [x20]",
        "    ubfx w15, w15, #28, #4",
        "    bfi w14, w15, #28, #4",
        "    str w14, [x20]",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_memory)",
        "A64S_CSYM(a64_compact_raw_profile_memory):",
        ".La64cr_memory:",
        # Complete ARM addressing-mode-2 byte/word LDR/STR semantics. Both
        # immediate and shifted-register offsets, pre/post indexing,
        # privileged/unprivileged tags, PC bases/sources/destinations and safe
        # writeback are decoded from the live instruction. Word accesses stay
        # aligned so arm_step owns SCTLR.A/U rotation and fault policy. Every
        # cache or runtime-target refusal occurs before architectural mutation.
        "    ubfx w12, w9, #16, #4",
        "    ubfx w13, w9, #12, #4",
        "    tbz w9, #22, .La64cr_memory_data_register_ready",
        "    cmp w13, #15",
        "    b.eq .La64cr_fallback",
        ".La64cr_memory_data_register_ready:",
        # Any post-indexed form writes back; a pre-indexed form does so only
        # with W. The interpreter rejects every base/data alias and PC base in
        # that subset before even calculating the address.
        "    tbz w9, #24, .La64cr_memory_writeback_guard",
        "    tbz w9, #21, .La64cr_memory_writeback_ready",
        ".La64cr_memory_writeback_guard:",
        "    cmp w12, #15",
        "    b.eq .La64cr_fallback",
        "    cmp w12, w13",
        "    b.eq .La64cr_fallback",
        # LDRT pc is separately UNPREDICTABLE. STRT pc remains a legal word
        # store and uses PC+12 below.
        "    tbnz w9, #24, .La64cr_memory_writeback_ready",
        "    tbz w9, #21, .La64cr_memory_writeback_ready",
        "    tbz w9, #20, .La64cr_memory_writeback_ready",
        "    cmp w13, #15",
        "    b.eq .La64cr_fallback",
        ".La64cr_memory_writeback_ready:",
        "    tbnz w9, #25, .La64cr_memory_register_offset",
        "    and w14, w9, #0xfff",
        "    b .La64cr_memory_offset_ready",
        ".La64cr_memory_register_offset:",
        "    tbnz w9, #4, .La64cr_fallback",
        "    and w10, w9, #0xf",
        "    cmp w10, #15",
        "    b.eq .La64cr_fallback",
        "    ldr w14, [x19, w10, uxtw #2]",
        "    ubfx w8, w9, #7, #5",
        "    ubfx w0, w9, #5, #2",
        "    cbz w0, .La64cr_memory_shift_lsl",
        "    cmp w0, #1",
        "    b.eq .La64cr_memory_shift_lsr",
        "    cmp w0, #2",
        "    b.eq .La64cr_memory_shift_asr",
        "    cbnz w8, .La64cr_memory_shift_ror_amount",
        # ROR #0 is RRX and consumes, but does not modify, guest C.
        "    ldr w0, [x20]",
        "    ubfx w0, w0, #29, #1",
        "    lsr w14, w14, #1",
        "    orr w14, w14, w0, lsl #31",
        "    b .La64cr_memory_offset_ready",
        ".La64cr_memory_shift_ror_amount:",
        "    rorv w14, w14, w8",
        "    b .La64cr_memory_offset_ready",
        ".La64cr_memory_shift_asr:",
        "    cbnz w8, .La64cr_memory_shift_asr_amount",
        "    asr w14, w14, #31",
        "    b .La64cr_memory_offset_ready",
        ".La64cr_memory_shift_asr_amount:",
        "    asrv w14, w14, w8",
        "    b .La64cr_memory_offset_ready",
        ".La64cr_memory_shift_lsr:",
        "    cbnz w8, .La64cr_memory_shift_lsr_amount",
        "    mov w14, wzr",
        "    b .La64cr_memory_offset_ready",
        ".La64cr_memory_shift_lsr_amount:",
        "    lsrv w14, w14, w8",
        "    b .La64cr_memory_offset_ready",
        ".La64cr_memory_shift_lsl:",
        "    cbz w8, .La64cr_memory_offset_ready",
        "    lslv w14, w14, w8",
        ".La64cr_memory_offset_ready:",
        "    cmp w12, #15",
        "    b.ne .La64cr_memory_base_register",
        "    add w8, w26, #8",
        "    b .La64cr_memory_base_ready",
        ".La64cr_memory_base_register:",
        "    ldr w8, [x19, w12, uxtw #2]",
        ".La64cr_memory_base_ready:",
        "    tbz w9, #23, .La64cr_memory_updated_sub",
        "    add w11, w8, w14",
        "    b .La64cr_memory_updated_ready",
        ".La64cr_memory_updated_sub:",
        "    sub w11, w8, w14",
        ".La64cr_memory_updated_ready:",
        "    tbnz w9, #24, .La64cr_memory_preindexed",
        "    mov w10, w8",
        "    b .La64cr_memory_address_ready",
        ".La64cr_memory_preindexed:",
        "    mov w10, w11",
        ".La64cr_memory_address_ready:",
        "    tbnz w9, #22, .La64cr_memory_lookup_select",
        "    tst w10, #3",
        "    b.ne .La64cr_fallback",
        ".La64cr_memory_lookup_select:",
        "    ubfx w15, w9, #20, #1",
        # P=0,W=1 selects the unprivileged DREAD/DWRITE half even when the
        # guest is currently privileged. Bit 1 is a private lookup selector;
        # existing scalar/VFP/block callers leave it clear.
        "    tbnz w9, #24, .La64cr_memory_lookup_ready",
        "    tbz w9, #21, .La64cr_memory_lookup_ready",
        "    orr w15, w15, #2",
        ".La64cr_memory_lookup_ready:",
        "    bl .La64cr_memory_lookup",
        "    cbz w2, .La64cr_fallback",
        "    mov x6, x0",
        "    mov x7, x1",
        "    tbnz w9, #20, .La64cr_memory_load",
        "    cmp w13, #15",
        "    b.eq .La64cr_memory_store_pc",
        "    ldr w0, [x19, w13, uxtw #2]",
        "    b .La64cr_memory_store_value",
        ".La64cr_memory_store_pc:",
        "    add w0, w26, #12",
        ".La64cr_memory_store_value:",
        "    tbnz w9, #22, .La64cr_memory_store_byte",
        "    str w0, [x6]",
        "    b .La64cr_memory_commit",
        ".La64cr_memory_store_byte:",
        "    strb w0, [x6]",
        "    b .La64cr_memory_commit",
        ".La64cr_memory_load:",
        "    tbnz w9, #22, .La64cr_memory_load_byte",
        "    ldr w0, [x6]",
        "    cmp w13, #15",
        "    b.ne .La64cr_memory_load_register",
        # Validate an interworking target before destination, writeback,
        # CPSR.T or cache telemetry changes.
        "    and w1, w0, #3",
        "    cmp w1, #2",
        "    b.eq .La64cr_fallback",
        "    b .La64cr_memory_commit",
        ".La64cr_memory_load_byte:",
        "    ldrb w0, [x6]",
        ".La64cr_memory_load_register:",
        "    str w0, [x19, w13, uxtw #2]",
        ".La64cr_memory_commit:",
        "    tbz w9, #24, .La64cr_memory_writeback",
        "    tbz w9, #21, .La64cr_memory_count_hit",
        ".La64cr_memory_writeback:",
        "    str w11, [x19, w12, uxtw #2]",
        ".La64cr_memory_count_hit:",
        "    cbz x7, .La64cr_memory_finish",
        "    ldr x1, [x7]",
        "    add x1, x1, #1",
        "    str x1, [x7]",
        ".La64cr_memory_finish:",
        "    tbz w9, #20, .La64cr_memory_fallthrough",
        "    cmp w13, #15",
        "    b.ne .La64cr_memory_fallthrough",
        "    and w1, w0, #1",
        "    ldr w2, [x20]",
        "    bfi w2, w1, #5, #1",
        "    str w2, [x20]",
        "    tbnz w0, #0, .La64cr_memory_thumb_target",
        "    bic w26, w0, #3",
        "    b .La64cr_retire",
        ".La64cr_memory_thumb_target:",
        "    bic w26, w0, #1",
        "    b .La64cr_retire",
        ".La64cr_memory_fallthrough:",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_block_control)",
        "A64S_CSYM(a64_compact_raw_profile_block_control):",
        ".La64cr_block:",
        # Ordinary A32 STM/LDM, including every address mode, safe writeback
        # and plain LDM-to-PC interworking. S/user-bank and exception-return
        # forms remain literal.
        # One complete aligned 1 KiB DREAD/DWRITE block is proved before the
        # first register or memory mutation, preserving the interpreter's
        # base-restored transactional abort model.
        "    tbnz w9, #22, .La64cr_fallback",
        "    ubfx w13, w9, #16, #4",
        "    cmp w13, #15",
        "    b.eq .La64cr_fallback",
        "    and w12, w9, #0xffff",
        "    cbz w12, .La64cr_fallback",
        "    tbnz w9, #20, .La64cr_block_load_shape",
        # STM writeback with the base in the list is defined only when the
        # base is the lowest-numbered register stored. Its original value is
        # read before writeback below, matching exec_block_transfer().
        "    tbz w9, #21, .La64cr_block_shape_ready",
        "    lsrv w10, w12, w13",
        "    tbz w10, #0, .La64cr_block_shape_ready",
        "    mov w10, #1",
        "    lslv w10, w10, w13",
        "    sub w10, w10, #1",
        "    tst w12, w10",
        "    b.ne .La64cr_fallback",
        "    b .La64cr_block_shape_ready",
        ".La64cr_block_load_shape:",
        "    tbz w9, #21, .La64cr_block_shape_ready",
        "    lsrv w10, w12, w13",
        "    tbnz w10, #0, .La64cr_fallback",
        ".La64cr_block_shape_ready:",
        # Count the non-empty register list without relying on optional SIMD.
        # At most sixteen clear-lowest-bit iterations run once per transfer.
        "    mov w14, wzr",
        "    mov w10, w12",
        ".La64cr_block_count:",
        "    add w14, w14, #1",
        "    sub w11, w10, #1",
        "    and w10, w10, w11",
        "    cbnz w10, .La64cr_block_count",
        "    ldr w11, [x19, w13, uxtw #2]",
        # w10=start and w11=writeback after this address-mode fold.
        "    tbz w9, #23, .La64cr_block_down",
        "    add w8, w11, w14, lsl #2",
        "    tbz w9, #24, .La64cr_block_up_after",
        "    add w10, w11, #4",
        "    mov w11, w8",
        "    b .La64cr_block_address_ready",
        ".La64cr_block_up_after:",
        "    mov w10, w11",
        "    mov w11, w8",
        "    b .La64cr_block_address_ready",
        ".La64cr_block_down:",
        "    sub w8, w11, w14, lsl #2",
        "    mov w10, w8",
        "    tbnz w9, #24, .La64cr_block_down_before",
        "    add w10, w10, #4",
        ".La64cr_block_down_before:",
        "    mov w11, w8",
        ".La64cr_block_address_ready:",
        "    tst w10, #3",
        "    b.ne .La64cr_fallback",
        # The cache lookup proves only one 1 KiB translation. Reject a span
        # crossing that boundary (or the top of 32-bit VA) before mutation.
        "    and w0, w10, #0x3ff",
        "    add w0, w0, w14, lsl #2",
        "    cmp w0, #1024",
        "    b.hi .La64cr_fallback",
        # Flat masked RAM also needs a contiguous host span; otherwise the
        # interpreter owns per-word wraparound.
        "    ldr x0, [x27, #0]",
        "    cbz x0, .La64cr_block_lookup",
        "    ldr w0, [x27, #8]",
        "    and w1, w10, w0",
        "    add w1, w1, w14, lsl #2",
        "    add w0, w0, #1",
        "    cmp w1, w0",
        "    b.hi .La64cr_fallback",
        ".La64cr_block_lookup:",
        "    ubfx w15, w9, #20, #1",
        "    bl .La64cr_memory_lookup",
        "    cbz w2, .La64cr_fallback",
        "    mov x6, x0",
        "    mov x7, x1",
        "    mov w8, wzr",
        "    tbz w9, #20, .La64cr_block_store_loop",
        # PC is the last word in an increasing register list. Validate its
        # interworking value before committing any loaded register, writeback
        # or cache telemetry so an invalid 0b10 target can fall back pristine.
        "    tbz w12, #15, .La64cr_block_load_loop",
        "    sub w0, w14, #1",
        "    ldr w5, [x6, w0, uxtw #2]",
        "    and w0, w5, #3",
        "    cmp w0, #2",
        "    b.eq .La64cr_fallback",
        "    b .La64cr_block_load_loop",
        ".La64cr_block_store_loop:",
        "    lsrv w0, w12, w8",
        "    tbz w0, #0, .La64cr_block_store_next",
        "    cmp w8, #15",
        "    b.eq .La64cr_block_store_pc",
        "    ldr w1, [x19, w8, uxtw #2]",
        "    b .La64cr_block_store_word",
        ".La64cr_block_store_pc:",
        "    add w1, w26, #12",
        ".La64cr_block_store_word:",
        "    str w1, [x6], #4",
        ".La64cr_block_store_next:",
        "    add w8, w8, #1",
        "    cmp w8, #16",
        "    b.lo .La64cr_block_store_loop",
        "    b .La64cr_block_finish",
        ".La64cr_block_load_loop:",
        "    lsrv w0, w12, w8",
        "    tbz w0, #0, .La64cr_block_load_next",
        "    ldr w1, [x6], #4",
        "    str w1, [x19, w8, uxtw #2]",
        ".La64cr_block_load_next:",
        "    add w8, w8, #1",
        "    cmp w8, #15",
        "    b.lo .La64cr_block_load_loop",
        ".La64cr_block_finish:",
        "    tbz w9, #21, .La64cr_block_count_hits",
        "    str w11, [x19, w13, uxtw #2]",
        ".La64cr_block_count_hits:",
        "    cbz x7, .La64cr_block_done",
        "    ldr x0, [x7]",
        "    add x0, x0, w14, uxtw",
        "    str x0, [x7]",
        ".La64cr_block_done:",
        "    tbz w9, #20, .La64cr_block_fallthrough",
        "    tbz w12, #15, .La64cr_block_fallthrough",
        "    and w0, w5, #1",
        "    ldr w1, [x20]",
        "    bfi w1, w0, #5, #1",
        "    str w1, [x20]",
        "    tbnz w5, #0, .La64cr_block_thumb_target",
        "    bic w26, w5, #3",
        "    b .La64cr_retire",
        ".La64cr_block_thumb_target:",
        "    bic w26, w5, #1",
        "    b .La64cr_retire",
        ".La64cr_block_fallthrough:",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        "",
        ".La64cr_indirect_bx:",
        "    mov w14, wzr",
        "    b .La64cr_indirect_decode",
        ".La64cr_indirect_blx:",
        "    mov w14, #1",
        ".La64cr_indirect_decode:",
        "    and w10, w9, #0xf",
        "    cmp w10, #15",
        "    b.ne .La64cr_indirect_register",
        "    cbnz w14, .La64cr_fallback",
        "    add w11, w26, #8",
        "    b .La64cr_indirect_target",
        ".La64cr_indirect_register:",
        "    ldr w11, [x19, w10, uxtw #2]",
        ".La64cr_indirect_target:",
        # 0b10 cannot denote either an aligned ARM word or a Thumb halfword.
        # Refuse before changing LR, CPSR.T or PC.
        "    and w12, w11, #3",
        "    cmp w12, #2",
        "    b.eq .La64cr_fallback",
        "    cbz w14, .La64cr_indirect_no_link",
        "    add w10, w26, #4",
        "    str w10, [x19, #56]",
        ".La64cr_indirect_no_link:",
        "    and w10, w11, #1",
        "    ldr w12, [x20]",
        "    bfi w12, w10, #5, #1",
        "    str w12, [x20]",
        "    bic w26, w11, #1",
        "    b .La64cr_retire",
        "",
        ".La64cr_branch:",
        "    tbz w9, #24, .La64cr_branch_no_link",
        "    add w10, w26, #4",
        "    str w10, [x19, #56]",
        ".La64cr_branch_no_link:",
        "    sbfx w10, w9, #0, #24",
        "    lsl w10, w10, #2",
        "    add w26, w26, #8",
        "    add w26, w26, w10",
        "    b .La64cr_retire",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_system)",
        "A64S_CSYM(a64_compact_raw_profile_system):",
        *compact_system_coprocessor_body(),
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_vfp)",
        "A64S_CSYM(a64_compact_raw_profile_vfp):",
        *compact_vfp_nonarith_body(),
        "",
        # Thumb-1 is decoded directly from the same live fetch window. The
        # broad family below mirrors decode_thumb(): unsupported encodings
        # reach the exact fallback before architectural mutation.
        ".globl A64S_CSYM(a64_compact_raw_profile_thumb_decode)",
        "A64S_CSYM(a64_compact_raw_profile_thumb_decode):",
        ".La64cr_thumb_decode:",
        "    ubfx w10, w9, #8, #8",
        "    cmp w10, #0x47",
        "    b.eq .La64cr_thumb_bx",
        "    lsr w10, w9, #12",
        "    add x15, x17, #56",
        "    ldrsw x14, [x15, w10, uxtw #2]",
        "    add x15, x15, x14",
        "    br x15",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_thumb_low_alu)",
        "A64S_CSYM(a64_compact_raw_profile_thumb_low_alu):",
        ".La64cr_thumb_top1:",
        "    tbnz w9, #11, .La64cr_thumb_add_sub",
        ".La64cr_thumb_shift_imm:",
        "    and w13, w9, #7",
        "    ubfx w10, w9, #3, #3",
        "    ldr w10, [x19, w10, uxtw #2]",
        "    ldr w8, [x20]",
        "    ubfx w8, w8, #29, #1",
        "    ubfx w15, w9, #6, #5",
        "    ubfx w14, w9, #11, #2",
        "    cbz w14, .La64cr_thumb_lsl_imm",
        "    cmp w14, #1",
        "    b.eq .La64cr_thumb_lsr_imm",
        "    cmp w14, #2",
        "    b.ne .La64cr_fallback",
        "    cbz w15, .La64cr_thumb_asr_imm_32",
        "    sub w14, w15, #1",
        "    lsrv w14, w10, w14",
        "    and w8, w14, #1",
        "    asrv w10, w10, w15",
        "    b .La64cr_thumb_shift_imm_ready",
        ".La64cr_thumb_asr_imm_32:",
        "    lsr w8, w10, #31",
        "    asr w10, w10, #31",
        "    b .La64cr_thumb_shift_imm_ready",
        ".La64cr_thumb_lsr_imm:",
        "    cbz w15, .La64cr_thumb_lsr_imm_32",
        "    sub w14, w15, #1",
        "    lsrv w14, w10, w14",
        "    and w8, w14, #1",
        "    lsrv w10, w10, w15",
        "    b .La64cr_thumb_shift_imm_ready",
        ".La64cr_thumb_lsr_imm_32:",
        "    lsr w8, w10, #31",
        "    mov w10, wzr",
        "    b .La64cr_thumb_shift_imm_ready",
        ".La64cr_thumb_lsl_imm:",
        "    cbz w15, .La64cr_thumb_shift_imm_ready",
        "    mov w14, #32",
        "    sub w14, w14, w15",
        "    lsrv w14, w10, w14",
        "    and w8, w14, #1",
        "    lslv w10, w10, w15",
        ".La64cr_thumb_shift_imm_ready:",
        "    mov w14, #13",
        "    b .La64cr_dp_flag_dispatch",
        "",
        ".La64cr_thumb_add_sub:",
        "    and w13, w9, #7",
        "    ubfx w11, w9, #3, #3",
        "    ldr w11, [x19, w11, uxtw #2]",
        "    ubfx w10, w9, #6, #3",
        "    tbnz w9, #10, .La64cr_thumb_add_sub_operand_ready",
        "    ldr w10, [x19, w10, uxtw #2]",
        ".La64cr_thumb_add_sub_operand_ready:",
        "    mov w14, #4",
        "    tbz w9, #9, .La64cr_dp_flag_dispatch",
        "    mov w14, #2",
        "    b .La64cr_dp_flag_dispatch",
        "",
        ".La64cr_thumb_immediate:",
        "    ubfx w13, w9, #8, #3",
        "    and w10, w9, #0xff",
        "    ubfx w14, w9, #11, #2",
        "    cbnz w14, .La64cr_thumb_immediate_not_mov",
        "    ldr w8, [x20]",
        "    ubfx w8, w8, #29, #1",
        "    mov w14, #13",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_immediate_not_mov:",
        "    ldr w11, [x19, w13, uxtw #2]",
        "    cmp w14, #1",
        "    b.ne .La64cr_thumb_immediate_add_sub",
        "    mov w14, #10",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_immediate_add_sub:",
        "    cmp w14, #2",
        "    mov w14, #4",
        "    b.eq .La64cr_dp_flag_dispatch",
        "    mov w14, #2",
        "    b .La64cr_dp_flag_dispatch",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_thumb_alu_high)",
        "A64S_CSYM(a64_compact_raw_profile_thumb_alu_high):",
        ".La64cr_thumb_top4:",
        "    tbnz w9, #11, .La64cr_thumb_pc_load",
        "    tbnz w9, #10, .La64cr_thumb_high",
        ".La64cr_thumb_alu:",
        "    and w13, w9, #7",
        "    ldr w11, [x19, w13, uxtw #2]",
        "    ubfx w10, w9, #3, #3",
        "    ldr w10, [x19, w10, uxtw #2]",
        "    ldr w8, [x20]",
        "    ubfx w8, w8, #29, #1",
        "    ubfx w14, w9, #6, #4",
        "    add x15, x17, #176",
        "    ldrsw x14, [x15, w14, uxtw #2]",
        "    add x15, x15, x14",
        "    br x15",
        ".La64cr_thumb_alu_and:",
        "    mov w14, #0",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_eor:",
        "    mov w14, #1",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_adc:",
        "    mov w14, #5",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_sbc:",
        "    mov w14, #6",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_tst:",
        "    mov w14, #8",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_neg:",
        "    mov w11, w10",
        "    mov w10, wzr",
        "    mov w14, #3",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_cmp:",
        "    mov w14, #10",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_cmn:",
        "    mov w14, #11",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_orr:",
        "    mov w14, #12",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_bic:",
        "    mov w14, #14",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_mvn:",
        "    mov w14, #15",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_alu_mul:",
        "    mul w10, w11, w10",
        "    str w10, [x19, w13, uxtw #2]",
        "    ands wzr, w10, w10",
        "    mrs x15, nzcv",
        "    ldr w14, [x20]",
        "    and w15, w15, #0xc0000000",
        "    and w8, w14, #0x30000000",
        "    orr w15, w15, w8",
        "    ubfx w15, w15, #28, #4",
        "    bfi w14, w15, #28, #4",
        "    str w14, [x20]",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        "",
        ".La64cr_thumb_alu_lsl:",
        "    and w15, w10, #0xff",
        "    mov w10, w11",
        "    cbz w15, .La64cr_thumb_shift_reg_ready",
        "    cmp w15, #32",
        "    b.eq .La64cr_thumb_lsl_reg_32",
        "    b.hi .La64cr_thumb_shift_reg_zero",
        "    mov w14, #32",
        "    sub w14, w14, w15",
        "    lsrv w14, w10, w14",
        "    and w8, w14, #1",
        "    lslv w10, w10, w15",
        "    b .La64cr_thumb_shift_reg_ready",
        ".La64cr_thumb_lsl_reg_32:",
        "    and w8, w10, #1",
        "    mov w10, wzr",
        "    b .La64cr_thumb_shift_reg_ready",
        ".La64cr_thumb_shift_reg_zero:",
        "    mov w8, wzr",
        "    mov w10, wzr",
        "    b .La64cr_thumb_shift_reg_ready",
        ".La64cr_thumb_alu_lsr:",
        "    and w15, w10, #0xff",
        "    mov w10, w11",
        "    cbz w15, .La64cr_thumb_shift_reg_ready",
        "    cmp w15, #32",
        "    b.eq .La64cr_thumb_lsr_reg_32",
        "    b.hi .La64cr_thumb_shift_reg_zero",
        "    sub w14, w15, #1",
        "    lsrv w14, w10, w14",
        "    and w8, w14, #1",
        "    lsrv w10, w10, w15",
        "    b .La64cr_thumb_shift_reg_ready",
        ".La64cr_thumb_lsr_reg_32:",
        "    lsr w8, w10, #31",
        "    mov w10, wzr",
        "    b .La64cr_thumb_shift_reg_ready",
        ".La64cr_thumb_alu_asr:",
        "    and w15, w10, #0xff",
        "    mov w10, w11",
        "    cbz w15, .La64cr_thumb_shift_reg_ready",
        "    cmp w15, #32",
        "    b.hs .La64cr_thumb_asr_reg_32",
        "    sub w14, w15, #1",
        "    lsrv w14, w10, w14",
        "    and w8, w14, #1",
        "    asrv w10, w10, w15",
        "    b .La64cr_thumb_shift_reg_ready",
        ".La64cr_thumb_asr_reg_32:",
        "    lsr w8, w10, #31",
        "    asr w10, w10, #31",
        "    b .La64cr_thumb_shift_reg_ready",
        ".La64cr_thumb_alu_ror:",
        "    and w15, w10, #0xff",
        "    mov w10, w11",
        "    cbz w15, .La64cr_thumb_shift_reg_ready",
        "    and w14, w15, #31",
        "    cbz w14, .La64cr_thumb_ror_reg_32",
        "    rorv w10, w10, w14",
        "    lsr w8, w10, #31",
        "    b .La64cr_thumb_shift_reg_ready",
        ".La64cr_thumb_ror_reg_32:",
        "    lsr w8, w10, #31",
        ".La64cr_thumb_shift_reg_ready:",
        "    mov w14, #13",
        "    b .La64cr_dp_flag_dispatch",
        "",
        ".La64cr_thumb_high:",
        "    ubfx w14, w9, #8, #2",
        "    cmp w14, #3",
        "    b.eq .La64cr_fallback",
        "    and w13, w9, #7",
        "    ubfx w10, w9, #7, #1",
        "    orr w13, w13, w10, lsl #3",
        "    cmp w13, #15",
        "    b.ne .La64cr_thumb_high_rd_valid",
        "    cmp w14, #1",
        "    b.ne .La64cr_fallback",
        ".La64cr_thumb_high_rd_valid:",
        "    ubfx w10, w9, #3, #4",
        "    cmp w10, #15",
        "    b.eq .La64cr_thumb_high_rm_pc",
        "    ldr w10, [x19, w10, uxtw #2]",
        "    b .La64cr_thumb_high_rm_ready",
        ".La64cr_thumb_high_rm_pc:",
        "    add w10, w26, #4",
        ".La64cr_thumb_high_rm_ready:",
        "    cmp w14, #2",
        "    b.eq .La64cr_thumb_high_mov",
        "    cmp w13, #15",
        "    b.eq .La64cr_thumb_high_rn_pc",
        "    ldr w11, [x19, w13, uxtw #2]",
        "    b .La64cr_thumb_high_rn_ready",
        ".La64cr_thumb_high_rn_pc:",
        "    add w11, w26, #4",
        ".La64cr_thumb_high_rn_ready:",
        "    cmp w14, #1",
        "    b.eq .La64cr_thumb_high_cmp",
        "    mov w14, #4",
        "    b .La64cr_dp_plain_dispatch",
        ".La64cr_thumb_high_cmp:",
        "    mov w14, #10",
        "    b .La64cr_dp_flag_dispatch",
        ".La64cr_thumb_high_mov:",
        "    mov w14, #13",
        "    b .La64cr_dp_plain_dispatch",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_thumb_memory_form)",
        "A64S_CSYM(a64_compact_raw_profile_thumb_memory_form):",
        ".La64cr_thumb_pc_load:",
        "    add w10, w26, #4",
        "    bic w10, w10, #3",
        "    and w11, w9, #0xff",
        "    add w10, w10, w11, lsl #2",
        "    ubfx w13, w9, #8, #3",
        "    mov w14, #0",
        "    mov w15, #1",
        "    b .La64cr_thumb_memory",
        "",
        ".La64cr_thumb_memory_reg:",
        "    and w13, w9, #7",
        "    ubfx w10, w9, #3, #3",
        "    ldr w10, [x19, w10, uxtw #2]",
        "    ubfx w11, w9, #6, #3",
        "    ldr w11, [x19, w11, uxtw #2]",
        "    add w10, w10, w11",
        "    ubfx w12, w9, #9, #3",
        "    cmp w12, #3",
        "    b.hs .La64cr_thumb_memory_reg_load",
        "    mov w15, wzr",
        "    cbz w12, .La64cr_thumb_memory_word",
        "    cmp w12, #1",
        "    mov w14, #2",
        "    b.eq .La64cr_thumb_memory",
        "    mov w14, #1",
        "    b .La64cr_thumb_memory",
        ".La64cr_thumb_memory_reg_load:",
        "    mov w15, #1",
        "    cmp w12, #3",
        "    b.eq .La64cr_thumb_memory_signed_byte",
        "    cmp w12, #4",
        "    b.eq .La64cr_thumb_memory_word",
        "    cmp w12, #5",
        "    b.eq .La64cr_thumb_memory_half",
        "    cmp w12, #6",
        "    b.eq .La64cr_thumb_memory_byte",
        "    mov w14, #4",
        "    b .La64cr_thumb_memory",
        ".La64cr_thumb_memory_signed_byte:",
        "    mov w14, #3",
        "    b .La64cr_thumb_memory",
        "",
        ".La64cr_thumb_memory_imm:",
        "    and w13, w9, #7",
        "    ubfx w10, w9, #3, #3",
        "    ldr w10, [x19, w10, uxtw #2]",
        "    ubfx w11, w9, #6, #5",
        "    tbnz w9, #12, .La64cr_thumb_memory_imm_byte",
        "    add w10, w10, w11, lsl #2",
        "    mov w14, #0",
        "    b .La64cr_thumb_memory_imm_direction",
        ".La64cr_thumb_memory_imm_byte:",
        "    add w10, w10, w11",
        "    mov w14, #1",
        ".La64cr_thumb_memory_imm_direction:",
        "    ubfx w15, w9, #11, #1",
        "    b .La64cr_thumb_memory",
        "",
        ".La64cr_thumb_memory_half_imm:",
        "    and w13, w9, #7",
        "    ubfx w10, w9, #3, #3",
        "    ldr w10, [x19, w10, uxtw #2]",
        "    ubfx w11, w9, #6, #5",
        "    add w10, w10, w11, lsl #1",
        "    mov w14, #2",
        "    ubfx w15, w9, #11, #1",
        "    b .La64cr_thumb_memory",
        "",
        ".La64cr_thumb_memory_sp:",
        "    ldr w10, [x19, #52]",
        "    and w11, w9, #0xff",
        "    add w10, w10, w11, lsl #2",
        "    ubfx w13, w9, #8, #3",
        "    mov w14, #0",
        "    ubfx w15, w9, #11, #1",
        "    b .La64cr_thumb_memory",
        "",
        ".La64cr_thumb_memory_word:",
        "    mov w14, #0",
        "    b .La64cr_thumb_memory",
        ".La64cr_thumb_memory_byte:",
        "    mov w14, #1",
        "    b .La64cr_thumb_memory",
        ".La64cr_thumb_memory_half:",
        "    mov w14, #2",
        ".La64cr_thumb_memory:",
        "    bl .La64cr_thumb_memory_access",
        "    cbz w0, .La64cr_fallback",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_thumb_misc)",
        "A64S_CSYM(a64_compact_raw_profile_thumb_misc):",
        ".La64cr_thumb_address:",
        "    ubfx w13, w9, #8, #3",
        "    and w10, w9, #0xff",
        "    lsl w10, w10, #2",
        "    tbz w9, #11, .La64cr_thumb_address_pc",
        "    ldr w11, [x19, #52]",
        "    b .La64cr_thumb_address_ready",
        ".La64cr_thumb_address_pc:",
        "    add w11, w26, #4",
        "    bic w11, w11, #3",
        ".La64cr_thumb_address_ready:",
        "    add w10, w11, w10",
        "    str w10, [x19, w13, uxtw #2]",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        "",
        ".La64cr_thumb_top_b:",
        "    ubfx w10, w9, #8, #8",
        "    cmp w10, #0xb0",
        "    b.eq .La64cr_thumb_sp_adjust",
        "    cmp w10, #0xb2",
        "    b.eq .La64cr_thumb_extend",
        "    mov w10, #0xf600",
        "    and w10, w9, w10",
        "    mov w11, #0xb400",
        "    cmp w10, w11",
        "    b.eq .La64cr_thumb_stack",
        "    b .La64cr_fallback",
        "",
        ".La64cr_thumb_sp_adjust:",
        "    ubfx w10, w9, #8, #8",
        "    and w10, w9, #0x7f",
        "    lsl w10, w10, #2",
        "    ldr w11, [x19, #52]",
        "    tbnz w9, #7, .La64cr_thumb_sp_sub",
        "    add w11, w11, w10",
        "    b .La64cr_thumb_sp_write",
        ".La64cr_thumb_sp_sub:",
        "    sub w11, w11, w10",
        ".La64cr_thumb_sp_write:",
        "    str w11, [x19, #52]",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        "",
        ".La64cr_thumb_extend:",
        "    and w13, w9, #7",
        "    ubfx w10, w9, #3, #3",
        "    ldr w10, [x19, w10, uxtw #2]",
        "    ubfx w11, w9, #6, #2",
        "    cbz w11, .La64cr_thumb_extend_sxth",
        "    cmp w11, #1",
        "    b.eq .La64cr_thumb_extend_sxtb",
        "    cmp w11, #2",
        "    b.eq .La64cr_thumb_extend_uxth",
        "    uxtb w10, w10",
        "    b .La64cr_thumb_extend_write",
        ".La64cr_thumb_extend_uxth:",
        "    uxth w10, w10",
        "    b .La64cr_thumb_extend_write",
        ".La64cr_thumb_extend_sxtb:",
        "    sxtb w10, w10",
        "    b .La64cr_thumb_extend_write",
        ".La64cr_thumb_extend_sxth:",
        "    sxth w10, w10",
        ".La64cr_thumb_extend_write:",
        "    str w10, [x19, w13, uxtw #2]",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        "",
        # Translate PUSH/POP into the already transactional A32 block engine.
        # w28 remains two, so normal fallthrough still retires one Thumb
        # halfword; POP-to-PC uses the shared interworking finish path.
        ".La64cr_thumb_stack:",
        "    and w12, w9, #0xff",
        "    tbnz w9, #11, .La64cr_thumb_pop",
        "    tbz w9, #8, .La64cr_thumb_push_list_ready",
        "    orr w12, w12, #0x4000",
        ".La64cr_thumb_push_list_ready:",
        "    cbz w12, .La64cr_fallback",
        "    mov w10, #0x092d",
        "    lsl w10, w10, #16",
        "    orr w9, w10, w12",
        "    b .La64cr_block",
        ".La64cr_thumb_pop:",
        "    tbz w9, #8, .La64cr_thumb_pop_list_ready",
        "    orr w12, w12, #0x8000",
        ".La64cr_thumb_pop_list_ready:",
        "    cbz w12, .La64cr_fallback",
        "    mov w10, #0x08bd",
        "    lsl w10, w10, #16",
        "    orr w9, w10, w12",
        "    b .La64cr_block",
        "",
        # Thumb STMIA always writes back. LDMIA suppresses writeback when the
        # base is in the list; encoding that distinction into the pseudo A32
        # block word reuses the same preflight/commit path without guessing.
        ".La64cr_thumb_multi:",
        "    and w12, w9, #0xff",
        "    cbz w12, .La64cr_fallback",
        "    ubfx w13, w9, #8, #3",
        "    tbnz w9, #11, .La64cr_thumb_ldmia",
        "    mov w10, #0x08a0",
        "    b .La64cr_thumb_multi_word",
        ".La64cr_thumb_ldmia:",
        "    lsrv w11, w12, w13",
        "    tbnz w11, #0, .La64cr_thumb_ldmia_no_writeback",
        "    mov w10, #0x08b0",
        "    b .La64cr_thumb_multi_word",
        ".La64cr_thumb_ldmia_no_writeback:",
        "    mov w10, #0x0890",
        ".La64cr_thumb_multi_word:",
        "    add w10, w10, w13",
        "    lsl w10, w10, #16",
        "    orr w9, w10, w12",
        "    b .La64cr_block",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_thumb_branch)",
        "A64S_CSYM(a64_compact_raw_profile_thumb_branch):",
        ".La64cr_thumb_bx:",
        "    ubfx w10, w9, #3, #4",
        "    tbnz w9, #7, .La64cr_thumb_bx_link_check",
        "    b .La64cr_thumb_bx_target",
        ".La64cr_thumb_bx_link_check:",
        "    cmp w10, #15",
        "    b.eq .La64cr_fallback",
        ".La64cr_thumb_bx_target:",
        "    cmp w10, #15",
        "    b.eq .La64cr_thumb_bx_pc",
        "    ldr w11, [x19, w10, uxtw #2]",
        "    b .La64cr_thumb_bx_validate",
        ".La64cr_thumb_bx_pc:",
        "    add w11, w26, #4",
        ".La64cr_thumb_bx_validate:",
        "    and w12, w11, #3",
        "    cmp w12, #2",
        "    b.eq .La64cr_fallback",
        "    tbz w9, #7, .La64cr_thumb_bx_state",
        "    add w10, w26, #2",
        "    orr w10, w10, #1",
        "    str w10, [x19, #56]",
        ".La64cr_thumb_bx_state:",
        "    and w10, w11, #1",
        "    ldr w12, [x20]",
        "    bfi w12, w10, #5, #1",
        "    str w12, [x20]",
        "    bic w26, w11, #1",
        "    b .La64cr_retire",
        "",
        ".La64cr_thumb_conditional:",
        "    ubfx w10, w9, #8, #4",
        "    cmp w10, #14",
        "    b.hs .La64cr_fallback",
        "    ldr w11, [x20]",
        "    msr nzcv, x11",
        "    add x15, x17, #120",
        "    ldrsw x14, [x15, w10, uxtw #2]",
        "    add x15, x15, x14",
        "    br x15",
        ".La64cr_thumb_branch_taken:",
        "    sbfx w10, w9, #0, #8",
        "    lsl w10, w10, #1",
        "    add w26, w26, #4",
        "    add w26, w26, w10",
        "    b .La64cr_retire",
        "",
        ".La64cr_thumb_unconditional:",
        "    tbnz w9, #11, .La64cr_thumb_blx_suffix",
        "    sbfx w10, w9, #0, #11",
        "    lsl w10, w10, #1",
        "    add w26, w26, #4",
        "    add w26, w26, w10",
        "    b .La64cr_retire",
        "",
        ".La64cr_thumb_blx_suffix:",
        "    ldr w10, [x19, #56]",
        "    and w11, w9, #0x7ff",
        "    add w10, w10, w11, lsl #1",
        "    bic w10, w10, #3",
        "    add w11, w26, #2",
        "    orr w11, w11, #1",
        "    str w11, [x19, #56]",
        "    ldr w12, [x20]",
        "    bic w12, w12, #0x20",
        "    str w12, [x20]",
        "    mov w26, w10",
        "    b .La64cr_retire",
        "",
        ".La64cr_thumb_long_branch:",
        "    tbnz w9, #11, .La64cr_thumb_bl_suffix",
        "    sbfx w10, w9, #0, #11",
        "    lsl w10, w10, #12",
        "    add w11, w26, #4",
        "    add w11, w11, w10",
        "    str w11, [x19, #56]",
        "    add w26, w26, w28",
        "    b .La64cr_retire",
        ".La64cr_thumb_bl_suffix:",
        "    ldr w10, [x19, #56]",
        "    and w11, w9, #0x7ff",
        "    add w10, w10, w11, lsl #1",
        "    add w11, w26, #2",
        "    orr w11, w11, #1",
        "    str w11, [x19, #56]",
        "    bic w26, w10, #1",
        "    b .La64cr_retire",
        "",
        # Internal memory helper. Inputs are address w10, destination/source
        # register w13, kind w14 (word/byte/half/signed-byte/signed-half), and
        # load flag w15. It validates alignment and the exact live witness
        # before touching guest state, returning one on success and zero on
        # refusal. x16/x17 and all resident architectural state stay pinned.
        ".globl A64S_CSYM(a64_compact_raw_profile_thumb_memory_access)",
        "A64S_CSYM(a64_compact_raw_profile_thumb_memory_access):",
        ".La64cr_thumb_memory_access:",
        "    cmp w14, #4",
        "    b.hi .La64cr_thumb_memory_fail",
        "    cbz w14, .La64cr_thumb_memory_align_word",
        "    cmp w14, #2",
        "    b.eq .La64cr_thumb_memory_align_half",
        "    cmp w14, #4",
        "    b.eq .La64cr_thumb_memory_align_half",
        "    b .La64cr_thumb_memory_select",
        ".La64cr_thumb_memory_align_word:",
        "    tst w10, #3",
        "    b.ne .La64cr_thumb_memory_fail",
        "    b .La64cr_thumb_memory_select",
        ".La64cr_thumb_memory_align_half:",
        "    tst w10, #1",
        "    b.ne .La64cr_thumb_memory_fail",
        ".La64cr_thumb_memory_select:",
        "    ldr x0, [x27, #0]",
        "    cbz x0, .La64cr_thumb_memory_cache",
        "    mov x1, xzr",
        "    ldr w2, [x27, #8]",
        "    and w2, w10, w2",
        "    add x0, x0, w2, uxtw",
        "    b .La64cr_thumb_memory_host",
        ".La64cr_thumb_memory_cache:",
        "    cbz w15, .La64cr_thumb_memory_cache_write",
        "    ldr x0, [x27, #16]",
        "    ldr x1, [x27, #32]",
        "    b .La64cr_thumb_memory_cache_common",
        ".La64cr_thumb_memory_cache_write:",
        "    cmp w14, #2",
        "    b.hi .La64cr_thumb_memory_fail",
        "    ldr x0, [x27, #24]",
        "    ldr x1, [x27, #40]",
        ".La64cr_thumb_memory_cache_common:",
        "    cbz x0, .La64cr_thumb_memory_fail",
        "    cbz x1, .La64cr_thumb_memory_fail",
        "    ldr w2, [x27, #84]",
        "    lsr w3, w10, #10",
        "    add w3, w3, w2, lsl #5",
        "    and w3, w3, #63",
        "    add x0, x0, w3, uxtw #4",
        "    ldr x2, [x0, #0]",
        "    cbz x2, .La64cr_thumb_memory_fail",
        "    lsr w3, w10, #10",
        "    lsl w3, w3, #10",
        "    ldr w4, [x27, #84]",
        "    orr w3, w3, w4",
        "    ldr w4, [x0, #8]",
        "    cmp w4, w3",
        "    b.ne .La64cr_thumb_memory_fail",
        "    ldr w4, [x0, #12]",
        "    ldr w3, [x27, #80]",
        "    cmp w4, w3",
        "    b.ne .La64cr_thumb_memory_fail",
        "    and w3, w10, #0x3ff",
        "    add x0, x2, w3, uxtw",
        ".La64cr_thumb_memory_host:",
        "    cbz w15, .La64cr_thumb_memory_store",
        "    cbz w14, .La64cr_thumb_memory_load_word",
        "    cmp w14, #1",
        "    b.eq .La64cr_thumb_memory_load_byte",
        "    cmp w14, #2",
        "    b.eq .La64cr_thumb_memory_load_half",
        "    cmp w14, #3",
        "    b.eq .La64cr_thumb_memory_load_signed_byte",
        "    ldrsh w2, [x0]",
        "    b .La64cr_thumb_memory_load_commit",
        ".La64cr_thumb_memory_load_word:",
        "    ldr w2, [x0]",
        "    b .La64cr_thumb_memory_load_commit",
        ".La64cr_thumb_memory_load_byte:",
        "    ldrb w2, [x0]",
        "    b .La64cr_thumb_memory_load_commit",
        ".La64cr_thumb_memory_load_half:",
        "    ldrh w2, [x0]",
        "    b .La64cr_thumb_memory_load_commit",
        ".La64cr_thumb_memory_load_signed_byte:",
        "    ldrsb w2, [x0]",
        ".La64cr_thumb_memory_load_commit:",
        "    str w2, [x19, w13, uxtw #2]",
        "    b .La64cr_thumb_memory_count",
        ".La64cr_thumb_memory_store:",
        "    cmp w14, #2",
        "    b.hi .La64cr_thumb_memory_fail",
        "    ldr w2, [x19, w13, uxtw #2]",
        "    cbz w14, .La64cr_thumb_memory_store_word",
        "    cmp w14, #1",
        "    b.eq .La64cr_thumb_memory_store_byte",
        "    strh w2, [x0]",
        "    b .La64cr_thumb_memory_count",
        ".La64cr_thumb_memory_store_word:",
        "    str w2, [x0]",
        "    b .La64cr_thumb_memory_count",
        ".La64cr_thumb_memory_store_byte:",
        "    strb w2, [x0]",
        ".La64cr_thumb_memory_count:",
        "    cbz x1, .La64cr_thumb_memory_success",
        "    ldr x2, [x1]",
        "    add x2, x2, #1",
        "    str x2, [x1]",
        ".La64cr_thumb_memory_success:",
        "    mov w0, #1",
        "    ret",
        ".La64cr_thumb_memory_fail:",
        "    mov w0, wzr",
        "    ret",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_thumb_condition)",
        "A64S_CSYM(a64_compact_raw_profile_thumb_condition):",
        ".La64cr_thumb_cond_eq:",
        "    b.eq .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_ne:",
        "    b.ne .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_cs:",
        "    b.hs .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_cc:",
        "    b.lo .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_mi:",
        "    b.mi .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_pl:",
        "    b.pl .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_vs:",
        "    b.vs .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_vc:",
        "    b.vc .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_hi:",
        "    b.hi .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_ls:",
        "    b.ls .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_ge:",
        "    b.ge .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_lt:",
        "    b.lt .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_gt:",
        "    b.gt .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        ".La64cr_thumb_cond_le:",
        "    b.le .La64cr_thumb_branch_taken",
        "    b .La64cr_condition_skip",
        "",
        # These shared condition stubs are reached by A32 instructions. They
        # used to be folded into the broad Thumb-to-retire address bucket.
        ".globl A64S_CSYM(a64_compact_raw_profile_a32_condition)",
        "A64S_CSYM(a64_compact_raw_profile_a32_condition):",
        ".La64cr_cond_eq:",
        "    b.eq .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_ne:",
        "    b.ne .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_cs:",
        "    b.hs .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_cc:",
        "    b.lo .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_mi:",
        "    b.mi .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_pl:",
        "    b.pl .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_vs:",
        "    b.vs .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_vc:",
        "    b.vc .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_hi:",
        "    b.hi .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_ls:",
        "    b.ls .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_ge:",
        "    b.ge .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_lt:",
        "    b.lt .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_gt:",
        "    b.gt .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        ".La64cr_cond_le:",
        "    b.le .La64cr_condition_pass",
        "    b .La64cr_condition_skip",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_retire)",
        "A64S_CSYM(a64_compact_raw_profile_retire):",
        ".La64cr_retire:",
        "    add w29, w29, #1",
        "    subs w25, w25, #1",
        "    b.ne .La64cr_loop",
        "    b .La64cr_exit",
        "",
        # An explicit experiment may retain eight full User-mode windows that
        # this invocation already received from the exact C callback. A hit
        # changes only the live code pointer/base; generation and privilege
        # cannot change inside an admitted User interval. Missing, privileged,
        # partial or unaligned windows still take the literal callback.
        ".La64cr_window_miss:",
        "    ldr w10, [x27, #172]",
        "    cbz w10, .La64cr_fallback",
        "    ldr w10, [x27, #84]",
        "    cbnz w10, .La64cr_fallback",
        "    lsr w9, w26, #10",
        "    lsl w9, w9, #10",
        "    add x10, x27, #192",
        "    mov w11, #8",
        ".La64cr_window_cache_probe:",
        "    ldr x12, [x10]",
        "    cbz x12, .La64cr_window_cache_next",
        "    ldr w13, [x10, #8]",
        "    cmp w13, w9",
        "    b.eq .La64cr_window_cache_hit",
        ".La64cr_window_cache_next:",
        "    add x10, x10, #16",
        "    subs w11, w11, #1",
        "    b.ne .La64cr_window_cache_probe",
        "    b .La64cr_fallback",
        ".La64cr_window_cache_hit:",
        "    mov x22, x12",
        "    mov w23, w13",
        "    ldr w24, [x10, #12]",
        "    str x22, [x27, #176]",
        "    str w23, [x27, #184]",
        "    str w24, [x27, #188]",
        "    ldr x12, [x27, #160]",
        "    add x12, x12, #1",
        "    str x12, [x27, #160]",
        "    b .La64cr_loop",
        "",
        # Commit native cycles before a fallback because arm_step owns the
        # next instruction's cycle accounting and may inspect the counter.
        # The callback result is 0=no retirement, 1=retire+continue,
        # 2=retire+stop, 3=no-retire+continue. x19-x29 survive the C call by
        # AAPCS64; the two table pointers are caller-saved and are rebuilt only
        # on continuation.
        ".globl A64S_CSYM(a64_compact_raw_profile_fallback)",
        "A64S_CSYM(a64_compact_raw_profile_fallback):",
        ".La64cr_fallback:",
        # The callback must explicitly publish a proven window for the next PC
        # when it asks to continue. Clearing the caller-owned output prevents a
        # stale witness from surviving a buggy or refusing callback.
        "    bl .La64cr_fp_session_restore",
        "    str xzr, [x27, #88]",
        "    str xzr, [x27, #96]",
        "    ldr x9, [x27, #48]",
        "    cbz x9, .La64cr_exit",
        "    str w26, [x19, #60]",
        "    cbz w29, .La64cr_fallback_committed",
        "    ldr x10, [x21]",
        "    add x10, x10, x29",
        "    str x10, [x21]",
        "    ldr x10, [x27, #64]",
        "    add x10, x10, x29",
        "    str x10, [x27, #64]",
        "    mov w29, wzr",
        ".La64cr_fallback_committed:",
        "    ldr x9, [x27, #48]",
        "    ldr x0, [x27, #56]",
        "    add x1, x27, #88",
        "    blr x9",
        "    mov w11, w0",
        "    ldr w26, [x19, #60]",
        "    cbz w11, .La64cr_exit",
        "    cmp w11, #3",
        "    b.eq .La64cr_fallback_reload",
        "    cmp w11, #2",
        "    b.hi .La64cr_exit",
        "    ldr x10, [x27, #72]",
        "    add x10, x10, #1",
        "    str x10, [x27, #72]",
        "    subs w25, w25, #1",
        "    b.eq .La64cr_exit",
        "    cmp w11, #1",
        "    b.ne .La64cr_exit",
        ".La64cr_fallback_reload:",
        # Reload and validate the callback's live fetch witness. The next loop
        # still repeats the exact PC-in-window check; these guards prevent a
        # NULL, unaligned or empty publication from causing a native read.
        "    mov w14, w23",
        "    ldr x22, [x27, #88]",
        "    ldr w23, [x27, #96]",
        "    ldr w24, [x27, #100]",
        "    cbz x22, .La64cr_exit",
        "    tst w23, #3",
        "    b.ne .La64cr_exit",
        "    cmp w24, #4",
        "    b.lo .La64cr_exit",
        "    tst w24, #3",
        "    b.ne .La64cr_exit",
        "    sub w8, w26, w23",
        "    cmp w8, w24",
        "    b.hs .La64cr_exit",
        "    ldr w10, [x20]",
        "    tst w10, #0x20",
        "    mov w10, #3",
        "    mov w12, #1",
        "    csel w10, w12, w10, ne",
        "    tst w8, w10",
        "    b.ne .La64cr_exit",
        # Preserve the exact current window for the C wrapper, then retain a
        # newly reached full 1 KiB User witness in an invocation-local
        # round-robin cache. Same-window interpreter fallbacks do not consume
        # a slot and partial generic windows are never cached.
        "    str x22, [x27, #176]",
        "    str w23, [x27, #184]",
        "    str w24, [x27, #188]",
        "    ldr w8, [x27, #172]",
        "    cbz w8, .La64cr_fallback_reload_tables",
        "    ldr w8, [x27, #84]",
        "    cbnz w8, .La64cr_fallback_reload_tables",
        "    cmp w23, w14",
        "    b.eq .La64cr_fallback_reload_tables",
        "    tst w23, #0x3ff",
        "    b.ne .La64cr_fallback_reload_tables",
        "    cmp w24, #0x400",
        "    b.ne .La64cr_fallback_reload_tables",
        "    ldr w8, [x27, #168]",
        "    and w8, w8, #7",
        "    add x10, x27, #192",
        "    add x10, x10, w8, uxtw #4",
        "    str x22, [x10]",
        "    str w23, [x10, #8]",
        "    str w24, [x10, #12]",
        "    add w8, w8, #1",
        "    and w8, w8, #7",
        "    str w8, [x27, #168]",
        ".La64cr_fallback_reload_tables:",
        "#if defined(__APPLE__)",
        "    adrp x16, .La64cr_dp_table@PAGE",
        "    add x16, x16, .La64cr_dp_table@PAGEOFF",
        "    adrp x17, .La64cr_cond_table@PAGE",
        "    add x17, x17, .La64cr_cond_table@PAGEOFF",
        "#else",
        "    adrp x16, .La64cr_dp_table",
        "    add x16, x16, :lo12:.La64cr_dp_table",
        "    adrp x17, .La64cr_cond_table",
        "    add x17, x17, :lo12:.La64cr_cond_table",
        "#endif",
        "    b .La64cr_loop",
        "",
        ".globl A64S_CSYM(a64_compact_raw_profile_exit)",
        "A64S_CSYM(a64_compact_raw_profile_exit):",
        ".La64cr_exit:",
        # No caller or C callback may observe the internal RunFast FP mode.
        "    bl .La64cr_fp_session_restore",
        "    cbz w29, .La64cr_exit_committed",
        "    ldr x9, [x21]",
        "    add x9, x9, x29",
        "    str x9, [x21]",
        "    ldr x10, [x27, #64]",
        "    add x10, x10, x29",
        "    str x10, [x27, #64]",
        "    mov w29, wzr",
        ".La64cr_exit_committed:",
        "    ldr w0, [sp, #96]",
        "    sub w0, w0, w25",
        "    str w26, [x19, #60]",
        "    ldp x27, x28, [sp, #80]",
        "    ldp x25, x26, [sp, #64]",
        "    ldp x23, x24, [sp, #48]",
        "    ldp x21, x22, [sp, #32]",
        "    ldp x19, x20, [sp, #16]",
        "    ldp x29, x30, [sp], #112",
        "    ret",
        ".globl A64S_CSYM(a64_compact_raw_profile_end)",
        "A64S_CSYM(a64_compact_raw_profile_end):",
        "#if !defined(__APPLE__)",
        ".size A64S_CSYM(a64_compact_raw_execute), .-A64S_CSYM(a64_compact_raw_execute)",
        "#endif",
        "",
        ".p2align 2",
        ".La64cr_dp_table:",
        "    .long .La64cr_dp_and - .La64cr_dp_table",
        "    .long .La64cr_dp_eor - .La64cr_dp_table",
        "    .long .La64cr_dp_sub - .La64cr_dp_table",
        "    .long .La64cr_dp_rsb - .La64cr_dp_table",
        "    .long .La64cr_dp_add - .La64cr_dp_table",
        "    .long .La64cr_dp_adc - .La64cr_dp_table",
        "    .long .La64cr_dp_sbc - .La64cr_dp_table",
        "    .long .La64cr_dp_rsc - .La64cr_dp_table",
        "    .long .La64cr_fallback - .La64cr_dp_table",
        "    .long .La64cr_fallback - .La64cr_dp_table",
        "    .long .La64cr_fallback - .La64cr_dp_table",
        "    .long .La64cr_fallback - .La64cr_dp_table",
        "    .long .La64cr_dp_orr - .La64cr_dp_table",
        "    .long .La64cr_dp_mov - .La64cr_dp_table",
        "    .long .La64cr_dp_bic - .La64cr_dp_table",
        "    .long .La64cr_dp_mvn - .La64cr_dp_table",
        "",
        ".La64cr_dp_flag_table:",
        "    .long .La64cr_dp_flag_and - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_eor - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_sub - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_rsb - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_add - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_adc - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_sbc - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_rsc - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_tst - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_teq - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_cmp - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_cmn - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_orr - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_mov - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_bic - .La64cr_dp_flag_table",
        "    .long .La64cr_dp_flag_mvn - .La64cr_dp_flag_table",
        "",
        ".p2align 2",
        ".La64cr_cond_table:",
        "    .long .La64cr_cond_eq - .La64cr_cond_table",
        "    .long .La64cr_cond_ne - .La64cr_cond_table",
        "    .long .La64cr_cond_cs - .La64cr_cond_table",
        "    .long .La64cr_cond_cc - .La64cr_cond_table",
        "    .long .La64cr_cond_mi - .La64cr_cond_table",
        "    .long .La64cr_cond_pl - .La64cr_cond_table",
        "    .long .La64cr_cond_vs - .La64cr_cond_table",
        "    .long .La64cr_cond_vc - .La64cr_cond_table",
        "    .long .La64cr_cond_hi - .La64cr_cond_table",
        "    .long .La64cr_cond_ls - .La64cr_cond_table",
        "    .long .La64cr_cond_ge - .La64cr_cond_table",
        "    .long .La64cr_cond_lt - .La64cr_cond_table",
        "    .long .La64cr_cond_gt - .La64cr_cond_table",
        "    .long .La64cr_cond_le - .La64cr_cond_table",
        "",
        # Keep these tables contiguous with the fourteen-entry ARM condition
        # table: the resident loop pins only that base in x17.
        ".La64cr_thumb_top_table:",
        "    .long .La64cr_thumb_shift_imm - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_top1 - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_immediate - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_immediate - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_top4 - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_memory_reg - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_memory_imm - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_memory_imm - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_memory_half_imm - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_memory_sp - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_address - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_top_b - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_multi - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_conditional - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_unconditional - .La64cr_thumb_top_table",
        "    .long .La64cr_thumb_long_branch - .La64cr_thumb_top_table",
        "",
        ".La64cr_thumb_cond_table:",
        "    .long .La64cr_thumb_cond_eq - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_ne - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_cs - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_cc - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_mi - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_pl - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_vs - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_vc - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_hi - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_ls - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_ge - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_lt - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_gt - .La64cr_thumb_cond_table",
        "    .long .La64cr_thumb_cond_le - .La64cr_thumb_cond_table",
        "",
        ".La64cr_thumb_alu_table:",
        "    .long .La64cr_thumb_alu_and - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_eor - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_lsl - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_lsr - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_asr - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_adc - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_sbc - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_ror - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_tst - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_neg - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_cmp - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_cmn - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_orr - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_mul - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_bic - .La64cr_thumb_alu_table",
        "    .long .La64cr_thumb_alu_mvn - .La64cr_thumb_alu_table",
        "",
    ]


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
        "    stp x29, x30, [sp, #-176]!",
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
        # 176-byte prologue moves them to [sp,#176] and [sp,#184]. The former
        # carries DREAD/DWRITE/VFP live-state pointers; the latter is NULL for
        # the legacy boundary or carries the persistent product-chain context.
        "    ldr x3, [sp, #176]",
        "    ldr x9, [sp, #184]",
        "    str x9, [sp, #96]",
        "    str x3, [sp, #128]",
        # Lazy host-FP session state. Offset 144 is the active word; 152 and
        # 160 preserve every caller-visible FPCR/FPSR bit until the common
        # epilogue or an external callback boundary.
        "    str wzr, [sp, #144]",
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
        ".La64s_fp_session_begin:",
        "    ldr w4, [sp, #144]",
        "    cbnz w4, .La64s_fp_session_begin_done",
        "    mrs x4, fpcr",
        "    mrs x5, fpsr",
        "    str x4, [sp, #152]",
        "    str x5, [sp, #160]",
        "    cbz x4, .La64s_fp_session_fpcr_ready",
        "    msr fpcr, xzr",
        ".La64s_fp_session_fpcr_ready:",
        "    mov w4, #1",
        "    str w4, [sp, #144]",
        ".La64s_fp_session_begin_done:",
        "    ret",
        "",
        ".La64s_fp_session_restore:",
        "    ldr w9, [sp, #144]",
        "    cbz w9, .La64s_fp_session_restore_done",
        "    ldr x10, [sp, #160]",
        "    msr fpsr, x10",
        "    ldr x10, [sp, #152]",
        "    msr fpcr, x10",
        "    str wzr, [sp, #144]",
        ".La64s_fp_session_restore_done:",
        "    ret",
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
        # A direct miss may call back into C to publish the exact partial
        # prefix. Restore the caller's FP environment before crossing that
        # external boundary; the common save path sees an inactive session.
        "    bl .La64s_fp_session_restore",
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
        # The callback-backed chain selector is ordinary C. End the lazy FP
        # session before the call; a later arithmetic head will reopen it.
        "    bl .La64s_fp_session_restore",
        "    ldr x0, [sp, #96]",
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
        # Account the block that just completed before selecting another. Each
        # head is bounded to sixteen and the total budget to 256, but validate
        # before adding so a corrupted descriptor can only stop, never expand
        # caller retirement.
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
        "    bl .La64s_fp_session_restore",
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
        "    ldp x29, x30, [sp], #176",
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
    lines.extend(compact_raw_function())
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
