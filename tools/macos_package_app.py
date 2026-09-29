#!/usr/bin/env python3
"""Package a verified ARM64 Halo engine as a relocatable local macOS app.

Python is a build-time dependency only. The resulting app uses a native launcher
and bundles its ELF and third-party dylibs. Signing is local ad-hoc signing.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RESOURCES = ROOT / "port/macos/resources"
SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")


def run(*args: str | Path, capture: bool = False) -> str:
    result = subprocess.run([str(arg) for arg in args], check=True,
                            text=True, stdout=subprocess.PIPE if capture else None)
    return result.stdout if capture else ""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def dependencies(path: Path) -> list[str]:
    return [line.strip().split(" (", 1)[0]
            for line in run("otool", "-L", path, capture=True).splitlines()[1:]]


def rpaths(path: Path) -> list[str]:
    lines = run("otool", "-l", path, capture=True).splitlines()
    result = []
    for index, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            result.append(lines[index + 2].strip().split(" (", 1)[0].removeprefix("path "))
    return result


def resolve_dependency(name: str, owner: Path, engine: Path) -> Path:
    if name.startswith("@loader_path/"):
        result = owner.parent / name.removeprefix("@loader_path/")
    elif name.startswith("@executable_path/"):
        result = engine.parent / name.removeprefix("@executable_path/")
    elif name.startswith("@rpath/"):
        result = None
        for root in rpaths(owner) + rpaths(engine):
            root = root.replace("@loader_path", str(owner.parent)).replace("@executable_path", str(engine.parent))
            candidate = Path(root) / name.removeprefix("@rpath/")
            if candidate.exists():
                result = candidate
                break
        if result is None:
            raise ValueError(f"Cannot resolve {name} referenced by {owner}")
    else:
        result = Path(name)
    if not result.exists():
        raise ValueError(f"Missing library {name} referenced by {owner}")
    return result.resolve()


def collect_libraries(engine: Path) -> tuple[dict[Path, list[tuple[str, Path]]], set[Path]]:
    graph = {}
    pending = [engine]
    libraries = set()
    while pending:
        owner = pending.pop()
        if owner in graph:
            continue
        refs = []
        for name in dependencies(owner):
            if name.startswith(SYSTEM_PREFIXES):
                continue
            target = resolve_dependency(name, owner, engine)
            # A dylib's LC_ID_DYLIB also appears in otool -L.
            if target == owner:
                continue
            refs.append((name, target))
            libraries.add(target)
            pending.append(target)
        graph[owner] = refs
    return graph, libraries


def version_tuple(value: str) -> tuple[int, ...]:
    pieces = value.split(".")
    if not pieces or not all(piece.isdigit() for piece in pieces):
        raise ValueError(f"Invalid Mach-O minimum macOS version: {value}")
    return tuple(map(int, pieces)) + (0,) * (3 - len(pieces))


def minimum_macos(path: Path) -> str:
    """Read the ARM64 slice, including older LC_VERSION_MIN_MACOSX headers."""
    command = None
    versions = []
    for line in run("otool", "-arch", "arm64", "-l", path, capture=True).splitlines():
        fields = line.strip().split()
        if len(fields) == 2 and fields[0] == "cmd":
            command = fields[1]
        elif command == "LC_BUILD_VERSION" and len(fields) == 2:
            if fields[0] == "platform" and fields[1].lower() not in ("1", "macos"):
                raise ValueError(f"Non-macOS ARM64 platform in {path}")
            if fields[0] == "minos":
                versions.append(fields[1])
        elif command == "LC_VERSION_MIN_MACOSX" and len(fields) == 2 and fields[0] == "version":
            versions.append(fields[1])
    if not versions:
        raise ValueError(f"Missing ARM64 macOS deployment load command in {path}")
    return max(versions, key=version_tuple)


def install_assets(source: Path, destination: Path) -> Path:
    """Copy original read-only content; never replace an existing installation."""
    maps = source / "maps"
    movies = source / "bink"
    if not movies.is_dir():
        movies = source / "bink-source"
    if not maps.is_dir() or not movies.is_dir():
        raise ValueError("Asset source must contain maps and bink or bink-source")
    if destination.exists():
        if not (destination / "maps").is_dir() or not (destination / "bink").is_dir():
            raise ValueError(f"Incomplete installed assets at {destination}; refusing to replace them")
        # Confirm the existing installation matches without rewriting any files.
        for src, dst in ((maps, destination / "maps"), (movies, destination / "bink")):
            source_files = {p.relative_to(src) for p in src.rglob("*") if p.is_file()}
            installed_files = {p.relative_to(dst) for p in dst.rglob("*") if p.is_file()}
            if source_files != installed_files:
                raise ValueError(f"Existing assets differ from requested source: {dst}")
            for relative in source_files:
                if sha256(src / relative) != sha256(dst / relative):
                    raise ValueError(f"Existing asset differs from requested source: {dst / relative}")
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".halo-assets-", dir=destination.parent))
    try:
        for src, name in ((maps, "maps"), (movies, "bink")):
            shutil.copytree(src, staging / name)
        # Content remains read-only; the engine writes only to data/save roots.
        for path in staging.rglob("*"):
            path.chmod(0o555 if path.is_dir() else 0o444)
        staging.chmod(0o555)
        staging.rename(destination)
    except BaseException:
        for path in staging.rglob("*"):
            if path.is_dir(): path.chmod(0o755)
        staging.chmod(0o755)
        shutil.rmtree(staging)
        raise
    return destination


REQUIRED_MAP_NAMES = set("a10 a30 a50 b30 b40 beavercreek bloodgulch boardingaction c10 c20 c40 carousel chillout d20 d40 damnation hangemhigh longest prisoner putput ratrace sidewinder ui wizard".split())
REQUIRED_MOVIE_NAMES = set("attract1 attract2 attract3 credits intro".split())


def clone_or_copy(source: Path, destination: Path) -> None:
    """APFS clone is an independent regular file; fallback never creates hardlinks."""
    try:
        system = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
        clone = system.clonefile
        clone.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int)
        clone.restype = ctypes.c_int
        if clone(os.fsencode(source), os.fsencode(destination), 0) == 0:
            return
    except (AttributeError, OSError):
        pass
    shutil.copy2(source, destination)


def bundle_assets(source: Path, destination: Path) -> dict:
    """Seal engine-consumed disc data; keep config/debug/generated state outside."""
    destination.mkdir()
    asset_records = []
    builds = set()
    directories = [path for path in sorted(source.iterdir())
                   if path.is_dir() and (path.name == "maps" or path.name.startswith("maps_"))]
    movie_source = source / "bink"
    if not movie_source.is_dir(): movie_source = source / "bink-source"
    directories.append(movie_source)
    for directory in directories:
        if directory.is_symlink(): raise ValueError(f"Asset input directory must be regular: {directory.name}")
        name = "bink" if directory == movie_source else directory.name
        target = destination / name
        target.mkdir()
        extension = ".bik" if name == "bink" else ".map"
        for original in sorted(directory.glob("*" + extension)):
            if not original.is_file() or original.is_symlink():
                raise ValueError(f"Asset input must be a regular file: {original.name}")
            with original.open("rb") as stream: header = stream.read(2048)
            record = {"path": name + "/" + original.name, "bytes": original.stat().st_size}
            if extension == ".map":
                if len(header) < 2048 or header[:4] != b"daeh" or struct.unpack_from("<I", header, 4)[0] != 5:
                    raise ValueError(f"Unsupported Xbox map cache: {original.name}")
                build = header[64:96].split(b"\0", 1)[0].decode("ascii")
                if build not in ("01.10.12.2276", "01.01.14.2342"):
                    raise ValueError(f"Unsupported map build: {build}")
                record.update(cache_version=5, cache_build=build)
                builds.add(build)
            elif len(header) < 8 or header[:3] != b"BIK":
                raise ValueError(f"Invalid movie header: {original.name}")
            copied = target / original.name
            clone_or_copy(original, copied)
            # Verify the final independent bytes, not a symlink or a pre-copy stamp.
            record["sha256"] = sha256(copied)
            if record["sha256"] != sha256(original):
                raise ValueError(f"Asset changed while packaging: {original.name}")
            copied.chmod(0o444)
            asset_records.append(record)
    present = {record["path"] for record in asset_records}
    required = {"maps/" + name + ".map" for name in REQUIRED_MAP_NAMES} | {"bink/" + name + ".bik" for name in REQUIRED_MOVIE_NAMES}
    if required - present: raise ValueError("Missing required Halo game data: " + ", ".join(sorted(required - present)))
    provenance = {}
    extraction = source / "extraction-provenance.json"
    if extraction.is_file():
        raw = json.loads(extraction.read_text())
        provenance = {key:raw[key] for key in ("image_sha256", "image_bytes", "partition_offset") if key in raw}
    manifest = {"format":1, "asset_count":len(asset_records), "bytes":sum(record["bytes"] for record in asset_records),
                "cache_builds":sorted(builds), "source_image":provenance, "assets":asset_records,
                "required_data":"Xbox maps and Bink movies; fonts/localization are cache tags; generated language/cache/save/debug files remain writable user state"}
    (destination / "asset-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (destination / "asset-manifest.json").chmod(0o444)
    return manifest


def build(args: argparse.Namespace) -> dict:
    binary, elf, stamp_path, output, assets = [path.expanduser().resolve()
        for path in (args.binary, args.elf, args.stamp, args.output, args.assets)]
    stamp = json.loads(stamp_path.read_text())
    for name, path in (("binary", binary), ("elf", elf)):
        if stamp.get(name + "_sha256") != sha256(path):
            raise ValueError(f"{name} does not match the verified input stamp")
    if "arm64" not in run("lipo", "-archs", binary, capture=True).split():
        raise ValueError("This package requires a native ARM64 engine")
    if output.exists() and not args.replace:
        raise ValueError(f"Output exists; use --replace to rebuild {output}")
    if args.install_assets:
        assets = install_assets(assets, args.install_assets.expanduser().resolve())
    movies = assets / "bink" if (assets / "bink").is_dir() else assets / "bink-source"
    if not (assets / "maps").is_dir() or not movies.is_dir():
        raise ValueError("Installed original maps or movies are missing")
    graph, libraries = collect_libraries(binary)
    names = {}
    for library in sorted(libraries):
        if "arm64" not in run("lipo", "-archs", library, capture=True).split():
            raise ValueError(f"Library lacks ARM64 support: {library}")
        if library.name in names and names[library.name] != library:
            raise ValueError(f"Dylib basename collision: {library}")
        names[library.name] = library
    output.parent.mkdir(parents=True, exist_ok=True)
    staging_parent = Path(tempfile.mkdtemp(prefix=".halo-app-", dir=output.parent))
    app = staging_parent / output.name
    try:
        macos = app / "Contents/MacOS"
        resources = app / "Contents/Resources"
        frameworks = app / "Contents/Frameworks"
        for directory in (macos, resources, frameworks): directory.mkdir(parents=True)
        asset_manifest = bundle_assets(assets, resources / "GameData")
        engine = macos / "halo_engine"
        shutil.copy2(binary, engine)
        shutil.copy2(elf, resources / "halo_guest.elf")
        # A rebuild may replace input files while a package is being staged.
        # Verify the copied bytes before relocating or signing anything.
        for name, copied in (("binary", engine), ("elf", resources / "halo_guest.elf")):
            if sha256(copied) != stamp.get(name + "_sha256"):
                raise ValueError(f"{name} changed while packaging; retry with its matching input stamp")
        shutil.copy2(stamp_path, resources / "verified-startup-input.json")
        shutil.copy2(RESOURCES / "HaloCombatEvolved.icns", resources / "HaloCombatEvolved.icns")
        shutil.copy2(RESOURCES / "master-chief-mark-v-icon.prompt.txt", resources / "icon-generation-prompt.txt")
        shutil.copy2(RESOURCES / "starter-config.toml", resources / "starter-config.toml")
        destinations = {binary: engine}
        for library in sorted(libraries):
            destinations[library] = frameworks / library.name
            shutil.copy2(library, destinations[library])
        for original, packaged in destinations.items():
            packaged.chmod(0o755)
            # Rewrite signed Mach-O files first, then replace their signatures.
            # Removing signatures beforehand leaves a LINKEDIT layout that older
            # Xcode install_name_tool versions reject.
            for old, dependency in graph[original]:
                run("install_name_tool", "-change", old, "@rpath/" + dependency.name, packaged)
            if original != binary:
                run("install_name_tool", "-id", "@rpath/" + original.name, packaged)
            for old in rpaths(packaged):
                if old.startswith("/"):
                    run("install_name_tool", "-delete_rpath", old, packaged)
            desired_rpath = "@executable_path/../Frameworks" if original == binary else "@loader_path"
            if desired_rpath not in rpaths(packaged):
                run("install_name_tool", "-add_rpath", desired_rpath, packaged)
            run("codesign", "--force", "--sign", "-", packaged)
        minimum_components = {"native_icon_api": "14.0", "engine": minimum_macos(binary)}
        minimum_components.update({path.name: minimum_macos(path) for path in sorted(libraries)})
        minos = max(minimum_components.values(), key=version_tuple)
        launcher = macos / "Halo Combat Evolved"
        run("xcrun", "clang", "-O2", "-Wall", "-Wextra", "-Werror", "-arch", "arm64",
            "-mmacosx-version-min=" + minos, RESOURCES / "macos_app_launcher.m",
            "-framework", "AppKit", "-o", launcher)
        manifest = {"format":1, "binary_sha256":sha256(engine), "elf_sha256":sha256(resources / "halo_guest.elf"),
                    "assets_relative":"GameData", "asset_manifest_sha256":sha256(resources / "GameData/asset-manifest.json"),
                    "asset_count":asset_manifest["asset_count"], "asset_bytes":asset_manifest["bytes"],
                    "minimum_macos":minos, "minimum_macos_components":minimum_components, "input_stamp":stamp,
                    "bundled_dylibs":{path.name:sha256(destinations[path]) for path in sorted(libraries)},
                    "signing":"ad-hoc", "native_launcher":True}
        (resources / "launch-config.json").write_text(json.dumps(manifest, indent=2) + "\n")
        plist = {"CFBundleDisplayName":"Halo: Combat Evolved", "CFBundleName":"Halo: Combat Evolved",
                 "CFBundleExecutable":"Halo Combat Evolved", "CFBundleIconFile":"HaloCombatEvolved.icns",
                 "CFBundleIdentifier":"local.halo.native-aot", "CFBundlePackageType":"APPL",
                 "CFBundleVersion":"0.2", "CFBundleShortVersionString":"0.2",
                 "LSMinimumSystemVersion":minos, "NSHighResolutionCapable":True}
        with (app / "Contents/Info.plist").open("wb") as stream: plistlib.dump(plist, stream)
        run("codesign", "--force", "--sign", "-", app)
        run("codesign", "--verify", "--deep", "--strict", app)
        for packaged in destinations.values():
            bad = [name for name in dependencies(packaged)
                   if name.startswith("/") and not name.startswith(SYSTEM_PREFIXES)]
            if bad: raise ValueError(f"External library references remain in {packaged}: {bad}")
        # This validates paths and hashes and explicitly never launches the game.
        check = json.loads(run(launcher, "--check", capture=True))
        if not check.get("verified") or check.get("launches_game"):
            raise ValueError("Native no-launch check did not verify the package")
        backup = output.with_name(output.name + ".previous")
        if output.exists():
            if backup.exists(): raise ValueError(f"Preserved backup exists: {backup}; refusing to overwrite it")
            output.rename(backup)
        app.rename(output)
        return {"app":str(output), "engine_sha256":manifest["binary_sha256"],
                "elf_sha256":manifest["elf_sha256"], "assets_bundled":True, "asset_count":asset_manifest["asset_count"], "asset_bytes":asset_manifest["bytes"],
                "bundled_dylibs":len(libraries), "minimum_macos":minos,
                "codesign_verified":True, "game_launched":False}
    finally:
        shutil.rmtree(staging_parent)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--stamp", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--install-assets", type=Path, help="Copy only maps and movies to a stable new installation; preserve originals")
    parser.add_argument("--replace", action="store_true", help="Preserve existing output as .app.previous before replacing it")
    args = parser.parse_args()
    try:
        print(json.dumps(build(args), indent=2))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"Packaging failed: {error}\n")


if __name__ == "__main__":
    main()
