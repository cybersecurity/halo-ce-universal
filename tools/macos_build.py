"""Ninja rules for the macOS builds (``ninja macos``, ``ninja macos_x86_64``).

The macOS port (port/macos/README.md) runs the game the way the Android port
does: as ILP32 code - 32-bit pointers, as the game's data formats require -
inside an ordinary 64-bit process, a host that loads it and serves its
requests. There are two builds:

- ``ninja macos``, native on Apple silicon (build/macos/Halo): the guest is
  compiled as the Android port's is, for arm64_32 (tools/android_asm_convert.py
  makes ELF assembly of it), and its memory accesses are rebased onto a
  4 GB-aligned region of the host's address space
  (tools/macos_arm64_rebase.py): arm64 macOS processes cannot map anything
  below 4 GB. The host is an arm64 executable.
- ``ninja macos_x86_64``, for Intel Macs or Rosetta 2
  (build/macos-x86_64/Halo): the guest is x32 code (x86-64 instructions with
  32-bit pointers) linked below 2 GB, and the host an x86-64 executable with
  a 64 KB __PAGEZERO, whose low 4 GB is the guest's region itself.

Both guests are the game sources, the platform layer shared with the Linux
port (port/linux/src, as on the desktop: HALO_MACOS), the Android port's
guest runtime (port/android/guest/runtime) with the desktop's SDL functions
(port/macos/guest/runtime) and a subset of musl, linked by ld.lld at a fixed
guest address. Both hosts (port/macos/host) run over SDL3 (built universal)
and draw with OpenGL ES through ANGLE on Metal (libEGL.dylib and
libGLESv2.dylib, taken from an installed Chromium-based application unless
--macos-angle names a folder holding them).

Requirements: Xcode's clang, ninja, cmake, and ld.lld with llvm-ar (the
``ziglang`` Python package's are found automatically).
"""

import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from .android_build import (GUEST_ABI_FLAGS as ANDROID_ABI_FLAGS, GUEST_CODE_FLAGS, MUSL_URL, MUSL_VERSION, SDL_TAG,
                            SDL_URL, VARIADIC_PROTOTYPE_FILES, KCP_DIR, TOML_DIR, _musl_sources, _quote)
from .linux_build import (MINIUPNPC_DEFINES, MINIUPNPC_DIR, MUSL_MATH_DIR, XDK_INCLUDE, compile_launcher,
                          game_defines_and_includes, game_sources, miniupnpc_sources, musl_math_sources,
                          xdk_headers)
from .ninja_syntax import Writer

PORT_DIR = Path("port/macos")
ANDROID_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/macos")
THIRD_PARTY = BUILD / "third_party"
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION}"
MUSL_SHA256 = "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"
SDL_DIR = THIRD_PARTY / "SDL3"
SDL_COMMIT = "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c"  # release-3.4.16
KHRONOS_DIR = THIRD_PARTY / "khronos"
KHRONOS_OPENGL = "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api"
KHRONOS_EGL = "https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api"
SDL_BUILD = BUILD / "sdl3-build"
MACOS_MINIMUM = "14.4"
HOST_FRAMEWORKS = ["Cocoa", "Foundation", "Metal", "QuartzCore", "IOKit"]

# the native build: the Android port's guest ABI, rebased (x28 the base,
# x27 the rebasing's scratch register), with the desktop platform layer
# Apple silicon rather than the Android port's ARMv8.0 (-mcpu=cortex-a53):
# the M1's instructions and scheduling. Floating-point results do not
# change: no fused multiply-add (-ffp-contract=off), no reassociation, and
# NEON's lanes round as scalar instructions do.
NATIVE_ABI_FLAGS = [("-mcpu=apple-m1" if flag == "-mcpu=cortex-a53" else flag)
                    for flag in ANDROID_ABI_FLAGS if flag != "-DHALO_ANDROID=1"] + [
    "-DHALO_MACOS=1", "-ffixed-x27", "-ffixed-x28",
    # the rebasing lengthens functions past what byte-sized jump table
    # entries reach
    "-mllvm", "-aarch64-enable-compress-jump-tables=false",
    # the watchOS triple with the macOS SDK's sysroot: the guest takes no
    # headers from it (-nostdinc)
    "-Wno-incompatible-sysroot",
    # musl's internal headers ask for protected visibility, which the
    # target lacks (default is the same here: one static image)
    "-Wno-unsupported-visibility",
]

