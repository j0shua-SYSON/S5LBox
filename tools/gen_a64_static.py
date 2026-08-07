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
EXPECTED_HANDLERS = 26508

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


def compact_raw_function() -> list[str]:
    """Return a small raw-A32 execution loop used only by the feasibility gate.

    Unlike the static handler engine above, this loop consumes live guest
    instruction bytes directly and keeps the architectural PC plus the flat
    RAM base resident across instructions.  It deliberately accepts only a
    narrow, exactly testable subset: conditional data-processing with complete
    immediate/register-immediate-shift NZCV semantics, aligned immediate word
    LDR/STR, and immediate B/BL. An unsupported instruction stops before
    mutation and the return value is the exact retired prefix. Product
    integration is intentionally a later decision.
    """
    return [
        "",
        ".p2align 2",
        ".globl A64S_CSYM(a64_compact_raw_execute)",
        "#if !defined(__APPLE__)",
        ".type A64S_CSYM(a64_compact_raw_execute), %function",
        "#endif",
        "A64S_CSYM(a64_compact_raw_execute):",
        # Eight AAPCS64 arguments. x7 names a caller-owned context containing
        # the flat-RAM oracle, optional resident fallback and exact split
        # counters. Preserve every callee-saved register used by the loop; x29
        # is the native-retirement count not yet committed across a fallback.
        "    stp x29, x30, [sp, #-96]!",
        "    stp x19, x20, [sp, #16]",
        "    stp x21, x22, [sp, #32]",
        "    stp x23, x24, [sp, #48]",
        "    stp x25, x26, [sp, #64]",
        "    stp x27, x28, [sp, #80]",
        "    mov x19, x0",       # architectural register file
        "    mov x20, x1",       # CPSR (read for ADC/SBC/RSC carry)
        "    mov x21, x2",       # cycle counter
        "    mov x22, x3",       # live code bytes at code_base
        "    mov w23, w4",       # code_base
        "    mov w24, w5",       # code_bytes
        "    mov w25, w6",       # remaining instruction budget
        "    mov x27, x7",       # compact raw execution context
        "    mov w28, w6",       # original budget, for exact total prefix
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
        # The code pointer names code_base.  Unsigned subtraction rejects both
        # below-base and past-end PCs; the C contract guarantees a word-sized,
        # word-aligned window.
        "    sub w8, w26, w23",
        "    cmp w8, w24",
        "    b.hs .La64cr_exit",
        "    tst w8, #3",
        "    b.ne .La64cr_exit",
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
        "    add w26, w26, #4",
        "    b .La64cr_retire",
        ".La64cr_condition_pass:",
        "    ubfx w10, w9, #25, #3",
        "    cmp w10, #5",
        "    b.eq .La64cr_branch",
        "    ubfx w10, w9, #26, #2",
        "    cbz w10, .La64cr_dp",
        "    cmp w10, #1",
        "    b.eq .La64cr_memory",
        "    b .La64cr_fallback",
        "",
        ".La64cr_dp:",
        # PC operands/destinations and register-specified shifts remain outside
        # this tranche. Immediate rotation, every immediate register shift and
        # every S/comparison opcode are exact, including shifter carry.
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
        "    tbnz w9, #4, .La64cr_fallback",
        "    and w10, w9, #0xf",
        "    cmp w10, #15",
        "    b.eq .La64cr_fallback",
        "    ldr w10, [x19, w10, uxtw #2]",
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
        "    add w26, w26, #4",
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
        "    add w26, w26, #4",
        "    b .La64cr_retire",
        ".La64cr_dp_arith_flags_write:",
        "    str w10, [x19, w13, uxtw #2]",
        ".La64cr_dp_arith_flags:",
        "    mrs x15, nzcv",
        "    ldr w14, [x20]",
        "    ubfx w15, w15, #28, #4",
        "    bfi w14, w15, #28, #4",
        "    str w14, [x20]",
        "    add w26, w26, #4",
        "    b .La64cr_retire",
        "",
        ".La64cr_memory:",
        # Immediate, pre-indexed, word, no-writeback LDR/STR only.  Requiring
        # alignment avoids depending on the guest SCTLR's legacy rotation
        # policy. Flat runs retain their masked-RAM contract. Resident MMU runs
        # may touch data only through the interpreter-owned DREAD/DWRITE cache:
        # a host pointer, exact VA/privilege tag and current generation are a
        # complete witness for this aligned word. Any miss reaches arm_step()
        # before mutation, so it alone owns walks, faults, MMIO and cache fill.
        "    tbnz w9, #25, .La64cr_fallback",
        "    tbz w9, #24, .La64cr_fallback",
        "    tbnz w9, #22, .La64cr_fallback",
        "    tbnz w9, #21, .La64cr_fallback",
        "    ubfx w11, w9, #16, #4",
        "    cmp w11, #15",
        "    b.eq .La64cr_fallback",
        "    ubfx w13, w9, #12, #4",
        "    cmp w13, #15",
        "    b.eq .La64cr_fallback",
        "    ldr w11, [x19, w11, uxtw #2]",
        "    and w10, w9, #0xfff",
        "    tbz w9, #23, .La64cr_memory_sub",
        "    add w10, w11, w10",
        "    b .La64cr_memory_address",
        ".La64cr_memory_sub:",
        "    sub w10, w11, w10",
        ".La64cr_memory_address:",
        "    tst w10, #3",
        "    b.ne .La64cr_fallback",
        "    ldr x14, [x27, #0]",
        "    cbnz x14, .La64cr_memory_flat",
        # Select both the cache and its architectural hit counter before any
        # guest memory access. A NULL DWRITE pointer is the live frontend-
        # consent refusal installed by the C wrapper.
        "    tbnz w9, #20, .La64cr_memory_cache_read",
        "    ldr x14, [x27, #24]",
        "    ldr x12, [x27, #40]",
        "    b .La64cr_memory_cache_common",
        ".La64cr_memory_cache_read:",
        "    ldr x14, [x27, #16]",
        "    ldr x12, [x27, #32]",
        ".La64cr_memory_cache_common:",
        "    cbz x14, .La64cr_fallback",
        "    cbz x12, .La64cr_fallback",
        # slot = ((va >> 10) + (priv ? 32 : 0)) & 63
        "    ldr w15, [x27, #84]",
        "    lsr w11, w10, #10",
        "    add w11, w11, w15, lsl #5",
        "    and w11, w11, #63",
        "    add x14, x14, w11, uxtw #4",
        "    ldr x15, [x14, #0]",
        "    cbz x15, .La64cr_fallback",
        # tag = 1 KiB-aligned VA | privilege
        "    lsr w11, w10, #10",
        "    lsl w11, w11, #10",
        "    ldr w8, [x27, #84]",
        "    orr w11, w11, w8",
        "    ldr w8, [x14, #8]",
        "    cmp w8, w11",
        "    b.ne .La64cr_fallback",
        "    ldr w8, [x14, #12]",
        "    ldr w11, [x27, #80]",
        "    cmp w8, w11",
        "    b.ne .La64cr_fallback",
        "    and w11, w10, #0x3ff",
        "    add x14, x15, w11, uxtw",
        "    tbnz w9, #20, .La64cr_memory_cache_load",
        "    ldr w11, [x19, w13, uxtw #2]",
        "    str w11, [x14]",
        "    b .La64cr_memory_cache_hit",
        ".La64cr_memory_cache_load:",
        "    ldr w11, [x14]",
        "    str w11, [x19, w13, uxtw #2]",
        ".La64cr_memory_cache_hit:",
        # Match dread_hit()/dwrite_hit(): one hit for a successful direct
        # access; a refused access changes no counter before the literal path.
        "    ldr x11, [x12]",
        "    add x11, x11, #1",
        "    str x11, [x12]",
        "    b .La64cr_memory_done",
        ".La64cr_memory_flat:",
        "    ldr w15, [x27, #8]",
        "    and w10, w10, w15",
        "    tbnz w9, #20, .La64cr_memory_load",
        "    ldr w11, [x19, w13, uxtw #2]",
        "    str w11, [x14, w10, uxtw]",
        "    b .La64cr_memory_done",
        ".La64cr_memory_load:",
        "    ldr w11, [x14, w10, uxtw]",
        "    str w11, [x19, w13, uxtw #2]",
        ".La64cr_memory_done:",
        "    add w26, w26, #4",
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
        ".La64cr_retire:",
        "    add w29, w29, #1",
        "    subs w25, w25, #1",
        "    b.ne .La64cr_loop",
        "    b .La64cr_exit",
        "",
        # Commit native cycles before a fallback because arm_step owns the
        # next instruction's cycle accounting and may inspect the counter.
        # The callback result is 0=no retirement, 1=retire+continue,
        # 2=retire+stop. x19-x29 survive the C call by AAPCS64; the two table
        # pointers are caller-saved and are rebuilt only on continuation.
        ".La64cr_fallback:",
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
        "    blr x9",
        "    mov w11, w0",
        "    ldr w26, [x19, #60]",
        "    cbz w11, .La64cr_exit",
        "    cmp w11, #2",
        "    b.hi .La64cr_exit",
        "    ldr x10, [x27, #72]",
        "    add x10, x10, #1",
        "    str x10, [x27, #72]",
        "    subs w25, w25, #1",
        "    b.eq .La64cr_exit",
        "    cmp w11, #1",
        "    b.ne .La64cr_exit",
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
        ".La64cr_exit:",
        "    cbz w29, .La64cr_exit_committed",
        "    ldr x9, [x21]",
        "    add x9, x9, x29",
        "    str x9, [x21]",
        "    ldr x10, [x27, #64]",
        "    add x10, x10, x29",
        "    str x10, [x27, #64]",
        "    mov w29, wzr",
        ".La64cr_exit_committed:",
        "    sub w0, w28, w25",
        "    str w26, [x19, #60]",
        "    ldp x27, x28, [sp, #80]",
        "    ldp x25, x26, [sp, #64]",
        "    ldp x23, x24, [sp, #48]",
        "    ldp x21, x22, [sp, #32]",
        "    ldp x19, x20, [sp, #16]",
        "    ldp x29, x30, [sp], #96",
        "    ret",
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
