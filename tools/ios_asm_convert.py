#!/usr/bin/env python3
"""Lift ILP32 guest accesses into a 4 GB aligned iOS arena.

The compiler reserves x15 (scratch) and x27 (arena base). Disk pointers and
structure layouts remain 32-bit. Only memory accesses and indirect branches
are widened. Direct branches and PC-relative references keep their offsets.
The host enters with x27 set, and preserves it across every host call.
"""

import re
import sys
from pathlib import Path

from guest_asm_convert import Converter, ConvertError


def lift(assembly: str) -> str:
    """Rewrite ELF assembly, rejecting unsupported addressing forms."""
    output = []
    for number, line in enumerate(assembly.splitlines(), 1):
        text = line.strip()
        if not text or text.startswith(".") or text.endswith(":"):
            output.append(line)
            continue
        if re.search(r"\b[wx](15|27)\b", text) and not (re.match(r"(?:stp|ldp) .*\[sp", text) and "x15" not in text):
            raise ConvertError(f"line {number}: reserved register used: {text}")
        op, _, args = text.partition(" ")
        if op in ("adr", "adrp"):
            # The ILP32 backend assumes symbol addresses are zero-extended.
            # Actual PC-relative addresses include the native arena bias.
            register = args.split(",", 1)[0].strip()
            output.extend([line, f"\tmov w{register[1:]}, w{register[1:]}"])
            continue
        if op in ("br", "blr"):
            if not re.fullmatch(r"x\d+", args.strip()):
                raise ConvertError(f"line {number}: unsupported branch: {text}")
            output.extend([f"\tmov w15, w{args.strip()[1:]}", f"\torr x15, x15, x27", f"\t{op} x15"])
            continue
        match = re.search(r"\[(x\d+|sp)([^\]]*)\](!?)(.*)$", args)
        if not match:
            output.append(line)
            continue
        base, offset, pre, tail = match.groups()
        if base == "sp":
            output.append(line)
            continue
        # Clang emits immediate pre/post increments. Preserve the original
        # register's representation and the instruction's flag behavior.
        update = None
        if pre:
            increment = offset.lstrip(", ")
            if not re.fullmatch(r"#-?\d+", increment):
                raise ConvertError(f"line {number}: unsupported pre-index: {text}")
            update = increment
        elif tail.strip():
            increment = tail.lstrip(", ")
            if not re.fullmatch(r"#-?\d+", increment):
                raise ConvertError(f"line {number}: unsupported post-index: {text}")
            update = increment
        output.append(f"\tmov w15, w{base[1:]}")
        output.append("\torr x15, x15, x27")
        rewritten = args[:match.start()] + "[x15" + offset + "]" + pre + tail
        output.append(f"\t{op} {rewritten}")
        if update:
            value = int(update[1:])
            output.append(f"\t{'sub' if value < 0 else 'add'} {base}, {base}, #{abs(value)}")
    return "\n".join(output) + "\n"


def main():
    """Convert the compiler's Darwin assembly and lift guest addressing."""
    source, destination = map(Path, sys.argv[1:])
    destination.write_text(lift(Converter(source.read_text().splitlines()).run()))


if __name__ == "__main__":
    main()
