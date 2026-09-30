#!/bin/sh
# Builds the maths determinism test (math_determinism_test.c) for the native
# and the x86-64 builds and compares their hashes: the same hashes mean the
# two compute the game's maths bit for bit alike. Run from the repository's
# root after python3 configure.py.
set -e
ninja macos_math_test macos_x86_64_math_test
native=$(cd build/macos/math_test/Halo && ./halo 2>/dev/null | grep -v "^halo host")
x86=$(cd build/macos-x86_64/math_test/Halo && ./halo 2>/dev/null | grep -v "^halo host")
echo "native (arm64):"; echo "$native"
echo "x86-64:"; echo "$x86"
if [ "$native" = "$x86" ]; then echo "SAME"; else echo "DIFFERENT"; exit 1; fi
