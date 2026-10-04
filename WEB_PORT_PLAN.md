# Web port plan

Goal: run the decompiled game in a browser as WebAssembly, built with
Emscripten, reusing the native ports' platform layer (`port/linux/src`) the
same way the Windows and Android builds do. The release is hosted on itch.io.
The player supplies their own Halo: Combat Evolved PAL disc image; nothing
copyrighted ships with the page.

wasm32 has 32-bit pointers, which suits the game: its tag data, cache files and
saved games embed 32-bit pointers, which is why every native build is 32-bit
(i386, and `arm64_32` for the Android guest).

The work is split into eight lanes that can proceed in parallel. Lane 8
integrates the others.

## 1. Build/toolchain

Add an `emscripten` / `emcc` target to the current build system.

- Add a `ninja web` graph to `configure.py` beside `linux`, `windows` and
  `android_apk`, with a `--web-cc` option (default `emcc`) modelled on
  `--linux-cc`. Keep it separate from the byte-matching graph, as the other
  native builds are.
- Reuse the XDK header overlay (`tools/linux_sdk_overlay.py`,
  `libs/d3d8/generate_sdk_overlay.py`). The web build needs `xbox/include`
  like every other build.
- Link with `-sUSE_SDL=3`, `-sMAX_WEBGL_VERSION=2`, `-sALLOW_MEMORY_GROWTH`
  and an explicit `-sMAXIMUM_MEMORY` (see lane 2). The output is
  `build/web/halo.{js,wasm}`.
- Start with a debug build (`-g`, `-sASSERTIONS`), matching the native
  debug-by-default behaviour. `configure.py --release` should turn on `-O2` or
  `-O3` and turn off assertions.

## 2. Memory

Attack the fixed-address / 32-bit Xbox pointer assumptions. This is the
riskiest issue in the port.

- `port/linux/src/xbox_memory.c` reserves the Xbox contiguous-memory arena
  with `mmap(..., MAP_FIXED_NOREPLACE)` at `PLATFORM_CONTIGUOUS_BASE`
  (`0x80000000`, 128 MB; see `platform.h`), and commits and decommits pages
  with `MAP_FIXED`. Emscripten's `mmap` cannot place memory at a fixed
  address.
- `PLATFORM_PHYSICAL_TO_VIRTUAL` / `PLATFORM_VIRTUAL_TO_PHYSICAL` convert
  addresses by setting or clearing the top bit, so the arena's address matters
  as well as its size.
- Options, in order of preference:
  1. Keep the base at `0x80000000`: set `-sMAXIMUM_MEMORY=4GB`, grow linear
     memory past `0x88000000` at start-up, and treat the arena as already
     committed. Page commit becomes zeroing plus bookkeeping, and protection
     becomes a no-op because wasm has no page protection. This needs
     wasm32 4 GB memory in the browser. Chrome and Firefox support it; check
     Safari.
  2. Move the arena to a base allocated at run time and change the two macros
     to add and subtract an offset instead of setting and clearing a bit.
     Audit the game code for anything else that assumes the high bit.
- Check how the Android host (`port/android/host/host_memory.c`) handles the
  same problem, and reuse its approach where it applies.
- `memory_watch.c` depends on page protection. Stub it out on web.

## 3. Graphics

Translate the existing OpenGL layer to WebGL2-compatible GL and identify
unsupported GL 4.5 calls.

- The D3D8-on-GL layer is `d3d8_gl.c`, `d3d8_resources.c`, `xbox_textures.c`
  and `gl_functions.c`. NV2A shader translation is in `nv2a_vsh.c` and
  `nv2a_psh.c`.
- The GLES 3 path already exists for Android: `d3d8_gl.c` picks
  `"300 es"` / `"310 es"` as `xgpu_capabilities.shading_language`. WebGL2 is
  GLES 3.0, so aim for `"300 es"` and make sure nothing requires 3.1.
- Two shaders hard-code `#version 450 core` (`nv2a_psh.c:327`,
  `nv2a_vsh.c:148`). Route them through the capability string, or give them
  ES variants.
- Audit `gl_functions.c` for anything missing from WebGL2: client-side vertex
  arrays, `glMapBuffer*`, `glPolygonMode`, depth clamp, texture swizzle,
  BGRA formats, compressed formats (S3TC needs
  `WEBGL_compressed_texture_s3tc`), and sync or query objects.
- The vertical-blank thread in `d3d8_gl.c` (line 357) should become a
  `requestAnimationFrame` callback.

## 4. SDL

Make SDL3's browser input, audio and window lifecycle compile under
Emscripten.

- Files: `sdl_platform.c`, `xinput_sdl.c`, `dsound_sdl.c`.
- Mouse aim needs pointer lock, which can only be requested from a user
  gesture. Map the F12 capture toggle onto it.
- Browsers only start audio after a user gesture, so resume the SDL audio
  device on the first click or key press.
- Gamepads come through the browser Gamepad API via SDL. Test a standard
  controller mapping.
- Replace window resizing and the `HALO_WINDOW_SCALE` letterboxing with canvas
  sizing, and handle the fullscreen API.

