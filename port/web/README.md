# Web (iPhone, iPad, Android and desktop browsers)

`ninja web` builds the game as WebAssembly, with a page that installs as a
home-screen web app: `build/web/site`. On an iPhone or iPad, open the page in
Safari, tap Share, then *Add to Home Screen*. On Android, open it in Chrome
(or Edge, or Samsung Internet) and push *Install app* on the page, or choose
*Install app* in the browser's menu. The installed app runs full screen and
works offline.

The GitHub Actions workflow `.github/workflows/web.yml` builds and tests the
site for each pushed commit. The public game is packaged and deployed
separately; see [GITHUB_PAGES.md](../../GITHUB_PAGES.md).

The web build uses the platform layer of the Linux build (`port/linux/src`)
and the code paths of the Android build (OpenGL ES 3, the display's shape).
Refer to [port/linux/README.md](../linux/README.md) and
[port/android/README.md](../android/README.md).

## Requirements

To play:

- iOS or iPadOS 17 or later (Safari, or the installed web app), Android with
  a recent Chrome, or a recent Chrome, Edge or Firefox on a computer. The browser needs WebGL 2 in a worker
  (OffscreenCanvas), SharedArrayBuffer and the Origin Private File System.
- About 2 GB of free storage for the game data.
- An Xbox disc image (`.iso` or `.xiso`) of Halo: Combat Evolved, any version.

To build:

- Python and ninja.
- Emscripten 6 or later: `emcc` on the `PATH`, or the SDK in `~/emsdk`
  (`configure.py --web-emcc` names another). Install it with
  [emsdk](https://emscripten.org/docs/getting_started/downloads.html).
- A network connection for the first build: `configure.py` downloads the
  SDL 3.4.16 headers to `build/web/third_party`.

```
python configure.py --release
ninja web
```

To try the site on a computer, serve `build/web/site` over HTTP, for example
with `python -m http.server -d build/web/site`, and open
`http://localhost:8000`. The page reloads once while its service worker
starts (see "Cross-origin isolation").

## Game data

1. Open the app. It checks what the browser supports.
2. Push *Choose disc image* and select the disc image. On an iPhone, it can
   be in the Files app (iCloud Drive, On My iPhone, a USB drive).
3. Wait while the page copies the `maps` folder (about 1.8 GB) into the
   app's private storage. The disc image is read in place and not changed.
4. Push *Play*.

The copy is kept in the site's Origin Private File System. The saved games
(`z:\`, `u:\`) and `config.toml` are there too. *Settings and data* can
export the saved games as a `.zip` and delete the game data. On iOS, data of
a site that is not installed can be removed by the system after some weeks
without use: install the app to keep it.

## Controls

### iPhone rotation and sound

- If the page stays upright when you turn the phone sideways, Portrait
  Orientation Lock may be on. Swipe down from the top-right to open Control
  Center and turn it off (the padlock with a circular arrow).
- For sound, swipe down to Control Center, turn off Silent Mode if its control
  is available, and raise the volume. Otherwise, use the Ring/Silent switch
  or the Action button if it is assigned to Silent Mode.
- On iPhones with a Home button, open Control Center by swiping up from the
  bottom instead.

Apple's guides cover [screen rotation](https://support.apple.com/guide/iphone/rotate-your-iphone-screen-iph3badf94ec/ios),
[Control Center](https://support.apple.com/guide/iphone/use-and-customize-control-center-iph59095ec58/ios),
and [Silent Mode](https://support.apple.com/guide/iphone/silence-iphone-iph81c7fd7d1/ios).

### Game input

Settings and data offers an optional **Compact FPS touch controls (gameplay)**
layout. Select it before starting; the original controller layout remains the
default and includes menu-navigation buttons. The compact layout has a floating
movement stick that is invisible at rest, drag-to-aim, and hold-to-fire with
immediate release. Dragging the Fire action also aims. Tap the upper-left
weapon/ammo HUD to swap weapons. Smaller action buttons provide jump, melee,
reload/use, crouch, zoom, grenade throw and grenade-type switching.

The compact HUD omits D-pad, Start/Select and flashlight controls. It assumes a
full-viewport landscape game canvas. Its input is released on interruption,
backgrounding, resize or controller connection. Test its input handling with
`node --test tools/test_web_touch_controls.mjs` (Node.js 22 or newer).


- A controller that the browser knows (Xbox, PlayStation, MFi, Switch Pro):
  as on Android. The browser sees a controller only after a button is pushed.
- Touch: the left half of the screen is a stick for moving; drag on the right
  half to aim; the buttons are the controller's. The touch controls hide
  while a controller is connected. *Settings and data* sets the aim
  sensitivity, or turns the touch controls off.
- A keyboard and mouse (iPad or computer): as on Linux. Click the game to lock
  the pointer; Esc releases it. Hold Tab (or F1) to show the multiplayer
  scoreboard; `(HOST)` identifies the current host beside their player name
  and follows host migrations. The Ping column shows each player's measured
  round-trip latency to that host in milliseconds (host: `0`, unavailable or
  stale: `--`). The host probes every three seconds and shares its measurements;
  changing hosts clears the previous measurements. Use the mouse wheel to
  change weapons.
- Empty or whitespace-only player names receive a random, unique name from the
  host, including players joining after the match starts.

## Online play

The September 29, 2026 release combines automatic startup, solo System Link,
host recovery, room switching and cached-launcher compatibility with the
Chrome/macOS presentation and repeated-rejoin fixes. Each public site's
`deployment.json` identifies the current source commit, runtime and packaged
file hashes. Apply an offered update to use the new worker frame path.

Historical Chrome testing on the earlier runtime `0780c5587f467c15` confirmed
a one-player Blood Gulch start and a second player joining the running match
(two players on two machines). Both tabs later suffered renderer crashes
with error 11. Those observations describe the old runtime, before the
presentation and reconnect fixes below.

Chrome-family browsers on macOS use a separate frame path. It reads
the completed default framebuffer into owned RGBA bytes and transfers those
bytes to a 2D page canvas, avoiding Chrome's crashing ImageBitmap serializer.
Readback preserves GL state, reverses rows, and limits queued frames to two.
The presentation height defaults to 480 lines to bound readback cost. Other
browsers retain ImageBitmap presentation, with an empty-frame guard before
transfer. `?frame_transport=rgba` and `?frame_transport=bitmap` select a path
for controlled diagnostics; `?render_height=...` overrides presentation size.
A new launcher can display frames from an older cached runtime; applying
its offered update enables the new worker frame path.

Reloads retire the old RTC peer at its virtual address, deliver native EOF on
connection loss, and replace its native stream only after the game releases
the old endpoint. Pending validation cleanup closes accepted endpoints too;
joining clients ignore gameplay traffic left over from the prior connection.
Departed players leave their machine's input slots while their scores remain
available. A browser host in distributed Free-for-All Slayer with unlimited lives keeps
waiting when an opponent leaves; score limits and other game modes retain
their own end conditions. These changes have socket and player lifecycle
regressions alongside the browser transport tests.

The prior local crash/rejoin build passed an initial late join followed by five
consecutive same-address reload/rejoin cycles in Chrome on macOS. The same host match
remained playable for over six minutes, including over two minutes after the
final rejoin. Browser transport and native lifecycle regressions passed for
that build. It predates combining the fixes with the host-recovery and room
changes. The combined release passed `ninja web`, all 104 browser tests,
cache and stream-recorder checks, and native lifecycle/restart regressions
under AddressSanitizer and UndefinedBehaviorSanitizer. Physical-iPhone
gameplay, other browsers and connections across different Internet/NAT
networks still need live verification.

Browser players can play together over the internet, with the game's own
system link. The launcher joins the public **FQLX01** room on a first visit.
Both hosted domains use that same room. Share a link ending in
`?room=FQLX01` to enter it directly.

Browser rooms need the UI and Beaver Creek maps before entering
multiplayer automatically. The full main menu and desktop invites need all
maps because they can use other scenarios. Import your own Xbox disc image
in the launcher; existing completed maps are reused on later visits.
The first ready player hosts Beaver Creek Slayer; the others join that host
without navigating the game's System Link menus. The public room does not
run a permanent game server.
Players spawn with the human pistol in place of the map's plasma pistol,
including late joins and respawns.

When the host disconnects, quick play freezes the loaded match and elects a
replacement from surviving players with a complete checkpoint of that match.
The replacement adopts the existing world and roster; the other players
reattach their original machine and player slots. Scores, positions, inventory
and the match timer are retained. There is no automatic new-match fallback.
If recovery cannot complete, the match stays paused until the player explicitly
leaves. Everyone must use the new launcher and runtime.

A brief lost connection has a 10-second grace period; silent WebRTC channels
are detected after 25 seconds. Election and reconnecting add time after
detection, without loading a new map. Authoritative checkpoints are sent every
15 ticks, so authority-only state can resume from a recent checkpoint rather
than the exact instant a tab crashed. Packet epochs fence previous hosts.
*Main menu*, *Leave*, idle launchers and native invites do not participate.
The room's public brokers are not a consensus service: network partitions can
still produce divergent continuations. See [host migration](../linux/HOST_MIGRATION.md)
for the checkpoint and resume contract and its verification limits.
In historical September 29 testing on runtime `0780c5587f467c15`, Chrome's
saved engine log confirmed that a two-player client joined at tick 655,
lost the host connection, created a replacement local
server and started a new solo match automatically. Those tabs later suffered
the old runtime's renderer crash. The production restart/cancellation checks
under sanitizers and Web/build CI passed for that release; the engine log
does not establish sustained gameplay for the combined update.

Connection, hosting and map-loading progress are shown on the page. Sound
starts automatically where allowed; normal game input unlocks it and mouse
controls when browser permissions require a gesture. *Main menu* cancels quick
play; `?menu=1` opens the normal launcher and game menu instead.

For a separate group, choose *New room*, then *Share link*. Opening that
link joins its browser room, and the launcher remembers the chosen room.
The in-game *Room* button opens the room controls. Choosing another room
restarts Halo into that room using the cached maps; *Leave* closes the session
and returns to the launcher. A link joins only the room named in `?room=`.
*Leave* stays out across reloads; *Join default room* returns to FQLX01.
Historical September 29 testing on the earlier runtime switched Chrome from
FQLXQTEST8 into a fresh FQLXQTEST9
match with cached maps, and a later client joined it as the second player
on a second machine. The tabs subsequently crashed on that old runtime.
Room-navigation regressions passed; these older observations do not verify
sustained gameplay for the combined update.
Manual System Link also allows a one-player start with distributed networking,
including hosting a non-team game while waiting for other players. The host
continues simulating in background tabs. Team readiness checks and lockstep
requirements still apply.

Quick play starts only when the maps and room are ready. Idle tabs and tabs
importing a disc image do not participate in host selection.

Everyone in a room is on one network, as on a LAN: up to the game's limits
of machines and players, split screen on each machine included.

How it works (`site/net.js`, `src/web_net.c`):

- Each copy of the game has an address on the room's network, 10.x.y.z, kept
  in the browser. The game's sockets put what they send to other addresses
  in a ring in the shared memory, and take what arrives from another ring.
- The page carries those packets over WebRTC to each other player: a
  reliable, ordered data channel for the game's connections and an
  unreliable one for its datagrams. Broadcasts (system link's discovery) go
  to every player. The connections are direct between the players; no server
  carries the game.
- Players find each other through public MQTT brokers over secure WebSockets
  (broker.emqx.io, broker.hivemq.com and test.mosquitto.org, all at once), in
  a topic derived from the room's code. Everything sent there is encrypted
  (AES-GCM) with a key derived from the code, so only those who have the code
  can read the room's messages.
- WebRTC crosses most home networks with STUN (Google's and Cloudflare's
  public servers). Some networks, mobile carriers' especially, need a TURN
  relay: *Settings and data* can name one.

Desktop and Android invitations use a separate UDP protocol. Joining those
hosts requires the [native relay](../../BROWSER_NATIVE_INVITES.md), which is
not configured on the public sites. Browser rooms do not use that relay.

## How the port operates

### WebAssembly

wasm32 is an ILP32 target: `int`, `long` and pointers have 32 bits, as the
game's data formats need, and the game runs as it does on the other ports.

- The WebAssembly memory is 0x88000000 bytes and does not grow. Its top
  128 MB is the Xbox memory window at 0x80000000 (`port/linux/src/platform.h`),
  so the tag cache and the game state get the fixed addresses of their files.
  The C heap stays below it (`src/web_main.c`, `emscripten_get_heap_size`).
- The game and the platform layer are compiled with link-time optimisation.
  C89 code calls functions declared differently from their definitions,
  which x86 tolerates and WebAssembly traps on; with the whole program in one
  module, LLVM gives each such call a wrapper that adapts the arguments.
- The multivalue ABI (`-target-abi experimental-mv`) passes and returns small
  structures and unions as values, as Win32 returns them in registers:
  `hs_runtime.c` calls functions that return unions through pointers typed as
  returning `long`.
- `-mnontrapping-fptoint`: a float that does not fit an integer converts as
  on x86 instead of trapping.
- Calls whose declarations disagree with the definition in a way LLVM cannot
  adapt (integer widths, since its wrappers only bitcast), and function
  pointers called with another signature, are repaired in `#ifdef HALO_WEB`:
  local prototypes that now match their definitions (`hs.c`, `rasterizer.c`,
  `ui_widget_event_handler_functions.c` and others), the cache thread's start
  routine (`cache_files_windows.c`), the stub game engine's callbacks
  (`game_engine_stub.c`), `weapon_preprocess_node_orientations`, a `va_list`
  (`terminal.c`), and two globals defined in a header (`object_lists.h`;
  WebAssembly has no common symbols). An unoptimised link
  (`-Wl,--lto-O0`) names any call LLVM could not adapt
  `<function>_bitcast_invalid`; only libtiff's remain, which the game does not
  use.

### Threads and the page

The game runs on a pthread (Emscripten's `PROXY_TO_PTHREAD`), a Web Worker,
so it can block as a native program does. It never returns to its event loop.
The page's main thread (`site/app.js`, `site/input.js`) serves it through
memory both share (`src/web_shared.h`):

- Graphics: the game's WebGL 2 context draws into an OffscreenCanvas of its
  own thread (`src/web_library.js`). Chrome-family browsers on macOS transfer
  owned RGBA pixels; other browsers use `transferToImageBitmap`. The page
  shows the completed frame on its canvas. The page advances a counter each
  animation frame; the game waits for
  it after each frame (`display.vsync`).
- Input: the page writes keyboard, mouse and focus events into a ring that
  `SDL_PollEvent` reads, and the state of the controllers (Gamepad API, and
  the touch controls as one more controller).
- Sound: a thread of the game fills a ring of 48 kHz samples with the mixer of
  `dsound_sdl.c`; an AudioWorklet (`site/audio-worklet.js`) plays it.
- Network: split screen is a network game whose host and clients are the
  same machine. `src/web_net.c` gives the Winsock layer (`port/linux/src/xnet.c`)
  sockets that reach each other inside the page: datagrams to the loopback,
  local or a broadcast address go to the socket bound to their port, and
  stream sockets connect through queues. `HALO_NET_DEBUG=1` logs the traffic.
- Time: `GetTickCount` and `QueryPerformanceCounter` count from the start, as
  an Xbox counts from its boot. A browser's monotonic clock counts from 1970,
  past 2^31 milliseconds, and the network code compares tick counts as
  signed longs.
- Files: WasmFS mounts the Origin Private File System at `/data`, the data
  root (`HALO_DATA_ROOT`); the saved games go to `/data/save`.

`src/web_sdl.c` gives the platform layer the SDL3 functions it calls (the
Android guest's list, `port/android/guest/runtime/guest_sdl.c`).

### Browser performance and diagnostics

By default, the source worker uses the same append-only stream upload batching as the
Apollo runner. The shared recorder is embedded in `halo.js`, installed on the
worker's WebGL context, and flushed before each image is sent to the page.
This avoids repeatedly updating the large vertex/index rings between draws.
It adds about 54 MiB of CPU shadow storage for the existing triple-buffer ring;
shader code, scene resolution, and draw order are unchanged. The source
renderer already caches render state, so the Apollo state-cache wrappers are
not installed a second time.

Add `?fps=1` to show actual frames received from the game worker. The HUD keeps
up to 120 visible measurement windows in its `data-samples` attribute, including
elapsed time and canvas size; animation callbacks do not count as game frames.
Hidden time is excluded, while visible stalls count toward the measured FPS.
Compare `?fps=1&batch_streams=0` to disable batching (or pass
`--HALO_WEB_BATCH_STREAMS=0` to the runtime). Reload between comparisons.
`?fps=1&render_height=720` overrides the presentation-height cap for diagnostics
(480–1440 lines). The default cap is 480 lines on iPhone/iPad and for RGBA
presentation, and 1440 elsewhere;
the engine still renders 480 lines, and CSS scales the displayed canvas to the
screen. At most two frame transfers can await the page's acknowledgement.
When that queue is full, the worker submits GPU commands without creating
another bitmap or pixel readback, or blocking simulation. Hidden or failed
presentations release their frame and acknowledgement too.

`?fps=1&geometry_cache=1` enables an experimental native geometry path (runtime
argument `--HALO_WEB_GEOMETRY_CACHE=1`) and automatically disables JavaScript
stream batching. Dynamic vertex/index uploads use separate buffers in three
rotating frame buckets. Vertex ranges observed unchanged twice are retained;
exact comparisons of the uploaded bytes, including color conversion, invalidate
changed data. Streams used by the current draw cannot evict each other. The
cache is limited to 8 MiB of CPU snapshots and up to 8 MiB of retained GPU data.
Stream storage has a 32 MiB target and trims old allocations under pressure;
a single larger draw may exceed that target until subsequent draws can trim it.
Driver-held copies for in-flight draws are outside this accounting. With
`--HALO_GPU_STATS=1`, the log reports cache reuse, upload bytes, and stream
storage/evictions. This path remains opt-in pending browser and device FPS
measurements.

On September 29, 2026, a local Apple Silicon/Chrome comparison used the
Silent Cartographer opening and the same 1960 × 1044 presentation size.
The updated source binary with `batch_streams=0` returned to roughly 2–4 FPS
in its early opening samples. With batching enabled, two launches measured
about 46–52 and 48–50 FPS in their first two sampling windows; later beach
combat in those runs was around 59–79 FPS. The brief Pelican ride reached the
120 Hz display limit. These are scene-specific local observations, not a
sustained or physical-iPhone performance guarantee. Each launch reused the
same assets and saves; rendering remained at the original quality settings.
Those measurements predate the native geometry cache and presentation changes
above and do not establish their FPS effect.

Validation covers the DOM-free worker installer, opt-out, explicit flush before
bitmap transfer, existing upload/readback/VAO/uniform snapshot cases, bounded
measurement history, hidden intervals, and visible stalls. Run:

```sh
node tools/test_stream_batch.mjs
node --test tools/tests/*.test.cjs
python3 tools/test_web_geometry.py
```

The native geometry test uses mocked GL storage with AddressSanitizer and
UndefinedBehaviorSanitizer to check changed-byte invalidation, multistream
isolation, exact upload sizes, frame rotation, and cache/storage limits.

### WebGL 2

The renderer takes its OpenGL ES 3.0 path (port/android/README.md), with
these differences for WebGL 2 (`#ifdef HALO_WEB` in `xbox_textures.c` and
`src/web_host.c`):

- WebGL has no texture swizzle: decoded textures are turned from BGRA to RGBA
  on the CPU.
- S3TC textures go to the GPU only as 2D textures whose sides are multiples of
  four; the others are decoded. iOS has no S3TC: all are decoded.
- The visibility tests (lens flares) report every sample visible: WebGL gives
  query results only between tasks, which the game's thread never reaches.
- Default streamed buffer writes use `glBufferSubData`, which copies. The
  opt-in geometry cache uses per-upload `glBufferData` and retained buffers;
  neither path uses GPU fences in the browser.
- Strides are at most 255 bytes: the immediate mode's vertices (16
  attributes of 4 floats) go up as one array per attribute.

The Xbox memory cannot be write-protected in WebAssembly. The memory watch
(`src/web_memory_watch.c`) keeps a hash of each page the renderer caches, and
a changed hash counts as a write. Textures of 128 KB or less, which the game
rewrites between draws (the text renderer's character cache), are checked at
every use; larger ones change through file reads, which announce themselves.

### Cross-origin isolation

SharedArrayBuffer needs the page to be cross-origin isolated
(`Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers).
Static hosts such as GitHub Pages cannot send headers, so the service worker
(`site/sw.js`) adds them. The first visit reloads the page once. The service
worker also keeps the site for use offline; a new build replaces it as a
whole when the player accepts the update.

### The game data

`site/xiso-worker.js` reads the XDVDFS file system of the disc image as
`port/linux/src/xiso.c` does and writes the files of `maps` with the OPFS
synchronous access handles. It writes `maps/.complete` last; the page starts
the game only when it is there.

## Limits

- The WebAssembly memory needs 2.1 GB of address space. If the browser does
  not give it, the page says so. Close other apps and tabs.
- System link is between copies of the web build in one room ("Online
  play"), not with Xboxes or the other ports on the local network: browsers
  have no UDP.
- Bink video is not available. The game skips the movies.
- Lens flares show through walls (see "WebGL 2").
- Performance depends on the device. The game draws 480 lines at the shape of
  the screen and scales them up.

## Find problems

*Settings and data* > *Show log* shows the page's log and `debug.txt`, the
game's log, and can copy them for a report. *Log graphics errors* sets
`debug.gl_debug`. When the game stops, the page shows why.
