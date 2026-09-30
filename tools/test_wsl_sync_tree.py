"""Tests of tools/wsl_sync_tree.sh, which copies a Windows working tree into
WSL for the Android build (tools/wsl_build_android.sh). Needs bash and
rsync, so it runs on Linux (WSL) only."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tools" / "wsl_sync_tree.sh"

pytestmark = pytest.mark.skipif(
    os.name != "posix" or not shutil.which("rsync") or not shutil.which("bash"),
    reason="needs bash and rsync (Linux)")


def sync(source, mirror, work):
    return subprocess.run(["bash", str(SCRIPT), str(source), str(mirror), str(work)],
                          capture_output=True, text=True)


@pytest.fixture
def trees(tmp_path):
    source, mirror, work = tmp_path / "source", tmp_path / "mirror", tmp_path / "work"
    (source / "src").mkdir(parents=True)
    (source / "src" / "a.c").write_bytes(b"int a;\r\n")
    (source / "src" / "b.c").write_bytes(b"int b;\n")
    return source, mirror, work


def test_first_sync_copies_with_lf(trees):
    source, mirror, work = trees
    result = sync(source, mirror, work)
    assert result.returncode == 0, result.stderr
    assert (work / "src" / "a.c").read_bytes() == b"int a;\n"
    assert (work / "src" / "b.c").read_bytes() == b"int b;\n"


def test_unchanged_sync_copies_nothing(trees):
    source, mirror, work = trees
    sync(source, mirror, work)
    result = sync(source, mirror, work)
    assert result.returncode == 0, result.stderr
    assert "copied 0 changed files" in result.stdout


def test_deleted_file_goes_away(trees):
    source, mirror, work = trees
    sync(source, mirror, work)
    (source / "src" / "b.c").unlink()
    result = sync(source, mirror, work)
    assert result.returncode == 0, result.stderr
    assert not (work / "src" / "b.c").exists()


def test_change_after_failed_sync_still_arrives(trees):
    source, mirror, work = trees
    sync(source, mirror, work)
    (source / "src" / "a.c").write_bytes(b"int a2;\r\n")
    # the copy into the build tree fails after rsync has updated the mirror
    (work / "src" / "a.c").chmod(0o000)
    (work / "src").chmod(0o500)
    failed = sync(source, mirror, work)
    (work / "src").chmod(0o755)
    (work / "src" / "a.c").chmod(0o644)
    assert failed.returncode != 0
    result = sync(source, mirror, work)
    assert result.returncode == 0, result.stderr
    assert (work / "src" / "a.c").read_bytes() == b"int a2;\n"


def test_deletion_survives_a_failed_sync(trees):
    source, mirror, work = trees
    sync(source, mirror, work)
    (source / "src" / "b.c").unlink()
    # rsync deletes b.c from the mirror, then fails on the unreadable new file
    (source / "src" / "c.c").write_bytes(b"int c;\n")
    (source / "src" / "c.c").chmod(0o000)
    failed = sync(source, mirror, work)
    (source / "src" / "c.c").chmod(0o644)
    assert failed.returncode != 0
    result = sync(source, mirror, work)
    assert result.returncode == 0, result.stderr
    assert not (work / "src" / "b.c").exists()
    assert (work / "src" / "c.c").read_bytes() == b"int c;\n"
