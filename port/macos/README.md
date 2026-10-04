# Apple Silicon macOS

The app runs native ARM64 CPU code and uses SDL3 with ANGLE's Metal backend.
The game retains its 32-bit pointers and data layout inside a 64-bit Mac host:
`compiler/guest_rebase.cpp` rebases guest memory accesses into a reserved range.
This is not an x86-64 port. Shared guest/libc tools under `port/android/` do not
require an Android SDK or device.

## Build

On an Apple Silicon Mac, install Xcode Command Line Tools and the public tools:

```sh
xcode-select --install
brew install python llvm@22 lld@22 sdl3 ninja
export PATH="$(brew --prefix lld@22)/bin:$PATH"
python3 tools/macos_setup.py
python3 tools/macos_build.py
open "build/macos/Halo CE Universal.app"
```

Use Python 3.10 or later. LLVM **22** builds the compiler pass; Apple's clang
builds the host. Keep Apple's compiler on `PATH`. The default LLVM path is
`/opt/homebrew/opt/llvm@22/bin`; an existing `HALO_MACOS_LLVM_BIN` override may
select another installation. [dependencies.json](dependencies.json) pins ANGLE
and Khronos headers by checksum. Setup needs no game files or private Xbox SDK.

`--host-only` rebuilds the host after a full build; `--jobs N` sets guest build
parallelism. Packaging stages a fresh bundle and preserves previous bundles in
`build/macos/app-backups.noindex`. It never installs or publishes the app.

## Install and game data

Download **halo-macos-arm64-dmg** from a successful **macOS build and package** Actions run,
open the disk image, and drag the app to Applications. Artifacts expire after
14 days and downloading them requires a GitHub account. Builds are ad-hoc
signed, without notarization or an automatic updater. macOS may block first
launch; use Apple's [instructions for opening an app](https://support.apple.com/en-us/102445).
The host targets macOS 14 or later; newer bundled SDL builds may raise the
minimum version, which the app records in its `Info.plist`.

First launch accepts your own original Xbox Halo disc image or an extracted
game/maps folder. The app checks version-5 cache headers from PAL build
`01.01.14.2342` or USA build `01.10.12.2276`, including matching `ui.map` and
`a10.map`. Other retail maps present must match that disc. Header validation
checks the supported format, not complete gameplay compatibility.

Imports use a new directory each time; a failed import preserves existing data.
The Halo menu offers **Choose Game Data on Next Launch**, **Open Saves Folder**,
About and Quit. Display and controls use the game's settings. Imported maps,
`macos-settings.json`, `config.toml`, saves and `halo.log` live outside the app:

```text
~/Library/Application Support/Halo CE Universal/
```

## Package and verify

```sh
python3 tools/macos_release.py local-dmg --output build/macos/Halo-local.dmg
```

The DMG contains the host, compiled guest, runtime libraries, build provenance
and dependency licenses. Bundle audits reject game data, local
paths and unexpected files, and verify ad-hoc signing before packaging.
`BuildInfo.txt` records the source revision and guest hash.

The single Mac CI job builds the app, verifies the DMG checksum, ARM64 architecture
and strict code signatures, then uploads the DMG. It uses standard `macos-26`,
read-only repository permissions and no signing secrets. Maintainers can run it
from GitHub without a Mac. Live gameplay still needs Mac playtesting.
