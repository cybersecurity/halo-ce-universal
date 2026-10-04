#!/usr/bin/env python3
"""LLVM 22 compiler adapter for the rebased ILP32 macOS guest."""
import os
from pathlib import Path
import subprocess
import sys


def main():
    llvm = Path(os.environ.get("HALO_MACOS_LLVM_BIN", "/opt/homebrew/opt/llvm@22/bin"))
    args = sys.argv[1:]
    if "-S" not in args:
        return subprocess.call([str(llvm / "clang"), *args])
    output_index = args.index("-o") + 1
    output = Path(args[output_index])
    ir = output.with_suffix(".ll")
    args[output_index] = str(ir)
    subprocess.run([str(llvm / "clang"), *args, "-emit-llvm", "-DHALO_MACOS=1"], check=True)
    rebased = ir.with_suffix(".rebased.ll")
    subprocess.run([str(llvm / "opt"), "-load-pass-plugin=build/macos/guest_rebase.dylib",
                    "-passes=halo-rebase,verify", "-S", str(ir), "-o", str(rebased)], check=True)
    subprocess.run([str(llvm / "llc"), "-O2", "-mtriple=arm64_32-apple-watchos",
                    "-aarch64-neon-syntax=generic", "-emulated-tls", str(rebased),
                    "-o", str(output)], check=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
