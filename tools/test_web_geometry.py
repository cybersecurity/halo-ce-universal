"""Compile and exercise the browser geometry cache with mocked GL storage."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def test_web_geometry():
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="halo-web-geometry-") as directory:
        executable = Path(directory) / "web-geometry-test"
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-g", "-O1",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            str(root / "tools/tests/web-geometry-test.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    test_web_geometry()
