#!/bin/sh
# Builds and runs the macOS port's runtime test (guest_runtime_test.c) for
# the native build, or the x86-64 one with "x86_64". Run from the
# repository's root after python configure.py.
set -e
if [ "$1" = "x86_64" ]; then
	target=macos_x86_64_test
	folder=build/macos-x86_64/test/Halo
else
	target=macos_test
	folder=build/macos/test/Halo
fi
ninja "$target"
cd "$folder"
exec ./halo
