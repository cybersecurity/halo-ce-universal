"""Ninja rules for the 64-bit ARM Linux build (``ninja linux_arm64``).

The game's data needs 32-bit pointers, and 64-bit ARM processors have no
32-bit mode on Linux any more than on Android. So this build runs the game as
the Android port does (port/android/README.md, "How the port operates"): the
guest image is the same ILP32 AArch64 code (tools/android_build.py,
generate_guest_image), with the Linux desktop's code paths instead of the
app's, and the host is a Linux executable instead of an app's library:

- build/linux_arm64/halo: the host (port/android/host, with
  port/linux/arm64/host_main.c as its entry point), with the guest image
  inside it;
- build/linux_arm64/libSDL3.so.0: SDL3, next to the executable, which finds
  it there (an rpath of $ORIGIN), as few distributions have SDL3 yet.

Refer to "64-bit ARM" in port/linux/README.md.
"""

import json
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List

from .android_build import GUEST_ABI_FLAGS, SDL_TAG, TOML_DIR, fetch_third_party, generate_guest_image
from .linux_build import MBEDTLS_DIR, MINIUPNPC_DEFINES, MINIUPNPC_DIR, compile_launcher, miniupnpc_sources
from .ninja_syntax import Writer

PORT_DIR = Path("port/linux/arm64")
ANDROID_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/linux_arm64")
THIRD_PARTY = BUILD / "third_party"
SDL_DIR = THIRD_PARTY / "SDL3"
# the system's OpenGL ES and EGL headers (libgles-dev and libegl-dev on
# Debian and Ubuntu, mesa on Arch Linux)
GL_HEADERS = Path("/usr/include")

# the Android guest's, with the desktop's code paths (no HALO_ANDROID)
LINUX_ARM64_GUEST_ABI_FLAGS = [flag for flag in GUEST_ABI_FLAGS if flag != "-DHALO_ANDROID=1"]

HOST_LIBRARIES = ["SDL3", "GLESv2", "EGL", "m", "dl", "pthread"]


def is_linux_arm64() -> bool:
    return sys.platform.startswith("linux") and platform.machine().lower() in ("aarch64", "arm64")


def linux_arm64_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR]


