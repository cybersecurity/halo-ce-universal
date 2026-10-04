#!/usr/bin/env python3
"""Build the native Apple Silicon host, rebased game, and local .app bundle."""
import argparse
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.linux_build import MINIUPNPC_DEFINES, MINIUPNPC_DIR, miniupnpc_sources
from tools.macos_setup import prepare_angle_distribution
BUILD = ROOT / "build/macos"
LLVM = Path(os.environ.get("HALO_MACOS_LLVM_BIN", "/opt/homebrew/opt/llvm@22/bin"))
SDL = Path(os.environ.get("HALO_MACOS_SDL_PREFIX", "/opt/homebrew/opt/sdl3"))
ANGLE = Path(os.environ.get("HALO_MACOS_ANGLE_DIR", str(BUILD / "angle/dist")))
GL = BUILD / "toolchain/gl"
APP_VERSION = "0.1.0"
APP_BUILD = "1"


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def require(path):
    if not path.exists():
        raise RuntimeError(f"Missing build dependency: {path}. See port/macos/README.md.")
    return path


def build_plugin():
    BUILD.mkdir(parents=True, exist_ok=True)
    require(LLVM / "llvm-config")
    flags = shlex.split(subprocess.check_output(
        [LLVM / "llvm-config", "--cxxflags", "--ldflags", "--libs", "core", "passes"], text=True))
    run(LLVM / "clang++", "-shared", "-fPIC", "port/macos/compiler/guest_rebase.cpp",
        "-o", BUILD / "guest_rebase.dylib", *flags)


def build_host():
    prepare_angle_distribution(ANGLE)
    obj_dir = BUILD / "host-obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    frameworks = [ANGLE / f"{name}.xcframework/macos-arm64" for name in ("EGL", "GLESv2")]
    for name, directory in zip(("libEGL", "libGLESv2"), frameworks):
        require(directory / f"{name}.framework" / name)
    flags = ["-arch", "arm64", "-mmacosx-version-min=14.0", "-O2", "-g", "-DHALO_MACOS=1", "-D_DARWIN_C_SOURCE",
             "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-parameter",
             "-I.", "-Iport/macos/host", "-Iport/macos/native", "-Iport/android/include", "-Iport/linux/src",
             f"-I{SDL / 'include'}", f"-I{GL}",
             f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}", *MINIUPNPC_DEFINES]
    sources = sorted((ROOT / "port/macos/host").glob("*.c"))
    sources += sorted((ROOT / "port/macos/native").glob("*.m"))
    sources += [ROOT / "port/linux/src/xiso.c"]
    sources += miniupnpc_sources()
    sources += [BUILD / "host/host_import_table.c", ROOT / "port/macos/host/entry.s"]
    objects = []
    for source in sources:
        obj = obj_dir / (source.name + ".o")
        native_flags = ["-fobjc-arc", "-fblocks"] if source.suffix == ".m" else []
        run("clang", *flags, *native_flags, "-c", source, "-o", obj)
        objects.append(obj)
    run("clang", "-arch", "arm64", "-mmacosx-version-min=14.0", *objects, f"-L{SDL / 'lib'}", "-lSDL3",
        *(f"-F{directory}" for directory in frameworks),
        "-framework", "libEGL", "-framework", "libGLESv2", "-framework", "Cocoa",
        *(f"-Wl,-rpath,{directory}" for directory in frameworks), "-o", BUILD / "halo")


def minimum_macos_version(binaries):
    output = subprocess.check_output(["otool", "-l", *map(str, binaries)], text=True)
    versions = []
    for command in re.split(r"Load command \d+", output):
        field = "minos" if "cmd LC_BUILD_VERSION" in command else "version" if "cmd LC_VERSION_MIN_MACOSX" in command else None
        if field:
            match = re.search(r"^\s+" + field + r" (\d+(?:\.\d+){1,2})\s*$", command, re.MULTILINE)
            if match:
                versions.append(match[1])
    return max(["14.0", *versions], key=lambda value: tuple(map(int, value.split("."))))


def package(*, version=APP_VERSION, build=APP_BUILD, repository="local"):
    destination = BUILD / "Halo CE Universal.app"
    with tempfile.TemporaryDirectory(prefix=".halo-package-", dir=BUILD) as temporary:
        staged = Path(temporary) / destination.name
        package_into(staged, version=version, build=build, repository=repository)
        # A fresh bundle prevents old development resources entering a release.
        # Preserve the previous build, including a running executable's inode.
        backup = None
        if destination.exists():
            backup = BUILD / "app-backups.noindex" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
            backup.mkdir(parents=True)
            destination.rename(backup / (destination.name + ".backup"))
        try:
            staged.rename(destination)
        except OSError:
            if backup:
                (backup / (destination.name + ".backup")).rename(destination)
            raise
    print(f"Built {destination}")


