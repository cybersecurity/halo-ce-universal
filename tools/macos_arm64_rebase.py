#!/usr/bin/env python3
"""Rebase an ILP32 AArch64 guest's memory accesses onto a base register.

The macOS port's native Apple silicon build (port/macos/README.md) runs the
game as arm64_32 code, like the Android port, but an arm64 macOS process
cannot map anything below 4 GB (its __PAGEZERO covers it). The guest's 4 GB
address space is instead a region anywhere in the process, aligned to 4 GB,
whose base the guest code keeps in x28 (compiled with -ffixed-x28 and
-ffixed-x27, which this pass uses as scratch).

Every register a memory access or indirect branch goes through holds either
a guest address (a 32-bit pointer, zero-extended) or a real address inside
the region (from sp, adrp or adr). The low 32 bits of both are the guest
address, so

    x27 = x28 + (w<base>, zero-extended)

is the real address in either case. This pass rewrites the ELF assembly
(after tools/android_asm_convert.py) so that each such access goes through
x27:

    ldr w0, [x1, #8]         ->  add x27, x28, w1, uxtw
                                 ldr w0, [x27, #8]
    ldr x0, [x1, #8]!        ->  add x1, x1, #8 ; add x27, x28, w1, uxtw ; ldr x0, [x27]
    ldr x0, [x1], #8         ->  add x27, x28, w1, uxtw ; ldr x0, [x27] ; add x1, x1, #8
    blr x8                   ->  add x27, x28, w8, uxtw ; blr x27
    ldrb w0, [x1, w2, sxtw]  ->  add x27, x1, w2, sxtw ; add x27, x28, w27, uxtw
                                 ldrb w0, [x27]

Accesses through sp and x29 (the frame pointer) are left alone: guest stacks
live inside the region, so those registers always hold real addresses.

The added instructions lengthen functions, so the guest is compiled with
uncompressed jump tables (-mllvm -aarch64-enable-compress-jump-tables=false):
a table of byte-sized offsets could no longer reach its targets.

Usage: macos_arm64_rebase.py input.s output.s
"""

import re
import sys

BASE = "x28"
SCRATCH = "x27"

# instructions with a [base...] memory operand
MEMORY = re.compile(
    r"^(ldr|ldrb|ldrh|ldrsb|ldrsh|ldrsw|str|strb|strh|ldur|ldurb|ldurh|ldursb|ldursh|ldursw|stur|sturb|sturh|"
    r"ldp|stp|ldpsw|ldnp|stnp|ldtr\w*|sttr\w*|"
    r"ldxr|ldxrb|ldxrh|ldxp|ldaxr|ldaxrb|ldaxrh|ldaxp|stxr|stxrb|stxrh|stxp|stlxr|stlxrb|stlxrh|stlxp|"
    r"ldar|ldarb|ldarh|stlr|stlrb|stlrh|ldapr\w*|ldapur\w*|stlur\w*|ldlar\w*|stllr\w*|"
    r"ld1|ld2|ld3|ld4|st1|st2|st3|st4|ld1r|ld2r|ld3r|ld4r|"
    r"prfm|prfum|"
    r"cas\w*|swp\w*|ld(add|clr|eor|set|smax|smin|umax|umin)\w*|st(add|clr|eor|set|smax|smin|umax|umin)\w*)$"
)
BRANCH = re.compile(r"^(br|blr)$")

# the bracketed operand: [reg] [reg, rest] [reg, rest]! or [reg] followed by
# a post-index operand
BRACKET = re.compile(r"\[\s*(x\d+|sp)\s*(?:,\s*([^\]]*))?\]\s*(!)?")


class RebaseError(Exception):
    pass


def split_instruction(line):
    """(indent, mnemonic, operands) or None for labels, directives, blanks"""
    m = re.match(r"^(\s*)([a-z][a-z0-9.]*)\b(.*)$", line)
    if not m or line.lstrip().startswith("."):
        return None
    return m.group(1), m.group(2), m.group(3).strip()


