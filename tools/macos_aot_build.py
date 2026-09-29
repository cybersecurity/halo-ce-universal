#!/usr/bin/env python3
"""Build the 2342 source guest as i386 ELF for ARM64 AOT translation.

Stages:
  compile  Compile the upstream game and shared platform C source manifest.
  combine  Preserve all section/symbol/relocation metadata in an ELF32 ET_REL.
  full     Run both stages.

This is not the final translator/linker: undefined symbols in the combined
object are reported for explicit libc/platform-import resolution.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/macos-aot"


def run_compile(args: argparse.Namespace, scope: str) -> None:
    cmd = [sys.executable, str(ROOT / "validation/sourceport-lp64/batch_compile_guest.py"),
           "--scope", scope, "--jobs", str(args.jobs), "--cc", args.guest_cc]
    if args.limit:
        cmd += ["--limit", str(args.limit)]
    subprocess.run(cmd, cwd=ROOT, check=True)


def combine(args: argparse.Namespace) -> None:
    # These musl units duplicate symbols already supplied by the title's
    # compatibility layer. Keep the game's strnlen and its stack-check hook.
    libc_objects = [
        path for path in sorted((BUILD / "libc-obj").rglob("*.o"))
        if path.as_posix().endswith((
            "/src/string/strnlen.o",
            "/src/env/__stack_chk_fail.o",
            # i386 assembly provides the ABI-specific x87 implementations.
            "/src/fenv/fenv.o",
        )) is False
    ]
    objects = (
        sorted((BUILD / "game-obj").rglob("*.o"))
        + [p for p in sorted((BUILD / "platform-obj").rglob("*.o"))
           if not p.as_posix().endswith("/port/android/guest/runtime/guest_memory_watch.o")]
        + sorted((BUILD / "math-obj").rglob("*.o"))
        + libc_objects
    )
    if not objects:
        raise SystemExit("no guest objects found; run compile first")
    output = BUILD / "halo_guest_combined.o"
    cmd = [args.link_cc, "--target=i386-unknown-linux-musl", "-m32",
           f"-fuse-ld={args.lld}", "-nostdlib", "-no-pie", "-Wl,-r", "-o", str(output),
           *map(str, objects)]
    subprocess.run(cmd, cwd=ROOT, check=True)
    nm = subprocess.run([args.llvm_nm, "--undefined-only", str(output)], cwd=ROOT,
                        check=True, text=True, stdout=subprocess.PIPE)
    undefined = sorted(line.strip().split()[-1] for line in nm.stdout.splitlines() if line.strip())
    (BUILD / "undefined-symbols.txt").write_text("\n".join(undefined) + "\n")
    defined_count = subprocess.run([args.llvm_nm, "--defined-only", str(output)], cwd=ROOT,
                                   check=True, text=True, stdout=subprocess.PIPE).stdout.count("\n")
    unique_undefined = sorted(set(undefined))
    groups = {
        "SDL3": [s for s in unique_undefined if s.startswith("SDL_")],
        "POSIX_platform": [s for s in unique_undefined if s.startswith("posix_")],
        "host_services": [s for s in unique_undefined if s.startswith("host_")],
        "other": [s for s in unique_undefined if not s.startswith(("SDL_", "posix_", "host_"))],
    }
    metadata = {
        "object_count": len(objects),
        "combined_elf": str(output.relative_to(ROOT)),
        "defined_symbol_entries": defined_count,
        "undefined_symbol_references": len(undefined),
        "undefined_unique": len(unique_undefined),
        "undefined_groups": {name: len(items) for name, items in groups.items()},
        "undefined_symbols": unique_undefined,
    }
    (BUILD / "combined-symbol-report.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps({key: value for key, value in metadata.items() if key != "undefined_symbols"}, indent=2))


def link(args: argparse.Namespace) -> None:
    # Generate named UD2 entries for imports. The AOT runtime must dispatch
    # these VAs to typed native adapters; executing an entry traps.
    subprocess.run([sys.executable, str(ROOT / "tools/macos_import_stubs.py"),
                    "--cc", args.link_cc], cwd=ROOT, check=True)
    compiler_rt_sources = ("divdi3", "moddi3", "udivdi3", "umoddi3", "udivmoddi4")
    compiler_rt_dir = ROOT / "port/macos/compiler-rt"
    compiler_rt_out = BUILD / "compiler-rt-obj"
    compiler_rt_out.mkdir(parents=True, exist_ok=True)
    compiler_rt_objects = []
    for name in compiler_rt_sources:
        obj = compiler_rt_out / f"{name}.o"
        subprocess.run([args.link_cc, "--target=i386-unknown-linux-musl", "-m32",
                        "-O2", "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-pie",
                        "-I", str(compiler_rt_dir), "-c", str(compiler_rt_dir / f"{name}.c"),
                        "-o", str(obj)], cwd=ROOT, check=True)
        compiler_rt_objects.append(obj)
    object_dirs = ("game-obj", "platform-obj", "math-obj", "libc-obj")
    objects = []
    for directory in object_dirs:
        objects.extend(sorted((BUILD / directory).rglob("*.o")))
    objects = [p for p in objects if not p.as_posix().endswith((
        "/src/string/strnlen.o", "/src/env/__stack_chk_fail.o", "/src/fenv/fenv.o",
        "/port/android/guest/runtime/guest_memory_watch.o"))]
    output = BUILD / "halo_guest.elf"
    mapfile = BUILD / "halo_guest.map"
    cmd = [args.lld, "-m", "elf_i386", "-T", str(ROOT / "port/macos/guest_aot.ld"),
           "-Map", str(mapfile), "--no-undefined", "-o", str(output),
           *map(str, objects), *map(str, compiler_rt_objects),
           str(BUILD / "host_import_stubs.o")]
    subprocess.run(cmd, cwd=ROOT, check=True)
    print(f"linked {output.relative_to(ROOT)} ({output.stat().st_size} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("compile", "combine", "link", "full"))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--limit", type=int, default=0, help="compile only the first N units per scope")
    parser.add_argument("--guest-cc", default="clang")
    parser.add_argument("--link-cc", default="/opt/homebrew/opt/llvm/bin/clang")
    parser.add_argument("--lld", default="/opt/homebrew/opt/lld/bin/ld.lld")
    parser.add_argument("--llvm-nm", default="/opt/homebrew/opt/llvm/bin/llvm-nm")
    parser.add_argument("--compiler-rt", default="")
    args = parser.parse_args()
    if args.stage in ("compile", "full"):
        run_compile(args, "game")
        run_compile(args, "platform")
        run_compile(args, "math")
        run_compile(args, "libc")
    if args.stage in ("combine", "full"):
        combine(args)
    if args.stage == "link":
        link(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