## 5. Filesystem

Replace POSIX and native file assumptions with the Emscripten FS and
IndexedDB where necessary.

- Files: `posix_files.c`, `xbox_files.c`, `posix.h`.
- `d:\` (the data root) maps to a read-only mount of the player's `maps/`.
  Use WORKERFS, or copy it into MEMFS or OPFS (see lane 7).
- The save root (`z:\` cache partition, about 800 MB, and `u:\` user data)
  goes on IDBFS or OPFS so that saves persist, with an explicit sync after
  writes. The 800 MB cache copy is too large for a first boot. Investigate
  skipping it or pointing it straight at the data mount.
- Environment variables (`HALO_DATA_ROOT`, `HALO_SAVE_ROOT`, and so on)
  become URL query parameters or `Module.ENV`.
- Keep the case-insensitive path matching.

## 6. Threads/timing

Isolate pthread and synchronization issues, and get a first boot working
either single-threaded or with browser threads.

- Threads are created in `xbox_kernel.c:553` (the game's own threads),
  `d3d8_gl.c:357` (vertical blank) and `dsound_sdl.c:502` (silent audio
  clock). There are about 50 mutex, condition-variable and `timedwait` call
  sites.
- **Option A, browser threads:** build with `-pthread` and
  `-sPROXY_TO_PTHREAD`. This needs `SharedArrayBuffer`, so the page must be
  served with COOP/COEP headers. Some hosts, including itch.io, need an opt-in
  to send them.
- **Option B, single-threaded:** run the 30 Hz simulation and the render
  interpolation (`port/linux/game/render_interpolation.c`) from
  `emscripten_set_main_loop`, and turn the other threads into per-frame
  callbacks. Anything that blocks on the main thread must be removed or run
  under `-sASYNCIFY`.
- Get a first boot with whichever option is quicker, then decide.

## 7. Assets/loader

Create the browser-side mechanism for supplying whatever game data users are
legally expected to provide.

The player supplies only their Halo: Combat Evolved PAL disc image, and the
page extracts it. There is no separate extraction step and no server.
A first version is in `port/web/` (`index.html`, `app.js`, `xiso.js`).

- **Reading the image:** `xiso.js` reads the Xbox disc filesystem (XDVDFS)
  straight from the chosen file, one slice at a time, so a multi-gigabyte
  image is never loaded whole. It finds the game partition whether the image
  is an extract-xiso "xiso" (offset 0) or a full XGD1/2/3 dump (for example
  `0x18300000` for XGD1, Halo's disc).
- **Checking it:** it reads the header of every `maps/*.map` before copying
  anything. The `head`/`foot` signatures, cache version 5 and build
  `01.01.14.2342` must all match, as `cache_files.c` requires. A wrong
  image is refused with a per-file reason.
- **Storing it:** it copies `maps/` (1.7 GB of the 3.1 GB game partition) into OPFS under
  `halo-data/`, which becomes `d:\` (lane 5). It skips `bink/` (cutscenes,
  which the port does not play yet), `Xdemos/` and `default.xbe`, since
  browsers can limit a site to under 3 GB. It requests persistent
  storage, checks the quota and shows a progress bar. A later visit reuses the
  data, and "Delete imported data" removes it.
- **Serving it locally:** `python3 port/web/serve.py` serves the page at
  `http://localhost:8000` with the COOP/COEP headers a threaded build needs.
- **Still to do:** a Safari fallback, since Safari's OPFS has no
  `createWritable` on the main thread (move the copy into a worker), and
  mounting `halo-data/` into the Emscripten FS once the WebAssembly build
  exists.

## 8. Integration/release

Own the itch.io page, `index.html`, the fullscreen canvas, packaging,
logging, crash reproduction, and continuous merging of the other lanes' work.

- `port/web/`: the HTML shell, the loader UI from lane 7, and COOP/COEP
  guidance for local hosting (a small `serve.py`).
- Logging: send `d:\debug.txt` and assertion failures to the console, with a
  button to download the log.
- Crash reproduction: keep a known-good commit, the browser version and an
  `init.txt` (for example `map_name levels\a10\a10`) with each bug report.
- Packaging: a zip of `index.html`, `halo.js`, `halo.wasm` and the shell's
  assets for itch.io, with SharedArrayBuffer enabled if lane 6 chooses
  threads.
- Merge the lanes into an integration branch continuously and keep
  `ninja web` building on every merge.

## Milestones

1. `ninja web` compiles and links (lanes 1, 3 and 4 stubbed as needed).
2. The page boots to the game log with data loaded (lanes 2, 5, 6 and 7).
3. The main menu renders and takes input (lanes 3 and 4).
4. `a10` is playable with audio and persistent saves.
5. A release build is published on itch.io (lane 8).

## Prerequisites

- Emscripten (`emcc`), plus ninja and Python.
- The XDK's `xbox/include`, as for every other build.
- A Halo: Combat Evolved PAL disc image (build 01.01.14.2342) for testing.
  It is never committed or shipped.
