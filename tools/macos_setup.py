#!/usr/bin/env python3
"""Prepare public Mac build dependencies; never downloads game data or the XDK."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/macos"


def download(entry, destination):
    if destination.exists() and hashlib.sha256(destination.read_bytes()).hexdigest() == entry["sha256"]:
        return
    print(f"Downloading {destination.name}", flush=True)
    with urllib.request.urlopen(entry["url"], timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != entry["sha256"]:
        raise RuntimeError(f"Checksum mismatch for {destination.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)


def prepare_angle_distribution(directory):
    """Keep EGL's dynamically loaded GLES companion inside this distribution."""
    egl = directory / "EGL.xcframework/macos-arm64/libEGL.framework/libEGL"
    gles = directory / "GLESv2.xcframework/macos-arm64/libGLESv2.framework/libGLESv2"
    for library in (egl, gles):
        if not library.is_file():
            raise RuntimeError(f"Missing ANGLE runtime library: {library}")
    companion = egl.with_name("libGLESv2.dylib")
    relative = Path(os.path.relpath(gles, companion.parent))
    if companion.is_symlink() and companion.readlink() != relative:
        companion.unlink()
    if not companion.exists():
        companion.symlink_to(relative)


def main():
    manifest = json.loads((ROOT / "port/macos/dependencies.json").read_text())
    llvm = Path(os.environ.get("HALO_MACOS_LLVM_BIN", "/opt/homebrew/opt/llvm@22/bin"))
    version = subprocess.check_output([llvm / "llvm-config", "--version"], text=True).strip()
    if version.split(".")[0] != str(manifest["llvm_major"]):
        raise RuntimeError("The compiler pass requires LLVM 22")
    toolchain = BUILD / "toolchain"
    binary = toolchain / "bin"
    binary.mkdir(parents=True, exist_ok=True)
    ar = binary / "llvm-ar"
    if not ar.exists():
        ar.symlink_to(llvm / "llvm-ar")
    linker = binary / "ld.lld"
    lld = shutil.which("ld.lld")
    if not lld and Path("/opt/homebrew/opt/lld@22/bin/ld.lld").exists():
        lld = "/opt/homebrew/opt/lld@22/bin/ld.lld"
    if not linker.exists():
        if not lld:
            raise RuntimeError("Install Homebrew lld@22 and add its bin directory to PATH")
        linker.symlink_to(lld)
    if not shutil.which("ninja"):
        raise RuntimeError("Install ninja: brew install ninja")
    for entry in manifest["headers"]:
        download(entry, toolchain / "gl" / entry["path"])
    archive = BUILD / "angle/angle.zip"
    download(manifest["angle"], archive)
    if not (BUILD / "angle/dist/BUILD_INFO.txt").exists():
        with zipfile.ZipFile(archive) as package:
            package.extractall(BUILD / "angle")
    prepare_angle_distribution(BUILD / "angle/dist")
    print(f"Mac build dependencies ready (LLVM {version})")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Setup failed: {error}", file=sys.stderr)
        sys.exit(1)
