"""Runs the C tests of the Android host's page-hash write tracking
(port/android/host/host_watch_hash.c) with the build machine's C compiler.
The unit has no Android dependency, so it compiles anywhere."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def compiler():
    for name in ("clang", "cc", "gcc"):
        path = shutil.which(name)
        if path:
            return path
    return None


def test_host_watch_hash(tmp_path):
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler")
    binary = tmp_path / "host_watch_hash_test"
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
         str(ROOT / "port" / "android" / "tests" / "host_watch_hash_test.c"),
         str(ROOT / "port" / "android" / "host" / "host_watch_hash.c"),
         "-o", str(binary)],
        check=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
