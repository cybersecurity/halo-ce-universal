#!/usr/bin/env bash
# Installs what the Android build needs on Ubuntu 24.04 (WSL2 or native),
# as the GitHub runner image has it (.github/workflows/build.yml): the
# compilers and tools from apt, and the Android SDK command-line tools,
# platform 35, build-tools and a pinned NDK in ~/Android/Sdk. Safe to run
# again: it installs only what is missing.
#
#     bash tools/wsl_setup_android.sh
set -euo pipefail

SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
TOOLS_ZIP=commandlinetools-linux-11076708_latest.zip
NDK_VERSION=27.2.12479018
PACKAGES=(clang lld ninja-build cmake openjdk-17-jdk-headless curl unzip rsync git python3 python3-pytest)

missing=()
for package in "${PACKAGES[@]}"; do
	dpkg -s "$package" >/dev/null 2>&1 || missing+=("$package")
done
if [ ${#missing[@]} -gt 0 ]; then
	echo "installing: ${missing[*]} (sudo asks for your password)"
	sudo apt-get update
	sudo apt-get install -y "${missing[@]}"
fi

if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
	mkdir -p "$SDK/cmdline-tools"
	temporary=$(mktemp -d)
	curl -fL -o "$temporary/tools.zip" "https://dl.google.com/android/repository/$TOOLS_ZIP"
	unzip -q "$temporary/tools.zip" -d "$temporary"
	rm -rf "$SDK/cmdline-tools/latest"
	mv "$temporary/cmdline-tools" "$SDK/cmdline-tools/latest"
	rm -rf "$temporary"
fi

sdkmanager="$SDK/cmdline-tools/latest/bin/sdkmanager"
yes | "$sdkmanager" --sdk_root="$SDK" --licenses >/dev/null || true
"$sdkmanager" --sdk_root="$SDK" --install "platforms;android-35" "build-tools;35.0.0" "ndk;$NDK_VERSION" "platform-tools"

profile="$HOME/.profile"
if ! grep -q 'ANDROID_HOME=' "$profile" 2>/dev/null; then
	{
		echo "export ANDROID_HOME=\"$SDK\""
		echo "export ANDROID_NDK_HOME=\"$SDK/ndk/$NDK_VERSION\""
	} >> "$profile"
fi
echo "Android build tools are ready (SDK $SDK, NDK $NDK_VERSION)."
