# macOS

`ninja macos` builds the game for Macs with Apple silicon (M1 and later),
as native arm64 code. `ninja macos_app` makes an application from it:
`build/macos/Halo.app`. `ninja macos_x86_64` builds the game for Intel Macs
(it also runs on Apple silicon through Rosetta 2).

The game draws with OpenGL ES 3 through ANGLE on Metal. It plays sound
through SDL3. It accepts a keyboard and mouse, and game controllers (Xbox,
PlayStation, Switch Pro and other controllers that macOS knows). The game
needs macOS 14.4 or later.

## Requirements

- Xcode or the Xcode Command Line Tools (clang, lipo, codesign).
- Python 3.9 or later, and these packages: `pip3 install --user ninja cmake ziglang`.
  The `ziglang` package supplies `ld.lld` and `llvm-ar`, which link the
  game's image. An `ld.lld` and `llvm-ar` on the `PATH` operate too.
- ANGLE, universal (x86_64 and arm64): `libEGL.dylib` and
  `libGLESv2.dylib`. `configure.py` takes them from Steam's or Google
  Chrome's Chromium Embedded Framework if one is installed. The option
  `--macos-angle <folder>` selects a different folder.
- A network connection for the first build. `configure.py` downloads musl
  1.2.5 (its SHA-256 is checked), SDL 3.4.16 (the release commit is
  checked) and the Khronos OpenGL ES headers to `build/macos/third_party`.

## Build and start the game

1. Go to the root folder of the repository.
2. Enter `python3 configure.py`.
3. Enter `ninja macos_app`.
4. Open `build/macos/Halo.app`, or enter `open build/macos/Halo.app`.

### Sign and notarize the application

To give `Halo.app` to other people, sign it with a Developer ID, and
notarize it so that macOS opens it without a warning:

1. One time: `xcrun notarytool store-credentials halo-notary --apple-id <Apple ID> --team-id <team>`.
   It asks for an app-specific password (made at appleid.apple.com).
2. `NOTARY_PROFILE=halo-notary port/macos/sign_app.sh`

The script signs with the hardened runtime and
`port/macos/Halo.entitlements`, notarizes, staples the ticket and writes
`build/macos/Halo-macos-arm64.zip`. Without `NOTARY_PROFILE` it only signs.

`ninja macos` builds the same game without the application bundle, in
`build/macos/Halo`. Start it with `build/macos/Halo/halo`.

## Game data

The game needs the `maps/` folder of the Xbox game (not the Windows game).
Make a disc image (`.iso` or `.xiso`) of your own Xbox disc of Halo: Combat
Evolved. A PC DVD drive cannot read an Xbox game disc: use a modified Xbox,
or an Xbox 360 DVD drive with the Kreon firmware, and a disc image tool
such as extract-xiso.

1. Start the game.
2. At the first start, the game asks for the disc image. Select it.
3. The game extracts `maps/` (approximately 2 GB). Then the game starts.

| Build | Location of `maps/`, `config.toml`, `debug.txt` and `host.txt` |
| --- | --- |
| `Halo.app` | `~/Library/Application Support/Halo` |
| `build/macos/Halo/halo` | `build/macos/Halo`, next to the executable |

The saved games are in `~/Library/Application Support/Halo` for both.
`paths.data` and `paths.saves` in `config.toml` select other folders.

## Controls and settings

The controls and most of the settings are those of the Linux build. Refer
to [port/linux/README.md](../linux/README.md). The keys that the macOS
build adds:

| Key | Mac keyboard | Function |
| --- | --- | --- |
| F7 | ⌘P | Show or hide the frames-a-second counter. |
| F8 | ⌘R | Change the resolution: native, 2160p, 1440p, 1080p, 720p, then the Xbox's 640x480. |
| F11 | ⌘F | Switch between fullscreen and a window. |
| F12 | ⌘G | Release or capture the mouse. |

⌘Q quits on a second press within two seconds (Q is the flashlight, and ⌘
is held for the keys above); the window's close button quits at once.

On a Mac keyboard, F11 shows the desktop and F8 to F12 are media keys
unless fn is held, so the Command shortcuts do the same. Each key shows
what it did in the game's console at the top of the screen (for example
`resolution: 1080p`), and writes it to `debug.txt`.

`HALO_FULLSCREEN=false` (or `display.fullscreen = false`) starts the game
in a window:

```bash
open --env HALO_FULLSCREEN=false --env HALO_FPS=1 build/macos/Halo.app
```

These settings are new, or have a different default on macOS:

