#!/usr/bin/env bash
# Builds the Android APK in WSL from a Windows working tree. The Android
# build needs Linux (the NDK paths and the shell commands of its ninja
# rules), and it is faster on the Linux file system, so the sources are
# copied to ~/halo-build/src first (tools/wsl_sync_tree.sh: LF line
# endings, only the changed files). build/ stays there, so builds are
# incremental. The APK is copied back to dist/android/.
#
#     wsl -d Ubuntu -- bash /mnt/c/path/to/the/repository/tools/wsl_build_android.sh [--release]
set -euo pipefail

# (not from ~/.profile: under set -u, Ubuntu's .bashrc stops at its unset PS1)
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/27.2.12479018}"
SOURCE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$HOME/halo-build/src"
# (the APK is signed with the debug key in both flavors: --release only
# configures an optimised build, and names the file app-release.apk)
FLAVOR=debug
CONFIGURE=()
if [ "${1:-}" = "--release" ]; then
	FLAVOR=release
	CONFIGURE+=(--release)
fi

bash "$SOURCE/tools/wsl_sync_tree.sh" "$SOURCE" "$HOME/halo-build/mirror" "$WORK"
cd "$WORK"
python3 configure.py "${CONFIGURE[@]}"
ninja android_apk
mkdir -p "$SOURCE/dist/android"
cp "port/android/app/build/outputs/apk/debug/app-debug.apk" "$SOURCE/dist/android/app-$FLAVOR.apk"
echo "APK: dist/android/app-$FLAVOR.apk"
