#!/bin/sh
# Builds port/macos/tests/raytrace_test.c with the ray-traced lighting
# (port/linux/src/raytrace_gl.c) and runs it on ANGLE, as the native macOS
# build draws; the pictures go to build/macos/raytrace_test. Run from the
# repository's root after ninja macos.
set -e
out=build/macos/raytrace_test
mkdir -p "$out/src"
# copies, so their "platform.h" is the shim's, not port/linux/src's
cp port/linux/src/raytrace_gl.c port/linux/src/gl_functions.c "$out/src/"
# with the macOS host's Metal ray tracing (world-space rays) as the game has it
clang -O2 -g -DHALO_GLES -DHALO_MACOS -fobjc-arc -Iport/macos/tests/shim -Iport/linux/src \
	-Iport/macos/guest/runtime -Iport/macos/host -Iport/android/include -Ibuild/macos/third_party/khronos \
	-Ibuild/macos/third_party/SDL3/include \
	port/macos/tests/raytrace_test.c "$out/src/raytrace_gl.c" "$out/src/gl_functions.c" \
	port/macos/host/host_metal_rt.m \
	-Lbuild/macos/sdl3-build -lSDL3 -Wl,-rpath,"$PWD/build/macos/sdl3-build" \
	-framework Metal -framework Foundation -o "$out/raytrace_test"
cd "$out"
./raytrace_test "$OLDPWD/build/macos/third_party/angle-arm64"
for picture in *.ppm; do
	sips -s format png "$picture" --out "${picture%.ppm}.png" > /dev/null
done
echo "pictures in $out"
