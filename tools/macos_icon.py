#!/usr/bin/env python3
"""Make the macOS application bundle's icon (Halo.app/Contents/Resources/
halo.icns) from the Android app's artwork, port/android/art/android-icon.png,
with the system's sips and iconutil.

Usage: macos_icon.py output.icns
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = Path("port/android/art/android-icon.png")


def main():
    output = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as folder:
        iconset = Path(folder) / "halo.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                pixels = size * scale
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                subprocess.run(["sips", "-s", "format", "png", "-z", str(pixels), str(pixels), str(SOURCE),
                                "--out", str(iconset / name)], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(output)], check=True)
    if not output.is_file():
        shutil.copy(SOURCE, output)


if __name__ == "__main__":
    main()