def generate_linux_arm64_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not PORT_DIR.is_dir():
        return
    if not is_linux_arm64():
        n.comment("Linux arm64 build: only on a 64-bit ARM Linux computer")
        return
    if not (GL_HEADERS / "GLES3" / "gl32.h").is_file():
        n.comment("Linux arm64 build: no OpenGL ES headers in /usr/include (libgles-dev)")
        return
    try:
        fetch_third_party(THIRD_PARTY)
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"Linux arm64 build disabled: cannot fetch musl/SDL3 ({error})", file=sys.stderr)
        return
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))
    cc = getattr(sln, "linux_arm64_cc", None) or "clang"

    n.comment("64-bit ARM Linux build (ninja linux_arm64); see port/linux/README.md")
    n.variable("linux_arm64_guest_cc", cc)
    n.variable("linux_arm64_cc", cc)

    guest = generate_guest_image(
        n, sln, config, prefix="linux_arm64", label="LINUX ARM64", build=BUILD, third_party=THIRD_PARTY,
        guest_cc=cc, gl_headers=GL_HEADERS, ar="llvm-ar", ld="ld.lld",
        builtins="$$($linux_arm64_cc -print-libgcc-file-name)", asm_target="aarch64-linux-gnu",
        abi_flags=LINUX_ARM64_GUEST_ABI_FLAGS, extra_runtime=[PORT_DIR / "guest_desktop.c"],
        extra_imports=[PORT_DIR / "host_imports.list"])

    # ---------- SDL3, built from the same source as the guest's headers

    sdl_build = BUILD / "sdl3-build"
    libsdl = sdl_build / "libSDL3.so.0"
    n.rule(
        name="linux_arm64_sdl3",
        command=(f"cmake -S {SDL_DIR} -B {sdl_build} -G Ninja -DCMAKE_C_COMPILER=$linux_arm64_cc "
                 f"-DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF "
                 f"-DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF > {BUILD}/sdl3-configure.log && "
                 f"ninja -C {sdl_build} > {BUILD}/sdl3-build.log"),
        description=f"LINUX ARM64 SDL3 {SDL_TAG}",
        pool="console",
    )
    n.build(outputs=libsdl, rule="linux_arm64_sdl3", implicit=[SDL_DIR / "CMakeLists.txt"])

    # ---------- the host

    host_obj_dir = BUILD / "host" / "obj"
    n.rule(
        name="linux_arm64_host_cc",
        command=f"{compile_launcher(sln)}$linux_arm64_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="LINUX ARM64 HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    host_cflags = " ".join([
        "-O2", "-g", "-fPIC", "-Wall", "-Wno-unused-function", "-D_GNU_SOURCE",
        f"-I{ANDROID_DIR}/include", f"-I{ANDROID_DIR}/host", f"-I{SDL_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{TOML_DIR}",
    ])
    host_sources = [source for source in sorted((ANDROID_DIR / "host").glob("*.c")) if source.name != "host_main.c"]
    host_sources += [
        PORT_DIR / "host_main.c", PORT_DIR / "host_desktop.c",
        LINUX_DIR / "src" / "posix_files.c", LINUX_DIR / "src" / "posix_net.c",
        TOML_DIR / "tomlc17.c",
    ]
    host_objects: List[Path] = []
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="linux_arm64_host_cc", inputs=source, variables={"cflags": host_cflags})
        host_objects.append(obj)
    miniupnpc_cflags = " ".join([host_cflags, f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}",
                                 *MINIUPNPC_DEFINES])
    for source in [LINUX_DIR / "src" / "posix_upnp.c", *miniupnpc_sources()]:
        obj = host_obj_dir / ("miniupnpc_" + source.name + ".o" if source.parent.parent == MINIUPNPC_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="linux_arm64_host_cc", inputs=source,
                variables={"cflags": miniupnpc_cflags + (" -w" if source.name != "posix_upnp.c" else "")})
        host_objects.append(obj)
    # the self-updater's download (posix_update.c, with port/third_party/mbedtls)
    mbedtls_cflags = " ".join([host_cflags, f"-I{MBEDTLS_DIR / 'include'}"])
    n.build(outputs=host_obj_dir / "posix_update.c.o", rule="linux_arm64_host_cc",
            inputs=LINUX_DIR / "src" / "posix_update.c", variables={"cflags": mbedtls_cflags})
    host_objects.append(host_obj_dir / "posix_update.c.o")
    for source in sorted((MBEDTLS_DIR / "library").glob("*.c")):
        obj = host_obj_dir / "mbedtls" / (source.name + ".o")
        n.build(outputs=obj, rule="linux_arm64_host_cc", inputs=source,
                variables={"cflags": f"{mbedtls_cflags} -I{MBEDTLS_DIR / 'library'} -w"})
        host_objects.append(obj)
    table_obj = host_obj_dir / "host_import_table.c.o"
    n.build(outputs=table_obj, rule="linux_arm64_host_cc", inputs=guest["host_table_c"],
            variables={"cflags": host_cflags})
    host_objects.append(table_obj)
    # the guest image, inside the executable (host_main.c)
    image_obj = host_obj_dir / "guest_image.S.o"
    n.build(outputs=image_obj, rule="linux_arm64_host_cc", inputs=PORT_DIR / "guest_image.S",
            implicit=[guest["image"]], variables={"cflags": f'-DGUEST_IMAGE=\\"{guest["image"]}\\"'})
    host_objects.append(image_obj)

    output = BUILD / "halo"
    staged_sdl = BUILD / "libSDL3.so.0"
    n.rule(
        name="linux_arm64_host_link",
        command=(f"$linux_arm64_cc -o $out $in -L{sdl_build} "
                 + " ".join(f"-l{lib}" for lib in HOST_LIBRARIES)
                 + " '-Wl,-rpath,$$ORIGIN' -Wl,--no-undefined"),
        description="LINUX ARM64 LINK $out",
    )
    n.build(outputs=output, rule="linux_arm64_host_link", inputs=host_objects, implicit=[libsdl])
    n.rule(name="linux_arm64_copy", command="cp -L $in $out", description="LINUX ARM64 STAGE $out")
    n.build(outputs=staged_sdl, rule="linux_arm64_copy", inputs=libsdl)
    n.build(outputs="linux_arm64", rule="phony", inputs=[output, staged_sdl])
    n.newline()
