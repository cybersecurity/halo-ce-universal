"""Convert arm64_32 (Apple ILP32) assembly into AArch64 ELF assembly.

The iOS port runs the game as ILP32 code: native AArch64 instructions
with 32-bit pointers, so the game's data formats (which embed 32-bit
pointers, exactly as on the Xbox) keep their layout. Clang only offers such
a target as ``arm64_32-apple-watchos``, which emits Mach-O. The generated
*code* is ordinary AArch64 though; only the assembly syntax, section names,
symbol decoration and relocation operators are Darwin-specific. This script
rewrites them for the ELF assembler, so the objects can be linked by ld.lld
into a guest image. The iOS instrumentation pass lifts its memory accesses
into an aligned native arena before embedding it in the app.

Usage: guest_asm_convert.py input.s output.s
"""

import re
import sys

IDENT = re.compile(r'"[^"]*"|[A-Za-z_.$][\w.$]*')

DROP_DIRECTIVES = {
    ".subsections_via_symbols", ".build_version", ".watchos_version_min",
    ".ios_version_min", ".macosx_version_min", ".tvos_version_min", ".loh",
    ".data_region", ".end_data_region", ".linker_option", ".indirect_symbol",
    ".addrsig", ".addrsig_sym", ".no_dead_strip", ".alt_entry", ".ident",
    ".desc", ".cfi_startproc", ".cfi_endproc", ".cfi_def_cfa",
    ".cfi_offset", ".cfi_def_cfa_offset", ".cfi_def_cfa_register",
    ".cfi_personality", ".cfi_lsda", ".cfi_remember_state",
    ".cfi_restore_state", ".cfi_restore", ".cfi_adjust_cfa_offset",
    ".cfi_same_value", ".cfi_escape",
}

RODATA_SECTIONS = {
    "__cstring", "__const", "__literal4", "__literal8", "__literal16",
    "__ustring",
}


class ConvertError(Exception):
    pass


def strip_comment(line: str) -> str:
    out = []
    in_string = False
    i = 0
    while i < len(line):
        c = line[i]
        if in_string:
            out.append(c)
            if c == "\\" and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
        else:
            if c == '"':
                in_string = True
            elif c == ";" or line.startswith("//", i):
                break
            out.append(c)
        i += 1
    return "".join(out).rstrip()