def package_into(app, *, version=APP_VERSION, build=APP_BUILD, repository="local"):
    contents = app / "Contents"
    macos = contents / "MacOS"
    frameworks = contents / "Frameworks"
    resources = contents / "Resources"
    for directory in (macos, frameworks, resources):
        directory.mkdir(parents=True, exist_ok=True)
    executable = macos / "halo"
    shutil.copy2(BUILD / "halo", executable)
    shutil.copy2(BUILD / "halo_guest.elf", resources / "halo_guest.elf")
    sdl = frameworks / "libSDL3.0.dylib"
    shutil.copy2(require(SDL / "lib/libSDL3.0.dylib"), sdl)
    run("install_name_tool", "-change", str(SDL / "lib/libSDL3.0.dylib"),
        "@rpath/libSDL3.0.dylib", executable)
    run("install_name_tool", "-id", "@rpath/libSDL3.0.dylib", sdl)
    for name in ("EGL", "GLESv2"):
        directory = ANGLE / f"{name}.xcframework/macos-arm64"
        source = directory / f"lib{name}.framework"
        target = frameworks / f"lib{name}.dylib"
        shutil.copy2(source / f"lib{name}", target)
        target.chmod(0o755)
        run("install_name_tool", "-change", f"@rpath/lib{name}.framework/lib{name}",
            f"@rpath/lib{name}.dylib", executable)
        run("install_name_tool", "-id", f"@rpath/lib{name}.dylib", target)
        run("install_name_tool", "-delete_rpath", str(directory), executable)
    run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", executable)
    info = {
        "CFBundleExecutable": "halo", "CFBundleIdentifier": "local.halo.ce-universal",
        "CFBundleName": "Halo CE Universal", "CFBundleDisplayName": "Halo CE Universal",
        "CFBundlePackageType": "APPL", "CFBundleShortVersionString": version,
        "CFBundleVersion": build, "LSMinimumSystemVersion": minimum_macos_version([
            executable, sdl, frameworks / "libEGL.dylib", frameworks / "libGLESv2.dylib"]),
        "CFBundleURLTypes": [{"CFBundleURLName": "Halo multiplayer invite",
                              # Match the shared discord.application_id default.
                              "CFBundleURLSchemes": ["halo", "discord-1553978809840050229"],
                              "CFBundleTypeRole": "Viewer"}],
        "NSLocalNetworkUsageDescription": "Connect to players hosting Halo multiplayer games.",
        "NSHighResolutionCapable": True,
        "NSHumanReadableCopyright": "Local experimental Apple Silicon port",
    }
    with (contents / "Info.plist").open("wb") as stream:
        plistlib.dump(info, stream)
    try:
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                           text=True, stderr=subprocess.DEVNULL).strip()
        if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip():
            revision += " (local changes)"
    except (OSError, subprocess.CalledProcessError):
        revision = "unknown"
    guest_hash = hashlib.sha256((resources / "halo_guest.elf").read_bytes()).hexdigest()
    (resources / "BuildInfo.txt").write_text(
        f"Halo CE Universal {version} (build {build})\n"
        f"Repository: {repository}\nSource: {revision}\nGuest SHA-256: {guest_hash}\n")
    licenses = resources / "Licenses"
    licenses.mkdir(exist_ok=True)
    for source, name in (
        (ROOT / "LICENSE.md", "Project.txt"),
        (ROOT / "port/assets/fonts/Overpass-OFL.txt", "Overpass-OFL.txt"),
        (ROOT / "port/assets/fonts/OpenCE-OFL.txt", "OpenCE-OFL.txt"),
        (ROOT / "port/assets/fonts/Newtown-LICENSE.txt", "Newtown-LICENSE.txt"),
        (ROOT / "port/macos/licenses/ANGLE.txt", "ANGLE.txt"),
        (SDL / "share/licenses/SDL3/LICENSE.txt", "SDL3.txt"),
        (ROOT / "build/android/third_party/musl-1.2.5/COPYRIGHT", "musl.txt"),
        (ROOT / "port/third_party/kcp/LICENSE", "KCP.txt"),
        (ROOT / "port/third_party/miniupnpc/LICENSE", "miniupnpc.txt"),
        (ROOT / "port/third_party/extract-xiso/LICENSE.TXT", "extract-xiso.txt"),
        (ROOT / "port/third_party/expat/COPYING", "Expat.txt"),
        (ROOT / "port/third_party/tomlc17/LICENSE", "tomlc17.txt"),
        (ROOT / "port/third_party/mbedtls/LICENSE", "mbedtls.txt"),
    ):
        shutil.copy2(require(source), licenses / name)
    for binary in (sdl, frameworks / "libGLESv2.dylib", frameworks / "libEGL.dylib", app):
        run("codesign", "--force", "--sign", "-", binary)
    run("codesign", "--verify", "--deep", "--strict", app)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugin-only", action="store_true")
    parser.add_argument("--host-only", action="store_true")
    parser.add_argument("--version", default=APP_VERSION)
    parser.add_argument("--build-number", default=APP_BUILD)
    parser.add_argument("--repository", default="local", help="Source repository recorded in BuildInfo.txt")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 4, 6))
    args = parser.parse_args()
    os.chdir(ROOT)
    if args.plugin_only:
        build_plugin()
        return
    if not args.host_only:
        llvm_bin = BUILD / "toolchain/bin"
        require(llvm_bin / "llvm-ar")
        require(llvm_bin / "ld.lld")
        require(GL / "GLES3/gl32.h")
        run(sys.executable, "configure.py", "--macos", "--android-guest-llvm-bin", llvm_bin,
            "--android-guest-gl-include", GL, "--pgo", "off")
        ninja = shutil.which("ninja") or str(BUILD / "toolchain/venv/bin/ninja")
        run(ninja, "-j", args.jobs, "macos_guest")
    build_host()
    package(version=args.version, build=args.build_number, repository=args.repository)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"macOS build failed: {error}", file=sys.stderr)
        sys.exit(1)
