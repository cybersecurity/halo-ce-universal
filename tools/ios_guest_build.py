"""Build the iOS ILP32 game image for embedding in the native app.

The portable guest runtime uses 32-bit game pointers. Its ARM64 instructions
are lifted into the iOS address arena; no Android SDK, NDK, or runtime is used.
"""

import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List

from .linux_build import MUSL_MATH_DIR, musl_math_sources, game_sources, game_defines_and_includes, LINUX_PROFILE, XDK_INCLUDE, compile_launcher, pgo_mode, pgo_profile, profile_use_flags, xdk_headers
from .ninja_syntax import Writer
from .embed_assets import hud_assets_build

PORT_DIR = Path("port/runtime")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/ios")
THIRD_PARTY = Path("build/third_party")
# the TOML parser config.toml is read with (port/linux/src/port_config.c)
TOML_DIR = Path("port/third_party/tomlc17")
KCP_DIR = Path("port/third_party/kcp")
MUSL_VERSION = "1.2.5"
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION}"
MUSL_URL = f"https://musl.libc.org/releases/musl-{MUSL_VERSION}.tar.gz"
SDL_TAG = "release-3.4.16"
SDL_DIR = THIRD_PARTY / "SDL3"
SDL_URL = "https://github.com/libsdl-org/SDL.git"

# The guest ABI: AArch64 code with 32-bit pointers (clang's only such target
# is Apple's arm64_32, whose Mach-O output is converted afterwards). The
# Darwin environment is hidden from the sources; the C library is musl.
GUEST_ABI_FLAGS = [
    "--target=arm64_32-apple-watchos",
    "-U__APPLE__",
    "-U__MACH__",
    "-fno-define-target-os-macros",
    "-D__linux__=1",
    "-D__unix__=1",
    "-DHALO_ILP32=1",
    # Baseline ARMv8 instructions; Darwin otherwise enables newer features.
    "-mcpu=cortex-a53",
    "-nostdinc",
    "-fshort-wchar",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-femulated-tls",
    "-mllvm",
    "-aarch64-neon-syntax=generic",
    # no fused multiply-add: the game was written for x87/SSE arithmetic,
    # and its debug assertions (colours within 0..1, unit vectors) trip on
    # the different rounding of fused operations
    "-ffp-contract=off",
    "-O2",
]

# as the Linux build (tools/linux_build.py), minus what only x86 needs
GUEST_CODE_FLAGS = [
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

MUSL_DIRECTORIES = [
    "conf", "ctype", "dirent", "env", "errno", "exit", "fcntl", "internal",
    "locale", "malloc", "malloc/mallocng", "math", "mman", "multibyte",
    "prng", "sched", "select", "signal", "stat", "stdio", "stdlib", "string",
    "time", "unistd",
]
MUSL_FILES = [
    "thread/__lock.c", "thread/__wait.c", "thread/__timedwait.c",
    "thread/__syscall_cp.c", "thread/vmlock.c", "thread/pthread_self.c",
    "thread/pthread_equal.c", "thread/pthread_once.c",
    "thread/pthread_setcancelstate.c", "thread/pthread_testcancel.c",
    "thread/default_attr.c", "thread/lock_ptc.c", "misc/getauxval.c",
    "linux/sysinfo.c", "misc/basename.c", "misc/dirname.c",
    "misc/realpath.c", "misc/uname.c", "misc/ioctl.c", "misc/getrlimit.c",
    "misc/syscall.c", "network/htonl.c", "network/htons.c", "network/ntohl.c",
    "network/ntohs.c", "network/inet_addr.c", "network/inet_aton.c",
    "network/inet_ntoa.c", "network/inet_pton.c", "network/inet_ntop.c",
]
MUSL_THREAD_PREFIXES = (
    "pthread_attr_", "pthread_cond", "pthread_mutex", "pthread_rwlock",
    "pthread_spin", "sem_",
)
# replaced by the guest runtime (port/runtime/guest/runtime)
MUSL_EXCLUDE = {
    "env/__stack_chk.c", "env/__init_tls.c", "env/__libc_start_main.c",
    "env/__reset_tls.c", "malloc/oldmalloc", "thread/pthread_create.c",
    # unused, and its compiler barrier is an inline assembly statement
    "string/explicit_bzero.c",
}
# game files that call variadic functions without a prototype in scope, which
# only works under x86's calling convention (tools/guest_abi_check.py)
VARIADIC_PROTOTYPE_FILES = {
    "source/ai/action_uncover.c", "source/ai/ai.c", "source/ai/ai_debug.c",
    "source/bungie_net/common/public_key_crypt.c", "source/camera/editor_flying_camera.c",
    "source/game/cheats.c", "source/game/game_engine.c", "source/game/players.c",
    "source/hs/hs.c", "source/interface/hud_nav_points.c",
    "source/networking/telnet_console.c", "source/rasterizer/xbox/rasterizer_xbox_errors.c",
    "source/render/render.c",
}


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def fetch_third_party() -> None:
    """Download musl and SDL3 (configure time, once)."""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    if not MUSL_DIR.is_dir():
        print(f"Downloading {MUSL_URL}")
        archive = THIRD_PARTY / f"musl-{MUSL_VERSION}.tar.gz"
        subprocess.run(["curl", "-sSfL", "-o", str(archive), MUSL_URL], check=True)
        subprocess.run(["tar", "xzf", archive.name], cwd=THIRD_PARTY, check=True)
        archive.unlink()
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG}")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)],
                       check=True)


