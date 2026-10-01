#!/usr/bin/env python3
"""Rewrite MSVC-era C so that `long` keeps its 32-bit meaning on LP64 hosts.

MSVC is LLP64: `long` is 32 bits even on 64-bit Windows. The game, its port
layer and the Xbox SDK declarations rely on that everywhere (tag and
game-state structures, file formats, DWORD/LONG/ULONG). 64-bit macOS and
Linux are LP64, where `long` is 64 bits, so the 64-bit builds compile a copy
of those sources in which every 32-bit `long` is spelled `int`:

    long, long int, signed long   -> int, int, signed int
    unsigned long [int]           -> unsigned int
    long long, long double        -> unchanged

and, in string literals, the printf length modifier that went with it:

    %ld, %lu, %lx, ...            -> %d, %u, %x, ...   (%lld, %ls, %lf unchanged)

Comments and character literals are left untouched, and the rewrite keeps
every line where it was: a `#line` directive names the original file, so
diagnostics and __FILE__ point at the source, not the copy. The sources in
the repository keep `long`, which the 32-bit builds and the byte-matching
build compile as they always have.

Usage:
    lp64_rewrite.py --output OUT INPUT     rewrite INPUT into OUT
    lp64_rewrite.py --check FILE...        exit 1 if any file would change
"""

import argparse
import re
import sys
from pathlib import Path

# comments, string/char literals, identifiers; everything else passes through
TOKEN = re.compile(
    r"""(?P<comment>/\*.*?\*/|//[^\n]*)|(?P<string>"(?:\\.|[^"\\\n])*")|(?P<char>'(?:\\.|[^'\\\n])*')"""
    r"""|(?P<word>[A-Za-z_]\w*)""",
    re.S,
)
SPACE = re.compile(r"(?:\s|/\*.*?\*/|\\\n)*", re.S)
WORD = re.compile(r"[A-Za-z_]\w*")
# a printf conversion with the `l` length modifier on an integer conversion
# (`%%` is matched first so that it is skipped)
FORMAT = re.compile(r"%%|(%[-+ #0']*(?:\*|\d+)?(?:\.(?:\*|\d+))?)l(?!l)([diouxX])")


def rewrite_format(literal: str) -> str:
    return FORMAT.sub(lambda m: m.group(0) if m.group(0) == "%%" else m.group(1) + m.group(2), literal)


def rewrite(text: str) -> str:
    out = []
    pos = 0
    second_half_end = -1
    for match in TOKEN.finditer(text):
        if match.group("string"):
            new = rewrite_format(match.group("string"))
            if new != match.group("string"):
                out.append(text[pos:match.start()])
                out.append(new)
                pos = match.end()
            continue
        if match.group("word") != "long":
            continue
        if match.start() < second_half_end:
            # second half of `long long`
            continue
        after = SPACE.match(text, match.end()).end()
        following = WORD.match(text, after)
        following = following.group(0) if following else None
        if following == "long":
            second_half_end = after + len("long")
            continue
        if following == "double":
            continue
        out.append(text[pos:match.start()])
        if following == "int":
            # `long int` -> `int`: drop `long` and the space after it
            pos = after
        else:
            out.append("int")
            pos = match.end()
    out.append(text[pos:])
    return "".join(out)


def read(path: Path) -> str:
    # latin-1 and newline="" keep every byte outside the rewrites as it was
    with open(path, encoding="latin-1", newline="") as f:
        return f.read()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    if args.output:
        if len(args.files) != 1:
            parser.error("--output takes one input")
        source = args.files[0]
        text = f'#line 1 "{source.as_posix()}"\n' + rewrite(read(source))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        # leave an unchanged copy alone, so that ninja's restat skips its users
        if not args.output.is_file() or read(args.output) != text:
            with open(args.output, "w", encoding="latin-1", newline="") as f:
                f.write(text)
        return
    changed = [path for path in args.files if rewrite(read(path)) != read(path)]
    for path in changed:
        print(path)
    if args.check and changed:
        sys.exit(1)


if __name__ == "__main__":
    main()
