"""Ninja rules for the native macOS build (``ninja macos``).

The game and most of the platform layer are the Linux build's
(tools/linux_build.py, port/linux); this build compiles them as native
64-bit arm64 code, which the 32-bit builds never are. Two things make that
work without changing what the other builds compile:

  - HALO_64BIT selects the game's 64-bit code paths: pointers inside Xbox
    data are 32-bit Xbox addresses (source/cseries/xbox_address.h) into a
    4 GB region the platform layer reserves (port/linux/src/xbox_memory.c).
  - the sources compiled with the Xbox's ABI are compiled from a copy in
    which `long` is spelled `int` (tools/lp64_rewrite.py): MSVC's `long` is
    32 bits, macOS's is 64.

The result is build/macos/halo and the application bundle
build/macos/Halo.app. See port/macos/README.md.
"""

import json
import os
import platform
import shlex
from pathlib import Path
from typing import Any, Dict, List

from .linux_build import (
    GAME_FLAGS as LINUX_GAME_FLAGS,
    KCP_DIR,
    MBEDTLS_DIR,
    MINIUPNPC_DEFINES,
    MINIUPNPC_DIR,
    MUSL_MATH_DIR,
    OPTIMISATION,
    PLATFORM_FLAGS as LINUX_PLATFORM_FLAGS,
    TOML_DIR,
    XDK_INCLUDE,
    compile_launcher,
    game_sources,
    miniupnpc_sources,
    musl_math_sources,
    updater_defines,
)
from .embed_assets import hud_assets_build, hud_configure_inputs
from .ninja_syntax import Writer

PORT_DIR = Path("port/macos")
PORT_CONFIG = PORT_DIR / "port.json"
LINUX_PORT_DIR = Path("port/linux")
LINUX_PORT_CONFIG = LINUX_PORT_DIR / "port.json"
# the oldest macOS the build runs on
MACOS_MINIMUM = "13.0"
# SDL3 and the other libraries, from Homebrew
HOMEBREW = Path("/opt/homebrew")

