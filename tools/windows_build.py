"""Ninja rules for the native Windows build (``ninja windows``).

Like the Linux build (tools/linux_build.py), it compiles the game sources
with clang for 32-bit x86 Windows (i686-pc-windows-msvc), adds the platform layer shared with
Linux (``port/linux/src``) and the Windows parts in ``port/windows``, and
links ``build/windows/halo.exe`` with lld. It is generated only when
configure.py runs on Windows. See port/windows/README.md for the design.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Dict, List, Optional

from .linux_build import (LINUX_PROFILE, MINIUPNPC_DIR, OPTIMISATION, WINDOWS_PROFILE, XDK_INCLUDE, lto_mode,
                          march_flag, miniupnpc_sources, pgo_mode, compile_launcher, game_defines_and_includes,
                          game_sources, musl_math_cflags, musl_math_sources, pgo_profile, profile_use_flags,
                          xdk_headers)
from .embed_assets import hud_assets_build, hud_configure_inputs
from .ninja_syntax import Writer

LINUX_DIR = Path("port/linux")
PORT_DIR = Path("port/windows")
PORT_CONFIG = PORT_DIR / "port.json"
BUILD = Path("build/windows")

SDL_VERSION = "3.4.16"
SDL_URL = (
    f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/"
    f"SDL3-devel-{SDL_VERSION}-VC.zip"
)
THIRD_PARTY = BUILD / "third_party"
SDL_DIR = THIRD_PARTY / f"SDL3-{SDL_VERSION}"

# Flags shared by every unit. The Microsoft target gives the game the ABI it
# was written against natively: 16-bit wchar_t, MSVC structure layout,
# __declspec, calling conventions and COMDAT inline functions.
#  - 32-bit time_t, as in the Xbox's C runtime (the same on both sides of the
#    platform layer, which share struct timespec),
#  - tentative definitions shared between units (-fcommon),
#  - no optimisations that assume the absence of MSVC-tolerated UB,
#  - EBP frames (MSVC /Oy-): get_return_eip and the stack walker follow the
#    frame chain.
# the TOML parser the platform layer reads config.toml with (port_config.c)
TOML_DIR = Path("port/third_party/tomlc17")
KCP_DIR = Path("port/third_party/kcp")


def updater_defines(release: bool) -> str:
    """the self-updater's build (port/linux/src/updater.c): its number, from
    HALO_BUILD_NUMBER (tools/ci_build.py gives it for builds of main; none
    elsewhere, which never look for updates), and its configuration"""
    number = os.environ.get("HALO_BUILD_NUMBER", "0")
    if not number.isdigit():
        number = "0"
    flavor = "release" if release else "debug"
    return f'-DHALO_BUILD_NUMBER={number} -DHALO_BUILD_FLAVOR=\\"{flavor}\\"'

WINDOWS_ABI_FLAGS = [
    "--target=i686-pc-windows-msvc",
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    # the same floating point results on every port (every machine in a
    # system link game simulates it from the same inputs): no
    # fused multiply-adds (port/include/halo_math.h)
    "-ffp-contract=off",
    OPTIMISATION,
    "-g",
    "-gcodeview",
    "-D_USE_32BIT_TIME_T",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-D_CRT_NONSTDC_NO_WARNINGS",
    "-D_WINSOCK_DEPRECATED_NO_WARNINGS",
    # the MSVC 7 signatures the game uses: swprintf without a buffer size,
    # two-argument wcstok
    "-D_CRT_NON_CONFORMING_SWPRINTFS",
    "-D_CRT_NON_CONFORMING_WCSTOK",
]

GAME_FLAGS = [
    "-std=gnu89",
    # a desktop application: the main menu offers EXIT GAME
    # (port/linux/include/halo_exit_game.h)
    "-DHALO_DESKTOP_APPLICATION",
    "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

PLATFORM_FLAGS = [
    "-std=gnu11",
    "-DHALO_LINUX_PLATFORM_LAYER",
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unknown-pragmas",
    "-Wno-microsoft-anon-tag",
    "-Wno-pragma-pack",
    "-Wno-ignored-attributes",
    "-Wno-duplicate-decl-specifier",
    "-Wno-missing-braces",
    "-Wno-unused-variable",
    "-Wno-ignored-pragmas",
    "-Wno-microsoft-enum-forward-reference",
    "-Wno-language-extension-token",
]

# port/windows/src/win32_*.c talk to Windows only: they see the Windows SDK
# instead of the Xbox SDK (the two define the same names differently).
WIN32_FLAGS = [
    "-std=gnu11",
    "-DWIN32_LEAN_AND_MEAN",
    "-DNOMINMAX",
    "-D_WIN32_WINNT=0x0A00",
    "-Wall",
]


def _load_config() -> Dict[str, Any]:
    with open(PORT_CONFIG, "r", encoding="utf-8") as f:
        return json.load(f)


def windows_configure_inputs() -> List[Path]:
    """Files whose change must re-run configure.py."""
    return [Path(__file__), PORT_CONFIG, PORT_DIR / "src", LINUX_DIR / "src", LINUX_DIR / "game", *hud_configure_inputs()]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def fetch_sdl() -> None:
    """Downloads SDL3's Visual C++ development package (headers, import
    library and DLL) once."""
    if (SDL_DIR / "lib" / "x86" / "SDL3.lib").is_file():
        return
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    archive = THIRD_PARTY / f"SDL3-devel-{SDL_VERSION}-VC.zip"
    print(f"Downloading {SDL_URL}")
    with urllib.request.urlopen(SDL_URL) as response, open(archive, "wb") as f:
        shutil.copyfileobj(response, f)
    with zipfile.ZipFile(archive) as z:
        z.extractall(THIRD_PARTY)
    archive.unlink()


# The runtime of instrumented builds (-fprofile-generate), which LLVM for
# Windows ships only for x86-64: its C sources, from the LLVM release of the
# compiler that uses them, built for the 32-bit game.
PROFILE_RUNTIME_SOURCES = [
    "InstrProfiling.c", "InstrProfilingBuffer.c", "InstrProfilingFile.c", "InstrProfilingInternal.c",
    "InstrProfilingMerge.c", "InstrProfilingMergeFile.c", "InstrProfilingNameVar.c",
    "InstrProfilingPlatformWindows.c", "InstrProfilingUtil.c", "InstrProfilingValue.c",
    "InstrProfilingVersionVar.c", "InstrProfilingWriter.c", "WindowsMMap.c",
]
PROFILE_RUNTIME_HEADERS = [
    "lib/profile/InstrProfiling.h", "lib/profile/InstrProfilingInternal.h", "lib/profile/InstrProfilingPort.h",
    "lib/profile/InstrProfilingUtil.h", "lib/profile/WindowsMMap.h", "include/profile/InstrProfData.inc",
    "include/profile/instr_prof_interface.h", "include/profile/MIBEntryDef.inc", "include/profile/MemProfData.inc",
]


def clang_release(cc: str) -> Optional[str]:
    """the release (22.1.0) of the clang named cc, or None"""
    try:
        output = subprocess.run([cc, "--version"], capture_output=True, text=True, check=False).stdout
    except OSError:
        return None
    match = re.search(r"clang version (\d+\.\d+\.\d+)", output)
    return match.group(1) if match else None


def fetch_profile_runtime(release: str) -> Path:
    """Downloads compiler-rt's profile runtime sources for this release once;
    returns their directory (with lib/profile and include/profile)."""
    root = THIRD_PARTY / f"compiler-rt-profile-{release}"
    files = [f"lib/profile/{name}" for name in PROFILE_RUNTIME_SOURCES] + PROFILE_RUNTIME_HEADERS
    for name in files:
        target = root / name
        if target.is_file():
            continue
        url = f"https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-{release}/compiler-rt/{name}"
        print(f"Downloading {url}")
        target.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(url) as response:
            data = response.read()
        target.write_bytes(data)
    return root


# a C __inline function with external linkage, defined in a .c file
EXPORTED_INLINE = re.compile(
    r"^(?!\s*static\b)[^;{}()\n]*\b(?:__inline|_inline|__forceinline)\b[^;{}()]*?"
    r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{",
    re.M,
)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def inline_export_wrapper(source: Path) -> Path:
    """MSVC emits a C __inline function with external linkage wherever a
    call to it is not inlined, and other units may call it through a
    prototype; clang can inline every call in the defining unit, leaving no
    copy. A unit that defines such a function is compiled through a
    generated wrapper that includes it and takes the functions' addresses,
    which makes clang emit them (as COMDATs). Returns the file to compile."""
    if source.suffix != ".c" or not str(source).replace(os.sep, "/").startswith("source/"):
        return source
    names = EXPORTED_INLINE.findall(COMMENT.sub("", source.read_text(encoding="utf-8", errors="replace")))
    if not names:
        return source
    wrapper = BUILD / "inline_exports" / source
    text = (
        "/* generated by tools/windows_build.py: see inline_export_wrapper */\n"
        f'#include "{source.resolve().as_posix()}"\n'
        "static void *const halo_windows_inline_exports[] __attribute__((used)) = {\n"
        + "".join(f"\t(void *){name},\n" for name in names)
        + "};\n"
    )
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    if not wrapper.is_file() or wrapper.read_text(encoding="utf-8") != text:
        wrapper.write_text(text, encoding="utf-8")
    return wrapper


def generate_windows_build(n: Writer, sln: Any) -> None:
    if sys.platform != "win32" or not PORT_CONFIG.is_file():
        return
    try:
        fetch_sdl()
    except OSError as error:
        print(f"Windows build disabled: cannot fetch SDL3 ({error})", file=sys.stderr)
        return
    linux_config: Dict[str, Any] = json.loads((LINUX_DIR / "port.json").read_text(encoding="utf-8"))
    config = _load_config()

    tags_header = BUILD / "halo_msvc_tags.h"
    obj_dir = BUILD / "obj"
    output = BUILD / "halo.exe"
    sdl_dll = BUILD / "SDL3.dll"
    cc = getattr(sln, "windows_cc", None) or "clang"
    prefix_header = PORT_DIR / "include" / "halo_windows_prefix.h"
    crt_include = PORT_DIR / "include" / "crt"
    posix_include = PORT_DIR / "include" / "posix"

    n.comment("Native Windows build (ninja windows)")
    n.variable("windows_cc", cc)
    # MSVC gives struct tags first named in a prototype file scope; clang
    # does not (the Linux build's generator also writes these declarations)
    n.rule(
        name="windows_msvc_tags",
        command="$python tools/linux_msvc_semantics.py --output $out --tags source",
        description="WINDOWS MSVC TAGS $out",
        restat=True,
    )
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    n.build(outputs=tags_header, rule="windows_msvc_tags",
            implicit=[Path("tools/linux_msvc_semantics.py"), *game_headers])
    n.rule(
        name="windows_cc",
        command=f"{compile_launcher(sln)}$windows_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="WINDOWS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="windows_link",
        command="$windows_cc $ldflags -o $out --rsp-quoting=windows @$out.rsp $libs",
        description="WINDOWS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="windows_copy",
        command="$python -c \"import shutil,sys; shutil.copyfile(sys.argv[1], sys.argv[2])\" $in $out",
        description="WINDOWS COPY $out",
    )

    # the high-res HUD's textures (port/assets/hud; port/linux/src/hud_hires.c)
    embedded_assets = hud_assets_build(n, "windows", BUILD / "generated" / "hud_hires_assets.c")

    abi = " ".join(WINDOWS_ABI_FLAGS + [march_flag(sln)] + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []))
    sdl_include = SDL_DIR / "include"
    libs = " ".join(
        [_quote(SDL_DIR / "lib" / "x86" / "SDL3.lib")]
        + [f"-l{lib}" for lib in config.get("libraries", [])]
    )
    base_ldflags = [
        "--target=i686-pc-windows-msvc",
        "-fuse-ld=lld",
        "-g",
        # the Xbox memory window is at 0x80000000, in the upper half of the
        # 32-bit address space
        "-Wl,/LARGEADDRESSAWARE",
        "-Wl,/STACK:0x800000",
        "-Wl,/SUBSYSTEM:CONSOLE",
    ]

    def emit(obj_dir: Path, output: Path, extra_cflags: List[str], extra_ldflags: List[str],
             extra_objects: List[Path], implicit_inputs: List[Path]) -> None:
        """the objects and the executable, with the given extra flags"""
        extra = " ".join(extra_cflags)
        objects: List[Path] = []

        def add_object(source: Path, cflags: str) -> None:
            obj = obj_dir / source.with_suffix(".o")
            objects.append(obj)
            compiled = inline_export_wrapper(source)
            n.build(
                outputs=obj,
                rule="windows_cc",
                inputs=compiled,
                implicit=[*xdk_headers(), prefix_header, tags_header, source, *implicit_inputs],
                variables={"cflags": f"{cflags} {extra}"},
            )

        game_cflags = " ".join([
            abi,
            " ".join(GAME_FLAGS),
            f"-include {prefix_header}",
            f"-include {tags_header}",
            f"-I{crt_include}",
            f"-I{PORT_DIR / 'include'}",
            game_defines_and_includes(linux_config),
            # the Xbox SDK declarations (port/include/xdk) come before the
            # Windows SDK, which has headers of the same names
            f"-I{XDK_INCLUDE}",
        ])
        for source in game_sources(linux_config):
            add_object(source, game_cflags)
        for source in sorted(Path(linux_config["game_sources"]).glob("*.c")):
            add_object(source, game_cflags)

        linux_platform = Path(linux_config["platform_sources"])
        platform_cflags = " ".join([
            abi,
            " ".join(PLATFORM_FLAGS),
            f"-include {prefix_header}",
            f"-I{posix_include}",
            f"-I{crt_include}",
            f"-I{linux_platform}",
            f"-I{PORT_DIR / 'include'}",
            f"-I{TOML_DIR}",
            f"-I{KCP_DIR}",
            # halo_linux_winsock_names.h, but not the Linux build's C runtime
            # wrappers next to it
            f"-iquote {LINUX_DIR / 'include'}",
            "-Isource -Isource/cseries",
            f"-I{_quote(sdl_include)}",
            f"-I{XDK_INCLUDE}",
        ])
        win32_cflags = " ".join([
            abi,
            " ".join(WIN32_FLAGS),
            f"-I{posix_include}",
            f"-I{linux_platform}",
            f"-I{_quote(sdl_include)}",
        ])
        replaced = set(config.get("replaced_platform_sources", []))
        for source in sorted(linux_platform.glob("*.c")):
            if source.name in replaced:
                continue
            if source.name == "updater.c":
                add_object(source, f"{platform_cflags} {updater_defines(getattr(sln, 'port_release', False))}")
            else:
                add_object(source, platform_cflags)
        miniupnpc_include = f"-I{MINIUPNPC_DIR / 'include'} -DMINIUPNP_STATICLIB"
        for source in sorted((PORT_DIR / "src").glob("*.c")):
            if source.name == "win32_upnp.c":
                add_object(source, f"{win32_cflags} {miniupnpc_include}")
            else:
                add_object(source, win32_cflags if source.name.startswith("win32_") else platform_cflags)
        # internet play's UPnP (port/third_party/miniupnpc), on Winsock, as
        # its own build has it
        for source in miniupnpc_sources():
            add_object(source, " ".join([abi, *WIN32_FLAGS, miniupnpc_include, f"-I{MINIUPNPC_DIR / 'src'}",
                                         "-D_CRT_SECURE_NO_WARNINGS", "-D_WINSOCK_DEPRECATED_NO_WARNINGS", "-w"]))
        for source in embedded_assets:
            add_object(source, platform_cflags)
        # the settings file's parser (port/third_party/tomlc17), with the
        # platform layer's ABI and nothing else
        add_object(TOML_DIR / "tomlc17.c", " ".join([abi, "-std=gnu11", "-w"]))
        # internet play's reliable streams (port/third_party/kcp; p2p.c)
        add_object(KCP_DIR / "ikcp.c", " ".join([abi, "-std=gnu11", "-w"]))
        # the game's sin, pow and the rest, the same on every port
        # (port/include/halo_math.h)
        for source in musl_math_sources():
            add_object(source, musl_math_cflags(abi))

        n.build(
            outputs=output,
            rule="windows_link",
            inputs=objects + extra_objects,
            variables={"ldflags": " ".join(base_ldflags + extra_ldflags), "libs": libs},
        )

    # Profile-guided optimisation: with the committed Windows profile (or
    # the Linux one, which matches most of the game), or with --pgo=train
    # one that an instrumented build records while playing
    # (tools/pgo_train.py). A profile is trained once: code changed since
    # simply goes without, and deleting it trains a new one.
    profile = pgo_profile(sln, WINDOWS_PROFILE, [LINUX_PROFILE], cc)
    if pgo_mode(sln) == "train" and profile == WINDOWS_PROFILE:
        release = clang_release(cc)
        if not release:
            sys.exit(f"cannot tell the release of {cc}, whose profile runtime the instrumented build needs")
        runtime = fetch_profile_runtime(release)
        runtime_cflags = " ".join([
            "--target=i686-pc-windows-msvc", OPTIMISATION, "-g", "-gcodeview", "-w",
            "-D_CRT_SECURE_NO_WARNINGS", "-DCOMPILER_RT_HAS_ATOMICS=1",
            f"-I{_quote(runtime / 'include')}", f"-I{_quote(runtime / 'lib' / 'profile')}",
        ])
        generate_dir = BUILD / "pgo-generate"
        runtime_objects: List[Path] = []
        for name in PROFILE_RUNTIME_SOURCES:
            obj = generate_dir / "profile_runtime" / name.replace(".c", ".o")
            n.build(outputs=obj, rule="windows_cc", inputs=runtime / "lib" / "profile" / name,
                    variables={"cflags": runtime_cflags})
            runtime_objects.append(obj)
        # the runtime's start-up, which LLVM writes in C++
        obj = generate_dir / "profile_runtime" / "halo_profile_runtime.o"
        n.build(outputs=obj, rule="windows_cc", inputs=PORT_DIR / "pgo" / "halo_profile_runtime.c",
                variables={"cflags": runtime_cflags})
        runtime_objects.append(obj)
        instrumented = generate_dir / "halo.exe"
        instrumented_dll = generate_dir / "SDL3.dll"
        # instrumented objects ask for LLVM's own runtime library, which
        # these objects replace
        emit(generate_dir / "obj", instrumented, ["-fprofile-generate"],
             ["-Wl,/NODEFAULTLIB:clang_rt.profile.lib"], runtime_objects, [])
        n.build(outputs=instrumented_dll, rule="windows_copy", inputs=SDL_DIR / "lib" / "x86" / "SDL3.dll")
        n.rule(
            name="windows_pgo_train",
            command="$python tools/pgo_train.py --binary $binary --work $work --output $out",
            description="WINDOWS PGO TRAINING: playing levels in the instrumented build",
            pool="console",
        )
        n.build(
            outputs=profile,
            rule="windows_pgo_train",
            order_only=[instrumented, instrumented_dll],
            variables={"binary": str(instrumented), "work": str(sln.build_dir / "pgo" / "windows")},
        )

    # link-time optimisation (configure.py --lto)
    lto = lto_mode(sln)
    lto_cflags = [] if lto == "off" else ["-flto=thin" if lto == "thin" else "-flto=full"]
    emit(obj_dir, output, lto_cflags + profile_use_flags(profile),
         lto_cflags + [OPTIMISATION] if lto_cflags else [], [], [profile] if profile else [])
    n.build(outputs=sdl_dll, rule="windows_copy", inputs=SDL_DIR / "lib" / "x86" / "SDL3.dll")
    n.build(outputs="windows", rule="phony", inputs=[output, sdl_dll])
    n.newline()
