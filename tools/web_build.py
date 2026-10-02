"""Ninja rules for the web build (``ninja web``).

The web port (port/web/README.md) runs the game as WebAssembly in a browser,
so it installs on an iPhone or iPad as a home-screen web app. wasm32 is an
ILP32 target (32-bit int, long and pointers), which the game's data formats
need, and the Xbox memory window at 0x80000000 lies inside a 4 GB wasm
memory, so the game code runs as it does on the other ports.

The game follows the Android port's code paths (HALO_ANDROID: OpenGL ES 3,
the display's shape, the configuration file in the data folder) with
HALO_WEB for what differs in a browser. This graph builds

- the game sources and the platform layer shared with Linux
  (port/linux/src), compiled by Emscripten;
- the web runtime (port/web/src): the SDL3 functions the platform layer
  calls, over the browser, and the services the Android host supplies;

and links build/web/site/halo.js and halo.wasm, next to the page and the
service worker from port/web/site.

Like the other native builds, this is independent of the byte-matching
graph.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from .linux_build import MUSL_MATH_DIR, XDK_INCLUDE, compile_launcher, musl_math_sources, xdk_headers
from .ninja_syntax import Writer

PORT_DIR = Path("port/web")
LINUX_DIR = Path("port/linux")
ANDROID_DIR = Path("port/android")
BUILD = Path("build/web")
THIRD_PARTY = BUILD / "third_party"
TOML_DIR = Path("port/third_party/tomlc17")
KCP_DIR = Path("port/third_party/kcp")
# the SDL3 headers only: the platform layer's types and constants (the
# functions are port/web/src/web_sdl.c)
SDL_TAG = "release-3.4.16"
SDL_DIR = THIRD_PARTY / "SDL3"
SDL_URL = "https://github.com/libsdl-org/SDL.git"

# The Xbox window (port/linux/src/platform.h) is the top of the wasm memory:
# 0x80000000 to 0x88000000. The C heap has everything below it
# (web_memory.c keeps sbrk out of the window).
WEB_MEMORY_BYTES = 0x88000000

WEB_ABI_FLAGS = [
    "-pthread",
    "-DHALO_ANDROID=1",
    "-DHALO_WEB=1",
    "-fshort-wchar",
    # the Win32 ABI returns small structures and unions in registers, and the
    # game calls functions that return unions through pointers typed as
    # returning long (hs_runtime.c): the multivalue ABI passes and returns
    # them as values too, so the two agree in WebAssembly
    "-Xclang", "-target-abi", "-Xclang", "experimental-mv",
    "-mmultivalue",
    "-mbulk-memory",
    "-mnontrapping-fptoint",
    "-msimd128",
    "-ffp-contract=off",
    "-O2",
    # one module at the link: LLVM then gives each call to a function
    # declared with another signature (C89's implicit declarations, which x86
    # tolerates) a wrapper that adapts it, where WebAssembly would trap
    "-flto",
]

GAME_CODE_FLAGS = [
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

GAME_FLAGS = [
    "-std=gnu89",
    "-D__STRICT_ANSI__",
    "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

# game files that call variadic functions without a prototype in scope
# (the Android build's list, tools/android_build.py): a WebAssembly call must
# match the callee's signature exactly
VARIADIC_PROTOTYPE_FILES = {
    "source/ai/action_uncover.c", "source/ai/ai.c", "source/ai/ai_debug.c",
    "source/bungie_net/common/public_key_crypt.c", "source/camera/editor_flying_camera.c",
    "source/game/cheats.c", "source/game/game_engine.c", "source/game/players.c",
    "source/hs/hs.c", "source/interface/hud_nav_points.c",
    "source/networking/telnet_console.c", "source/rasterizer/xbox/rasterizer_xbox_errors.c",
    "source/render/render.c",
    # (and for WebAssembly, a call that passes fewer arguments than a
    # variadic function's fixed ones)
    "source/interface/ui_widget_game_data_input_functions.c",
}

# game files that call C library functions without a prototype, whose
# WebAssembly signatures differ from the undeclared ones (time_t is 64-bit)
PROTOTYPE_FILES = {
    "source/bungie_net/common/random_numbers.c": "time.h",
}

# platform files the browser does without
PLATFORM_EXCLUDE = {
    "memory_watch.c",   # page protection: port/web/src/web_memory_watch.c
    "posix_update.c",   # the desktop self-updater
    "posix_upnp.c",
    "updater.c",
    "xiso.c",           # the page extracts the maps (port/web/site/xiso.js)
    "p2p_discord.c",
}

LINK_FLAGS = [
    "-pthread",
    "-O2",
    "-flto",
    "-mmultivalue",
    "-sPROXY_TO_PTHREAD=1",
    "-sEXIT_RUNTIME=0",
    f"-sINITIAL_MEMORY={WEB_MEMORY_BYTES}",
    "-sALLOW_MEMORY_GROWTH=0",
    "-sSTACK_SIZE=8MB",
    "-sDEFAULT_PTHREAD_STACK_SIZE=1MB",
    "-sPTHREAD_POOL_SIZE=10",
    "-sWASMFS=1",
    "-sMIN_WEBGL_VERSION=2",
    "-sMAX_WEBGL_VERSION=2",
    "-sGL_ENABLE_GET_PROC_ADDRESS=1",
    "-sENVIRONMENT=web,worker",
    "-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_web_shared_state,_web_shared_offsets,_web_quick_play_cancel,_web_quick_play_migrate,_web_quick_play_reconnect,_web_quick_play_hold,_web_quick_play_set_address,_web_quick_play_set_epoch",
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32,HEAPF32,wasmMemory,UTF8ToString,stringToUTF8,lengthBytesUTF8",
    "-sINCOMING_MODULE_JS_API=print,printErr,locateFile,onAbort,preRun,arguments,mainScriptUrlOrBlob,"
    "instantiateWasm,wasmMemory,setStatus,monitorRunDependencies,onRuntimeInitialized",
    "-sSTACK_OVERFLOW_CHECK=0",
    # function names in stack traces (the name section only)
    "--profiling-funcs",
    "-sASSERTIONS=0",
    "-lGL",
]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _find_emcc(sln: Any) -> Optional[str]:
    explicit = getattr(sln, "web_emcc", None)
    if explicit:
        return explicit
    found = shutil.which("emcc")
    if found:
        return found
    for candidate in (Path.home() / "emsdk" / "upstream" / "emscripten" / "emcc",
                      Path(os.environ.get("EMSDK", "/nonexistent")) / "upstream" / "emscripten" / "emcc"):
        if candidate.is_file():
            return str(candidate)
    return None


def fetch_third_party() -> None:
    """Download the SDL3 headers (configure time, once)."""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG} (headers)")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)],
                       check=True)


def web_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "src", PORT_DIR / "site", LINUX_DIR / "src"]


def generate_web_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not (PORT_DIR / "src").is_dir():
        return
    emcc = _find_emcc(sln)
    if not emcc:
        n.comment("Web build: no Emscripten found (put emcc on the PATH or pass --web-emcc)")
        return
    try:
        fetch_third_party()
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"Web build disabled: cannot fetch the SDL3 headers ({error})", file=sys.stderr)
        return
    import json
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))

    obj_dir = BUILD / "obj"
    site_dir = BUILD / "site"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    release = getattr(sln, "port_release", False)

    n.comment("Web build (ninja web); see port/web/README.md")
    n.variable("web_emcc", emcc)
    n.rule(
        name="web_cc",
        command=f"{compile_launcher(sln)}$web_emcc -MMD -MF $out.d $cflags -c $in -o $out",
        description="WEB CC $out",
        depfile="$out.d",
        deps="gcc",
    )

    abi = " ".join(WEB_ABI_FLAGS + (["-DHALO_RELEASE"] if release else []) + ["-g2"])
    code = " ".join(GAME_CODE_FLAGS)
    implicit_headers = [*xdk_headers(), semantics_header, platform_semantics_header, prefix_header]

    objects: List[Path] = []

    def add_object(source: Path, cflags: str) -> None:
        obj = obj_dir / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(BUILD)):
            obj = obj_dir / source.relative_to(BUILD).with_suffix(".o")
        n.build(outputs=obj, rule="web_cc", inputs=source, implicit=implicit_headers,
                variables={"cflags": cflags})
        objects.append(obj)

    # the game
    excluded = set(config.get("exclude_sources", []))
    for proj in sln.projects:
        if proj.name not in config["projects"]:
            continue
        options = proj.options
        defines = " ".join(f"-D{d}" for d in options.get("defines") or [])
        includes = " ".join(
            f"-I{_quote(d)}" for d in options.get("include_dirs") or [] if Path(d) != Path("xbox/include")
        )
        game_cflags = " ".join([
            abi, code, " ".join(GAME_FLAGS),
            f"-include {prefix_header}", f"-include {semantics_header}", defines,
            f"-I{PORT_DIR}/include", f"-I{LINUX_DIR}/include", includes, f"-idirafter {XDK_INCLUDE}",
        ])
        for obj in proj.objects:
            name = str(obj.file_path).replace(os.sep, "/")
            if obj.status.name == "Missing" or name in excluded or obj.file_path.suffix.lower() != ".c":
                continue
            cflags = game_cflags
            if name in VARIADIC_PROTOTYPE_FILES:
                cflags += f" -include {ANDROID_DIR}/include/halo_android_variadic_prototypes.h"
            if name in PROTOTYPE_FILES:
                cflags += f" -include {PROTOTYPE_FILES[name]}"
            if name == "source/shell/shell_xbox.c":
                # port/web/src/web_main.c starts the game once the browser's
                # storage is mounted
                cflags += " -Dmain=halo_game_main"
            add_object(obj.file_path, cflags)
        for source in sorted(Path(config["game_sources"]).glob("*.c")):
            add_object(source, game_cflags)

    # the platform layer shared with Linux, and the web runtime
    platform_cflags = " ".join([
        abi, code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w",
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{PORT_DIR}/include", f"-I{PORT_DIR}/src", f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include",
        f"-I{TOML_DIR}", f"-I{KCP_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", f"-idirafter {XDK_INCLUDE}",
    ])
    # posix_*.c talk to the C library only, with its own ABI (as on Linux)
    posix_cflags = " ".join([
        "-pthread", "-DHALO_ANDROID=1", "-DHALO_WEB=1", "-Xclang", "-target-abi", "-Xclang", "experimental-mv",
        "-mmultivalue", "-mbulk-memory", "-mnontrapping-fptoint", "-msimd128", "-O2", "-g2", "-flto",
        "-std=gnu11", "-D_GNU_SOURCE", "-D_FILE_OFFSET_BITS=64", "-w", f"-I{LINUX_DIR}/src",
    ])
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name in PLATFORM_EXCLUDE:
            continue
        add_object(source, posix_cflags if source.name.startswith("posix_") else platform_cflags)
    for source in sorted((PORT_DIR / "src").glob("*.c")):
        add_object(source, posix_cflags if source.name in ("web_stubs.c", "web_net.c") else platform_cflags)
    add_object(TOML_DIR / "tomlc17.c", " ".join([abi, "-std=gnu11", "-w"]))
    add_object(KCP_DIR / "ikcp.c", " ".join([abi, "-std=gnu11", "-w"]))
    musl_math_cflags = " ".join([
        abi, "-std=gnu11", "-w", f"-I{MUSL_MATH_DIR}/include", f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        add_object(source, musl_math_cflags)

    # ---------- the link

    library_js = PORT_DIR / "src" / "web_library.js"
    pre_js = PORT_DIR / "src" / "web_pre.js"
    stream_batch_js = PORT_DIR / "stream-batch.js"
    halo_js = site_dir / "halo.js"
    halo_wasm = site_dir / "halo.wasm"
    link_flags = " ".join(LINK_FLAGS + [f"--js-library {library_js}", f"--pre-js {stream_batch_js}", f"--pre-js {pre_js}"])
    n.rule(
        name="web_link",
        command=f"$web_emcc $ldflags -o $out @$out.rsp",
        description="WEB LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=halo_js, implicit_outputs=[halo_wasm], rule="web_link", inputs=objects,
            implicit=[library_js, stream_batch_js, pre_js], variables={"ldflags": link_flags})

    # ---------- the site: the page, the service worker and the app manifest

    site_files: List[Path] = []
    n.rule(name="web_copy", command="cp $in $out", description="WEB STAGE $out")
    for source in sorted((PORT_DIR / "site").rglob("*")):
        if source.is_file():
            target = site_dir / source.relative_to(PORT_DIR / "site")
            n.build(outputs=target, rule="web_copy", inputs=source)
            site_files.append(target)
    n.rule(
        name="web_version",
        command=f"$python {PORT_DIR}/stamp_version.py $out $in",
        description="WEB VERSION $out",
    )
    version_file = site_dir / "version.json"
    n.build(outputs=version_file, rule="web_version", inputs=[halo_js, halo_wasm, *site_files],
            implicit=[PORT_DIR / "stamp_version.py"])
    n.build(outputs="web", rule="phony", inputs=[halo_js, halo_wasm, version_file, *site_files])
    n.newline()
