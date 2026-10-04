#!/usr/bin/env python3

# root build script: writes build.ninja for the native ports (Linux, Windows,
# Android, macOS)

import argparse
import io
import os
import sys
from pathlib import Path
from types import SimpleNamespace

from tools import ninja_syntax
from tools.android_build import android_configure_inputs, generate_android_build
from tools.linux_build import generate_linux_build, linux_configure_inputs
from tools.windows_build import generate_windows_build, windows_configure_inputs

# arguments
parser = argparse.ArgumentParser()
parser.add_argument(
    "--linux-cc",
    metavar="BINARY",
    help="compiler for the native Linux build, `ninja linux` (default: clang)",
)
parser.add_argument(
    "--compiler-launcher",
    metavar="BINARY",
    help="a program that runs each compile, such as ccache "
    "(links and the Android host's few units run the compiler directly)",
)
parser.add_argument(
    "--release",
    action="store_true",
    help="release builds (Linux, Windows, Android): assertions are not checked",
)
parser.add_argument(
    "--lto",
    choices=["full", "thin", "off"],
    default="full",
    help="link-time optimisation (Linux, Windows): full (the default: the whole game "
    "optimised as one module, the fastest code and the slowest links), thin (parallel and incremental) or off",
)
parser.add_argument(
    "--portable",
    action="store_true",
    help="x86 builds (Linux, Windows): code for any x86-64 processor (SSE2) rather than for this "
    "machine's (-march=native, the default); use it for builds that run on other computers",
)
parser.add_argument(
    "--pgo",
    nargs="?",
    const="train",
    default="use",
    choices=["use", "train", "off"],
    help="profile-guided optimisation: use (the default) optimises with the profiles in pgo/, "
    "train (or plain --pgo) first records this platform's profile if it is missing, by letting an instrumented "
    "build play every campaign level (needs the game data in assets/ and a display; Linux and Windows), off does "
    "without",
)
parser.add_argument(
    "--pgo-profile",
    metavar="PROFDATA",
    type=Path,
    help="profile-guided optimisation from this profile instead",
)
parser.add_argument(
    "--android-ndk",
    type=str,
    help="Android NDK for `ninja android` (default: ANDROID_NDK_HOME, or the newest under the Android SDK)",
)
parser.add_argument(
    "--android-guest-cc",
    type=str,
    help="clang with the arm64_32 target for the Android guest (default: clang)",
)
parser.add_argument("--macos", action="store_true", help="generate the rebased Apple Silicon guest target")
parser.add_argument("--android-guest-only", action="store_true", help="build the portable ARM image without an Android host")
parser.add_argument("--android-guest-llvm-bin", type=Path, help="directory containing llvm-ar and ld.lld")
parser.add_argument("--android-guest-gl-include", type=Path, help="directory containing Khronos GLES headers")
parser.add_argument("--android-guest-builtins", type=Path, help="optional AArch64 compiler builtins archive")
args = parser.parse_args()

# the settings the builds read
sln = SimpleNamespace(
    build_dir=Path("build"),
    linux_cc=args.linux_cc,
    compiler_launcher=args.compiler_launcher,
    port_release=args.release,
    port_lto=args.lto,
    port_portable=args.portable,
    port_pgo=args.pgo,
    port_pgo_profile=args.pgo_profile,
    android_ndk=args.android_ndk,
    android_guest_cc="tools/macos_guest_cc.py" if args.macos else args.android_guest_cc,
    android_guest_only=args.android_guest_only or args.macos,
    android_guest_llvm_bin=args.android_guest_llvm_bin,
    android_guest_gl_include=args.android_guest_gl_include,
    android_guest_builtins=args.android_guest_builtins,
    macos=args.macos,
)


def is_windows() -> bool:
    return os.name == "nt"


# build.ninja
out = io.StringIO()
n = ninja_syntax.Writer(out)
n.variable("ninja_required_version", "1.3")
n.newline()

configure_script = Path(os.path.relpath(os.path.abspath(sys.argv[0])))
n.comment("The arguments passed to configure.py, for rerunning it.")
n.variable(
    "configure_args",
    [f'"{arg}"' if any(ch.isspace() for ch in arg) else arg for arg in sys.argv[1:]],
)
n.variable("python", f'"{sys.executable}"')
n.newline()

generate_linux_build(n, sln)
generate_android_build(n, sln)
generate_windows_build(n, sln)

n.comment("Reconfigure on change")
n.rule(
    name="configure",
    command=f"$python {configure_script} $configure_args",
    generator=True,
    description=f"RUN {configure_script}",
)
n.build(
    outputs="build.ninja",
    rule="configure",
    implicit=[
        configure_script,
        Path("tools/ninja_syntax.py"),
        *linux_configure_inputs(),
        *android_configure_inputs(),
        *windows_configure_inputs(),
    ],
)
n.newline()

# the build for this computer, where it could be generated (the Windows
# build is left out when SDL cannot be fetched, for instance)
default = "macos_guest" if args.macos else "windows" if is_windows() else "linux"
if f"\nbuild {default}: " in out.getvalue():
    n.comment("Default rule: the build for this computer")
    n.default(default)

with open("build.ninja", "w", encoding="utf-8") as f:
    f.write(out.getvalue())
out.close()
