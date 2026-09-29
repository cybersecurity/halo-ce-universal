#!/usr/bin/env python3
"""Compile the upstream 2342 game C units with a native-host i386 musl ABI.

This is a compile-only feasibility stage: it does not link host services or
translate the resulting ELF objects.  Results are saved under build/macos-aot.
"""
import argparse
import concurrent.futures
import json
import re
import shutil
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


def generate_guest_syscall_header(template: Path, destination: Path) -> None:
    source = template.read_text()
    # Keep the existing arm64_32 dispatch IDs, adding Linux's shared time64
    # extension so musl 1.2's 64-bit time_t selects a 16-byte kernel record.
    # Without this macro it passes timespec64 to legacy clock syscall 113,
    # whose eight-byte result puts nanoseconds in the seconds' high word.
    if not re.search(r"^#define __NR_clock_gettime64\s", source, re.M):
        source += "\n#define __NR_clock_gettime64 403\n"
    if not re.search(r"^#define __NR_clock_nanosleep_time64\s", source, re.M):
        # Presence also enables musl's explicit time32 marshalling for small
        # legacy115/101 requests; otherwise it sends a 16-byte timespec there.
        source += "\n#define __NR_clock_nanosleep_time64 407\n"
    aliases = []
    for line in source.splitlines():
        match = re.match(r"#define __NR_([^\s]+)(\s+.+)$", line)
        if match:
            aliases.append("#define SYS_" + match.group(1) + match.group(2))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(source + "\n" + "\n".join(aliases) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--scope", choices=("game", "platform", "libc", "math"), default="game")
    parser.add_argument("--source", action="append", default=[],
                        help="compile only this manifest source; repeat for a selected batch")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--include-root", type=Path, default=ROOT / "build/deps/musl-include")
    parser.add_argument("--musl-root", type=Path, default=ROOT / "build/deps/musl-1.2.5")
    args = parser.parse_args()
    os.chdir(ROOT)
    out = Path("build/macos-aot")
    objdir = out / f"{args.scope}-obj"
    objdir.mkdir(parents=True, exist_ok=True)
    sources = []
    cfg = json.loads(Path("config/config.json").read_text())
    excluded = set(json.loads(Path("port/linux/port.json").read_text()).get("exclude_sources", []))
    if args.scope == "game":
        for proj in cfg["projects"]:
            if proj["name"] != "halobetacache":
                continue
            for obj in proj["objects"]:
                src = obj["name"]
                if str(obj["status"]).lower() == "missing" or src in excluded or Path(src).suffix.lower() != ".c":
                    continue
                sources.append(Path(src))
            break
        sources.extend(sorted(Path("port/linux/game").glob("*.c")))
    elif args.scope == "platform":
        sources.extend(source for source in sorted(Path("port/linux/src").glob("*.c"))
                       if not source.name.startswith("posix_"))
        sources.extend([Path("port/third_party/tomlc17/tomlc17.c"), Path("port/third_party/kcp/ikcp.c")])
        sources.extend(source for source in sorted(Path("port/android/guest/runtime").glob("*.c"))
                       if source.name != "guest_memory_watch.c")
    else:
        if args.scope == "libc":
            from tools import android_build
            android_build.MUSL_DIR = args.musl_root
            sources.extend(android_build._musl_sources())
            sources.extend(args.musl_root / "src" / name for name in (
                "env/__reset_tls.c", "misc/getpriority.c", "misc/setpriority.c",
                "thread/pthread_sigmask.c",
                "temp/__randname.c", "thread/clone.c", "thread/pthread_cleanup_push.c",
                "thread/pthread_barrier_init.c", "thread/pthread_barrier_wait.c",
                "thread/pthread_barrier_destroy.c", "thread/synccall.c",
            ))
            sources.extend(sorted((args.musl_root / "src/fenv").glob("*.c")))
            sources.extend(args.musl_root / "src" / name for name in (
                "setjmp/i386/setjmp.s", "setjmp/i386/longjmp.s", "fenv/i386/fenv.s",
            ))
        else:
            sources.extend(sorted(Path("port/third_party/musl-math/src").glob("*.c")))
    sources = sorted(set(sources))
    if args.source:
        selected = {Path(source).resolve() for source in args.source}
        known = {source.resolve() for source in sources}
        missing = selected - known
        if missing:
            parser.error("sources outside this scope's manifest: " + ", ".join(map(str, sorted(missing))))
        sources = [source for source in sources if source.resolve() in selected]
    if args.limit:
        sources = sources[:args.limit]

    incs = [
        args.include_root,
        args.musl_root / "arch/i386",
        args.musl_root / "arch/generic",
        args.musl_root / "include",
        Path("port/linux/include"),
        Path("port/include"),
        *[Path(p) for p in json.loads(Path("config/config.json").read_text())["projects"][0]["options"]["include_dirs"] if p != "xbox/include"],
    ]
    xdk = Path("port/include/xdk")
    common = [
        args.cc, "--target=i386-unknown-linux-musl", "-m32", "-fms-extensions", "-fshort-wchar",
        "-fcommon", "-fno-pic", "-fno-strict-aliasing", "-fwrapv",
        "-fno-delete-null-pointer-checks", "-freg-struct-return", "-fno-omit-frame-pointer",
        "-ffp-contract=off", "-O0", "-g", "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types", "-Wno-error=implicit-function-declaration",
        "-Wno-error=incompatible-function-pointer-types", "-Wno-error=int-conversion",
        "-Wno-error=implicit-int", "-Wno-error=return-type", "-nostdinc",
        "-fno-builtin-wcslen", "-fno-builtin-wcsnlen", "-fno-builtin-wcschr",
        "-fno-builtin-wcsrchr", "-fno-builtin-wcscmp", "-fno-builtin-wcsncmp",
        "-fno-builtin-wcscpy", "-fno-builtin-wcsncpy", "-fno-builtin-wcscat",
        "-fno-builtin-wcsncat", "-fno-builtin-wmemchr", "-fno-builtin-wmemcmp",
        "-fno-builtin-wmemcpy", "-fno-builtin-wmemmove", "-fno-builtin-wmemset",
        "-isystem", str(args.include_root), "-isystem", "port/macos/musl-i386",
        "-isystem", str(args.musl_root / "arch/i386"),
        "-isystem", str(args.musl_root / "arch/generic"), "-isystem", str(args.musl_root / "include"),
    ]
    # Game/platform code uses the Xbox-compatible 8-byte alignment mode.
    # The i386 musl ABI itself uses 4-byte alignment for 12-byte long double.
    if args.scope not in ("libc", "math"):
        common.append("-malign-double")
    if args.scope in ("game", "platform"):
        # Android uses compiler-emulated TLS for these guest objects. Native
        # i386 GS-base accesses would dereference host address zero in the
        # high-arena AOT runtime, so preserve the guest TLS ABI via emutls.
        common.append("-femulated-tls")
    if args.scope != "libc":
        common += ["-include", "port/linux/include/halo_linux_prefix.h",
                   "-include", "build/macos-aot/halo_msvc_semantics.h" if args.scope == "game"
                   else "build/macos-aot/platform_msvc_semantics.h"]
        for inc in incs[4:]:
            common += ["-I", str(inc)]
        common += ["-idirafter", str(xdk)]
        # Keep the project config's platform/game defines.
        common += ["-DDEBUG", "-Dxbox", "-DHALO_MACOS=1", "-DHALO_ANDROID=1"]
    if args.scope == "platform":
        syscall_arch_dir = ROOT / "build/deps/syscall-arch"
        syscall_arch_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile("port/android/guest/libc/arch/arm64_32/syscall_arch.h",
            syscall_arch_dir / "syscall_arch.h")
        common += ["-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER=1", "-std=gnu11",
                   "-I", "port/linux/src", "-I", "port/third_party/tomlc17",
                   "-I", "port/third_party/kcp", "-I", "port/android/guest/runtime",
                   "-I", "port/android/include", "-I", "port/android/guest/libc/src_include",
                   # The platform's musl internal syscall wrappers must use
                   # the same host-dispatched guest syscall ABI as libc, but
                   # retain the i386 arch's signal/fenv layouts.
                   "-I", str(syscall_arch_dir),
                   "-I", str(ROOT / "build/deps/musl-internal"), "-I", str(args.musl_root / "src/internal"),
                   "-I", str(args.musl_root / "src/include"), "-I", os.environ.get("HALO_BREW_INCLUDE", "/opt/homebrew/include")]
    elif args.scope == "libc":
        # Keep syscall IDs aligned with Android's guest-host syscall ABI,
        # while all other machine types remain the i386 ILP32 definitions.
        syscall_template = Path("port/android/guest/libc/arch/arm64_32/bits/syscall.h.in")
        syscall_header = args.include_root / "bits/syscall.h"
        syscall_arch_dir = ROOT / "build/deps/syscall-arch"
        syscall_arch_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile("port/android/guest/libc/arch/arm64_32/syscall_arch.h",
                        syscall_arch_dir / "syscall_arch.h")
        if syscall_template.exists():
            generate_guest_syscall_header(syscall_template, syscall_header)
        # Reuse Android's syscall interposer so translated musl code never
        # executes raw i386 int 0x80 on Darwin.
        common += ["-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700",
                   "-I", str(syscall_arch_dir),
                   "-I", "port/macos/musl-i386",
                   "-I", str(args.musl_root / "arch/i386"),
                   "-I", str(args.musl_root / "arch/generic"),
                   "-I", str(ROOT / "build/deps/musl-internal"),
                   "-I", str(args.musl_root / "src/include"),
                   "-I", str(args.musl_root / "src/internal")]
    elif args.scope == "math":
        common += ["-std=gnu11", "-w", "-I", "port/third_party/musl-math/include",
                   "-include", "port/third_party/musl-math/include/libm.h"]

    def compile_one(src: Path):
        try:
            rel_src = src.resolve().relative_to(args.musl_root.resolve())
        except ValueError:
            rel_src = src.resolve().relative_to(ROOT)
        obj = objdir / rel_src.with_suffix(".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        cmd = [*common, "-c", str(src), "-o", str(obj)]
        start = time.monotonic()
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return src.as_posix(), p.returncode, p.stderr, time.monotonic() - start

    started = time.monotonic()
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, src) for src in sources]
        for i, fut in enumerate(concurrent.futures.as_completed(futures), 1):
            result = fut.result()
            results.append(result)
            if i % 50 == 0 or i == len(sources):
                ok = sum(r[1] == 0 for r in results)
                print(f"progress {i}/{len(sources)} complete={ok} failed={i-ok}", flush=True)

    results.sort()
    log_scope = args.scope + ("-selected" if args.source else "")
    log = out / f"{log_scope}-compile-results.json"
    log.write_text(json.dumps([{"source": s, "returncode": rc, "stderr": err, "seconds": sec}
                              for s, rc, err, sec in results], indent=2) + "\n")
    failures = [r for r in results if r[1] != 0]
    summary = [
        f"sources={len(sources)} compiled={len(sources)-len(failures)} failed={len(failures)}",
        f"elapsed_seconds={time.monotonic()-started:.1f}",
        f"results={log}",
    ]
    (out / f"{log_scope}-compile-summary.txt").write_text("\n".join(summary) + "\n")
    print("\n".join(summary))
    for src, rc, stderr, _ in failures[:20]:
        errors = [line for line in stderr.splitlines() if "error:" in line or "fatal error:" in line]
        print(f"FAIL {src}: " + (errors[0] if errors else stderr[:300].replace("\n", " ")))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