# the x86-64 build: x32, linked below 2 GB (where x86-64 code can use
# sign-extended 32-bit absolute addresses)
X86_IMAGE_BASE = 0x20000000
X86_ABI_FLAGS = [
    "--target=x86_64-linux-gnux32",
    "-DHALO_MACOS=1",
    f"-DHALO_GUEST_IMAGE_BASE=0x{X86_IMAGE_BASE:08x}u",
    # what Rosetta 2 translates
    "-march=x86-64-v2",
    # as the MSVC runtime, and as the guest's musl headers say
    "-mlong-double-64",
    "-nostdinc",
    "-fshort-wchar",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-femulated-tls",
    "-fno-pic",
    "-fno-pie",
    # no fused multiply-add (the game's debug assertions trip on the
    # different rounding)
    "-ffp-contract=off",
    "-O2",
]


def _find_tool(name: str) -> Optional[str]:
    """ld.lld and llvm-ar: from PATH, else from the ziglang package"""
    found = shutil.which(name)
    if found:
        return found
    try:
        import ziglang  # type: ignore
    except ImportError:
        return None
    zig = Path(ziglang.__file__).parent / "zig"
    if not zig.is_file():
        return None
    return f"{_quote(zig)} {'ld.lld' if name == 'ld.lld' else 'ar'}"


def _find_angle(sln: Any) -> Optional[Path]:
    """a folder holding libEGL.dylib and libGLESv2.dylib for both
    architectures"""
    wanted = getattr(sln, "macos_angle", None)
    candidates = [Path(wanted)] if wanted else []
    home = Path.home()
    candidates += [
        home / "Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/"
        "Chromium Embedded Framework.framework/Versions/A/Libraries",
        Path("/Applications/Google Chrome.app/Contents/Frameworks/Google Chrome Framework.framework/"
             "Versions/Current/Libraries"),
    ]
    for folder in candidates:
        gles = folder / "libGLESv2.dylib"
        egl = folder / "libEGL.dylib"
        if not gles.is_file() or not egl.is_file():
            continue
        try:
            archs = subprocess.run(["lipo", "-archs", str(gles)], capture_output=True, text=True, check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            continue
        if {"x86_64", "arm64"} <= set(archs.split()):
            return folder
    return None


def _stage_angle(folder: Path, arch: str) -> Path:
    """one architecture of ANGLE, copied to build/macos/third_party/angle-<arch>
    (the originals' paths have spaces, which ninja commands do not quote)"""
    target = THIRD_PARTY / f"angle-{arch}"
    target.mkdir(parents=True, exist_ok=True)
    for name in ("libEGL.dylib", "libGLESv2.dylib"):
        source = folder / name
        staged = target / name
        if staged.is_file() and staged.stat().st_mtime >= source.stat().st_mtime:
            continue
        subprocess.run(["lipo", str(source), "-thin", arch, "-output", str(staged)], check=True)
    return target


def fetch_third_party() -> None:
    """Download musl, SDL3 and the Khronos headers (configure time, once)."""
    import hashlib
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    if not MUSL_DIR.is_dir():
        print(f"Downloading {MUSL_URL}")
        archive = THIRD_PARTY / f"musl-{MUSL_VERSION}.tar.gz"
        subprocess.run(["curl", "-sSfL", "-o", str(archive), MUSL_URL], check=True)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != MUSL_SHA256:
            archive.unlink()
            raise OSError(f"{MUSL_URL}: unexpected SHA-256 {digest}")
        subprocess.run(["tar", "xzf", archive.name], cwd=THIRD_PARTY, check=True)
        archive.unlink()
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG}")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)], check=True)
    commit = subprocess.run(["git", "-C", str(SDL_DIR), "rev-parse", "HEAD"], capture_output=True, text=True).stdout
    if commit.strip() != SDL_COMMIT:
        raise OSError(f"{SDL_DIR} is at {commit.strip()}, not {SDL_TAG} ({SDL_COMMIT})")
    headers = {
        "GLES3/gl32.h": KHRONOS_OPENGL, "GLES3/gl3platform.h": KHRONOS_OPENGL, "GLES2/gl2ext.h": KHRONOS_OPENGL,
        "GLES2/gl2.h": KHRONOS_OPENGL, "GLES2/gl2platform.h": KHRONOS_OPENGL, "KHR/khrplatform.h": KHRONOS_EGL,
    }
    for name, base in headers.items():
        target = KHRONOS_DIR / name
        if not target.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(["curl", "-sSfL", "-o", str(target), f"{base}/{name}"], check=True)


