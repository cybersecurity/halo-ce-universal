#!/usr/bin/env python3
"""Check that a change leaves the 32-bit builds' code as it was.

Compiles every unit of the 32-bit Linux build (ninja linux) at two revisions,
without debug information, disassembles each object and compares them. A
unit whose code differs is listed; a game unit (source/) that differs makes
the check fail, since the game sources are also what the byte-matching
build and the other 32-bit ports compile. Platform units (port/linux) may
differ on purpose, and are reported for review.

    tools/port_neutrality_check.py [--base upstream/main] [--head HEAD]
        [--workdir DIR] [--sysroot DIR]

The revisions are checked out as worktrees in --workdir (default
build/neutrality), which must be on a case-sensitive file system: the port's
include/StdDef.h would otherwise stand in for <stddef.h>. On Linux the host's
32-bit headers serve (--sysroot unneeded). Elsewhere, --sysroot names a tree
holding a 32-bit x86 Linux usr/include, for instance Debian's i386 libc6-dev
and linux-libc-dev packages extracted into one folder; on macOS a
case-sensitive work folder is a disk image:

    hdiutil create -size 4g -type SPARSE -fs 'Case-sensitive APFS' -volname halocs cs.sparseimage
    hdiutil attach -mountpoint /some/dir cs.sparseimage
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(command, cwd, check=True):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=check)


def objdump_tool() -> str:
    for candidate in ("llvm-objdump", "/opt/homebrew/opt/llvm/bin/llvm-objdump", "/usr/local/opt/llvm/bin/llvm-objdump"):
        if shutil.which(candidate) or Path(candidate).is_file():
            return candidate
    sys.exit("llvm-objdump not found (install LLVM)")


def case_sensitive(directory: Path) -> bool:
    probe = directory / "CaseProbe"
    probe.write_text("")
    try:
        return not (directory / "caseprobe").exists()
    finally:
        probe.unlink()


def prepare(workdir: Path, name: str, revision: str) -> Path:
    tree = workdir / name
    if tree.exists():
        run(["git", "worktree", "remove", "--force", str(tree)], ROOT, check=False)
        shutil.rmtree(tree, ignore_errors=True)
    run(["git", "worktree", "add", "--detach", "-f", str(tree), revision], ROOT)
    run([sys.executable, "configure.py"], tree)
    run(["ninja", "build/linux/halo_msvc_semantics.h", "build/linux/platform_msvc_semantics.h"], tree)
    # the sources the build generates (the high-res HUD's textures), which
    # the units are compiled from
    generated = [line.split(":")[0] for line in run(["ninja", "-t", "targets", "all"], tree).stdout.splitlines()
                 if line.startswith("build/linux/generated/") and line.split(":")[0].endswith(".c")]
    if generated:
        run(["ninja", *generated], tree)
    return tree


def units(tree: Path):
    targets = run(["ninja", "-t", "targets", "all"], tree).stdout
    return sorted(line.split(":")[0] for line in targets.splitlines()
                  if line.startswith("build/linux/obj/") and line.split(":")[0].endswith(".o")
                  and "/third_party/" not in line and "/posix_" not in line)


def disassemble(tree: Path, unit: str, out: Path, sysroot, objdump: str, extra_includes):
    command = run(["ninja", "-t", "commands", unit], tree).stdout.strip().splitlines()[-1]
    command = re.sub(r"-MMD -MF ('[^']*'\S*|\"[^\"]*\"|\S+) ", "", command)
    command = command.replace("-march=native", "").replace(" -g ", " -g0 ")
    command = re.sub(r"-flto=\S+", "", command)
    command = re.sub(r"-fprofile-use=\S+", "", command)
    obj = out / (unit.replace("/", "_").replace(" ", "_"))
    command = command.rsplit(" -o ", 1)[0] + f" -o '{obj}' -w"
    if sysroot:
        command += f" --sysroot='{sysroot}'"
    for directory in extra_includes:
        command += f" -idirafter '{directory}'"
    result = subprocess.run(command, cwd=tree, shell=True, capture_output=True, text=True)
    if result.returncode:
        return None, result.stderr[-300:]
    text = run([objdump, "-d", "-r", "--no-show-raw-insn", "--no-leading-addr", str(obj)], tree).stdout
    return "\n".join(text.splitlines()[2:]), None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="upstream/main")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--workdir", type=Path, default=ROOT / "build" / "neutrality")
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--include-after", type=Path, action="append", default=None,
                        help="more headers, after the system's (SDL3's); default Homebrew's include folder if any")
    args = parser.parse_args()
    args.workdir.mkdir(parents=True, exist_ok=True)
    if not case_sensitive(args.workdir):
        sys.exit(f"{args.workdir} is on a case-insensitive file system (see --help)")
    objdump = objdump_tool()
    if args.include_after is None:
        args.include_after = [path for path in (Path("/opt/homebrew/include"), Path("/usr/local/include"))
                              if (path / "SDL3").is_dir()] if args.sysroot else []
    base_rev = run(["git", "rev-parse", args.base], ROOT).stdout.strip()
    head_rev = run(["git", "rev-parse", args.head], ROOT).stdout.strip()
    trees = {"base": prepare(args.workdir, "base", base_rev), "head": prepare(args.workdir, "head", head_rev)}
    code = {}
    for name, tree in trees.items():
        out = args.workdir / f"{name}.obj"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir()
        with ThreadPoolExecutor(os.cpu_count()) as pool:
            results = dict(zip(units(tree), pool.map(lambda unit: disassemble(tree, unit, out, args.sysroot, objdump, args.include_after),
                                                     units(tree))))
        failed = {unit: error for unit, (text, error) in results.items() if error}
        if failed:
            for unit, error in list(failed.items())[:5]:
                print(f"{name}: {unit} failed to compile:\n{error}")
            sys.exit(f"{len(failed)} units of {name} failed to compile")
        code[name] = {unit: text for unit, (text, _) in results.items()}
    base, head = code["base"], code["head"]
    common = sorted(set(base) & set(head))
    differing = [unit for unit in common if base[unit] != head[unit]]
    game = [unit for unit in differing if unit.startswith("build/linux/obj/source/")]
    print(f"{len(common) - len(differing)} of {len(common)} units byte-identical "
          f"({args.base} {base_rev[:8]} -> {args.head} {head_rev[:8]})")
    for unit in sorted(set(head) - set(base)):
        print(f"  new:      {unit}")
    for unit in sorted(set(base) - set(head)):
        print(f"  gone:     {unit}")
    for unit in differing:
        print(f"  {'GAME' if unit in game else 'platform'}: {unit}")
    sys.exit(1 if game else 0)


if __name__ == "__main__":
    main()