# The Xbox ABI the sources were written against, as far as a 64-bit ARM host
# can reproduce it (compare LINUX_ABI_FLAGS): 16-bit wchar_t, MSVC's
# extensions, tentative definitions shared between units, no optimisations
# that assume the absence of UB MSVC tolerated, and the floating point
# results of the other ports (no fused multiply-adds).
MACOS_ABI_FLAGS = [
    "-fms-extensions",
    "-fshort-wchar",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    "-ffp-contract=off",
    "-DHALO_64BIT",
    # the C library's checked printf macros collide with the MSVC names
    "-D_FORTIFY_SOURCE=0",
    OPTIMISATION,
    "-g",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

# The Linux build's game flags, with the conversions that truncate a 64-bit
# pointer made errors: each is a place that still treats an Xbox address as
# a pointer or the reverse.
MACOS_GAME_FLAGS = [
    *(flag for flag in LINUX_GAME_FLAGS if flag not in ("-w", "-Wno-error=int-conversion",
                                                       "-Wno-error=implicit-function-declaration")),
    # Apple's libc restricted to ISO C (glibc's is by __STRICT_ANSI__)
    "-D_ANSI_SOURCE",
    "-ferror-limit=0",
    # -w would also hide the errors below
    "-Wno-everything",
    "-Werror=int-to-pointer-cast",
    "-Werror=pointer-to-int-cast",
    "-Werror=void-pointer-to-int-cast",
    "-Werror=int-conversion",
    "-Werror=pointer-integer-compare",
    # an undeclared function returns int, truncating a returned pointer
    "-Werror=implicit-function-declaration",
    "-Werror=format",
]

MACOS_PLATFORM_FLAGS = [
    *LINUX_PLATFORM_FLAGS,
    "-Werror=incompatible-pointer-types",
    "-Werror=int-to-pointer-cast",
    "-Werror=pointer-to-int-cast",
    "-Werror=void-pointer-to-int-cast",
    "-Werror=int-conversion",
    "-Werror=format",
]

# Platform files named posix_*.c talk to the C library only, with the host's
# own ABI and no rewrite (their boundary types are posix.h's).
MACOS_POSIX_FLAGS = [
    "-std=gnu11",
    "-D_DARWIN_C_SOURCE",
    OPTIMISATION,
    "-g",
    "-Wall",
    "-Werror=incompatible-pointer-types",
    "-Werror=int-conversion",
]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _load(path: Path) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def macos_configure_inputs() -> List[Path]:
    """Files whose change must re-run configure.py."""
    if not PORT_CONFIG.is_file():
        return [Path(__file__)]
    return [PORT_CONFIG, Path(__file__), LINUX_PORT_CONFIG, LINUX_PORT_DIR / "src", LINUX_PORT_DIR / "game",
            *hud_configure_inputs()]


def _rewritten_inputs() -> List[Path]:
    """Every file compiled or included with the Xbox's ABI: the rewrite's
    inputs. Headers come along whole, so that includes relative to the
    including file find the rewritten copies."""
    roots = [Path("source"), LINUX_PORT_DIR / "game", LINUX_PORT_DIR / "src", LINUX_PORT_DIR / "include",
             Path("port/include"), KCP_DIR, TOML_DIR]
    files = []
    for root in roots:
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.suffix.lower() in (".c", ".h", ".inl", ".inc") and not path.name.startswith("posix_"):
                files.append(path)
    return files


def generate_macos_build(n: Writer, sln: Any) -> None:
    if not PORT_CONFIG.is_file() or platform.system() != "Darwin":
        return
    config = _load(PORT_CONFIG)
    linux_config = _load(LINUX_PORT_CONFIG)
    build_dir: Path = sln.build_dir / "macos"
    obj_dir = build_dir / "obj"
    lp64_dir = build_dir / "lp64"
    output = build_dir / "halo"
    cc = getattr(sln, "macos_cc", None) or "clang"
    arch = "arm64" if platform.machine() == "arm64" else "x86_64"
    target = f"--target={arch}-apple-macos{MACOS_MINIMUM}"

    def lp64(path: Path) -> Path:
        return lp64_dir / path

    n.comment("Native macOS build (ninja macos)")
    n.variable("macos_cc", cc)
    n.rule(
        name="macos_lp64",
        command="$python tools/lp64_rewrite.py --output $out $in",
        description="MACOS LP64 $in",
        restat=True,
    )
    rewritten = []
    for source in _rewritten_inputs():
        n.build(outputs=lp64(source), rule="macos_lp64", inputs=source, implicit=[Path("tools/lp64_rewrite.py")])
        rewritten.append(lp64(source))
    n.build(outputs=build_dir / "lp64.stamp", rule="phony", inputs=rewritten)

    semantics_header = build_dir / "halo_msvc_semantics.h"
    platform_semantics_header = build_dir / "platform_msvc_semantics.h"
    n.rule(
        name="macos_msvc_semantics",
        command="$python tools/linux_msvc_semantics.py --output $out $scan",
        description="MACOS MSVC SEMANTICS $out",
        restat=True,
    )
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    # (the scan reads the original sources: tags and inline names, not types)
    n.build(outputs=semantics_header, rule="macos_msvc_semantics",
            implicit=[Path("tools/linux_msvc_semantics.py"), *game_headers, *sorted(XDK_INCLUDE.glob("*.h"))],
            variables={"scan": f"--all-inlines --tags source --inlines source --inlines {XDK_INCLUDE}"})
    n.build(outputs=platform_semantics_header, rule="macos_msvc_semantics",
            implicit=[Path("tools/linux_msvc_semantics.py"), *sorted(XDK_INCLUDE.glob("*.h"))],
            variables={"scan": f"--inlines {XDK_INCLUDE}"})
    xdk = _quote(lp64(XDK_INCLUDE))

    n.rule(
        name="macos_cc",
        command=f"{compile_launcher(sln)}$macos_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="macos_link",
        command="$macos_cc $ldflags -o $out @$out.rsp $libs",
        description="MACOS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )

    release = ["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []
    abi = " ".join([target, *MACOS_ABI_FLAGS, *release])
    prefix_header = lp64(LINUX_PORT_DIR / "include" / "halo_linux_prefix.h")
    port_include = lp64(LINUX_PORT_DIR / "include")
    homebrew_include = f"-idirafter {HOMEBREW / 'include'}"
    excluded = set(config.get("exclude_sources", []))
    objects: List[Path] = []

    def add_object(source: Path, cflags: str) -> None:
        # (a rewritten copy's object sits where the original's would)
        relative = source.relative_to(lp64_dir) if lp64_dir in source.parents else source
        obj = obj_dir / relative.with_suffix(".o")
        objects.append(obj)
        n.build(outputs=obj, rule="macos_cc", inputs=source,
                implicit=[semantics_header, platform_semantics_header],
                order_only=[build_dir / "lp64.stamp"],
                variables={"cflags": cflags})

    game = linux_config["game"]
    defines = " ".join(f"-D{d}" for d in game.get("defines", []))
    includes = " ".join(f"-I{_quote(lp64(Path(d)))}" for d in game.get("include_dirs", []))
    game_cflags = " ".join([
        abi, " ".join(MACOS_GAME_FLAGS),
        f"-include {_quote(prefix_header)}", f"-include {_quote(semantics_header)}",
        defines, f"-I{_quote(port_include)}", includes, f"-idirafter {xdk}",
    ])
    for source in game_sources(linux_config):
        if source.as_posix() not in excluded:
            add_object(lp64(source), game_cflags)
    # the port's own units that see the game as its sources do (port/linux/game)
    for source in sorted(Path(linux_config["game_sources"]).glob("*.c")):
        if source.as_posix() not in excluded:
            add_object(lp64(source), game_cflags)

    platform_dir = Path(linux_config["platform_sources"])
    platform_cflags = " ".join([
        abi, " ".join(MACOS_PLATFORM_FLAGS),
        f"-include {_quote(prefix_header)}", f"-include {_quote(platform_semantics_header)}",
        f"-I{_quote(lp64(platform_dir))}", f"-I{_quote(port_include)}",
        f"-I{_quote(lp64(TOML_DIR))}", f"-I{_quote(lp64(KCP_DIR))}",
        f"-I{_quote(lp64(Path('source')))} -I{_quote(lp64(Path('source/cseries')))}",
        homebrew_include, f"-idirafter {xdk}",
    ])
    posix_cflags = " ".join([target, *MACOS_POSIX_FLAGS, f"-I{platform_dir}", homebrew_include])
    mbedtls_include = f"-I{MBEDTLS_DIR / 'include'}"
    for source in sorted(platform_dir.glob("*.c")):
        if str(source) in excluded:
            continue
        if source.name == "posix_update.c":
            add_object(source, f"{posix_cflags} {mbedtls_include}")
        elif source.name == "posix_upnp.c":
            add_object(source, f"{posix_cflags} -I{MINIUPNPC_DIR / 'include'} -DMINIUPNP_STATICLIB")
        elif source.name.startswith("posix_"):
            add_object(source, posix_cflags)
        elif source.name == "updater.c":
            add_object(lp64(source), f"{platform_cflags} {updater_defines(getattr(sln, 'port_release', False))}")
        else:
            add_object(lp64(source), platform_cflags)
    # the high-res HUD's textures (port/assets/hud; port/linux/src/hud_hires.c),
    # with the platform units' flags: its table is hud_hires.h's, from the
    # 64-bit tree
    for source in hud_assets_build(n, "macos", build_dir / "generated" / "hud_hires_assets.c"):
        add_object(source, platform_cflags)
    # macOS-only platform units, with the host's ABI (port/macos/src)
    for source in sorted((PORT_DIR / "src").glob("*.c")):
        add_object(source, posix_cflags)
    for source in sorted((MBEDTLS_DIR / "library").glob("*.c")):
        add_object(source, " ".join([target, "-std=gnu11", OPTIMISATION, "-g", "-w", mbedtls_include,
                                     f"-I{MBEDTLS_DIR / 'library'}"]))
    for source in miniupnpc_sources():
        add_object(source, " ".join([target, "-std=gnu11", OPTIMISATION, "-g", "-w", *MINIUPNPC_DEFINES,
                                     f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}"]))
    third_party = " ".join([abi, "-std=gnu11", "-w"])
    add_object(lp64(TOML_DIR / "tomlc17.c"), third_party)
    add_object(lp64(KCP_DIR / "ikcp.c"), third_party)
    for source in musl_math_sources():
        add_object(source, " ".join([abi, "-std=gnu11", "-w", f"-I{MUSL_MATH_DIR}/include",
                                     f"-include {MUSL_MATH_DIR}/include/libm.h"]))

    libraries = config.get("libraries", [])
    frameworks = config.get("frameworks", [])
    n.build(
        outputs=output,
        rule="macos_link",
        inputs=objects,
        variables={
            "ldflags": f"{target} -g",
            "libs": " ".join([f"-L{HOMEBREW / 'lib'}", *(f"-l{lib}" for lib in libraries),
                              *(f"-framework {fw}" for fw in frameworks)]),
        },
    )

    # the application bundle, which macOS shows with the game's name and icon
    bundle = build_dir / "Halo.app"
    n.rule(
        name="macos_bundle",
        command=f"$python {PORT_DIR / 'bundle.py'} --executable $in --output {_quote(bundle)}",
        description="MACOS BUNDLE $out",
    )
    n.build(outputs=bundle / "Contents" / "MacOS" / "halo", rule="macos_bundle", inputs=output,
            implicit=[PORT_DIR / "bundle.py", PORT_DIR / "Info.plist", PORT_DIR / "AppIcon.icns"])
    n.build(outputs="macos", rule="phony", inputs=[output, bundle / "Contents" / "MacOS" / "halo"])
    n.newline()
