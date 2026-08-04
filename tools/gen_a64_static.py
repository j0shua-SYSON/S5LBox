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
EXPECTED_HANDLERS = 10271


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


def build_handlers() -> list[tuple[str, list[str]]]:
    handlers: list[tuple[str, list[str]]] = []

    handlers.append((".La64s_end", [
        "    ldur w12, [x13, #-12]",
        "    sub x15, x15, #1",
        "    cbz x15, .La64s_exit",
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

    # A failed ARM condition skips exactly the following semantic record.
    # AL has no guard and cond=0xf is rejected by the decoder.
    for condition in CONDITIONS:
        handlers.append((f".La64s_cond_{condition}", [
            f"    b.{condition} 1f",
            "    add x13, x13, #16",
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
        "    adr x8, .La64s_table",
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
        ".La64s_exit:",
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
        "    mov w0, wzr",
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