| Setting | Default on macOS | Function |
| --- | --- | --- |
| `display.resolution` | `"native"` | The picture's pixels. `"native"`: the display's in fullscreen, the window's in a window. `"720p"`, `"1080p"`, `"1440p"`, `"2160p"`: that many lines, in the shape of the display or window. `"<width>x<height>"`: that picture. `"xbox"`: 640x480. |
| `display.show_fps` | `false` | Start with the frames-a-second counter shown (F7 / ⌘P). |
| `display.render_scale` | `1.0` | Multiplies the resolution: below 1.0 is faster, above 1.0 supersamples (up to 4.0). |
| `network.allow_upnp` | `false` | The local network does not need a forwarded port. |
| `network.join_from_clipboard` | `false` | An invite link on the clipboard does not join a game. |

## Multiplayer

### Local network

Start the game on each machine. One machine creates a system link game. The
other machines see the game in the list of system link games.

macOS asks one time for permission to use the local network. Select
"Allow".

### The internet, with forwarded ports

A machine can join a game on the internet when the host forwards ports on
its router:

1. The host forwards TCP port 5150 and UDP port 5150 to its computer, and
   creates a system link game.
2. The other machine sets `network.broadcast = "<host's internet address>"`
   in `config.toml`, starts the game and opens the list of system link
   games.

The search goes to the host. The host answers each machine outside its
local network that searched in the last minute, and the other machine
connects to the address that the answer came from. If the other machine's
router changes the ports of its connections, that machine also forwards UDP
port 5151.

### Internet play

Invite links (`halo://join/...`) operate as on Linux. Refer to "Internet
play" in [port/linux/README.md](../linux/README.md). The host's game puts
its link on the clipboard and in `debug.txt`. To join on macOS, give the
link to the other player. Clicking it opens `Halo.app` (which registers
`halo://`) and joins the game, whether the game runs already or not. Or
set `network.join_from_clipboard = true`, copy the link, and switch to the
game.

## How the port operates

### The guest and the host

The game's data contains 32-bit pointers, so the game must operate with
32-bit pointers (refer to "ILP32 code" in
[port/android/README.md](../android/README.md)). The macOS port uses the
design of the Android port: the game ("the guest") is ILP32 code in a
static ELF image, and a 64-bit program ("the host",
[host/](host)) loads it and does its system calls, its SDL calls and its
OpenGL calls.

An arm64 macOS process cannot map memory below 4 GB (its `__PAGEZERO`
covers the low 4 GB), and arm64_32 code keeps its addresses below 4 GB. The
two builds solve this in different ways:

| Build | Guest code | Guest memory |
| --- | --- | --- |
| `macos` (native) | arm64_32, as on Android, with its memory accesses rebased (below) | A 4 GB region at any 4 GB-aligned address |
| `macos_x86_64` | x32 (x86-64 instructions, 32-bit pointers) | The low 4 GB: the host has a 64 KB `__PAGEZERO` |

### Rebasing (the native build)

`tools/macos_arm64_rebase.py` changes the guest's assembly: each memory
access and each indirect branch goes through `x28`, the base of the guest's
region, plus the low 32 bits of the address register:

```
ldr w0, [x1, #8]   ->   add x27, x28, w1, uxtw
                        ldr w0, [x27, #8]
```

The guest is compiled with `-ffixed-x27 -ffixed-x28`, so the compiler does
not use the two registers. A register contains a guest address (a 32-bit
pointer) or an address in the region (from `sp` or `adrp`). The low 32 bits
of both are the guest address, so the one formula is correct for both.
Accesses through `sp` and `x29` do not change: the guest's stacks are in
the region.

The functions of the host that the guest calls get thunks
(`tools/macos_host_thunks.py`, `tools/android_gl_stubs.py --host-thunks`).
A thunk adds the base of the region to each pointer argument. A host
pointer that the guest keeps (a directory of `posix_directory_open`) goes
to the guest as a small handle.

### The host

| File | Function |
| --- | --- |
| `host_main.c` | Finds the folders, loads ANGLE and the image, runs the game's `main()` on the main thread (Cocoa needs it) on a stack in guest memory. |
| `host_memory.c` | The guest's region, the Xbox memory window, `mmap` for the guest, and the write tracking of the texture cache. |
| `host_loader.c` | Loads the ELF image and fills its import table. |
| `host_thread.c` | Threads with stacks in guest memory, and the calls from the host into the guest. |
| `host_syscall.c` | The guest's Linux system calls on macOS: the numbers, flags, error codes, `stat` and `dirent`, and futexes on `os_sync_wait_on_address`. |
| `host_sdl.c`, `host_gl.c` | SDL3 and OpenGL ES for the guest. |

Apple silicon has 16 KB pages, and the Xbox had 4 KB pages. The game puts
blocks at fixed 4 KB-aligned addresses in the Xbox memory window, so the
window stays mapped, and the guest's `mmap` and `munmap` in it only clear
the memory.