def macos_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "host", PORT_DIR / "guest" / "runtime", ANDROID_DIR / "guest" / "runtime",
            LINUX_DIR / "src"]


def _musl_sources_in(musl_dir: Path) -> List[Path]:
    """android_build._musl_sources, for this build's copy of musl"""
    from . import android_build
    saved = android_build.MUSL_DIR
    android_build.MUSL_DIR = musl_dir
    try:
        return _musl_sources()
    finally:
        android_build.MUSL_DIR = saved


def generate_macos_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if sys.platform != "darwin" or not config_path.is_file() or not (PORT_DIR / "host").is_dir():
        return
    lld = _find_tool("ld.lld")
    ar = _find_tool("llvm-ar")
    if not lld or not ar:
        n.comment("macOS build: no ld.lld/llvm-ar found (pip install ziglang)")
        return
    angle = _find_angle(sln)
    if not angle:
        n.comment("macOS build: no universal ANGLE (libEGL.dylib, libGLESv2.dylib) found; pass --macos-angle")
        return
    try:
        fetch_third_party()
        angles = {arch: _stage_angle(angle, arch) for arch in ("arm64", "x86_64")}
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"macOS build disabled: cannot fetch musl/SDL3/Khronos headers ({error})", file=sys.stderr)
        return
    import json
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))

    n.comment("macOS builds (ninja macos, ninja macos_x86_64); see port/macos/README.md")
    n.variable("macos_lld", lld)
    n.variable("macos_ar", ar)

    # ---------- shared: SDL3 (universal), the rules

    libsdl = SDL_BUILD / "libSDL3.0.dylib"
    n.rule(
        name="macos_sdl3",
        command=(f"cmake -S {SDL_DIR} -B {SDL_BUILD} -G Ninja \"-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64\" "
                 f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_MINIMUM} -DCMAKE_BUILD_TYPE=Release "
                 f"-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
                 f"> {BUILD}/sdl3-configure.log && ninja -C {SDL_BUILD} > {BUILD}/sdl3-build.log"),
        description="MACOS SDL3",
        pool="console",
    )
    n.build(outputs=libsdl, rule="macos_sdl3", implicit=[SDL_DIR / "CMakeLists.txt"])
    n.rule(name="macos_copy", command="mkdir -p $$(dirname $out) && cp $in $out", description="MACOS STAGE $out")
    n.rule(name="macos_thin", command="mkdir -p $$(dirname $out) && lipo $in -thin $arch -output $out",
           description="MACOS STAGE $out")
    # the bundle's icon, from the Android app's artwork (port/android/art)
    n.rule(name="macos_icon", command="mkdir -p $$(dirname $out) && $python tools/macos_icon.py $out",
           description="MACOS ICON $out")
    n.rule(
        name="macos_guest_cc",
        command=f"{compile_launcher(sln)}clang -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="macos_native_cc",
        command=(f"{compile_launcher(sln)}clang -MMD -MF $out.d $cflags -S $in -o $out.darwin.s && "
                 "$python tools/android_asm_convert.py $out.darwin.s $out.elf.s && "
                 "$python tools/macos_arm64_rebase.py $out.elf.s $out.s && "
                 "clang --target=aarch64-linux-gnu -mcpu=apple-m1 -c $out.s -o $out"),
        description="MACOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(name="macos_as", command="clang $asflags -c $in -o $out", description="MACOS AS $out")
    n.rule(
        name="macos_ar",
        command="rm -f $out && $macos_ar rcs $out @$out.rsp",
        description="MACOS AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="macos_guest_link",
        command="mkdir -p $$(dirname $out) && $macos_lld $ldflags -static -nostdlib -Map $out.map -o $out @$out.rsp $libs",
        description="MACOS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="macos_host_cc",
        command="clang -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="macos_host_link",
        command=(f"mkdir -p $$(dirname $out) && clang $ldflags -mmacosx-version-min={MACOS_MINIMUM} -o $out $in "
                 f"-L{SDL_BUILD} -lSDL3 -Wl,-rpath,@executable_path "
                 + " ".join(f"-framework {f}" for f in HOST_FRAMEWORKS)),
        description="MACOS HOST LINK $out",
    )

    n.rule(name="macos_alltypes", command=f"mkdir -p $$(dirname $out) && sed -f {MUSL_DIR}/tools/mkalltypes.sed $in > $out",
           description="MACOS MUSL $out")
    n.rule(name="macos_syscall_h",
           command="mkdir -p $$(dirname $out) && cp $in $out && sed -n -e s/__NR_/SYS_/p < $in >> $out",
           description="MACOS MUSL $out")
    n.rule(name="macos_version_h", command=f"mkdir -p $$(dirname $out) && echo '#define VERSION \"{MUSL_VERSION}\"' > $out",
           description="MACOS MUSL $out")
    n.rule(name="macos_gl_stubs", command="mkdir -p $gen_dir $host_dir && $python tools/android_gl_stubs.py $command_arguments",
           description="MACOS GL STUBS $out")
    n.rule(name="macos_posix_stubs",
           command=f"mkdir -p $gen_dir && $python tools/android_posix_stubs.py {LINUX_DIR}/src/posix.h $out_c $out_list",
           description="MACOS POSIX STUBS $out")
    n.rule(name="macos_imports", command="$python tools/android_imports.py $arch_arguments $out_s $in",
           description="MACOS IMPORTS $out")
    n.rule(name="macos_host_thunks",
           command="mkdir -p $$(dirname $out) && $python tools/macos_host_thunks.py $out --headers $headers --lists $in",
           description="MACOS HOST THUNKS $out")

    _generate_variant(n, sln, config, "native", angles["arm64"], libsdl)
    # the same with the original's debug checks off (HALO_RELEASE), for
    # playing: build/macos-release/Halo.app (ninja macos_release_app)
    _generate_variant(n, sln, config, "release", angles["arm64"], libsdl)
    _generate_variant(n, sln, config, "x86_64", angles["x86_64"], libsdl)
    n.newline()


def _generate_variant(n: Writer, sln: Any, config: Dict[str, Any], variant: str, angle: Path, libsdl: Path) -> None:
    native = variant in ("native", "release")
    release = variant == "release" or getattr(sln, "port_release", False)
    build = BUILD if variant == "native" else Path("build/macos-release") if variant == "release" else \
        Path("build/macos-x86_64")
    stage = build / "Halo"
    guest_dir = build / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    arch = (ANDROID_DIR / "guest" / "libc" / "arch" / "arm64_32") if native else \
        (PORT_DIR / "guest" / "libc" / "arch" / "x32")
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = stage / "halo_guest.elf"
    host_executable = stage / "halo"
    python = "$python"
    target = {"native": "macos", "release": "macos_release"}.get(variant, "macos_x86_64")
    cc_rule = "macos_native_cc" if native else "macos_guest_cc"
    host_arch = "arm64" if native else "x86_64"

    # ---------- generated headers and sources

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.build(outputs=alltypes, rule="macos_alltypes",
            inputs=[arch / "bits" / "alltypes.h.in", MUSL_DIR / "include" / "alltypes.h.in"])
    n.build(outputs=syscall_h, rule="macos_syscall_h", inputs=arch / "bits" / "syscall.h.in")
    n.build(outputs=version_h, rule="macos_version_h")

    guest_gl_c = gen_dir / "guest_gl.c"
    gl_imports = gen_dir / "gl_imports.list"
    gl_thunks_c = build / "host" / "host_gl_thunks.c"
    registers = 8 if native else 6
    n.build(outputs=[guest_gl_c, gl_imports, gl_thunks_c], rule="macos_gl_stubs",
            implicit=[Path("tools/android_gl_stubs.py"), LINUX_DIR / "src" / "gl.h"],
            variables={"command_arguments": (f"--integer-registers {registers} --host-thunks {gl_thunks_c} "
                                             f"{LINUX_DIR}/src/gl.h {KHRONOS_DIR}/GLES3/gl32.h "
                                             f"{KHRONOS_DIR}/GLES2/gl2ext.h {guest_gl_c} {gl_imports}"),
                       "gen_dir": str(gen_dir), "host_dir": str(build / "host")})

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.build(outputs=[guest_posix_c, posix_imports], rule="macos_posix_stubs",
            implicit=[Path("tools/android_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"],
            variables={"out_c": str(guest_posix_c), "out_list": str(posix_imports), "gen_dir": str(gen_dir)})

    imports_s = gen_dir / "imports.s"
    host_imports_list = PORT_DIR / "host_imports.list"
    n.build(outputs=imports_s, rule="macos_imports", inputs=[host_imports_list, posix_imports, gl_imports],
            implicit=[Path("tools/android_imports.py")],
            variables={"out_s": str(imports_s), "arch_arguments": "" if native else "--arch x86_64"})
    host_table_c = build / "host" / "host_import_table.c"
    thunk_headers = [ANDROID_DIR / "guest" / "runtime" / "guest_host.h",
                     PORT_DIR / "guest" / "runtime" / "guest_host_desktop.h", LINUX_DIR / "src" / "posix.h"]
    n.build(outputs=host_table_c, rule="macos_host_thunks", inputs=[host_imports_list, posix_imports],
            implicit=[Path("tools/macos_host_thunks.py"), *thunk_headers],
            variables={"headers": " ".join(str(h) for h in thunk_headers)})

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, semantics_header, platform_semantics_header]

    # ---------- the guest

    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch}", f"-isystem {MUSL_DIR}/arch/generic",
        f"-isystem {MUSL_DIR}/include",
    ]
    abi_flags = NATIVE_ABI_FLAGS if native else X86_ABI_FLAGS
    guest_abi = " ".join(abi_flags + (["-DHALO_RELEASE"] if release else []))
    guest_code = " ".join(GUEST_CODE_FLAGS)
    tool_implicit = list(generated_headers)
    if native:
        tool_implicit += [Path("tools/android_asm_convert.py"), Path("tools/macos_arm64_rebase.py")]
    # Darwin's weak aliases for the arm64_32 compiler (the Android port's)
    darwin_features = [f"-I{ANDROID_DIR}/guest/libc/src_include"] if native else []

    def guest_object(source: Path, cflags: str, prefix: str = "") -> Path:
        obj = obj_dir / prefix / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(build)):
            obj = obj_dir / prefix / source.relative_to(build).with_suffix(".o")
        elif str(source).startswith(str(BUILD)):
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        n.build(outputs=obj, rule=cc_rule, inputs=source, implicit=tool_implicit, variables={"cflags": cflags})
        return obj

    # musl
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}", *darwin_features,
        f"-I{MUSL_DIR}/src/include", f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources_in(MUSL_DIR)]
    libguestc = guest_dir / "libguestc.a"
    n.build(outputs=libguestc, rule="macos_ar", inputs=musl_objects)

    # the game
    objects: List[Path] = []
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
        guest_abi, guest_code, " ".join(game_flags),
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include", game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {ANDROID_DIR}/include/halo_android_variadic_prototypes.h"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))

    # the platform layer shared with Linux, and the guest runtime
    runtime_dirs = f"-I{ANDROID_DIR}/guest/runtime -I{PORT_DIR}/guest/runtime -I{ANDROID_DIR}/include"
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w",
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", runtime_dirs,
        f"-I{TOML_DIR}", f"-I{KCP_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", f"-I{KHRONOS_DIR}", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    # replaced by the guest runtime (memory_watch.c) or not part of the
    # macOS port (updater.c: no self-updating; posix_*.c: in the host)
    guest_host_only = {"memory_watch.c", "updater.c"}
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_host_only:
            continue
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", *libc_includes, f"-I{MUSL_MATH_DIR}/include",
        f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    math_objects: List[Path] = []
    for source in musl_math_sources():
        math_objects.append(guest_object(source, musl_math_cflags))
    objects += math_objects
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        runtime_dirs, f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}", *darwin_features,
        f"-I{MUSL_DIR}/src/include", f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", runtime_dirs, f"-I{LINUX_DIR}/src",
        f"-I{SDL_DIR}/include", f"-I{KHRONOS_DIR}", *libc_includes,
    ])
    runtime_sources = sorted((ANDROID_DIR / "guest" / "runtime").glob("*.c")) + \
        sorted((PORT_DIR / "guest" / "runtime").glob("*.c"))
    runtime_objects: List[Path] = []
    for source in runtime_sources:
        if source.name in ("guest_thread.c", "guest_start.c"):
            runtime_objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name == "guest_memory_watch.c":
            runtime_objects.append(guest_object(source, platform_cflags))
        else:
            runtime_objects.append(guest_object(source, runtime_cflags))
    runtime_objects.append(guest_object(guest_gl_c, runtime_cflags))
    runtime_objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="macos_as", inputs=imports_s,
            variables={"asflags": "--target=aarch64-linux-gnu -mcpu=apple-m1" if native else "--target=x86_64-linux-gnux32"})
    runtime_objects.append(imports_o)
    objects += runtime_objects

    linker_script = (ANDROID_DIR / "guest" / "guest.ld") if native else (PORT_DIR / "guest" / "guest.ld")
    n.build(outputs=image, rule="macos_guest_link", inputs=objects, implicit=[libguestc, linker_script],
            variables={"ldflags": f"-m {'aarch64linux' if native else 'elf32_x86_64'} -T {linker_script}",
                       "libs": str(libguestc)})

    # ---------- the host

    host_objects: List[Path] = []
    host_obj_dir = build / "host" / "obj"
    image_define = [] if native else [f"-DHALO_GUEST_IMAGE_BASE=0x{X86_IMAGE_BASE:08x}u"]
    host_cflags = " ".join([
        f"-arch {host_arch}", f"-mmacosx-version-min={MACOS_MINIMUM}", "-O2", "-g", "-Wall", "-Wno-unused-function",
        "-Wno-deprecated-declarations", *image_define,
        f"-I{ANDROID_DIR}/include", f"-I{PORT_DIR}/host", f"-I{SDL_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{TOML_DIR}", f"-I{KHRONOS_DIR}",
    ])
    host_sources = sorted((PORT_DIR / "host").glob("*.c")) + sorted((PORT_DIR / "host").glob("*.m")) + [
        LINUX_DIR / "src" / "posix_files.c", LINUX_DIR / "src" / "posix_net.c",
    ]
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        cflags = host_cflags + (" -fobjc-arc" if source.suffix == ".m" else "")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source, variables={"cflags": cflags},
                implicit=[libsdl])
        host_objects.append(obj)
    miniupnpc_cflags = " ".join([host_cflags, f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}",
                                 *MINIUPNPC_DEFINES])
    for source in [LINUX_DIR / "src" / "posix_upnp.c", *miniupnpc_sources()]:
        obj = host_obj_dir / ("miniupnpc_" + source.name + ".o" if source.parent.parent == MINIUPNPC_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source,
                variables={"cflags": miniupnpc_cflags + (" -w" if source.name != "posix_upnp.c" else "")})
        host_objects.append(obj)
    for generated in (host_table_c, gl_thunks_c):
        obj = host_obj_dir / (generated.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=generated, variables={"cflags": host_cflags + " -w"})
        host_objects.append(obj)
    # the x86-64 host leaves the low 4 GB to the guest (a 64 KB __PAGEZERO)
    host_ldflags = f"-arch {host_arch}" + ("" if native else " -Wl,-pagezero_size,0x10000")
    n.build(outputs=host_executable, rule="macos_host_link", inputs=host_objects, implicit=[libsdl],
            variables={"ldflags": host_ldflags})

    # ---------- staging: SDL3 and ANGLE next to the executable

    # SDL3 is built for both; each build stages its own architecture only
    # (the native one has no Intel code, which Rosetta would run)
    staged = [stage / "libSDL3.0.dylib"]
    n.build(outputs=staged[0], rule="macos_thin", inputs=libsdl, variables={"arch": host_arch})
    for name in ("libEGL.dylib", "libGLESv2.dylib"):
        n.build(outputs=stage / name, rule="macos_copy", inputs=angle / name)
        staged.append(stage / name)
    n.build(outputs=target, rule="phony", inputs=[host_executable, image, *staged])

    # ---------- the application bundle (ninja macos_app): the same files in
    # Halo.app/Contents/MacOS, with an Info.plist; the game's own folder is
    # then ~/Library/Application Support/Halo (host_main.c)

    if native:
        bundle = build / "Halo.app" / "Contents"
        bundle_files = []
        for built in (host_executable, *staged):
            n.build(outputs=bundle / "MacOS" / built.name, rule="macos_copy", inputs=built)
            bundle_files.append(bundle / "MacOS" / built.name)
        # the guest image is data to macOS: Resources, not MacOS (signing)
        n.build(outputs=bundle / "Resources" / image.name, rule="macos_copy", inputs=image)
        bundle_files.append(bundle / "Resources" / image.name)
        n.build(outputs=bundle / "Info.plist", rule="macos_copy", inputs=PORT_DIR / "Info.plist")
        n.build(outputs=bundle / "Resources" / "halo.icns", rule="macos_icon", inputs=PORT_DIR / "Info.plist",
                implicit=[Path("port/android/art")])
        if variant == "native":
            n.rule(name="macos_sign", command="codesign --force --deep --sign - $bundle > /dev/null 2>&1 && touch $out",
                   description="MACOS SIGN $bundle")
        signed = build / "Halo.app.signed"
        n.build(outputs=signed, rule="macos_sign",
                inputs=[*bundle_files, bundle / "Info.plist", bundle / "Resources" / "halo.icns"],
                variables={"bundle": str(build / "Halo.app")})
        n.build(outputs=target + "_app", rule="phony", inputs=[signed])

    # ---------- the runtime test (port/macos/tests): its own guest image,
    # staged with the same host

    test_stage = build / "test" / "Halo"
    test_source = PORT_DIR / "tests" / "guest_runtime_test.c"
    test_object = guest_object(test_source, runtime_cflags)
    test_image = test_stage / "halo_guest.elf"
    # (the memory watch is the renderer's, and brings in the platform layer)
    test_runtime = [o for o in runtime_objects if o.name != "guest_memory_watch.o"]
    n.build(outputs=test_image, rule="macos_guest_link", inputs=[test_object, *test_runtime],
            implicit=[libguestc, linker_script],
            variables={"ldflags": f"-m {'aarch64linux' if native else 'elf32_x86_64'} -T {linker_script}",
                       "libs": str(libguestc)})
    test_staged = []
    for built in (host_executable, *staged):
        n.build(outputs=test_stage / built.name, rule="macos_copy", inputs=built)
        test_staged.append(test_stage / built.name)
    n.build(outputs=target + "_test", rule="phony", inputs=[test_image, *test_staged])

    # the maths determinism test (port/macos/tests/math_determinism_test.c):
    # the game's matrix maths and halo_ functions, as compiled for the game;
    # what they would call on a failed assertion is left unresolved
    math_stage = build / "math_test" / "Halo"
    math_test_object = guest_object(PORT_DIR / "tests" / "math_determinism_test.c", runtime_cflags)
    matrix_object = next(o for o in objects if o.as_posix().endswith("source/math/matrix_math.o"))
    math_image = math_stage / "halo_guest.elf"
    n.build(outputs=math_image, rule="macos_guest_link",
            inputs=[math_test_object, matrix_object, *math_objects, *test_runtime],
            implicit=[libguestc, linker_script],
            variables={"ldflags": f"-m {'aarch64linux' if native else 'elf32_x86_64'} -T {linker_script} "
                                  "--unresolved-symbols=ignore-all",
                       "libs": str(libguestc)})
    math_staged = []
    for built in (host_executable, *staged):
        n.build(outputs=math_stage / built.name, rule="macos_copy", inputs=built)
        math_staged.append(math_stage / built.name)
    n.build(outputs=target + "_math_test", rule="phony", inputs=[math_image, *math_staged])

