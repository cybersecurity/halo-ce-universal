#!/usr/bin/env python3
"""Find calls that only work under the x86 calling convention.

The decompiled sources sometimes declare a function differently from its
definition (a local prototype in another file, or no prototype at all).
Under 32-bit x86 every argument is a stack slot, so a `long` holding a
float's bits, or an unprototyped call to a variadic function, still works.
The iOS guest (port/ios/README.md) is AArch64 code with Darwin's
calling convention, where floats travel in floating-point registers and
variadic arguments on the stack, so such calls pass garbage.

Compile the sources to LLVM IR with the guest's flags (-S -emit-llvm) and
run this on the .ll files: it compares every external declaration with the
definition and reports the ones whose register classes or variadic-ness
differ. Differences in return type (void/int) or argument count are
reported only with --all, as they are harmless on AArch64.

Usage: guest_abi_check.py [--all] file.ll...
"""

import collections
import re
import sys

ATTRIBUTES = re.compile(
    r"\b(noundef|signext|zeroext|nocapture|readonly|writeonly|nonnull|"
    r"dereferenceable(_or_null)?\(\d+\)|align \d+|noalias|returned|nofree|inreg|"
    r"byval\([^)]*\)|sret\([^)]*\)|captures\([^)]*\)|initializes\([^)]*\)|"
    r"range\([^)]*\)|nofpclass\([^)]*\)|dead_on_unwind|writable)\b")
LINKAGE = re.compile(
    r"\b(dso_local|internal|private|weak|linkonce_odr|hidden|available_externally|"
    r"extern_weak|protected|local_unnamed_addr|unnamed_addr|common)\b")
FUNCTION = re.compile(r"^(declare|define)\s+(.*?)@(\"?[\w.$]+\"?)\((.*)\)(.*)$")


def split_parameters(text):
    parts, depth, current = [], 0, ""
    for character in text:
        if character in "({[<":
            depth += 1
        elif character in ")}]>":
            depth -= 1
        if character == "," and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += character
    if current.strip():
        parts.append(current)
    return parts


def register_class(type_text):
    if type_text == "...":
        return "..."
    if type_text in ("float", "half"):
        return "f"
    if type_text == "double":
        return "d"
    if type_text == "i64":
        return "l"
    if type_text.startswith("i") or type_text == "ptr":
        return "i"
    return type_text


def signature(return_text, parameters):
    classes = []
    for parameter in split_parameters(parameters):
        parameter = ATTRIBUTES.sub("", parameter).strip()
        classes.append(register_class(parameter.split()[0] if parameter else ""))
    words = ATTRIBUTES.sub("", LINKAGE.sub("", return_text)).split()
    returned = words[-1] if words else "void"
    return register_class(returned) if returned != "void" else "v", tuple(classes)


def main():
    arguments = sys.argv[1:]
    show_all = "--all" in arguments
    files = [a for a in arguments if a != "--all"]
    declarations = collections.defaultdict(set)
    definitions = {}
    for path in files:
        with open(path, errors="replace") as f:
            for line in f:
                if not line.startswith(("declare", "define")):
                    continue
                match = FUNCTION.match(line.rstrip())
                if not match:
                    continue
                kind, prefix, name, parameters, _ = match.groups()
                if kind == "define":
                    if re.search(r"\b(internal|private)\b", prefix):
                        continue
                    definitions[name] = (signature(prefix, parameters), path)
                else:
                    declarations[name].add((signature(prefix, parameters), path))
    dangerous = 0
    for name in sorted(declarations):
        if name not in definitions:
            continue
        (defined_return, defined_parameters), defined_in = definitions[name]
        for (declared_return, declared_parameters), declared_in in sorted(declarations[name]):
            if (declared_return, declared_parameters) == (defined_return, defined_parameters):
                continue
            floats = ("f", "d", "...")
            unprototyped = declared_parameters == ("...",)
            if unprototyped:
                risky = any(c in floats for c in defined_parameters) or defined_return in ("f", "d")
            else:
                shared = min(len(declared_parameters), len(defined_parameters))
                risky = any(
                    (a in floats or b in floats) and a != b
                    for a, b in zip(declared_parameters[:shared], defined_parameters[:shared]))
                risky |= (declared_return in ("f", "d")) != (defined_return in ("f", "d")) and \
                    "v" not in (declared_return, defined_return)
            if risky:
                dangerous += 1
            if risky or show_all:
                print(f"{'UNSAFE' if risky else 'benign'} {name}")
                print(f"    defined  {defined_return} {defined_parameters} in {defined_in}")
                print(f"    declared {declared_return} {declared_parameters} in {declared_in}")
    print(f"{dangerous} unsafe declaration(s)")
    sys.exit(1 if dangerous else 0)


if __name__ == "__main__":
    main()