### Game source changes

`HALO_ANDROID` marked all the code of the Android port. It is now three
macros (`port/linux/include/halo_linux_prefix.h`):

- `HALO_GUEST`: the ILP32 guest (Android and macOS), for example the
  stack walker and the true signature of `player_effect_screen_fade_in`.
- `HALO_GLES`: the OpenGL ES renderer (Android and macOS).
- `HALO_ANDROID`: only the Android app (its storage, touch, toasts).

The macOS guest is compiled with `HALO_MACOS`, so it uses the desktop's
code for the window, the mouse, the keyboard and the first start.

## Tests

- `ninja macos_test` (and `ninja macos_x86_64_test`) builds
  [tests/guest_runtime_test.c](tests/guest_runtime_test.c) as a guest image
  with the host. Start it with `build/macos/test/Halo/halo`. It tests musl,
  threads and futexes, thread-local storage, files and the host's
  directory handles, time, the rebased code and sockets. `port/macos/tests/run_guest_tests.sh` does both steps.
- `port/macos/tests/run_determinism_test.sh` runs the game's matrix maths
  and `halo_` functions over 1.4 million inputs on the native and the
  x86-64 builds and compares hashes of the results. Machines in a system
  link game must compute alike, so an optimisation (compiler flags, SIMD)
  must keep these hashes.

## Performance

- Two builds: `ninja macos_app` makes `build/macos/Halo.app`, with the
  original's debug checks (assertions: the game stops at the one that
  fails and says where), for finding bugs; `ninja macos_release_app` makes
  `build/macos-release/Halo.app` without them (`HALO_RELEASE`), for playing.
  Both use the same data, saves and settings.
  `APP=build/macos-release/Halo.app port/macos/sign_app.sh` signs the
  release build.

- The native guest is compiled for the M1 (`-mcpu=apple-m1`), without
  fused multiply-add and without `-ffast-math`, so the results are those
  of the other builds (refer to "Tests").
- `HALO_PROFILE=1 build/macos/Halo/halo` samples every thread 1000 times a
  second, and writes `profile.txt` to the game's folder at exit: the game
  functions and the host functions (SDL, ANGLE) where the time goes.
- `HALO_FPS=1` writes to `host.txt` every 5 seconds: the frames per
  second, and the average and the slowest frame's time.
- `display.render_scale = 0.75` (or F8 / ⌘R to 1440p or 1080p) draws fewer
  pixels, the largest speed-up on a Retina display.

## Find problems

- `HALO_MAP=b30` starts a map without the menus: a campaign level's name
  (`a10` to `d40`), a multiplayer map's (`bloodgulch`) or a scenario path.
- `HALO_TEST_INPUT="script:47=switch,50=zoom,52-60=turnright"` plays the
  scripted actions in those seconds since start (forward, back, left, right,
  turnleft, turnright, up, down, fire, grenade, jump, crouch, zoom, action,
  flashlight, reload, switch, start); with `HALO_SCREENSHOT_DIR` and
  `HALO_SCREENSHOT_EVERY=<frames>` it records a test drive, and
  `HALO_EXIT_AFTER=<seconds>` ends it.
- `HALO_COMMANDS="47=cheat_all_weapons;50=cheat_spawn_warthog"` runs
  console commands at those seconds since start: with the scripted input,
  a test drive can stage a scene (weapons, vehicles, grenades).

- `host.txt` in the game's folder is the log of the host, and, when the
  game does not start from a terminal, of the game's port (for one, why
  it quit: `window closed` is ⌘Q or the window's close button). `debug.txt`
  is the log of the game. `host.old.txt` is the run before's `host.txt`. Start `halo` in a terminal to see both.
- If the guest code stops, `host.txt` shows the registers and the frame
  chain. To find the functions, enter
  `llvm-symbolizer --obj=build/macos/Halo/halo_guest.elf <address>` with
  the guest addresses.
- `HALO_HIDDEN_WINDOW=1 HALO_EXIT_AFTER=5 build/macos/Halo/halo` starts
  the game without a window and stops it after 5 seconds.

## Security

Before this port was made, the repository was examined for code that could
harm a Mac (refer to `SECURITY_AUDIT.md` next to the repository). The
macOS port:

- does not update itself (`port/linux/src/updater.c` is not part of it);
- does not ask the router to forward a port, and does not join games from
  the clipboard, unless `config.toml` says so;
- lets the internet play tunnel give a remote machine's traffic only to the
  ports of the game's own sockets (`p2p_socket_port` in
  `port/linux/src/p2p.c`), not to other
  programs on the Mac.
