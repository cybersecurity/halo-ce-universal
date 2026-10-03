#!/usr/bin/env bash
# Compiles the given platform files (port/linux/src) as the Android guest
# build does, but without HALO_ANDROID, syntax only: the desktop branches of
# the shared code. Warnings are off, but a call of an undeclared function is
# still an error. Only files that the guest build compiles can be checked
# (not memory_watch.c or posix_*). Run in ~/halo-build/src after a build.
set -uo pipefail
cd "$HOME/halo-build/src"
status=0
for file in "$@"; do
	object="build/android/guest/obj/port/linux/src/${file%.c}.o"
	command=$(ninja -t commands "$object" | tail -1)
	if [ -z "$command" ]; then echo "no command for $object"; status=1; continue; fi
	# (the command chains the compile with the assembly steps: keep the compile)
	command=$(echo "$command" | sed -E 's/ && .*//; s/ -DHALO_ANDROID(=[^ ]*)?//g; s/ -w( |$)/ /g; s/ -o [^ ]+//; s/ -MMD//; s/ -MF [^ ]+//')
	# (__i386__: the prefix header accepts only the 32-bit x86 or Android targets)
	if eval "$command -D__i386__ -fsyntax-only -w -Werror=implicit-function-declaration"; then echo "ok $file"; else echo "FAILED $file"; status=1; fi
done
exit $status