def immediate_add(register, value_text, indent):
    """add/sub register, register, #value (value_text '#8', '#-16' or a
    register)"""
    text = value_text.strip()
    if text.startswith("#"):
        value = int(text[1:], 0)
        if value == 0:
            return []
        op = "add" if value > 0 else "sub"
        return [f"{indent}{op}\t{register}, {register}, #{abs(value)}"]
    return [f"{indent}add\t{register}, {register}, {text}"]


def rewrite(line):
    parts = split_instruction(line)
    if not parts:
        return [line]
    indent, mnemonic, operands = parts
    indent = indent or "\t"
    base_mnemonic = mnemonic.split(".")[0]
    if BRANCH.match(base_mnemonic):
        register = operands.split()[0].rstrip(",") if operands else ""
        if not re.fullmatch(r"x\d+", register):
            return [line]
        if register in (SCRATCH, BASE):
            raise RebaseError(f"branch through a reserved register: {line.strip()}")
        return [f"{indent}add\t{SCRATCH}, {BASE}, w{register[1:]}, uxtw",
                f"{indent}{mnemonic}\t{SCRATCH}"]
    if not MEMORY.match(base_mnemonic):
        # an instruction this pass does not know that addresses memory
        # through a register would escape the rebasing: refuse it
        if re.search(r"\[\s*x\d+", operands):
            raise RebaseError(f"unknown instruction with a memory operand: {line.strip()}")
        return [line]
    m = BRACKET.search(operands)
    if not m:
        # a literal load (ldr x0, label) or prfm of a label
        return [line]
    register, inside, writeback = m.group(1), m.group(2), m.group(3)
    if register in ("sp", "x29"):
        return [line]
    if register in (SCRATCH, BASE):
        raise RebaseError(f"access through a reserved register: {line.strip()}")
    number = register[1:]
    before = operands[:m.start()]
    after = operands[m.end():]
    post = None
    pm = re.match(r"^\s*,\s*(#-?\w+|x\d+)\s*$", after)
    if pm:
        post = pm.group(1)
        after = ""
    elif after.strip():
        raise RebaseError(f"unexpected operand after the address: {line.strip()}")
    out = []
    if writeback:
        # pre-index: update the register first, then access at it
        if not inside:
            raise RebaseError(f"pre-index without an offset: {line.strip()}")
        out += immediate_add(register, inside, indent)
        out.append(f"{indent}add\t{SCRATCH}, {BASE}, w{number}, uxtw")
        out.append(f"{indent}{mnemonic}\t{before}[{SCRATCH}]")
        return out
    if inside and re.match(r"^[wx]\d+\b", inside.strip()):
        # register offset: LLVM puts the pointer in either register (and an
        # index may be negative), so the two are added first, as arm64_32
        # adds them, then the sum's low 32 bits rebased
        out.append(f"{indent}add\t{SCRATCH}, {register}, {inside.strip()}")
        out.append(f"{indent}add\t{SCRATCH}, {BASE}, w{SCRATCH[1:]}, uxtw")
        out.append(f"{indent}{mnemonic}\t{before}[{SCRATCH}]")
        if post is not None:
            out += immediate_add(register, post, indent)
        return out
    out.append(f"{indent}add\t{SCRATCH}, {BASE}, w{number}, uxtw")
    address = f"[{SCRATCH}, {inside}]" if inside else f"[{SCRATCH}]"
    out.append(f"{indent}{mnemonic}\t{before}{address}")
    if post is not None:
        out += immediate_add(register, post, indent)
    return out


def main():
    source, target = sys.argv[1:3]
    lines = open(source, encoding="utf-8").read().split("\n")
    out = []
    # (tools/android_asm_convert.py has taken the comments out)
    for number, line in enumerate(lines, 1):
        try:
            out.extend(rewrite(line) if line.strip() else [line])
        except RebaseError as error:
            raise SystemExit(f"{source}:{number}: {error}")
    with open(target, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
