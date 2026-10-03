#!/usr/bin/env python3
"""Build and run the preliminary Apple Silicon memory probe; no game data needed."""

import platform
import subprocess
from pathlib import Path


def main():
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise SystemExit("Run on native Apple Silicon macOS (not under Rosetta).")
    root = Path(__file__).resolve().parent.parent
    output = root / "build/macos-runtime"
    output.mkdir(parents=True, exist_ok=True)
    executable = output / "memory_probe"
    subprocess.run([
        "xcrun", "clang", "-arch", "arm64", "-std=c11", "-O2", "-g",
        "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow",
        str(root / "port/macos/runtime/guest_memory.c"),
        str(root / "port/macos/tests/memory_probe.c"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
