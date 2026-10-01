# macOS

`ninja macos` builds the game as a native 64-bit arm64 (Apple silicon)
executable, `build/macos/halo`, and an application bundle,
`build/macos/Halo.app`. It is the Linux port (`port/linux`) compiled as
64-bit code: the game sources, the platform layer and the settings are the
Linux build's, and [port/linux/README.md](../linux/README.md) describes them.

## Requirements

- macOS 13 or later on Apple silicon.
- The Xcode command line tools (`xcode-select --install`): clang and the SDK.
- SDL3, FFmpeg (Bink video) and ninja from Homebrew: `brew install sdl3 ffmpeg ninja`.
- The game data in `assets/maps` (see the [README](../../README.md)).

```sh
python3 configure.py
ninja macos
build/macos/halo          # or open build/macos/Halo.app
```

## 64-bit

The game was written for the Xbox's 32-bit CPU, and its data formats embed
32-bit pointers: cache files hold tag data whose blocks and references point
at each other by address, game state is saved and restored as raw memory, and
Direct3D resources carry physical addresses. The Linux, Windows and Android
builds keep 32-bit pointers (Android as ILP32 code in a 64-bit process), but
a 64-bit macOS process cannot: arm64 macOS reserves the low 4 GB of every
process. So this build is 64-bit code, with two changes that the other builds
never see:

- **Pointers inside Xbox data** (`HALO_64BIT`). The platform layer reserves
  4 GB at a fixed host address (`port/linux/src/xbox_memory.c`) and puts
  everything the game's data can point at there: the contiguous memory (game
  state, tag cache, Direct3D resources) and the game's heap
  (`port/linux/src/xbox_heap.c`). Xbox address X is host address base + X.
  Structures laid out like the Xbox's declare their pointer fields
  `XPTR(type)`, a 32-bit Xbox address, and convert with `xbox_pointer` and
  `XBOX_ADDRESS` (`source/cseries/xbox_address.h`). Everywhere else these are
  plain pointers and the conversions do nothing, so the 32-bit builds and the
  byte-matching build compile what they always did. Code that differs on
  64-bit is under `#ifdef HALO_64BIT`, and the size asserts of structures
  whose layout 64-bit pointers change are left out there.
- **`long`**. MSVC's `long` is 32 bits; macOS's is 64. The build compiles a
  copy of every source with the Xbox's ABI (the game, the platform layer, the
  Xbox SDK declarations) in which `long` is spelled `int` and `%ld` is `%d`
  (`tools/lp64_rewrite.py`, into `build/macos/lp64`). The sources keep
  `long`; diagnostics name the original files.

The build turns the conversions that would truncate a 64-bit pointer into
errors (`-Werror=pointer-to-int-cast` and the like), so a place that still
treats an Xbox address as a pointer, or the reverse, does not compile.

## macOS

- OpenGL 4.1 (through Metal) is the newest macOS has. The renderer does
  without the later features it uses elsewhere, as the Android build does
  with OpenGL ES: clip control is done in the vertex shader
  (`HALO_GL_NO_CLIP_CONTROL`), mipmap copies are blits, and visibility tests
  wait for their results. Buffer uploads are unsynchronized appends, which
  Apple's driver needs to keep up (`d3d8_gl.c`, `buffer_append`).
- Vertical sync is the OpenGL context's own (`port/macos/src/macos_video.c`):
  SDL 3.4's waits for the main thread's run loop, which the game's first
  frames do not run.
- Apple silicon has 16 KB pages; page protection works in host pages
  (`xbox_memory.c`, `memory_watch.c`).
- Bink video (the intro, attract and credits movies) is decoded with FFmpeg
  (`port/macos/src/macos_bink.c`) behind the Bink calls the game makes, in
  place of the other ports' `bink_null.c`.
- The application bundle (`port/macos/bundle.py`, `Info.plist`) registers the
  `halo://` and Discord URL schemes that internet play invites use.

## Status

A beta. The game plays its movies, campaign and multiplayer, and joins and
hosts internet games with the Linux and Windows builds. Known problem:
vertical sync is not yet confirmed to pace the frames.
