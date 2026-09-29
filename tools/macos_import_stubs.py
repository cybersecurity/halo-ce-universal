#!/usr/bin/env python3
"""Emit deterministic fail-fast i386 import entrypoints for AOT linking.

These trap bodies exist only to give the ELF image stable symbol addresses.
The native dispatcher must intercept each VA and invoke its typed adapter;
executing a stub is always fatal (UD2), never a successful empty return.
"""
from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/macos-aot"
BASE = 0x70000000
STRIDE = 16


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="/opt/homebrew/opt/llvm/bin/clang")
    args = parser.parse_args()
    names = set((BUILD / "undefined-symbols.txt").read_text().splitlines())
    imports = sorted(name for name in names if name.startswith(("host_", "posix_")) or
                     name in {"guest_gl_get_proc_address", "halo_gl41_copy_image_2d",
                              "platform_screen_mode"})
    if len(imports) != len(set(imports)):
        raise SystemExit("duplicate import names")
    BUILD.mkdir(parents=True, exist_ok=True)
    manifest = [{"name": name, "va": BASE + STRIDE * i, "slot": i}
                for i, name in enumerate(imports)]
    (BUILD / "host-import-manifest.json").write_text(json.dumps({
        "abi": "i386-cdecl-entry-trap-dispatched-by-VA",
        "base": BASE, "stride": STRIDE, "imports": manifest,
    }, indent=2) + "\n")
    header = ["/* Generated from host-import-manifest.json; do not edit. */",
              "#ifndef HALO_MACOS_HOST_IMPORT_NAMES_H",
              "#define HALO_MACOS_HOST_IMPORT_NAMES_H",
              f"#define MAC_GUEST_HOST_IMPORT_COUNT {len(imports)}u",
              "static const char *const mac_guest_host_import_names[MAC_GUEST_HOST_IMPORT_COUNT] = {"]
    header.extend(f'    "{name}",' for name in imports)
    header += ["};", "#endif", ""]
    (BUILD / "host_import_names.h").write_text("\n".join(header))
    asm = [".section .host_imports,\"ax\",@progbits", ".code32", ".balign 16"]
    for item in manifest:
        asm += [f".globl {item['name']}", f".type {item['name']},@function",
                f"{item['name']}:", "ud2", ".fill 14,1,0x90"]
    asm += [".section .rodata.host_import_names,\"a\",@progbits", ".balign 4",
            ".globl __host_import_names", "__host_import_names:"]
    for item in manifest:
        asm += [f".asciz \"{item['name']}\""]
    asm += [".section .rodata.host_import_count,\"a\",@progbits", ".balign 4",
            ".globl __host_import_count", "__host_import_count:",
            f".long {len(manifest)}"]
    asm += [".section .rodata.host_import_table,\"a\",@progbits", ".balign 4",
            ".globl __host_import_table", "__host_import_table:"]
    for item in manifest:
        asm += [f".long {item['name']}"]
    asm_path = BUILD / "host_import_stubs.S"
    asm_path.write_text("\n".join(asm) + "\n")
    obj = BUILD / "host_import_stubs.o"
    subprocess.run([args.cc, "--target=i386-unknown-linux-musl", "-m32", "-c",
                    str(asm_path), "-o", str(obj)], cwd=ROOT, check=True)
    print(f"generated {len(manifest)} fail-fast stubs at 0x{BASE:08X}.."
          f"0x{BASE + STRIDE * len(manifest):08X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
