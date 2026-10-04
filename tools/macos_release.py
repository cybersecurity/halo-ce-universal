#!/usr/bin/env python3
"""Package an asset-free, ad-hoc signed Mac app as a drag-to-Applications DMG."""
import argparse
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "build/macos/Halo CE Universal.app"
ACCOUNT = "local.halo.ce-universal"


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def audit_bundle(app):
    """An explicit package boundary, not a license clearance for compiled code."""
    allowed = {"Info.plist", "MacOS", "Frameworks", "Resources", "_CodeSignature"}
    contents = app / "Contents"
    if {path.name for path in contents.iterdir()} - allowed:
        raise RuntimeError("Unexpected top-level content in the app")
    resources = contents / "Resources"
    if {path.name for path in resources.iterdir()} - {"halo_guest.elf", "BuildInfo.txt", "Licenses"}:
        raise RuntimeError("Release resources must contain only the compiled engine, build record and licenses")
    if {path.name for path in (contents / "MacOS").iterdir()} != {"halo"}:
        raise RuntimeError("Release executables must contain only the native host")
    if {path.name for path in (contents / "Frameworks").iterdir()} != {"libSDL3.0.dylib", "libEGL.dylib", "libGLESv2.dylib"}:
        raise RuntimeError("Unexpected bundled runtime dependency")
    for path in app.rglob("*"):
        if path.suffix.lower() in {".iso", ".xiso", ".map", ".xbe", ".pdb", ".p12", ".mobileprovision"}:
            raise RuntimeError("Restricted or private input in app: " + str(path.relative_to(app)))
        if path.is_symlink() and not path.resolve().is_relative_to(app.resolve()):
            raise RuntimeError("External symlink in the signed app")
    return True


def audit_adhoc_signing(app):
    """Check all bundled code without exposing unexpected signing metadata."""
    with (app / "Contents/Info.plist").open("rb") as stream:
        if plistlib.load(stream)["CFBundleIdentifier"] != ACCOUNT:
            raise RuntimeError("Unexpected app bundle identifier")
    # Check every architecture of every bundled Mach-O, including dependencies.
    magic = {bytes.fromhex(value) for value in (
        "feedface", "cefaedfe", "feedfacf", "cffaedfe",
        "cafebabe", "bebafeca", "cafebabf", "bfbafeca",
    )}
    checked = 0
    for path in app.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as stream:
            if stream.read(4) not in magic:
                continue
        architectures = subprocess.check_output(["lipo", "-archs", str(path)], text=True).split()
        if not architectures:
            raise RuntimeError("No Mach-O architectures found: " + str(path.relative_to(app)))
        for architecture in architectures:
            result = subprocess.run(
                ["codesign", "-d", "--verbose=4", "--arch", architecture, str(path)],
                capture_output=True, text=True, check=True)
            fields = result.stderr.splitlines()
            if ("Signature=adhoc" not in fields or "TeamIdentifier=not set" not in fields
                    or any(field.startswith("Authority=") for field in fields)):
                raise RuntimeError("Expected ad-hoc signing without a certificate or team: "
                                   + str(path.relative_to(app)) + " (" + architecture + ")")
        checked += 1
    if not checked:
        raise RuntimeError("No signed Mach-O code found in app")
    print(f"Verified {checked} code objects: ad-hoc signing, no certificate authorities or team identifiers")
    return checked


def create_dmg(app, destination):
    if destination.exists():
        raise RuntimeError("Disk image output already exists; choose a new filename")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="halo-dmg-") as temporary:
        layout = Path(temporary)
        run("ditto", app, layout / app.name)
        (layout / "Applications").symlink_to("/Applications")
        run("hdiutil", "create", "-volname", "Halo CE Universal", "-srcfolder", layout,
            "-format", "UDZO", destination)


def local_dmg(args):
    audit_bundle(APP)
    run("codesign", "--verify", "--deep", "--strict", APP)
    audit_adhoc_signing(APP)
    with (APP / "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    destination = args.output or ROOT / "build/macos" / ("Halo-" + info["CFBundleShortVersionString"] + "-local.dmg")
    create_dmg(APP, destination.resolve())
    print("Prepared ad-hoc DMG (not notarized): " + str(destination))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    local = commands.add_parser("local-dmg", help="Package the built app without credentials or game data")
    local.add_argument("--output", type=Path)
    local_dmg(parser.parse_args())


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print("Packaging failed: " + str(error), file=sys.stderr)
        sys.exit(1)