class Converter:
    def __init__(self, lines):
        self.lines = lines
        self.local_labels = set()
        self.out = []
        self.in_dropped_section = False
        for line in lines:
            text = strip_comment(line).strip()
            m = re.match(r'^([A-Za-z_.$][\w.$]*|"[^"]*"):', text)
            if m and m.group(1)[0] in "lL":
                self.local_labels.add(m.group(1))

    def sym(self, name: str) -> str:
        if name.startswith('"'):
            inner = name[1:-1]
            if inner.startswith("_"):
                inner = inner[1:]
            return '"' + inner + '"'
        if name.startswith("_"):
            return name[1:]
        if name in self.local_labels:
            return ".L" + name
        return name

    def operands(self, text: str) -> str:
        # relocation operators first
        text = re.sub(r'([\w.$"]+)@GOTPAGE\b', r"\1", text)
        text = re.sub(r'([\w.$"]+)@PAGE\b', r"\1", text)
        text = re.sub(r'(\([^)]*\)|[\w.$"]+(?:[+-]\d+)?)@PAGEOFF\b', r":lo12:\1", text)
        if "@" in text and not re.search(r'"[^"]*@[^"]*"', text):
            raise ConvertError("unsupported relocation operator: " + text)

        def repl(m):
            tok = m.group(0)
            if tok.startswith(".") and not tok.startswith(".L"):
                return tok
            if tok.startswith("lo12"):
                return tok
            return self.sym(tok)
        # rename identifiers outside :lo12: markers
        parts = re.split(r'(:lo12:)', text)
        return "".join(p if p == ":lo12:" else IDENT.sub(repl, p) for p in parts)

    def emit(self, s: str):
        self.out.append(s)

    def section(self, segment: str, name: str):
        self.in_dropped_section = False
        if segment == "__DWARF" or segment == "__LD" or name in ("__compact_unwind", "__eh_frame"):
            self.in_dropped_section = True
            return
        if name == "__guest_header":
            self.emit('\t.section .guest_header,"a",@progbits')
        elif name == "__mod_init_func":
            self.emit('\t.section .guest_init,"aw",@progbits')
        elif name == "__mod_term_func":
            self.emit('\t.section .guest_fini,"aw",@progbits')
        elif segment == "__TEXT" and name == "__text":
            self.emit("\t.text")
        elif segment == "__TEXT" and name in RODATA_SECTIONS:
            self.emit('\t.section .rodata,"a",@progbits')
        elif segment == "__TEXT":
            self.emit("\t.text")
        elif name in ("__bss", "__common"):
            self.emit("\t.bss")
        else:
            self.emit("\t.data")

    def zerofill(self, args):
        parts = [a.strip() for a in args.split(",")]
        if len(parts) < 4:
            return
        segment, name, symbol, size = parts[:4]
        align = int(parts[4]) if len(parts) > 4 else 0
        symbol = self.sym(symbol)
        # a definition in either section: tentative definitions (-fcommon)
        # are emitted as .comm, and __common here only holds zero-initialized
        # ones
        self.emit("\t.pushsection .bss")
        self.emit(f"\t.p2align {align}")
        self.emit(f"{symbol}:")
        self.emit(f"\t.zero {size}")
        self.emit("\t.popsection")

    def convert_line(self, raw: str):
        text = strip_comment(raw)
        stripped = text.strip()
        if not stripped:
            return
        # labels
        m = re.match(r'^([A-Za-z_.$][\w.$]*|"[^"]*"):(.*)$', stripped)
        if m:
            if not self.in_dropped_section:
                self.emit(self.sym(m.group(1)) + ":")
            rest = m.group(2).strip()
            if rest:
                self.convert_line(rest)
            return
        # symbol assignment (aliases)
        m = re.match(r'^([A-Za-z_.$][\w.$]*)\s*=\s*(.+)$', stripped)
        if m:
            if not self.in_dropped_section:
                self.emit(f"\t.set {self.sym(m.group(1))}, {self.operands(m.group(2))}")
            return
        parts = stripped.split(None, 1)
        op = parts[0]
        args = parts[1] if len(parts) > 1 else ""

        if op == ".section":
            seg, _, rest = args.partition(",")
            name = rest.split(",")[0].strip()
            self.section(seg.strip(), name)
            return
        shorthand = {
            ".text": ("__TEXT", "__text"), ".data": ("__DATA", "__data"),
            ".const": ("__TEXT", "__const"), ".cstring": ("__TEXT", "__cstring"),
            ".literal4": ("__TEXT", "__literal4"), ".literal8": ("__TEXT", "__literal8"),
            ".literal16": ("__TEXT", "__literal16"), ".const_data": ("__DATA", "__const"),
            ".static_data": ("__DATA", "__data"), ".mod_init_func": ("__DATA", "__mod_init_func"),
            ".mod_term_func": ("__DATA", "__mod_term_func"),
        }
        if op in shorthand:
            self.section(*shorthand[op])
            return
        if op == ".zerofill":
            self.zerofill(args)
            return
        if self.in_dropped_section:
            return
        if op in DROP_DIRECTIVES or op.endswith("_version_min"):
            return
        if op in (".tbss", ".tlv", ".tdata") or "@TLVP" in args:
            raise ConvertError("thread-local storage is not supported: " + stripped)
        if op == ".globl":
            self.emit(f"\t.globl {self.sym(args.strip())}")
            return
        if op == ".private_extern":
            s = self.sym(args.strip())
            self.emit(f"\t.hidden {s}")
            return
        if op in (".weak_definition", ".weak_reference", ".weak_def_can_be_hidden", ".weak"):
            self.emit(f"\t.weak {self.sym(args.strip())}")
            return
        if op == ".comm" or op == ".lcomm":
            a = [x.strip() for x in args.split(",")]
            s = self.sym(a[0])
            align = 1 << int(a[2]) if len(a) > 2 else 1
            if op == ".lcomm":
                self.emit(f"\t.local {s}")
            self.emit(f"\t.comm {s},{a[1]},{align}")
            return
        if op == ".align":
            self.emit(f"\t.p2align {args}")
            return
        if op.startswith("."):
            self.emit(f"\t{op} {self.operands(args)}" if args else f"\t{op}")
            return
        # instructions
        if "@GOTPAGEOFF" in args:
            # ldr wN/xN, [xM, sym@GOTPAGEOFF] -> add xN, xM, :lo12:sym (the
            # image is linked statically below 4 GB, so every GOT load can be
            # relaxed to the address itself)
            gm = re.match(r'^\s*([wx])(\d+|zr)\s*,\s*\[\s*(x\d+|sp)\s*,\s*([\w.$"]+)@GOTPAGEOFF\s*\]\s*$', args)
            if op != "ldr" or not gm:
                raise ConvertError("unsupported GOT access: " + stripped)
            self.emit(f"\tadd x{gm.group(2)}, {gm.group(3)}, :lo12:{self.sym(gm.group(4))}")
            return
        self.emit(f"\t{op} {self.operands(args)}" if args else f"\t{op}")

    def run(self):
        for i, raw in enumerate(self.lines):
            try:
                self.convert_line(raw)
            except ConvertError as e:
                raise ConvertError(f"line {i + 1}: {e}") from None
        # a weak symbol is already global; a second directive only warns
        weak = {line.split(None, 1)[1] for line in self.out if line.startswith("\t.weak ")}
        out = [line for line in self.out
               if not (line.startswith("\t.globl ") and line.split(None, 1)[1] in weak)]
        return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    with open(sys.argv[1], "r", encoding="utf-8", errors="surrogateescape") as f:
        lines = f.read().split("\n")
    try:
        result = Converter(lines).run()
    except ConvertError as e:
        print(f"{sys.argv[1]}: {e}", file=sys.stderr)
        sys.exit(1)
    with open(sys.argv[2], "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(result)


if __name__ == "__main__":
    main()