def _musl_sources() -> List[Path]:
    src = MUSL_DIR / "src"
    result = set()
    for directory in MUSL_DIRECTORIES:
        for path in (src / directory).glob("*.c"):
            result.add(path)
    for name in MUSL_FILES:
        result.add(src / name)
    for path in (src / "thread").glob("*.c"):
        if path.name.startswith(MUSL_THREAD_PREFIXES):
            result.add(path)
    sources = []
    for path in sorted(result):
        relative = path.relative_to(src).as_posix()
        if relative in MUSL_EXCLUDE or any(relative.startswith(e + "/") for e in MUSL_EXCLUDE):
            continue
        sources.append(path)
    return sources


def ios_guest_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "guest" / "runtime", Path("port/ios/host"), LINUX_DIR / "src"]


def generate_ios_guest_build(n: Writer, sln: Any) -> None:
    if not getattr(sln, "ios", False):
        return
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file():
        return
    try:
        fetch_third_party()
    except (subprocess.CalledProcessError, OSError) as error:
        raise RuntimeError(f"Cannot fetch musl/SDL3: {error}") from error
    import json
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))

    toolchain = Path(sln.ios_llvm)
    sysroot_include = Path("build/ios/gl_include").resolve()
    guest_cc = str(toolchain / "bin/clang")
    converter = "tools/ios_asm_convert.py"

    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    gl_include = guest_dir / "gl_include"
    arch = PORT_DIR / "guest" / "libc" / "arch" / "arm64_32"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = BUILD / "halo_guest.elf"
    python = "$python"

    n.comment("iOS guest (ninja ios_guest); see port/ios/README.md")
    n.variable("guest_cc", guest_cc)
    n.variable("guest_tool_bin", str(toolchain / "bin"))
    n.variable("guest_linker", sln.ios_lld)

    # ---------- generated headers and sources

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name="guest_alltypes",
        command=f"sed -f {MUSL_DIR}/tools/mkalltypes.sed $in > $out",
        description="IOS MUSL $out",
    )
    n.build(outputs=alltypes, rule="guest_alltypes",
            inputs=[arch / "bits" / "alltypes.h.in", MUSL_DIR / "include" / "alltypes.h.in"])
    n.rule(
        name="guest_syscall_h",
        command="cp $in $out && sed -n -e s/__NR_/SYS_/p < $in >> $out",
        description="IOS MUSL $out",
    )
    n.build(outputs=syscall_h, rule="guest_syscall_h", inputs=arch / "bits" / "syscall.h.in")
    n.rule(
        name="guest_version_h",
        command=f"echo '#define VERSION \"{MUSL_VERSION}\"' > $out",
        description="IOS MUSL $out",
    )
    n.build(outputs=version_h, rule="guest_version_h")

    # Khronos OpenGL ES declarations for the guest
    gl_stamp = gl_include / "stamp"
    n.rule(
        name="guest_gl_include",
        command=(f"mkdir -p {gl_include} && ln -sfn {sysroot_include}/GLES2 {gl_include}/GLES2 && "
                 f"ln -sfn {sysroot_include}/GLES3 {gl_include}/GLES3 && "
                 f"ln -sfn {sysroot_include}/KHR {gl_include}/KHR && touch $out"),
        description="IOS GL HEADERS",
    )
    n.build(outputs=gl_stamp, rule="guest_gl_include")

    guest_gl_c = gen_dir / "guest_gl.c"
    gl_imports = gen_dir / "gl_imports.list"
    n.rule(
        name="guest_gl_stubs",
        command=(f"{python} tools/guest_gl_stubs.py {LINUX_DIR}/src/gl.h {sysroot_include}/GLES3/gl32.h "
                 f"{sysroot_include}/GLES2/gl2ext.h {guest_gl_c} {gl_imports}"),
        description="IOS GL STUBS",
    )
    n.build(outputs=[guest_gl_c, gl_imports], rule="guest_gl_stubs",
            implicit=[Path("tools/guest_gl_stubs.py"), LINUX_DIR / "src" / "gl.h"])

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.rule(
        name="guest_posix_stubs",
        command=f"{python} tools/guest_posix_stubs.py {LINUX_DIR}/src/posix.h {guest_posix_c} {posix_imports}",
        description="IOS POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, posix_imports], rule="guest_posix_stubs",
            implicit=[Path("tools/guest_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"])

    imports_s = gen_dir / "imports.s"
    host_table_c = BUILD / "host" / "host_import_table.c"
    host_imports_list = PORT_DIR / "host_imports.list"
    n.rule(
        name="guest_imports",
        command=f"{python} tools/guest_imports.py --host-table {host_table_c} {imports_s} $in",
        description="IOS IMPORTS",
    )
    n.build(outputs=[imports_s, host_table_c], rule="guest_imports",
            inputs=[host_imports_list, Path("port/ios/host_imports.list"), posix_imports, gl_imports],
            implicit=[Path("tools/guest_imports.py")])

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, gl_stamp,
                         semantics_header, platform_semantics_header]

    # ---------- guest compilation: C -> Darwin assembly -> ELF assembly -> object

    n.rule(
        name="guest_cc",
        command=(f"{compile_launcher(sln)}$guest_cc -MMD -MF $out.d $cflags -S $in -o $out.darwin.s && "
                 f"{python} {converter} $out.darwin.s $out.s && "
                 f"$guest_cc --target=aarch64-none-elf -c $out.s -o $out"),
        description="IOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="guest_as",
        command="$guest_cc --target=aarch64-none-elf -c $in -o $out",
        description="IOS AS $out",
    )

    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch}", f"-isystem {MUSL_DIR}/arch/generic",
        f"-isystem {MUSL_DIR}/include",
    ]
    guest_abi = " ".join(GUEST_ABI_FLAGS + ["-DHALO_IOS=1", "-DHALO_IOS_BROWSER=1", "-DHALO_LINUX=1", "-ffixed-x15", "-ffixed-x27", "-mllvm", "-aarch64-enable-compress-jump-tables=false"] + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []))
    guest_code = " ".join(GUEST_CODE_FLAGS)
    tool_implicit = [Path(converter), Path("tools/guest_asm_convert.py"), *generated_headers]
    # profile-guided optimisation with the Linux build's profile (committed,
    # or trained by the Linux build with --pgo=train): the game and platform
    # code are the same, and functions that differ simply go without
    profile = pgo_profile(sln, LINUX_PROFILE if pgo_mode(sln) == "train" else None, [LINUX_PROFILE], guest_cc)
    profile_flags = " ".join(profile_use_flags(profile))
    if profile:
        tool_implicit.append(profile)

    def guest_object(source: Path, cflags: str, prefix: str = "") -> Path:
        obj = obj_dir / prefix / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(BUILD)):
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        n.build(outputs=obj, rule="guest_cc", inputs=source, implicit=tool_implicit,
                variables={"cflags": cflags})
        return obj

    # musl
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{PORT_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources()]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name="guest_ar",
        command="rm -f $out && $guest_tool_bin/llvm-ar rcs $out @$out.rsp",
        description="IOS AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule="guest_ar", inputs=musl_objects)

    # the game
    objects: List[Path] = []
    excluded = set(config.get("exclude_sources", []))
    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(game_flags), profile_flags,
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include", game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {PORT_DIR}/include/halo_guest_variadic_prototypes.h"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))

    # the platform layer shared with Linux, and the guest runtime
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w", profile_flags,
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{PORT_DIR}/guest/runtime",
        f"-I{PORT_DIR}/include", f"-I{TOML_DIR}", f"-I{KCP_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", f"-I{gl_include}", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    guest_host_only = {"memory_watch.c"}  # replaced by guest_memory_watch.c
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_host_only:
            continue
        objects.append(guest_object(source, platform_cflags))
    for source in [Path("port/apple/no_embedded_art.c")]:
        objects.append(guest_object(source, platform_cflags))
    math_cflags = " ".join([guest_abi, "-std=gnu11", "-w", *libc_includes,
        f"-I{MUSL_MATH_DIR}/include", f"-include {MUSL_MATH_DIR}/include/libm.h"])
    for source in musl_math_sources():
        objects.append(guest_object(source, math_cflags))
    objects.append(guest_object(Path("port/ios/network/guest_room.c"), platform_cflags))
    # the settings file's parser (port/third_party/tomlc17)
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    # internet play's reliable streams (port/third_party/kcp; p2p.c)
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        f"-I{PORT_DIR}/guest/runtime", f"-I{PORT_DIR}/include",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{PORT_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{PORT_DIR}/guest/runtime", f"-I{PORT_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{SDL_DIR}/include", f"-I{gl_include}", *libc_includes,
    ])
    runtime_dir = PORT_DIR / "guest" / "runtime"
    for source in sorted(runtime_dir.glob("*.c")):
        if source.name in ("guest_thread.c", "guest_start.c"):
            objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name == "guest_memory_watch.c":
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    objects.append(guest_object(guest_gl_c, runtime_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="guest_as", inputs=imports_s)
    objects.append(imports_o)

    # ---------- the guest image

    linker_script = Path("port/ios/guest.ld")
    n.rule(
        name="guest_link",
        command=(f"$guest_linker -m aarch64linux -static -nostdlib -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc}"),
        description="IOS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=image, rule="guest_link", inputs=objects, implicit=[libguestc, linker_script])

    n.build(outputs="ios_guest", rule="phony", inputs=image)
